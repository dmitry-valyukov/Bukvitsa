// Движок вёрстки: DirectWrite делает анализ и шейпинг, Кнут-Пласс — строки.
//
// Порядок работы над абзацем:
//   1. анализ всего абзаца сразу — письменности, направления, места переломов;
//   2. текст режется на прогоны формата: пересечение прогонов стиля,
//      письменности, направления и подобранного шрифта;
//   3. каждый прогон шейпится в глифы;
//   4. из ширин кластеров и мест переломов собираются боксы, клей и штрафы;
//   5. Кнут-Пласс выбирает переломы;
//   6. строки нарезаются из прогонов и выключаются, как велит стиль блока:
//      по формату, влево, вправо или по центру.
//
// Анализ делается по абзацу целиком, а не по прогону: и BiDi, и правила
// переносов строки смотрят по сторонам от границы, и прогон, разобранный
// отдельно, дал бы другой ответ на своих краях.

#include <algorithm>
#include <format>
#include <stdexcept>
#include <map>
#include <span>
#include <tuple>
#include <unordered_map>

#include <dwrite_2.h>
#include <wrl/client.h>

// ensure — макрос: с импортом он не приходит.
#include "abi.h"

// Свои заголовки после всех стандартных: они ведут к импорту модуля книги, а
// стандартный заголовок после импорта MSVC уже не принимает.
#include "bukvitsa/typography/hyphenation.h"
#include "bukvitsa/typography/layout.h"
#include "bukvitsa/typography/linebreak.h"

import wxl.core;

// Память вёрстки — из STA-пула (`sta_vector`), а не из кучи CRT: вся вёрстка
// перекладывается на каждую смену кегля и размера окна, эта память повторяется.

namespace bukvitsa::typography {
namespace {

/// DirectWrite отвечает HRESULT, а вёрстка бросает исключения: обёртка, чтобы
/// каждый вызов не превращался в три строки.
inline void check(HRESULT hr) {
    if (FAILED(hr))
        throw std::runtime_error(std::format("DirectWrite: HRESULT 0x{:08X}", static_cast<unsigned>(hr)));
}
using Microsoft::WRL::ComPtr;

constexpr float kScriptScale = 0.66f;       ///< кегль над- и подстрочных
constexpr float kSuperscriptRise = 0.34f;   ///< доля кегля вверх
constexpr float kSubscriptDrop = 0.16f;
constexpr float kSpacedTracking = 0.22f;    ///< разрядка, доля кегля на глиф

/// Шаг позиций табуляции, в ширинах пробела. Четыре — выбор, а не стандарт:
/// ни Юникод, ни FB3 шага не задают; это обычный шаг редакторов кода, а
/// табуляция в книге живёт почти только в преформате.
constexpr uint32_t kTabColumns = 4;

/// Предел длины прогона шейпинга. Кластерная карта DirectWrite -- массив
/// UINT16, так что прогон, дающий больше 65535 глифов, разобрать нельзя;
/// запас взят с большим отрывом, потому что глифов бывает больше, чем
/// символов, и потому что короткие прогоны шейпятся заметно быстрее.
constexpr uint32_t kMaxRunLength = 4000;

/// На чём меряется средняя ширина знака. Не алфавит: в строке книги есть
/// пробелы и запятые, и они тоже знаки. Обе фразы — панграммы, то есть в
/// каждой все буквы своего алфавита ровно по разу.
constexpr wchar_t kCyrillicSample[] =
    L"съешь же ещё этих мягких французских булок, да выпей чаю";
constexpr wchar_t kLatinSample[] = L"the quick brown fox jumps over the lazy dog";

/* ================================================================== */
/* Источник и приёмник анализа                                        */

/// Источник текста для анализаторов DirectWrite.
///
/// Живёт на стеке ровно на время анализа, поэтому счётчик ссылок здесь
/// формальность: DirectWrite не удерживает источник дольше вызова. Так же
/// сделано в образцах Microsoft (CustomLayout).
class AnalysisSource final : public IDWriteTextAnalysisSource {
public:
    AnalysisSource(const wchar_t* text, uint32_t length, const wchar_t* locale)
        : text_(text), length_(length), locale_(locale) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) noexcept override {
        if (riid == __uuidof(IDWriteTextAnalysisSource) || riid == __uuidof(IUnknown)) {
            *object = this;
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return 1; }
    ULONG STDMETHODCALLTYPE Release() noexcept override { return 1; }

    HRESULT STDMETHODCALLTYPE GetTextAtPosition(UINT32 position, const WCHAR** text,
                                                UINT32* length) noexcept override {
        if (position >= length_) {
            *text = nullptr;
            *length = 0;
        } else {
            *text = text_ + position;
            *length = length_ - position;
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetTextBeforePosition(UINT32 position, const WCHAR** text,
                                                    UINT32* length) noexcept override {
        if (position == 0 || position > length_) {
            *text = nullptr;
            *length = 0;
        } else {
            *text = text_;
            *length = position;
        }
        return S_OK;
    }

    DWRITE_READING_DIRECTION STDMETHODCALLTYPE GetParagraphReadingDirection() noexcept override {
        return DWRITE_READING_DIRECTION_LEFT_TO_RIGHT;
    }

    HRESULT STDMETHODCALLTYPE GetLocaleName(UINT32 position, UINT32* length,
                                            const WCHAR** locale) noexcept override {
        *length = length_ - std::min(position, length_);
        *locale = locale_;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE
    GetNumberSubstitution(UINT32 position, UINT32* length,
                          IDWriteNumberSubstitution** substitution) noexcept override {
        *length = length_ - std::min(position, length_);
        *substitution = nullptr;
        return S_OK;
    }

private:
    const wchar_t* text_;
    uint32_t length_;
    const wchar_t* locale_;
};

/// Приёмник: складывает всё, что сказал анализ, в таблицы по символам.
/// По символу, а не списком интервалов, потому что дальше их читают вразнобой —
/// и при сборке прогонов формата, и при нарезке строк.
class AnalysisSink final : public IDWriteTextAnalysisSink {
public:
    explicit AnalysisSink(uint32_t length)
        : scripts_(length, DWRITE_SCRIPT_ANALYSIS{}), levels_(length, 0), breaks_(length) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) noexcept override {
        if (riid == __uuidof(IDWriteTextAnalysisSink) || riid == __uuidof(IUnknown)) {
            *object = this;
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return 1; }
    ULONG STDMETHODCALLTYPE Release() noexcept override { return 1; }

    HRESULT STDMETHODCALLTYPE
    SetScriptAnalysis(UINT32 position, UINT32 length,
                      const DWRITE_SCRIPT_ANALYSIS* analysis) noexcept override {
        for (UINT32 i = position; i < position + length && i < scripts_.size(); ++i)
            scripts_[i] = *analysis;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE
    SetLineBreakpoints(UINT32 position, UINT32 length,
                       const DWRITE_LINE_BREAKPOINT* points) noexcept override {
        for (UINT32 i = 0; i < length && position + i < breaks_.size(); ++i)
            breaks_[position + i] = points[i];
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetBidiLevel(UINT32 position, UINT32 length, UINT8 /*explicitLevel*/,
                                           UINT8 resolvedLevel) noexcept override {
        for (UINT32 i = position; i < position + length && i < levels_.size(); ++i)
            levels_[i] = resolvedLevel;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetNumberSubstitution(UINT32, UINT32,
                                                    IDWriteNumberSubstitution*) noexcept override {
        return S_OK;
    }

    const sta_vector<DWRITE_SCRIPT_ANALYSIS>& scripts() const { return scripts_; }
    const sta_vector<uint8_t>& levels() const { return levels_; }
    const sta_vector<DWRITE_LINE_BREAKPOINT>& breakpoints() const { return breaks_; }

private:
    sta_vector<DWRITE_SCRIPT_ANALYSIS> scripts_;
    sta_vector<uint8_t> levels_;
    sta_vector<DWRITE_LINE_BREAKPOINT> breaks_;
};

/* ================================================================== */
/* Прогоны                                                            */

/// Прогон формата: кусок текста, у которого одинаковы стиль, письменность,
/// направление и шрифт. Это единица шейпинга.
struct FormatRun {
    uint32_t start = 0;
    uint32_t length = 0;
    FontStyle style;
    DWRITE_SCRIPT_ANALYSIS script{};
    uint8_t bidiLevel = 0;
    IDWriteFontFace* fontFace = nullptr;
    float fontSize = 0.0f;
    float ascenderOffset = 0.0f;   ///< для над- и подстрочных
};

/// Прогон после шейпинга. Кластерная карта остаётся с ним: без неё нельзя ни
/// нарезать прогон по границе строки, ни выключить строку по формату.
struct ShapedRun {
    FormatRun format;
    sta_vector<uint16_t> glyphIndices;
    sta_vector<uint16_t> clusterMap;   ///< на символ: индекс первого глифа кластера
    sta_vector<float> advances;
    sta_vector<DWRITE_GLYPH_OFFSET> offsets;
    sta_vector<DWRITE_JUSTIFICATION_OPPORTUNITY> opportunities;
    float ascent = 0.0f;
    float descent = 0.0f;

    /// Дефис переноса — глиф того же шрифта, что и прогон, и его ширина при
    /// опорном кегле. Меряется один раз здесь: строка, кончившаяся переносом,
    /// дописывает этот глиф к себе, а разбивка знает наперёд, во что ей
    /// обойдётся перенос в этом месте.
    uint16_t hyphenGlyph = 0;
    float hyphenAdvance = 0.0f;
};

/// Прогон, попавший в строку. Возможности выключки по формату едут вместе с
/// глифами: они посчитаны по исходному тексту, которого у нарезанной строки
/// уже нет.
struct LineRun {
    GlyphRun glyphs;
    sta_vector<DWRITE_JUSTIFICATION_OPPORTUNITY> opportunities;

    /// Дефис шрифта этого прогона, в кегле строки: его дописывает к себе
    /// строка, кончившаяся переносом.
    uint16_t hyphenGlyph = 0;
    float hyphenAdvance = 0.0f;

    /// Вертикальный сдвиг над- или подстрочного прогона, в кегле строки: с
    /// ним дефис встаёт вровень со своими буквами, а не на базовую линию.
    /// Берётся из формата прогона, а не у соседнего глифа: в смещении глифа
    /// сидит ещё и посадка знака шрифтом (GPOS), а дефису она чужая.
    float hyphenAscenderOffset = 0.0f;
};

/// Прогон без видимых глифов: управляющие и форматирующие символы —
/// табуляция, переводы строки, мягкий перенос, пробел нулевой ширины, метки
/// направления. Так их помечает анализ письменности DirectWrite
/// (DWRITE_SCRIPT_SHAPES_NO_VISUAL), и это договор с вызывающим: глифы,
/// которые шейпер им всё же даёт (заглушку .notdef со своей шириной),
/// показывать нельзя — IDWriteTextLayout их и не показывает. Здесь — всё
/// обоснование; дальше по коду на него только ссылаются: шрифт такому прогону
/// — базовый (formatRuns), ширины и возможности выключки — свои (shape),
/// глифы не рисуются (GlyphRun::invisible).
bool isInvisible(const DWRITE_SCRIPT_ANALYSIS& script) {
    return (static_cast<UINT32>(script.shapes) & static_cast<UINT32>(DWRITE_SCRIPT_SHAPES_NO_VISUAL)) != 0;
}

/// Колонка исходной строки — мерка позиций табуляции. Строка исходного текста
/// начинается с абзаца и за каждым обязательным разрывом (перевод строки и
/// прочие — как их видит анализ переломов); колонка — число букв от её
/// начала, а табуляция доводит её до следующей кратной kTabColumns. Буква, а
/// не единица текста: «й» из двух кодовых точек и знак вне BMP — одна
/// колонка. Буква без вида (isInvisible: мягкий перенос, пробел нулевой
/// ширины, метка направления) колонки не занимает: на строке её нет, и
/// табуляция за ней встала бы мимо позиции. Ограничение: широкий знак
/// (иероглиф, UAX #11 East Asian Width W/F) — тоже одна колонка, а не две, как
/// в терминале. Колонка зависит только от текста, а не от того, где разбивка
/// разорвёт строку, и потому ширина табуляции считается при шейпинге.
///
/// Курсор идёт только вперёд: прогоны шейпятся по порядку текста, и абзац
/// проходится один раз, сколько бы табуляций в нём ни было.
struct TabColumns {
    u16_view text;
    std::span<const DWRITE_LINE_BREAKPOINT> breakpoints;
    std::span<const DWRITE_SCRIPT_ANALYSIS> scripts;
    uint32_t at = 0;       ///< до какого места текста колонки сосчитаны
    uint32_t column = 0;   ///< колонок до at, если строка на at не начинается заново

    static uint32_t nextStop(uint32_t from) { return (from / kTabColumns + 1) * kTabColumns; }

    /// Колонка буквы, которая начинается на position.
    uint32_t columnAt(uint32_t position) {
        assert(position >= at && "табуляции спрашиваются по порядку текста");
        while (at < position) {
            const uint32_t current = lineColumn();
            if (text.plain()[at] == u'\t')
                column = nextStop(current);
            else
                column = isInvisible(scripts[at]) ? current : current + 1;
            at = static_cast<uint32_t>(unicode::next_grapheme_boundary(text, at));
        }
        // Табуляция — управляющий символ, и буква на ней кончается и с неё
        // начинается (UAX #29, GB4 и GB5): шаг по буквам приходит на неё
        // ровно. Иначе — сломаны границы букв, и считать дальше не от чего.
        ensure(at == position);
        return lineColumn();
    }

private:
    uint32_t lineColumn() const {
        return at > 0 && breakpoints[at].breakConditionBefore == DWRITE_BREAK_CONDITION_MUST_BREAK
                   ? 0
                   : column;
    }
};

/// Пробел шрифта на кегль 1 — мерка табуляции (Engine::Impl::spaceOf).
struct SpaceMeasure {
    float advance = 0.0f;
    DWRITE_JUSTIFICATION_OPPORTUNITY opportunity{};
};

/// Ключ кэша шрифтов: семейство плюс начертание.
struct FaceKey {
    std::wstring family;
    DWRITE_FONT_WEIGHT weight;
    DWRITE_FONT_STYLE style;

    bool operator<(const FaceKey& other) const {
        return std::tie(family, weight, style) < std::tie(other.family, other.weight, other.style);
    }
};

}  // namespace

/* ================================================================== */

/// Всё, что вёрстка узнаёт об абзаце один раз и переиспользует.
struct ShapedParagraph::Data {
    /// Хранилище книги и абзац в нём, а не разрешённые виды: из хранилища же
    /// берутся позиция каждой строки (charOffsetAt) и якоря сносок, и тем же
    /// вызовом shape абзац перешейпливается, когда стиль разошёлся. Абзац —
    /// копией: это четыре диапазона в массивы книги, ничем не владеющие, и
    /// место, где его держал зовущий, переживать ничего не обязано.
    /// Хранилище — обязано, и на том же месте: его держит книга, пока жива
    /// её вёрстка (см. заголовок).
    const BookText* book = nullptr;
    Paragraph paragraph;

    sta_vector<ShapedRun> runs;
    sta_vector<DWRITE_LINE_BREAKPOINT> breakpoints;

    /// Места переноса (hyphenPointsOf): байт на символ. Ноль — разрыва здесь
    /// нет; иначе это число букв части слова, которые перенос унесёт на
    /// следующую строку, — по нему разбивка дороже берёт перенос с коротким
    /// хвостом, и считаются именно буквы: «фер-му»,» уносит две, а не четыре
    /// знака до пробела, и знак ударения буквой не считается. Считается один
    /// раз здесь, а не при каждой вёрстке: перенос — свойство слова, а не
    /// полосы, и переживает и смену окна, и смену кегля.
    sta_vector<uint8_t> hyphens;

    /// Кегль, на котором посчитаны метрики прогонов.
    float referenceFontSize = 0.0f;

    /// То в стиле, что меняет сам шейпинг, а не его масштаб. Если при вёрстке
    /// это разойдётся — глифы уже не те, и абзац надо шейпить заново.
    bool bold = false;
    bool italic = false;
    bool monospace = false;
    uint64_t styleGeneration = 0;
};

ShapedParagraph::ShapedParagraph() : data_(std::make_unique<Data>()) {}
ShapedParagraph::~ShapedParagraph() = default;

float ShapedParagraph::referenceFontSize() const { return data_->referenceFontSize; }

/* ================================================================== */

struct Engine::Impl {
    ComPtr<IDWriteFactory> factory;
    ComPtr<IDWriteTextAnalyzer> analyzer;
    ComPtr<IDWriteTextAnalyzer1> analyzer1;      ///< выключка по формату; может не быть
    ComPtr<IDWriteFontCollection> systemFonts;
    ComPtr<IDWriteFontFallback> fallback;        ///< подбор шрифта; может не быть

    TextStyle style;

    /// Растёт на каждую смену шрифта книги. Отшейпленный абзац помнит своё
    /// поколение, и по несовпадению видно, что глифы в нём уже не те, —
    /// иначе смена шрифта тихо не подействовала бы на всё уже отшейпленное.
    uint64_t styleGeneration = 0;

    /// Запомненная средняя ширина знака (averageCharWidth) и то, при каком
    /// кегле и каком поколении стиля она измерена: смена шрифта книги видна
    /// по поколению, как и у отшейпленных абзацев.
    float charWidth = 0.0f;
    float charWidthFontSize = 0.0f;
    uint64_t charWidthGeneration = 0;

    std::map<FaceKey, ComPtr<IDWriteFontFace>> faces;
    std::unordered_map<IDWriteFontFace*, DWRITE_FONT_METRICS> metrics;
    /// Шрифты, найденные фолбэком. Ключ — сам указатель: DirectWrite отдаёт
    /// один и тот же объект на одно и то же начертание, поэтому карта не растёт
    /// от того, что фолбэк спрашивают на каждом абзаце.
    std::unordered_map<IDWriteFontFace*, ComPtr<IDWriteFontFace>> mapped;

    /// Пробелы шрифтов — мерка табуляции (spaceOf). Сбрасывается со сменой
    /// стиля: возможность выключки спрашивается на языке книги.
    std::unordered_map<IDWriteFontFace*, SpaceMeasure> spaces;

    /* ---------------- шрифты ---------------- */

    /// Имя семейства -- нуль-терминированный указатель, а не вид: и
    /// FindFamilyName, и MapCharacters берут указатель без длины, а копию на
    /// каждый прогон формата платить не за что.
    IDWriteFontFace* baseFace(const wchar_t* family, bool bold, bool italic) {
        const FaceKey key{family, bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                          italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL};

        if (const auto found = faces.find(key); found != faces.end())
            return found->second.Get();

        UINT32 index = 0;
        BOOL exists = FALSE;
        check(systemFonts->FindFamilyName(family, &index, &exists));

        // Шрифта нет — берём то, что есть у любой системы: молча падать из-за
        // настройки читателя нельзя.
        if (!exists)
            check(systemFonts->FindFamilyName(L"Segoe UI", &index, &exists));
        if (!exists)
            check(systemFonts->FindFamilyName(L"Arial", &index, &exists));

        ComPtr<IDWriteFontFamily> fontFamily;
        check(systemFonts->GetFontFamily(index, fontFamily.GetAddressOf()));

        ComPtr<IDWriteFont> font;
        check(
            fontFamily->GetFirstMatchingFont(key.weight, DWRITE_FONT_STRETCH_NORMAL, key.style, font.GetAddressOf()));

        ComPtr<IDWriteFontFace> face;
        check(font->CreateFontFace(face.GetAddressOf()));

        IDWriteFontFace* const raw = face.Get();
        faces.emplace(key, std::move(face));
        return raw;
    }

    const DWRITE_FONT_METRICS& metricsOf(IDWriteFontFace* face) {
        if (const auto found = metrics.find(face); found != metrics.end())
            return found->second;

        DWRITE_FONT_METRICS value{};
        face->GetMetrics(&value);
        return metrics.emplace(face, value).first->second;
    }

    const std::wstring& familyFor(const FontStyle& fontStyle) const {
        return fontStyle.monospace ? style.monospaceFamily : style.fontFamily;
    }

    float lineHeightOf(const ParagraphStyle& blockStyle) const {
        return blockStyle.fontSize * blockStyle.lineHeight;
    }

    /* ---------------- прогоны формата ---------------- */

    /// Режет прогон стиля на куски, каждому из которых нашёлся свой шрифт.
    /// Без фолбэка кусок один: базовый шрифт на всё.
    void appendFontRuns(sta_vector<FormatRun>& runs, const FormatRun& prototype, const wchar_t* text,
                        uint32_t textLength, const wchar_t* family, bool bold, bool italic) {
        const uint32_t end = prototype.start + prototype.length;

        const auto withBaseFont = [&](uint32_t from, uint32_t to) {
            FormatRun run = prototype;
            run.start = from;
            run.length = to - from;
            run.fontFace = baseFace(family, bold, italic);
            runs.push_back(run);
        };

        if (!fallback) {
            withBaseFont(prototype.start, end);
            return;
        }

        AnalysisSource source(text, textLength, style.locale.c_str());
        uint32_t at = prototype.start;

        while (at < end) {
            UINT32 mappedLength = 0;
            ComPtr<IDWriteFont> font;
            FLOAT scale = 1.0f;

            const HRESULT hr = fallback->MapCharacters(
                &source, at, end - at, systemFonts.Get(), const_cast<wchar_t*>(family),
                bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, &mappedLength, font.GetAddressOf(), &scale);

            if (FAILED(hr) || mappedLength == 0) {
                withBaseFont(at, end);
                return;
            }

            FormatRun run = prototype;
            run.start = at;
            // Ограничение обязательно: источник анализа отдаёт фолбэку текст до
            // конца абзаца, а не до конца прогона, и найденный кусок может
            // оказаться длиннее, чем мы спрашивали. Без этого прогоны налезли бы
            // друг на друга и глифы попали бы в строку дважды.
            run.length = std::min<uint32_t>(mappedLength, end - at);

            // Масштаб фолбэка выравнивает высоту строчных запасного шрифта по
            // основному. Применять его можно только когда шрифт действительно
            // найден: иначе MapCharacters оставляет в scale ноль, а кегль ноль
            // — это E_INVALIDARG на шейпинге.
            if (font && scale > 0.0f)
                run.fontSize = prototype.fontSize * scale;

            if (font) {
                ComPtr<IDWriteFontFace> face;
                if (SUCCEEDED(font->CreateFontFace(face.GetAddressOf()))) {
                    run.fontFace = face.Get();
                    mapped.emplace(face.Get(), face);
                }
            }
            if (!run.fontFace)
                run.fontFace = baseFace(family, bold, italic);

            runs.push_back(run);
            at += run.length;
        }
    }

    /// Пересечение всех разбиений сразу: стиль, письменность, направление, шрифт.
    ///
    /// Прогоны стиля — разбиение текста по построению: абзац без разметки
    /// несёт один прогон на весь текст, а не ни одного, и подставлять здесь
    /// нечего. Абзац с текстом без прогонов или прогон за концом текста —
    /// нарушенный инвариант хранилища, а не случай вёрстки.
    sta_vector<FormatRun> formatRuns(u16_view text, std::span<const StyleSpan> spans,
                                     const ParagraphStyle& blockStyle,
                                     const AnalysisSink& analysis) {
        const auto textLength = static_cast<uint32_t>(text.size());
        sta_vector<FormatRun> runs;

        // Границы прогонов — счёт flatten по набранному тексту, не числа из
        // файла. Пустой список, дыра, перекрытие памяти не трогают — текст
        // вышел бы ненабранным или набранным дважды, — их ловит Debug. Прогон
        // за концом текста читал бы за концом и текст, и массивы анализа: эта
        // граница — во всех сборках, раз на прогон стиля, что рядом с анализом
        // абзаца в DirectWrite ничто.
        [[maybe_unused]] uint32_t covered = 0;   // читают только проверки Debug
        for (const StyleSpan& span : spans) {
            assert(span.start == covered && "прогоны стиля идут подряд, без дыр и перекрытий");
            ensure(span.start <= textLength && span.length <= textLength - span.start);

            uint32_t at = span.start;
            const uint32_t spanEnd = span.start + span.length;
            covered = spanEnd;

            while (at < spanEnd) {
                uint32_t end = at + 1;
                while (end < spanEnd && analysis.levels()[end] == analysis.levels()[at] &&
                       analysis.scripts()[end].script == analysis.scripts()[at].script &&
                       analysis.scripts()[end].shapes == analysis.scripts()[at].shapes)
                    ++end;

                // Кластерная карта DirectWrite шестнадцатибитная, поэтому
                // прогон длиннее её ёмкости шейпер отвергает. Делим по пробелу,
                // если он рядом: шейпинг через границу прогона теряет лигатуры,
                // и лучше потерять их на пробеле, чем в середине слова.
                //
                // Пробела рядом может и не быть — китайскому тексту они не
                // нужны, — а за пробелом может стоять знак, для которого он
                // основа. Поэтому разрез в любом случае отодвигается к началу
                // буквы: знак, отданный шейперу без основы, повисает, а
                // конъюнкт распадается. Прогон начинается с буквы, так что
                // границы считаются от него, а не от начала абзаца.
                //
                // Буква длиннее самого предела — основа с тысячами знаков —
                // остаётся целой и выходит за предел: разорвать её нельзя, а
                // шейпер отвергает только прогон, чьих глифов не вмещает карта.
                if (end - at > kMaxRunLength) {
                    uint32_t cut = at + kMaxRunLength;
                    const uint32_t limit = cut - std::min<uint32_t>(kMaxRunLength / 8, cut - at - 1);
                    while (cut > limit && text.plain()[cut - 1] != u' ')
                        --cut;

                    const u16_view rest = text.substr(at);
                    size_t letters = unicode::floor_grapheme_boundary(rest, cut - at);
                    if (letters == 0)
                        letters = unicode::next_grapheme_boundary(rest, 0);

                    end = at + static_cast<uint32_t>(letters);
                }

                // Начертание блока и начертание разметки складываются: курсив
                // внутри курсивного эпиграфа остаётся курсивом, а не спорит.
                FontStyle merged = span.style;
                merged.bold = merged.bold || blockStyle.bold;
                merged.italic = merged.italic || blockStyle.italic;
                merged.monospace = merged.monospace || blockStyle.monospace;

                FormatRun prototype;
                prototype.start = at;
                prototype.length = end - at;
                prototype.style = merged;
                prototype.script = analysis.scripts()[at];
                prototype.bidiLevel = analysis.levels()[at];
                prototype.fontSize = blockStyle.fontSize;

                if (merged.script > 0) {
                    prototype.fontSize *= kScriptScale;
                    prototype.ascenderOffset = blockStyle.fontSize * kSuperscriptRise;
                } else if (merged.script < 0) {
                    prototype.fontSize *= kScriptScale;
                    prototype.ascenderOffset = -blockStyle.fontSize * kSubscriptDrop;
                }

                // Прогону без видимых глифов (isInvisible) фолбэк не нужен:
                // шрифт, подобранный управляющему символу, принёс бы в строку
                // свою высоту — строка с табуляцией в преформате вышла бы выше
                // той же строки без неё.
                if (isInvisible(prototype.script)) {
                    prototype.fontFace = baseFace(familyFor(merged).c_str(), merged.bold, merged.italic);
                    runs.push_back(prototype);
                } else {
                    appendFontRuns(runs, prototype, text.wchars().data(), textLength,
                                   familyFor(merged).c_str(), merged.bold, merged.italic);
                }
                at = end;
            }
        }

        assert(covered == textLength && "прогоны стиля покрывают текст абзаца до конца");
        return runs;
    }

    /* ---------------- шейпинг ---------------- */

    /// Пробел шрифта — мерка табуляции: его ширина и возможность выключки по
    /// формату на кегль 1. Метрики шейпинга линейны по кеглю, и табуляция
    /// любого кегля берёт отсюда умножением. Меряется раз на шрифт: табуляций
    /// в преформате тысячи, а шрифтов — единицы.
    ///
    /// Шрифт без пробела — сломанный шрифт: мерить табуляцию нечем, и
    /// подставить что-нибудь значило бы набрать её наугад. Сейчас шрифт книги
    /// не выбирает читатель (а отсутствующий заменяется системным), так что это
    /// остановка; когда шрифт станет настройкой, здесь нужен отказ вёрстки с
    /// ответом читателю, а не падение.
    const SpaceMeasure& spaceOf(IDWriteFontFace* face, float fontSize) {
        if (const auto found = spaces.find(face); found != spaces.end())
            return found->second;

        const UINT32 code = U' ';
        UINT16 glyph = 0;
        check(face->GetGlyphIndices(&code, 1, &glyph));
        if (glyph == 0)
            wxl::core::abort("Engine::shape: у шрифта нет пробела — табуляцию нечем мерить");

        DWRITE_GLYPH_METRICS design{};
        check(face->GetDesignGlyphMetrics(&glyph, 1, &design));
        SpaceMeasure measure;
        measure.advance =
            static_cast<float>(design.advanceWidth) / static_cast<float>(metricsOf(face).designUnitsPerEm);

        // Возможность выключки пробела — та, что DirectWrite даёт ему в тексте:
        // анализ письменности самого пробела, его глиф от шейпера и вопрос о
        // возможностях. Не вышло спросить — нулей, как у прогона текста.
        if (analyzer1) {
            constexpr wchar_t kSpace[] = L" ";
            AnalysisSource source(kSpace, 1, style.locale.c_str());
            AnalysisSink analysis(1);
            check(analyzer->AnalyzeScript(&source, 0, 1, &analysis));

            UINT16 clusterMap = 0;
            DWRITE_SHAPING_TEXT_PROPERTIES textProperties{};
            UINT16 glyphs[4]{};
            DWRITE_SHAPING_GLYPH_PROPERTIES glyphProperties[4]{};
            UINT32 glyphCount = 0;
            check(analyzer->GetGlyphs(kSpace, 1, face, FALSE, FALSE, &analysis.scripts()[0],
                                      style.locale.c_str(), nullptr, nullptr, nullptr, 0, 4, &clusterMap,
                                      &textProperties, glyphs, glyphProperties, &glyphCount));
            // Пробел — один глиф: иначе мерить по нему табуляцию нечем.
            ensure(glyphCount == 1);

            DWRITE_JUSTIFICATION_OPPORTUNITY opportunity{};
            if (SUCCEEDED(analyzer1->GetJustificationOpportunities(face, fontSize, analysis.scripts()[0], 1, 1,
                                                                   kSpace, &clusterMap, glyphProperties,
                                                                   &opportunity))) {
                opportunity.expansionMinimum /= fontSize;
                opportunity.expansionMaximum /= fontSize;
                opportunity.compressionMaximum /= fontSize;
                measure.opportunity = opportunity;
            }
        }

        return spaces.emplace(face, measure).first->second;
    }

    ShapedRun shape(u16_view paragraph, const FormatRun& format, TabColumns& columns) {
        ShapedRun shaped;
        shaped.format = format;

        const wchar_t* const text = paragraph.wchars().data() + format.start;
        const UINT32 length = format.length;
        const BOOL rightToLeft = (format.bidiLevel & 1) != 0;

        // Маленькие прописные — единственная возможность OpenType, которую
        // формат просит явно (<smallcaps>); остальное шейпер решает сам.
        DWRITE_FONT_FEATURE smallCaps{DWRITE_FONT_FEATURE_TAG_SMALL_CAPITALS, 1};
        DWRITE_TYPOGRAPHIC_FEATURES features{&smallCaps, 1};
        const DWRITE_TYPOGRAPHIC_FEATURES* featureList = &features;
        const UINT32 featureRangeLength = length;

        const bool wantsFeatures = format.style.smallCaps;
        const DWRITE_TYPOGRAPHIC_FEATURES** featuresArgument = wantsFeatures ? &featureList : nullptr;
        const UINT32* rangesArgument = wantsFeatures ? &featureRangeLength : nullptr;
        const UINT32 rangeCount = wantsFeatures ? 1 : 0;

        shaped.clusterMap.resize(length);
        sta_vector<DWRITE_SHAPING_TEXT_PROPERTIES> textProperties(length);
        sta_vector<DWRITE_SHAPING_GLYPH_PROPERTIES> glyphProperties;

        UINT32 maxGlyphs = 3 * length / 2 + 16;
        UINT32 actualGlyphs = 0;

        for (;;) {
            shaped.glyphIndices.resize(maxGlyphs);
            glyphProperties.resize(maxGlyphs);

            const HRESULT hr = analyzer->GetGlyphs(
                text, length, format.fontFace, FALSE, rightToLeft, &format.script,
                style.locale.c_str(), nullptr, featuresArgument, rangesArgument, rangeCount, maxGlyphs,
                shaped.clusterMap.data(), textProperties.data(), shaped.glyphIndices.data(),
                glyphProperties.data(), &actualGlyphs);

            if (hr == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER)) {
                maxGlyphs *= 2;
                continue;
            }
            check(hr);
            break;
        }

        shaped.glyphIndices.resize(actualGlyphs);
        glyphProperties.resize(actualGlyphs);
        shaped.advances.resize(actualGlyphs);
        shaped.offsets.resize(actualGlyphs);

        check(analyzer->GetGlyphPlacements(
            text, shaped.clusterMap.data(), textProperties.data(), length, shaped.glyphIndices.data(),
            glyphProperties.data(), actualGlyphs, format.fontFace, format.fontSize, FALSE, rightToLeft,
            &format.script, style.locale.c_str(), featuresArgument, rangesArgument, rangeCount,
            shaped.advances.data(), shaped.offsets.data()));

        const DWRITE_FONT_METRICS& fontMetrics = metricsOf(format.fontFace);
        const float scale = format.fontSize / static_cast<float>(fontMetrics.designUnitsPerEm);

        const bool invisible = isInvisible(format.script);
        if (invisible) {
            // Прогон без вида (isInvisible): глифы и кластерная карта остаются
            // как есть — по ним строка режется и связывается с текстом, — а
            // ширины и возможности выключки ставятся свои. У перевода строки,
            // мягкого переноса и прочих ширины нет: иначе строка до `<br/>`
            // несла бы невидимый хвост, центровка съезжала бы на его половину,
            // а при выключке вправо у края оставалась бы дыра. Табуляция
            // доводит перо до следующей позиции по колонкам исходной строки
            // (TabColumns); ширина её зависит только от текста, поэтому и
            // считается здесь, раз на шейпинг, — разбивка и вывод мерят её
            // одинаково.
            //
            // Выключка табуляцию не тянет: табуляция ставит текст за собой на
            // позицию, а позиция от полосы не зависит. А сжимает — как пробел
            // того же шрифта, с тем же приоритетом, на столько пробелов, во
            // сколько встала: отступ из табуляций, которому сжиматься нельзя,
            // отдал бы остаток переполнения буквам короткого слова за ним, и
            // они вышли бы заметно уже своей ширины. Разрядка (ниже) таких
            // глифов не касается: просвет дал бы переводу строки ту самую
            // невидимую ширину.
            std::fill(shaped.advances.begin(), shaped.advances.end(), 0.0f);
            shaped.opportunities.assign(actualGlyphs, DWRITE_JUSTIFICATION_OPPORTUNITY{});

            for (UINT32 i = 0; i < length; ++i) {
                if (text[i] != L'\t')
                    continue;

                const SpaceMeasure& space = spaceOf(format.fontFace, format.fontSize);
                const uint32_t column = columns.columnAt(format.start + i);
                const auto stops = static_cast<float>(TabColumns::nextStop(column) - column);

                // Ширину кластера несёт его первый глиф — на него и указывает
                // карта у каждого символа кластера.
                ensure(shaped.clusterMap[i] < actualGlyphs);
                const uint16_t glyph = shaped.clusterMap[i];
                shaped.advances[glyph] += stops * space.advance * format.fontSize;

                DWRITE_JUSTIFICATION_OPPORTUNITY& opportunity = shaped.opportunities[glyph];
                opportunity.compressionMaximum +=
                    stops * space.opportunity.compressionMaximum * format.fontSize;
                opportunity.compressionPriority = space.opportunity.compressionPriority;
                opportunity.allowResidualCompression = space.opportunity.allowResidualCompression;
                opportunity.applyToLeadingEdge = space.opportunity.applyToLeadingEdge;
                opportunity.applyToTrailingEdge = space.opportunity.applyToTrailingEdge;
            }
        } else {
            // Возможности выключки по формату спрашиваются здесь, пока на руках
            // исходный текст и свойства глифов: у нарезанной строки их уже не
            // будет.
            shaped.opportunities.resize(actualGlyphs);
            if (analyzer1 && actualGlyphs > 0) {
                if (FAILED(analyzer1->GetJustificationOpportunities(
                        format.fontFace, format.fontSize, format.script, length, actualGlyphs, text,
                        shaped.clusterMap.data(), glyphProperties.data(), shaped.opportunities.data())))
                    shaped.opportunities.assign(actualGlyphs, DWRITE_JUSTIFICATION_OPPORTUNITY{});
            }
        }

        // Разрядка — не возможность шрифта, а типографский приём: раздвигаем
        // сами. Раздвигаются буквы, а не глифы: просвет прибавляется к
        // последнему глифу буквы, и знак, посаженный на неё, остаётся на
        // месте. ApplyCharacterSpacing из DirectWrite тоже не трогает знаки,
        // но раздвигает по своим кластерам, а кластер уже буквы — чамо
        // хангыля без готового слога шрифт держит отдельными кластерами.
        // Буквы, слитые шрифтом в одну лигатуру, раздвинуть нечем: у
        // границы между ними нет своего глифа.
        if (format.style.spaced && !invisible) {
            const float spacing = format.fontSize * kSpacedTracking;
            const u16_view letters = paragraph.substr(format.start, length);

            for (size_t letter = 0; letter < length;) {
                const size_t next = unicode::next_grapheme_boundary(letters, letter);
                const uint32_t glyphEnd = next < length ? shaped.clusterMap[next] : actualGlyphs;

                if (glyphEnd > shaped.clusterMap[next - 1])
                    shaped.advances[glyphEnd - 1] += spacing;

                letter = next;
            }
        }

        // Мягкий перенос, стоящий в самой книге, не виден, пока строка на нём
        // не кончилась; разрыв в этом месте разбивка получит штрафом, как и
        // всякий другой перенос, а дефис на краю полосы допишет строка. Анализ
        // помечает его прогоном без вида, и тогда ширина уже снята выше. Если
        // не пометил — шрифт даёт ему обычный дефис со своей шириной, и
        // «сло-во» раздвинулось бы посреди строки: ширина снимается здесь. Так
        // правило «мягкий перенос ширины не имеет» живёт в одном месте при
        // любом ответе анализа. Только если знак — кластер сам по себе: иначе
        // ноль достался бы букве, с которой шейпер его склеил.
        if (!invisible) {
            for (UINT32 i = 0; i < length; ++i) {
                if (text[i] != L'\x00AD')
                    continue;

                const uint16_t glyph = shaped.clusterMap[i];
                if (i > 0 && shaped.clusterMap[i - 1] == glyph)
                    continue;

                const auto end = i + 1 < length ? shaped.clusterMap[i + 1] : static_cast<uint16_t>(actualGlyphs);
                for (uint16_t at = glyph; at < end; ++at)
                    shaped.advances[at] = 0.0f;
            }
        }

        // И вертикальный сдвиг над- и подстрочных.

        if (format.ascenderOffset != 0.0f)
            for (DWRITE_GLYPH_OFFSET& offset : shaped.offsets)
                offset.ascenderOffset += format.ascenderOffset;

        shaped.ascent = fontMetrics.ascent * scale;
        shaped.descent = fontMetrics.descent * scale;

        // Дефис переноса — глиф этого же шрифта: строка, кончившаяся переносом,
        // дописывает его к себе. U+2010 — дефис типографский, и берётся он
        // первым; в шрифте его может не быть, и тогда идёт минус-дефис, который
        // есть везде.
        for (const UINT32 code : {0x2010u, 0x002Du}) {
            UINT16 glyph = 0;
            if (FAILED(format.fontFace->GetGlyphIndices(&code, 1, &glyph)) || glyph == 0)
                continue;

            DWRITE_GLYPH_METRICS hyphen{};
            if (SUCCEEDED(format.fontFace->GetDesignGlyphMetrics(&glyph, 1, &hyphen))) {
                shaped.hyphenGlyph = glyph;
                shaped.hyphenAdvance = static_cast<float>(hyphen.advanceWidth) * scale;
            }
            break;
        }

        return shaped;
    }

    /// Анализ и шейпинг абзаца целиком — шаги 1–3 вёрстки. Дорогая часть:
    /// именно здесь происходят все обращения к DirectWrite, кроме выключки по
    /// формату.
    void shapeInto(ShapedParagraph::Data& into, const BookText& book, const Paragraph& paragraph,
                   const ParagraphStyle& blockStyle) {
        into.book = &book;
        into.paragraph = paragraph;
        into.referenceFontSize = blockStyle.fontSize;
        into.bold = blockStyle.bold;
        into.italic = blockStyle.italic;
        into.monospace = blockStyle.monospace;
        into.styleGeneration = styleGeneration;

        const u16_view text = book.textOf(paragraph);
        const auto textLength = static_cast<uint32_t>(text.size());
        if (textLength == 0)
            return;

        AnalysisSource source(text.wchars().data(), textLength, style.locale.c_str());
        AnalysisSink analysis(textLength);

        check(analyzer->AnalyzeScript(&source, 0, textLength, &analysis));
        check(analyzer->AnalyzeBidi(&source, 0, textLength, &analysis));
        check(analyzer->AnalyzeLineBreakpoints(&source, 0, textLength, &analysis));

        into.breakpoints = analysis.breakpoints();
        into.hyphens = hyphenPointsOf(text, book.notesOf(paragraph));

        const sta_vector<FormatRun> formats =
            formatRuns(text, book.spansOf(paragraph), blockStyle, analysis);
        into.runs.reserve(formats.size());

        TabColumns columns{text,
                           {into.breakpoints.data(), into.breakpoints.size()},
                           {analysis.scripts().data(), analysis.scripts().size()}};
        for (const FormatRun& format : formats) {
            // Пустой прогон и прогон без шрифта — не ошибка вёрстки, а следствие
            // того, что разбиений четыре и они не обязаны совпадать. Шейперу такое
            // отдавать нельзя: он отвечает E_INVALIDARG.
            if (format.length == 0 || format.fontFace == nullptr || format.fontSize <= 0.0f)
                continue;
            into.runs.push_back(shape(text, format, columns));
        }
    }

    /* ---------------- ширины по символам ---------------- */

    /// Ширина каждого символа абзаца и то, насколько он готов сжаться.
    ///
    /// Ширину кластера несёт его первый символ, остальные получают ноль:
    /// разрывать кластер нельзя, и алгоритму строк достаточно знать, что внутри
    /// него ничего не стоит.
    ///
    /// Сжимаемость берётся не из головы, а из ответа DirectWrite: сколько он
    /// разрешил снять с этого глифа, столько Кнут-Пласс и вправе рассчитывать.
    /// Иначе алгоритм выбирает строку, надеясь сжать пробелы сильнее, чем
    /// выключка по формату потом сумеет, — и строка вылезает за поле.
    struct CharacterMetrics {
        sta_vector<float> width;
        sta_vector<float> shrink;

        /// Ширина, которую выключка вправе тянуть: у символа прогона без
        /// видимых глифов (isInvisible) — ноль. Табуляции выключка растяжения
        /// не даёт (shape), и разбивка, посчитавшая её клей растяжимым, ждала
        /// бы от строки запаса, которого вывод не отдаст.
        sta_vector<float> stretchable;

        /// Во что обойдётся перенос на этом месте: ширина дефиса того шрифта,
        /// каким набран сам символ. Разбивке она нужна наперёд — штраф на
        /// переломе занимает место только на той строке, где перелом взят.
        sta_vector<float> hyphen;
    };

    static CharacterMetrics characterMetrics(const sta_vector<ShapedRun>& runs,
                                             uint32_t textLength, float scale) {
        CharacterMetrics metrics{sta_vector<float>(textLength, 0.0f),
                                 sta_vector<float>(textLength, 0.0f),
                                 sta_vector<float>(textLength, 0.0f),
                                 sta_vector<float>(textLength, 0.0f)};

        for (const ShapedRun& run : runs) {
            const uint32_t start = run.format.start;
            const uint32_t length = run.format.length;
            const bool invisible = isInvisible(run.format.script);

            for (uint32_t i = 0; i < length; ++i)
                metrics.hyphen[start + i] = run.hyphenAdvance * scale;

            for (uint32_t i = 0; i < length; ++i) {
                const uint16_t glyph = run.clusterMap[i];
                if (i > 0 && run.clusterMap[i - 1] == glyph)
                    continue;

                uint32_t next = i + 1;
                while (next < length && run.clusterMap[next] == glyph)
                    ++next;

                const auto glyphEnd = next < length
                                          ? run.clusterMap[next]
                                          : static_cast<uint16_t>(run.glyphIndices.size());

                float width = 0.0f;
                float shrink = 0.0f;
                for (uint32_t g = glyph; g < glyphEnd && g < run.advances.size(); ++g) {
                    width += run.advances[g];
                    if (g < run.opportunities.size())
                        shrink += run.opportunities[g].compressionMaximum;
                }

                metrics.width[start + i] = width * scale;
                metrics.shrink[start + i] = shrink * scale;
                metrics.stretchable[start + i] = invisible ? 0.0f : width * scale;
            }
        }

        return metrics;
    }

    /* ---------------- боксы, клей и штрафы ---------------- */

    /// Цена переноса. У Кнута 50, и здесь она та же: перенос — не изъян
    /// набора, а обычное его средство, и дороже он ровно настолько, чтобы при
    /// прочих равных строка предпочла кончиться на пробеле.
    static constexpr float kHyphenPenalty = 50.0f;

    /// Перенос, уносящий на следующую строку всего две буквы, дороже вшестеро.
    /// Запрещать его нельзя — в трудном абзаце он бывает единственным, и запрет
    /// вернул бы нас к строке с дырой, — но пусть разбивка берёт его последним.
    static constexpr float kShortTailHyphenPenalty = 300.0f;

    /// Разрыв между отступом строки и её первым словом — последнее средство:
    /// строка из одного отступа хуже любого переноса, и берёт её разбивка,
    /// только когда отступ со словом в полосу не входят никак. При выключке
    /// влево клей не сжимается, и строку «отступ + слово», переполненную на
    /// любую величину, первый проход отвергает: слово уходит на следующую
    /// строку без отступа, а строка остаётся из одного отступа — так принято
    /// сознательно, и здесь это не чинится.
    static constexpr float kIndentBreakPenalty = 1000.0f;

    static sta_vector<BreakItem> breakItems(u16_view text, const CharacterMetrics& metrics,
                                            const sta_vector<DWRITE_LINE_BREAKPOINT>& points,
                                            std::span<const uint8_t> hyphens,
                                            bool ragged, float maxWidth) {
        const auto textLength = static_cast<uint32_t>(text.size());

        sta_vector<BreakItem> items;
        items.reserve(textLength / 4 + 8);

        // Слово, которое не помещается в полосу целиком (длинный адрес,
        // склеенный текст, а в примере nightmare — нарочно целая строка
        // клавиатуры), приходится рубить по символу. Правила переносов такое
        // не разрывают и правы: разрыва там нет. Но показать книгу важнее
        // правила, которое здесь всё равно ничего не спасает.
        //
        // Рубится оно на четверти полосы, а не на полосу целиком: кусков на
        // строку тогда приходится несколько, и строка может кончиться там, где
        // надо, а не там, где случайно легла граница куска. Но проверяется
        // именно полная ширина слова — иначе под нож пошло бы каждое слово
        // длиннее четверти полосы, то есть половина слов в заголовке.
        const float chunkWidth = std::max(maxWidth * 0.25f, 1.0f);
        bool chopWord = false;

        // Конец слова, которое набирается сейчас: по нему видно, сколько букв
        // перенос унесёт на следующую строку.
        uint32_t wordEnd = 0;

        // Граница буквы, до которой уже дошли внутри рубимого слова или
        // отступа. Идёт только вперёд и только в том, что рубится, — то есть
        // почти никогда: весь остальной текст рвётся по переломам DirectWrite,
        // а внутри буквы их не бывает.
        size_t letter = 0;

        float pending = 0.0f;
        float pendingShrink = 0.0f;
        float pendingStretchable = 0.0f;
        bool pendingIsGlue = false;
        bool pendingStarted = false;
        uint32_t pendingStart = 0;

        // Набирается отступ строки — пробелы, с которых она начинается: в начале
        // абзаца или сразу за обязательным разрывом. Он бокс, а не клей. Клей за
        // разрывом разбивка съедает, как съедает его Кнут, а нарезка строк
        // начинает следующую строку с конца разрыва, то есть с этих самых
        // пробелов: мерить отступ разбивка должна так же, как его рисует
        // нарезка, — иначе строка с отступом выходит за полосу на его ширину. Отступ
        // оставляет свёртка пробелов: в преформате — любой; в прочем тексте она
        // складывает только пробел, табуляцию и переводы строк ASCII, и в начале
        // абзаца и за переводом строки от `<br/>` доходят неразрывный пробел,
        // пробелы U+2000–U+200A, U+202F, U+205F, U+3000, перевод строки абзаца,
        // начатого с `<br/>`, и пробел — основа знака. Всё это поставил автор, и
        // всё это надо и показать, и измерить. Как бокс отступ не тянется — ни
        // разбивкой, ни выключкой (sliceRun снимает с его глифов возможности
        // растяжения), — и рубится, если сам шире полосы, как слово (chopWord).
        bool pendingIsIndent = false;

        // Место переноса по образцам годится, только если здесь начинается и
        // кластер шейпинга: места стоят на границах букв, а кластер бывает шире
        // буквы — лигатура «ffi» в «official» одна на три буквы, и место
        // «of-ficial» внутри неё унесло бы глиф целиком на следующую строку,
        // оставив на этой «o-». У второй и следующих единиц кластера ширины
        // нет — её несёт первая.
        const auto hyphenAt = [&](uint32_t at) {
            return at < hyphens.size() && hyphens[at] != 0 && metrics.width[at] > 0.0f;
        };

        const auto penalty = [&](uint32_t at, float value, bool flagged = false,
                                 float width = 0.0f) {
            BreakItem item;
            item.kind = BreakItem::Kind::Penalty;
            item.penalty = value;
            item.flagged = flagged;
            item.width = width;
            item.textPosition = at;
            item.textEnd = at;
            items.push_back(item);
        };

        const auto flush = [&](uint32_t endPosition) {
            if (!pendingStarted)
                return;
            pendingStarted = false;

            BreakItem item;
            item.textPosition = pendingStart;
            item.textEnd = endPosition;

            if (pendingIsGlue) {
                // Неразрывный пробел: издатель расставил их там, где строка
                // рваться не должна, — «И. И. Иванов», «в 1860 г.», «стр. 5»;
                // в «Отцах и детях» их полторы тысячи. Разрешён ли разрыв после
                // пробельной череды, говорит символ за ней: перед самим
                // пробелом разрыва не бывает никогда, и спрашивать надо не о
                // нём. Запрет ставится штрафом перед клеем, как `~` у Кнута:
                // клей, перед которым стоит штраф, а не бокс, местом разрыва
                // уже не считается, но растяжимость строке отдаёт по-прежнему.
                //
                // Тот же запрет — там, где за клеем не начинается буква, что бы
                // ни ответил DirectWrite: пробел бывает основой знака за ним
                // («аааааа ́бббб»), и по правилам переноса строк (UAX #14, LB10)
                // разрыв перед таким знаком разрешён, а следующая строка
                // начиналась бы со знака без основы. Вывод букву не рвёт;
                // граница — из wxl::core, по куску текста от начала клея: есть
                // ли она перед символом за клеем, решают этот символ и пробел
                // перед ним, а не то, что стоит до клея.
                if (endPosition < textLength) {
                    const u16_view glue = text.substr(pendingStart);
                    const size_t glueLength = endPosition - pendingStart;
                    if (points[endPosition].breakConditionBefore == DWRITE_BREAK_CONDITION_MAY_NOT_BREAK ||
                        unicode::floor_grapheme_boundary(glue, glueLength) != glueLength)
                        penalty(pendingStart, kInfinitePenalty);
                }

                item.kind = BreakItem::Kind::Glue;
                item.width = pending;

                // Пробел тянется наполовину — значение plain TeX: оно задаёт ту
                // меру дыры, которую глаз ещё считает набором. Сжимаемость
                // берётся из ответа DirectWrite: сколько он разрешил снять,
                // столько разбивка и вправе рассчитывать.
                //
                // Свободный правый край делается не здесь, а через lineEndStretch
                // в настройках разбивки: клей на переломе съедается разрывом.
                //
                // Тянется только то, что тянет выключка: табуляция посреди
                // строки — клей, но растяжимости у неё нет (stretchable).
                item.stretch = pendingStretchable * 0.5f;

                // Сжимаемость — меньшее из двух: сколько разрешил DirectWrite и
                // восьмая часть пробела. Второй предел нужен потому, что
                // GetJustificationOpportunities называет теоретический потолок,
                // а JustifyGlyphAdvances до него не дотягивает — он раздаёт
                // сжатие по приоритетам и останавливается раньше. Разбивка,
                // поверившая в потолок, выбирает строку, которую потом некому
                // сжать, и та вылезает за поле. Восьмая доля — то, что сжимается
                // всегда и незаметно для глаза.
                item.shrink = ragged ? 0.0f : std::min(pendingShrink, pending * 0.125f);
            } else {
                item.kind = BreakItem::Kind::Box;
                item.width = pending;
            }

            items.push_back(item);
            pending = 0.0f;
            pendingShrink = 0.0f;
            pendingStretchable = 0.0f;
        };

        for (uint32_t i = 0; i < textLength; ++i) {
            const DWRITE_LINE_BREAKPOINT& point = points[i];
            const bool space = point.isWhitespace != 0;

            if (i > 0 && point.breakConditionBefore == DWRITE_BREAK_CONDITION_MUST_BREAK) {
                flush(i);

                // Клей бесконечной растяжимости перед обязательным разрывом —
                // `\hfil\break` у Кнута. Без него строка, кончающаяся на <br>,
                // обязана заполнить полосу тем, что в ней есть, а она короткая
                // по смыслу; её коэффициент подгонки вылетает за терпимость,
                // разрыв признаётся невозможным — и, поскольку обязательный
                // разрыв закрывает разом все активные узлы, разбивка теряет
                // последний из них и не находит абзацу ни одного решения.
                BreakItem fill;
                fill.kind = BreakItem::Kind::Glue;
                fill.stretch = kInfinitePenalty;
                fill.textPosition = i;
                fill.textEnd = i;
                items.push_back(fill);

                penalty(i, -kInfinitePenalty);
            } else if (i > 0 && point.breakConditionBefore == DWRITE_BREAK_CONDITION_CAN_BREAK &&
                       !space && (points[i - 1].isWhitespace == 0)) {
                // Разрыв внутри непробельного текста: после дефиса, после
                // мягкого переноса из книги, между иероглифами.
                //
                // Мягкий перенос дефис печатает, настоящий дефис уже напечатан;
                // оба помечаются флагом, чтобы два переноса подряд обошлись
                // дороже. Между иероглифами не перенос вовсе: место разрешено,
                // ничем не помечено и ничего не стоит.
                flush(i);

                const bool soft = points[i - 1].isSoftHyphen != 0;
                const char16_t before = text.plain()[i - 1];
                const bool afterHyphen = before == u'-' || before == u'\x2010';

                penalty(i, soft || afterHyphen ? kHyphenPenalty : 0.0f, soft || afterHyphen,
                        soft ? metrics.hyphen[i - 1] : 0.0f);
            } else if (i > 0 && !space && hyphenAt(i)) {
                // Место переноса по образцам. Стоит оно ширины дефиса, и платит
                // её только та строка, которая здесь и кончится.
                flush(i);
                penalty(i, hyphens[i] == 2 ? kShortTailHyphenPenalty : kHyphenPenalty, true,
                        metrics.hyphen[i]);
            }

            // Набирается ли сейчас пробельное — клей или отступ: пробел их
            // продолжает, непробельный символ кончает. Отступ кончается на
            // первом непробельном символе.
            const bool pendingIsSpace = pendingIsGlue || pendingIsIndent;
            const bool indentEnds = pendingStarted && pendingIsIndent && !space;

            if (!pendingStarted || space != pendingIsSpace) {
                // Между отступом и словом — место разрыва, иначе отступ со
                // словом были бы одним боксом, а он может и не войти в
                // полосу, хотя каждый по отдельности входит. Только где
                // разрыв разрешён — неразрывный пробел держит слово при себе —
                // и только на границе буквы: пробел бывает основой знака за
                // ним, и вывод букву не рвёт.
                if (indentEnds) {
                    const u16_view indent = text.substr(pendingStart);
                    const size_t indentLength = i - pendingStart;
                    flush(i);
                    if (point.breakConditionBefore != DWRITE_BREAK_CONDITION_MAY_NOT_BREAK &&
                        unicode::floor_grapheme_boundary(indent, indentLength) == indentLength)
                        penalty(i, kIndentBreakPenalty);
                } else {
                    flush(i);
                }

                const bool lineStart =
                    i == 0 || point.breakConditionBefore == DWRITE_BREAK_CONDITION_MUST_BREAK;
                pendingStarted = true;
                pendingIsIndent = space && lineStart;
                pendingIsGlue = space && !pendingIsIndent;
                pendingStart = i;

                // Отступ шире полосы рубится, как слово шире полосы, — по
                // границам букв (startsLetter). Мерится он до первого
                // непробельного символа или до следующего обязательного
                // разрыва: там он кончается.
                if (pendingIsIndent) {
                    float indentWidth = 0.0f;
                    for (uint32_t at = i; at < textLength && points[at].isWhitespace != 0 &&
                                          (at == i || points[at].breakConditionBefore !=
                                                          DWRITE_BREAK_CONDITION_MUST_BREAK);
                         ++at)
                        indentWidth += metrics.width[at];

                    chopWord = indentWidth > maxWidth;
                    if (chopWord)
                        letter = i;
                }

                // Начинается слово — сразу смотрим, поместится ли оно в полосу.
                // Не целиком, а кусками между его собственными местами разрыва:
                // переносами и разрывами после дефиса. Слово шире полосы, но
                // с переносами, которые делят его на помещающиеся куски, рубить
                // нельзя — иначе разбивка получит бесплатные места рубки и
                // предпочтёт их честному переносу с дефисом.
                //
                // Только в начале слова, а не после каждого разрыва внутри
                // него: это то же слово, и просмотр вперёд так проходит по
                // каждому символу ровно один раз за слово, то есть по книге —
                // один раз.
                if (!space && (i == 0 || points[i - 1].isWhitespace != 0)) {
                    float piece = 0.0f;
                    float widest = 0.0f;
                    uint32_t at = i;
                    for (; at < textLength && points[at].isWhitespace == 0; ++at) {
                        const bool breaksHere =
                            at > i && (points[at].breakConditionBefore == DWRITE_BREAK_CONDITION_CAN_BREAK ||
                                       hyphenAt(at));
                        if (breaksHere) {
                            widest = std::max(widest, piece + metrics.hyphen[at]);
                            piece = 0.0f;
                        }
                        piece += metrics.width[at];
                    }
                    widest = std::max(widest, piece);

                    chopWord = widest > maxWidth;
                    wordEnd = at;
                    if (chopWord)
                        letter = i;
                }
            }

            // Слово шире всей полосы: длинный адрес, склеенный текст, а в
            // официальном примере nightmare — нарочно целая строка клавиатуры.
            // Правила переносов строки такое не разрывают и правы: разрыва там
            // нет. Но показать книгу важнее, чем соблюсти правило, которое
            // здесь всё равно ничего не спасает, поэтому рвём по символу — и
            // только здесь, когда иначе строка не поместится никак.
            //
            // По букве, а не по единице текста, и не внутри кластера. Рвать
            // можно только там, где начинается и буква, и кластер: у второй и
            // следующих единиц кластера ширины нет — её несёт первая, — а
            // кластер DirectWrite бывает уже буквы: чамо хангыля без готового
            // слога шрифт держит отдельными кластерами, и рубка по кластеру
            // отрезала бы от слога его конечную согласную.
            const auto startsLetter = [&](uint32_t at) {
                while (letter < at)
                    letter = unicode::next_grapheme_boundary(text, letter);
                return letter == at;
            };

            if (chopWord && !pendingIsGlue && pending > 0.0f && metrics.width[i] > 0.0f &&
                pending + metrics.width[i] > chunkWidth && startsLetter(i)) {
                flush(i);

                // Клей нулевой ширины: место, где рвать можно, но ничего не
                // стоит. Кусков на строку выходит несколько, поэтому строка,
                // кончающаяся на таком клее, растяжимость всё же получает — от
                // тех же клеев, что остались внутри неё.
                BreakItem anywhere;
                anywhere.kind = BreakItem::Kind::Glue;
                anywhere.stretch = chunkWidth;
                anywhere.textPosition = i;
                anywhere.textEnd = i;
                items.push_back(anywhere);

                pendingStarted = true;
                pendingIsGlue = false;
                pendingStart = i;
            }

            pending += metrics.width[i];
            pendingShrink += metrics.shrink[i];
            pendingStretchable += metrics.stretchable[i];
        }

        flush(textLength);

        // Хвост абзаца по Кнуту: бесконечно растяжимый клей и обязательный
        // разрыв. Так последняя строка не выключается по формату.
        BreakItem tail;
        tail.kind = BreakItem::Kind::Glue;
        tail.stretch = kInfinitePenalty;
        tail.textPosition = textLength;
        tail.textEnd = textLength;
        items.push_back(tail);

        penalty(textLength, -kInfinitePenalty);
        return items;
    }

    /* ---------------- нарезка строк ---------------- */

    /// Часть прогона, попавшая в строку. Кластерная карта указывает на глифы:
    /// чтобы взять символы [from, to), надо взять глифы, на которые они
    /// указывают, — и ни одним меньше, иначе развалится лигатура.
    /// @param scale отношение нужного кегля к опорному: метрики шейпинга
    ///        линейны по кеглю, поэтому строка при другом размере получается
    ///        умножением, а не новым обращением к DirectWrite. Умножается всё
    ///        сразу здесь, при нарезке, — копия глифов на строку всё равно
    ///        делается, а копировать книгу целиком ради масштаба незачем.
    /// @param rigidEnd конец отступа строки: глифам символов до него выключка
    ///        не отдаёт растяжения.
    static void sliceRun(const ShapedRun& run, uint32_t from, uint32_t to, uint32_t rigidEnd,
                         LineRun& into, float scale) {
        const uint32_t runStart = run.format.start;
        const uint32_t runEnd = runStart + run.format.length;

        if (to <= runStart || from >= runEnd)
            return;

        const uint32_t localFrom = std::max(from, runStart) - runStart;
        const uint32_t localTo = std::min(to, runEnd) - runStart;
        if (localFrom >= localTo)
            return;

        const uint16_t firstGlyph = run.clusterMap[localFrom];
        const auto lastGlyph = localTo < run.format.length
                                   ? run.clusterMap[localTo]
                                   : static_cast<uint16_t>(run.glyphIndices.size());

        into.glyphs.fontFace = run.format.fontFace;
        into.glyphs.fontSize = run.format.fontSize * scale;
        into.glyphs.bidiLevel = run.format.bidiLevel;
        into.glyphs.style = run.format.style;
        into.glyphs.invisible = isInvisible(run.format.script);
        into.glyphs.textStart = runStart + localFrom;
        into.glyphs.textLength = localTo - localFrom;
        into.hyphenGlyph = run.hyphenGlyph;
        into.hyphenAdvance = run.hyphenAdvance * scale;
        into.hyphenAscenderOffset = run.format.ascenderOffset * scale;

        // Глифы отступа — глифы кластеров, что лежат в отступе целиком, то есть
        // до первого глифа кластера символа rigidEnd: кластер пробела со знаком
        // на нём — уже буква слова, а не отступ.
        const uint32_t localRigid =
            std::clamp(rigidEnd, runStart + localFrom, runStart + localTo) - runStart;
        const auto rigidGlyph = localRigid < run.format.length
                                    ? run.clusterMap[localRigid]
                                    : static_cast<uint16_t>(run.glyphIndices.size());

        for (uint32_t g = firstGlyph; g < lastGlyph; ++g) {
            const float advance = run.advances[g] * scale;

            into.glyphs.glyphIndices.push_back(run.glyphIndices[g]);
            into.glyphs.advances.push_back(advance);

            DWRITE_GLYPH_OFFSET offset = run.offsets[g];
            offset.advanceOffset *= scale;
            offset.ascenderOffset *= scale;
            into.glyphs.offsets.push_back(offset);

            DWRITE_JUSTIFICATION_OPPORTUNITY opportunity =
                g < run.opportunities.size() ? run.opportunities[g]
                                             : DWRITE_JUSTIFICATION_OPPORTUNITY{};
            // Отступ выключка не тянет: разбивка мерит его боксом, без
            // растяжимости, и вывод мерит так же. Сжатие у отступа остаётся:
            // DirectWrite сжимает и буквы, и без сжимаемого отступа остаток
            // переполнения уходит в буквы короткого слова за ним (замер:
            // 0,86 их ширины вместо не меньше 0,92). Равномерное сжатие в
            // justify — по всей строке — страхует случай, когда DirectWrite
            // остаток не снял.
            if (g < rigidGlyph) {
                opportunity.expansionMinimum = 0.0f;
                opportunity.expansionMaximum = 0.0f;
                opportunity.expansionPriority = 0;
                opportunity.allowResidualExpansion = 0;
            }
            opportunity.expansionMinimum *= scale;
            opportunity.expansionMaximum *= scale;
            opportunity.compressionMaximum *= scale;
            into.opportunities.push_back(opportunity);

            into.glyphs.width += advance;
        }
    }

    /// Порядок прогонов на экране при смешанном направлении (правило L2 из
    /// алгоритма BiDi): последовательности уровня не ниже текущего
    /// переворачиваются, начиная с самого высокого уровня и вниз до самого
    /// низкого нечётного.
    static void reorderVisually(sta_vector<LineRun>& runs) {
        if (runs.size() < 2)
            return;

        uint8_t highest = 0;
        uint8_t lowestOdd = 63;
        for (const LineRun& run : runs) {
            highest = std::max(highest, run.glyphs.bidiLevel);
            if (run.glyphs.bidiLevel & 1)
                lowestOdd = std::min(lowestOdd, run.glyphs.bidiLevel);
        }

        for (int level = highest; level >= lowestOdd && level > 0; --level) {
            for (size_t i = 0; i < runs.size();) {
                if (runs[i].glyphs.bidiLevel < level) {
                    ++i;
                    continue;
                }
                size_t j = i;
                while (j < runs.size() && runs[j].glyphs.bidiLevel >= level)
                    ++j;
                std::reverse(runs.begin() + static_cast<ptrdiff_t>(i),
                             runs.begin() + static_cast<ptrdiff_t>(j));
                i = j;
            }
        }
    }

    /// Раздвигает строку до полосы. Ширины считает DirectWrite: он знает, где
    /// в этой письменности можно добавить, а где нельзя.
    void justify(sta_vector<LineRun>& runs, float available) {
        if (!analyzer1)
            return;

        sta_vector<float> advances;
        sta_vector<DWRITE_GLYPH_OFFSET> offsets;
        sta_vector<DWRITE_JUSTIFICATION_OPPORTUNITY> opportunities;

        for (const LineRun& run : runs) {
            advances.insert(advances.end(), run.glyphs.advances.begin(), run.glyphs.advances.end());
            offsets.insert(offsets.end(), run.glyphs.offsets.begin(), run.glyphs.offsets.end());
            opportunities.insert(opportunities.end(), run.opportunities.begin(), run.opportunities.end());
        }

        const auto glyphCount = static_cast<UINT32>(advances.size());
        if (glyphCount == 0)
            return;

        sta_vector<float> justified(glyphCount);
        sta_vector<DWRITE_GLYPH_OFFSET> justifiedOffsets(glyphCount);

        // Неудача здесь — не повод сдаться: строку всё равно надо уместить, и
        // ниже это доделает равномерное сжатие. Поэтому берём исходные ширины
        // и идём дальше, а не выходим.
        if (FAILED(analyzer1->JustifyGlyphAdvances(available, glyphCount, opportunities.data(),
                                                   advances.data(), offsets.data(), justified.data(),
                                                   justifiedOffsets.data()))) {
            justified = advances;
            justifiedOffsets = offsets;
        }

        // Остаток. DirectWrite сжимает ровно настолько, насколько разрешают
        // возможности, и на последних долях пикселя может не дотянуть. Строка,
        // вылезшая за поле, ломает всю страницу, поэтому такой остаток снимаем
        // равномерно — доля процента на межбуквенном просвете не видна.
        // Большой промах не трогаем: он означал бы ошибку в разбивке, и её
        // надо видеть, а не заглаживать.
        float total = 0.0f;
        for (const float advance : justified)
            total += advance;

        // Сжимаются ширины, а глиф буквы не сжимается — поэтому знак,
        // посаженный на букву, не должен уехать вместе с сократившейся
        // шириной. У такого знака ширины нет, и стоит он сдвигом от пера после
        // своей основы: насколько сократилась ширина основы, настолько его
        // сдвиг и возвращается назад. Знак с собственной шириной — отдельный
        // глиф в строке и сжимается как буква.
        //
        // Отступ выключка не тянет (sliceRun), а сжимает вместе со всеми, и
        // равномерное сжатие тоже идёт по всей строке, с отступом: снять
        // остаток с одних букв короткого слова за отступом во всю полосу
        // значило бы сжать их без меры, до наложения глифов, — порог в 8 %
        // меряет строку целиком.
        if (total > available && total <= available * 1.08f && total > 0.0f) {
            const float squeeze = available / total;
            float drift = 0.0f;   // на сколько сократилась ширина последней основы

            for (size_t i = 0; i < justified.size(); ++i) {
                if (justified[i] == 0.0f) {
                    justifiedOffsets[i].advanceOffset += drift;
                } else {
                    drift = justified[i] * (1.0f - squeeze);
                    justified[i] -= drift;
                }
            }
        }

        UINT32 read = 0;
        for (LineRun& run : runs) {
            run.glyphs.width = 0.0f;
            for (size_t i = 0; i < run.glyphs.advances.size(); ++i, ++read) {
                run.glyphs.advances[i] = justified[read];
                run.glyphs.offsets[i] = justifiedOffsets[read];
                run.glyphs.width += justified[read];
            }
        }
    }
};

/* ================================================================== */

Engine::Engine(IDWriteFactory* factory) : impl_(std::make_unique<Impl>()) {
    impl_->factory = factory;
    check(factory->CreateTextAnalyzer(impl_->analyzer.GetAddressOf()));
    impl_->analyzer.As(&impl_->analyzer1);   // может не быть: см. justify()

    check(factory->GetSystemFontCollection(impl_->systemFonts.GetAddressOf(), FALSE));

    // Подбор запасного шрифта появился в IDWriteFactory2 (Windows 8.1). Без
    // него книга с редким символом покажет прямоугольник вместо глифа —
    // неприятно, но не смертельно, поэтому это не ошибка.
    ComPtr<IDWriteFactory2> factory2;
    if (SUCCEEDED(impl_->factory.As(&factory2)))
        factory2->GetSystemFontFallback(impl_->fallback.GetAddressOf());
}

Engine::~Engine() = default;

void Engine::setTextStyle(const TextStyle& style) {
    impl_->style = style;
    impl_->spaces.clear();
    ++impl_->styleGeneration;
}

const TextStyle& Engine::textStyle() const { return impl_->style; }

float Engine::lineHeightFor(const ParagraphStyle& style) const { return impl_->lineHeightOf(style); }

float Engine::averageCharWidth(float fontSize) const {
    Impl& impl = *impl_;
    const float fallback = fontSize * 0.5f;

    // Мерить каждый раз незачем: ответ зависит только от шрифта и кегля, а
    // спрашивают его на каждой перевёрстке.
    if (impl.charWidth > 0.0f && impl.charWidthFontSize == fontSize &&
        impl.charWidthGeneration == impl.styleGeneration) {
        return impl.charWidth;
    }

    // Латиница или кириллица — по языку книги: средняя ширина знака у них
    // разная, и мерить английскую фразу для русской книги значило бы мерить
    // не то.
    const TextStyle& text = impl.style;
    const bool cyrillic = text.locale.starts_with(L"ru");
    const std::wstring_view sample = cyrillic ? std::wstring_view{kCyrillicSample}
                                              : std::wstring_view{kLatinSample};

    // Целой строкой через IDWriteTextLayout, а не своей вёрсткой: здесь нужна
    // одна ширина фразы, и разбивка на строки ей ни к чему.
    ComPtr<IDWriteTextFormat> format;
    if (FAILED(impl.factory->CreateTextFormat(text.fontFamily.c_str(), nullptr,
                                              DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                              DWRITE_FONT_STRETCH_NORMAL, fontSize,
                                              text.locale.c_str(), format.GetAddressOf()))) {
        return fallback;
    }

    ComPtr<IDWriteTextLayout> phrase;
    if (FAILED(impl.factory->CreateTextLayout(sample.data(), static_cast<UINT32>(sample.size()),
                                              format.Get(), 1.0e6f, 1.0e6f,
                                              phrase.GetAddressOf()))) {
        return fallback;
    }

    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(phrase->GetMetrics(&metrics)) || metrics.width <= 0.0f) return fallback;

    impl.charWidth = metrics.width / static_cast<float>(sample.size());
    impl.charWidthFontSize = fontSize;
    impl.charWidthGeneration = impl.styleGeneration;
    return impl.charWidth;
}

ShapedParagraphPtr Engine::shape(const BookText& book, const Paragraph& paragraph,
                                 const ParagraphStyle& style) {
    // Конструктор закрыт: отшейпленный абзац умеет делать только движок.
    ShapedParagraphPtr result(new ShapedParagraph());
    impl_->shapeInto(*result->data_, book, paragraph, style);
    return result;
}

sta_vector<Line> Engine::layout(const BookText& book, const Paragraph& paragraph, float width,
                                const ParagraphStyle& style) {
    const ShapedParagraphPtr shaped = shape(book, paragraph, style);
    return layout(*shaped, width, style);
}

sta_vector<Line> Engine::layout(const ShapedParagraph& given, float width,
                                const ParagraphStyle& style) {
    Impl& impl = *impl_;

    // Стиль мог разойтись с тем, на котором абзац шейпили, не только размером.
    // Тогда сохранённые глифы просто не те, и правильный ответ дороже быстрого:
    // шейпим заново на месте. Кэш зовущего от этого не чинится — но и не врёт.
    const ShapedParagraph::Data* data = given.data_.get();
    ShapedParagraphPtr reshaped;

    if (data->styleGeneration != impl.styleGeneration || data->bold != style.bold ||
        data->italic != style.italic || data->monospace != style.monospace) {
        reshaped = shape(*data->book, data->paragraph, style);
        data = reshaped->data_.get();
    }

    const BookText& book = *data->book;
    const Paragraph& paragraph = data->paragraph;
    const u16_view text = book.textOf(paragraph);
    const auto textLength = static_cast<uint32_t>(text.size());
    if (textLength == 0 || width <= 0.0f || data->referenceFontSize <= 0.0f)
        return {};

    /* 1-3. Анализ, прогоны формата и шейпинг уже сделаны — берём готовое.
       Кегль подгоняется отношением: метрики шейпинга линейны по нему. */
    const sta_vector<ShapedRun>& shaped = data->runs;
    const float scale = style.fontSize / data->referenceFontSize;

    /* 4-5. Переломы. */
    const bool ragged = style.alignment != Alignment::Justify;
    const Impl::CharacterMetrics metrics = Impl::characterMetrics(shaped, textLength, scale);
    // Переносы выключены — разбивка их просто не видит; перешейпливать абзац
    // при этом не надо, место переноса от настроек не зависит.
    const std::span<const uint8_t> hyphens =
        impl.style.hyphenation ? std::span<const uint8_t>(data->hyphens)
                               : std::span<const uint8_t>();

    const sta_vector<BreakItem> items =
        Impl::breakItems(text, metrics, data->breakpoints, hyphens, ragged, width);

    const float indent = style.firstLineIndent;
    const bool indented = indent != 0.0f;
    const float firstWidth = std::max(width - indent, width * 0.2f);
    const float lineWidths[2] = {firstWidth, width};

    BreakSettings settings;

    // С переносами разбивке есть из чего выбирать, и терпеть растянутую строку
    // больше незачем: порог возвращается к тому, что у Кнута. Без переносов он
    // остаётся прежним — строгий порог тогда означал бы не красивый набор, а
    // отказ разбить абзац и аварийный проход, где не соблюдается уже ничего.
    if (impl.style.hyphenation)
        settings.tolerance = 3.0f;

    if (ragged) {
        // Свободный правый край: строка вправе кончиться где угодно, и выбор
        // переломов идёт по штрафам, а не по плотности набора.
        settings.lineEndStretch = kInfinitePenalty;
    }

    std::vector<uint32_t> breaks =
        breakLines(items, std::span<const float>(lineWidths, indented ? 2 : 1), settings);

    if (breaks.empty())
        return {};

    // Строки за концом текста не бывает. За последним боксом абзаца стоит
    // клей — пробел, который свёртка оставила в конце (преформат, перевод
    // строки от `<br/>`, неразрывный пробел), или хвостовой клей самого
    // абзаца, — и это место разрыва: разбивка его берёт, когда строка с
    // пробелом вылезает за поле, а без него помещается, или когда аварийный
    // проход потерял все прочие узлы. Разрыв пробел съедает, и завершающему
    // штрафу остаётся строка нулевой длины, начатая на самом конце текста:
    // ставить в ней нечего, и позиции у её начала нет. Её и не заводим —
    // последней становится строка, кончившаяся на этом клее, и выключается
    // как последняя.
    if (breaks.size() > 1 && items[breaks[breaks.size() - 2]].textEnd == textLength)
        breaks.pop_back();

    /* 6. Строки. */
    sta_vector<Line> lines;
    lines.reserve(breaks.size());

    uint32_t from = 0;
    for (size_t index = 0; index < breaks.size(); ++index) {
        const BreakItem& at = items[breaks[index]];
        const uint32_t to = std::max(from, at.textPosition);

        sta_vector<LineRun> runs;
        Line line;
        line.textStart = from;
        line.textLength = to - from;
        line.charOffset = book.charOffsetAt(paragraph, from);

        // Отступ строки — пробелы, с которых она начинается. Строка начинается
        // с пробела только там, где breakItems набрал отступ боксом (в начале
        // абзаца, за обязательным разрывом, на рубке отступа шире полосы): клей
        // разрыв съедает целиком. Бокс разбивка не тянет, и растяжения его
        // глифов выключке здесь тоже не отдать — иначе разбивка мерила бы одно,
        // а вывод рисовал бы другое: почти весь запас строки с длинным отступом
        // и парой пробелов между словами уходил бы в отступ, и левый край кода
        // гулял бы от строки к строке. Сжатие у отступа остаётся (sliceRun):
        // без него остаток переполнения DirectWrite снимал бы с букв короткого
        // слова за отступом; сжатие пробелов на доли процента не видно.
        uint32_t indentEnd = from;
        while (indentEnd < to && data->breakpoints[indentEnd].isWhitespace != 0)
            ++indentEnd;

        for (const ShapedRun& run : shaped) {
            LineRun piece;
            Impl::sliceRun(run, from, to, indentEnd, piece, scale);
            if (piece.glyphs.glyphIndices.empty())
                continue;

            runs.push_back(std::move(piece));
            line.ascent = std::max(line.ascent, run.ascent * scale);
            line.descent = std::max(line.descent, run.descent * scale);
        }

        // Строка кончилась переносом — дописываем дефис в последний по тексту
        // прогон: до перестановки по направлению письма, чтобы он уехал вместе
        // со своим прогоном, и до выключки строки — по формату, вправо или по
        // центру, — чтобы вошёл в её ширину. Место ему разбивка уже оставила:
        // ширину штрафа она считает частью строки, на которой этот перелом
        // взят.
        //
        // Возможность выключки по формату у него нулевая: дефис на краю полосы
        // не раздвигают и не сжимают, как бы ни требовалось строке. Сдвиг —
        // только вертикальный, тот же, что у букв прогона: в `<sup>`, `<sub>`
        // и знаке сноски дефис стоит рядом с ними, а не на базовой линии.
        if (at.kind == BreakItem::Kind::Penalty && at.flagged && at.width > 0.0f &&
            !runs.empty() && runs.back().hyphenGlyph != 0) {
            // Строка, кончившаяся на мягком переносе (U+00AD), кончается его
            // прогоном без вида (isInvisible): анализ помечает так этот
            // форматирующий символ. Глифы такого прогона вывод не рисует, и
            // дефис встаёт своим прогоном того же шрифта сразу за ним, на
            // конце его текста.
            if (runs.back().glyphs.invisible) {
                const LineRun& before = runs.back();
                LineRun dash;
                dash.glyphs.fontFace = before.glyphs.fontFace;
                dash.glyphs.fontSize = before.glyphs.fontSize;
                dash.glyphs.bidiLevel = before.glyphs.bidiLevel;
                dash.glyphs.style = before.glyphs.style;
                dash.glyphs.textStart = before.glyphs.textStart + before.glyphs.textLength;
                dash.hyphenGlyph = before.hyphenGlyph;
                dash.hyphenAdvance = before.hyphenAdvance;
                dash.hyphenAscenderOffset = before.hyphenAscenderOffset;
                runs.push_back(std::move(dash));
            }
            LineRun& last = runs.back();

            last.glyphs.glyphIndices.push_back(last.hyphenGlyph);
            last.glyphs.advances.push_back(last.hyphenAdvance);
            last.glyphs.offsets.push_back(DWRITE_GLYPH_OFFSET{0.0f, last.hyphenAscenderOffset});
            last.opportunities.push_back(DWRITE_JUSTIFICATION_OPPORTUNITY{});
            last.glyphs.width += last.hyphenAdvance;
        }

        Impl::reorderVisually(runs);

        const float available = index == 0 ? firstWidth : width;
        const bool lastLine = index + 1 == breaks.size();

        float natural = 0.0f;
        for (const LineRun& run : runs)
            natural += run.glyphs.width;

        // Выключка по формату идёт в обе стороны: Кнут-Пласс нарочно берёт
        // строку шире полосы, когда её выгодно сжать, и без сжатия она вылезет
        // за поле. Последнюю строку абзаца по формату не выключают, она
        // остаётся выключенной влево — её короткость это конец мысли, а не
        // изъян набора, — но если и она переполнена, сжать надо и её: за поле
        // не должно вылезать ничего.
        //
        // Так же набирается строка, кончившаяся обязательным разрывом, —
        // `<br/>` или переводом строки в преформате. Разбивка уже считает её
        // короткой по смыслу: перед разрывом стоит клей бесконечной
        // растяжимости, `\hfil\break` Кнута (breakItems). Растяни её выключка
        // до полосы — пробелы в ней разъехались бы, а одно слово получило бы
        // разрядку остатком.
        const bool forcedBreak = isForcedBreak(at);
        const bool overfull = natural > available + 0.05f;
        const bool wantsJustification =
            style.alignment == Alignment::Justify && !lastLine && !forcedBreak;

        if (natural > 0.0f && (wantsJustification || overfull))
            impl.justify(runs, available);

        float x = index == 0 ? indent : 0.0f;
        float total = 0.0f;
        for (const LineRun& run : runs)
            total += run.glyphs.width;

        switch (style.alignment) {
        case Alignment::Right:  x += available - total; break;
        case Alignment::Center: x += (available - total) * 0.5f; break;
        default: break;
        }

        for (LineRun& run : runs) {
            run.glyphs.originX = x;
            x += run.glyphs.width;
            line.runs.push_back(std::move(run.glyphs));
        }
        line.width = total;

        // Знаки сносок, попавшие на эту строку. Знак с якорем всегда начинает
        // свой прогон стиля (flatten), а на его конце кончается и прогон
        // формата: стиль за знаком уже другой. Поэтому его место на строке —
        // это просто объединение прогонов, пересекающих его, без чужого текста
        // — ни `<sup>` вплотную, ни соседнего знака; отдельно измерять ничего
        // не нужно.
        for (const NoteAnchor& note : book.notesOf(paragraph)) {
            if (note.position + note.length <= from || note.position >= to)
                continue;

            PlacedNote placed;
            placed.body = note.body;
            bool started = false;

            for (const GlyphRun& run : line.runs) {
                if (run.textStart + run.textLength <= note.position ||
                    run.textStart >= note.position + note.length)
                    continue;

                if (!started) {
                    placed.x = run.originX;
                    started = true;
                }
                placed.width += run.width;
            }

            if (started)
                line.notes.push_back(placed);
        }

        if (line.ascent == 0.0f) {
            // Строка без единого глифа всё равно занимает высоту: иначе пустая
            // строка между строфами исчезнет.
            IDWriteFontFace* const face = impl.baseFace(impl.style.fontFamily.c_str(), style.bold, style.italic);
            const DWRITE_FONT_METRICS& fontMetrics = impl.metricsOf(face);
            const float emScale = style.fontSize / static_cast<float>(fontMetrics.designUnitsPerEm);
            line.ascent = fontMetrics.ascent * emScale;
            line.descent = fontMetrics.descent * emScale;
        }

        line.height = std::max(impl.lineHeightOf(style), (line.ascent + line.descent) * 1.05f);

        lines.push_back(std::move(line));
        from = at.textEnd;
    }

    return lines;
}

const PlacedNote* noteUnder(const Line& line, float x, float reach) {
    // Ближайший, а не первый по строке: зона знака шире его самого, и у двух
    // знаков вплотную зоны перекрываются — первый по строке забрал бы щелчок
    // по второму.
    const PlacedNote* nearest = nullptr;
    float nearestDistance = 0.0f;
    for (const PlacedNote& note : line.notes) {
        const float distance = std::max({note.x - x, x - (note.x + note.width), 0.0f});
        if (distance > reach || (nearest && distance >= nearestDistance)) continue;

        nearest = &note;
        nearestDistance = distance;
    }
    return nearest;
}

}  // namespace bukvitsa::typography
