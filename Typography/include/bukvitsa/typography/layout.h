#pragma once
// Вёрстка абзаца в строки глифов.
//
// Разделение труда с DirectWrite такое:
// система делает итемизацию, BiDi, шейпинг, подбор запасного шрифта, ищет места
// возможных переломов и раскладывает по глифам выключку по формату; своими
// остаются разбивка абзаца на строки (Кнут-Пласс, linebreak.h) и всё книжное
// поверх строк.
//
// Что отдаётся наружу — прогоны глифов, готовые к ID2D1DeviceContext::
// DrawGlyphRun. Никакого промежуточного представления «строка текста» нет
// намеренно: между вёрсткой и растеризацией не должно быть места, где текст
// можно измерить иначе, чем его нарисуют.
//
// Владение: шрифты живут в Engine, а прогоны на них только ссылаются. Значит,
// строки нельзя пережить движком — что естественно, ведь при смене кегля
// перевёрстывается всё.

// Через block.h, а не своими: там собрано объединение стандартных заголовков
// всех публичных заголовков вёрстки и стоит импорт, после которого стандартный
// заголовок MSVC уже не примет.
#include "bukvitsa/typography/block.h"

namespace bukvitsa::typography {

/// Как строка выключается по полосе.
enum class Alignment : uint8_t { Left, Right, Center, Justify };

/// Шрифт и язык — то, что задаётся один раз на книгу.
struct TextStyle {
    std::wstring fontFamily = L"Georgia";
    std::wstring monospaceFamily = L"Consolas";
    std::wstring locale = L"ru-RU";
    float fontSize = 20.0f;        ///< кегль основного текста, DIP
    float lineHeight = 1.45f;      ///< множитель кегля

    /// Переносить ли слова. Без переносов выключенный по формату русский текст
    /// набирается дырами, поэтому по умолчанию они есть; обойтись без них —
    /// выбор читателя, и разбивка тогда становится терпимее к растянутой
    /// строке, потому что иначе ей нечем набрать абзац вовсе.
    bool hyphenation = true;
};

/// Как набирается один блок. Всё, чем заголовок отличается от абзаца.
struct ParagraphStyle {
    float fontSize = 20.0f;
    float lineHeight = 1.45f;
    float firstLineIndent = 0.0f;  ///< DIP; отрицательный — висячая строка
    Alignment alignment = Alignment::Justify;
    bool bold = false;
    bool italic = false;
    bool monospace = false;
};

/// Прогон глифов одного шрифта — аргумент DrawGlyphRun в готовом виде; прогон
/// без вида (invisible) не рисуется вовсе.
struct GlyphRun {
    IDWriteFontFace* fontFace = nullptr;   ///< живёт в Engine
    float fontSize = 0.0f;
    uint8_t bidiLevel = 0;
    FontStyle style;                       ///< подчёркивание, зачёркивание и роль — для вывода

    /// Глифов не рисовать: прогон, который анализ DirectWrite пометил как не
    /// имеющий вида (DWRITE_SCRIPT_SHAPES_NO_VISUAL) — табуляция, переводы
    /// строки, мягкий перенос и прочие управляющие и форматирующие символы.
    /// Глифы в нём — заглушки шрифта: они есть, чтобы прогон, как любой
    /// другой, связывал строку с текстом. Ширина — ноль, у табуляции — до
    /// позиции табуляции.
    bool invisible = false;

    sta_vector<uint16_t> glyphIndices;
    sta_vector<float> advances;
    sta_vector<DWRITE_GLYPH_OFFSET> offsets;

    /// Кусок Paragraph::text, из которого набран прогон. Нужен всем, кто
    /// связывает нарисованное с исходным текстом: знаку сноски, подсветке
    /// найденного, выделению.
    uint32_t textStart = 0;
    uint32_t textLength = 0;

    float originX = 0.0f;                  ///< от левого края полосы
    float width = 0.0f;
};

/// Знак сноски, попавший на эту строку, — уже в координатах строки, чтобы
/// поймать по нему щелчок, ничего больше не пересчитывая.
struct PlacedNote {
    uint32_t body = 0;                     ///< номер тела сноски (NoteAnchor::body)
    float x = 0.0f;                        ///< от левого края полосы
    float width = 0.0f;
};

/// Одна набранная строка.
struct Line {
    sta_vector<GlyphRun> runs;
    sta_vector<PlacedNote> notes;

    float ascent = 0.0f;              ///< от базовой линии вверх
    float descent = 0.0f;
    float height = 0.0f;              ///< с учётом межстрочного множителя
    float width = 0.0f;               ///< фактически занятое место

    uint32_t textStart = 0;           ///< в Paragraph::text
    uint32_t textLength = 0;
    uint32_t charOffset = 0;          ///< позиция первого символа строки в книге
};

/// Знак сноски на строке под этим местом, или nullptr. x — от левого края
/// полосы, как у PlacedNote; reach — на сколько зона щелчка шире самого знака
/// с каждой стороны: попасть мышью в знак шириной в цифру трудно. Знаки
/// вплотную делят зону, и берётся ближайший — внутри знака он сам.
const PlacedNote* noteUnder(const Line& line, float x, float reach);

/// Абзац, прошедший анализ и шейпинг, — то, что не надо делать заново.
///
/// Ни итемизация, ни BiDi, ни поиск переломов, ни подбор глифов не зависят ни
/// от ширины полосы, ни от кегля: `GetGlyphs` не принимает размера шрифта
/// вовсе. От кегля зависят только метрики — ширины, сдвиги, возможности
/// выключки по формату, — и зависят линейно, потому что естественный режим
/// измерения DirectWrite считает их как «единицы дизайна × кегль ÷ единиц на
/// кегель».
///
/// Отсюда весь смысл: шейпинг делается один раз на книгу, а смена кегля и
/// размера окна стоят одной разбивки на строки — на романе это разница между
/// четвертью секунды и сорока миллисекундами.
///
/// Владение: внутри лежат виды в хранилище книги (текст и прогоны абзаца) и
/// указатели на шрифты движка, поэтому объект нельзя пережить ни книге, ни
/// `Engine`. Владелец у него один — тот, кто его завёл.
class ShapedParagraph {
public:
    ~ShapedParagraph();

    ShapedParagraph(const ShapedParagraph&) = delete;
    ShapedParagraph& operator=(const ShapedParagraph&) = delete;

    /// Кегль, на котором абзац отшейпили. Вёрстка на другом кегле пересчитывает
    /// метрики отношением, не обращаясь к DirectWrite заново.
    float referenceFontSize() const;

private:
    friend class Engine;
    ShapedParagraph();

    struct Data;
    std::unique_ptr<Data> data_;
};

using ShapedParagraphPtr = std::unique_ptr<ShapedParagraph>;

/// Движок вёрстки: держит DirectWrite и кэш шрифтов, верстает абзацы.
///
/// Один на приложение (шрифты кэшируются), не потокобезопасен: DirectWrite
/// позволяет, но у читалки вёрстка идёт на своём потоке целиком.
class Engine {
public:
    /// @param factory фабрика DirectWrite; читалка берёт её у CompositionHost,
    ///        тесты создают свою.
    explicit Engine(IDWriteFactory* factory);
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    void setTextStyle(const TextStyle& style);
    const TextStyle& textStyle() const;

    /// Анализирует и шейпит абзац — самая дорогая часть вёрстки, та, которую
    /// стоит сохранить. Кегль из `style` запоминается как опорный. Текст и
    /// прогоны абзаца берутся из хранилища книги, и результат смотрит в него.
    ShapedParagraphPtr shape(const BookText& book, const Paragraph& paragraph,
                             const ParagraphStyle& style);

    /// Разбивает отшейпленный абзац на строки шириной width. Дёшево: обращений
    /// к DirectWrite здесь только выключка готовых строк по формату.
    ///
    /// Если стиль разошёлся с тем, на котором абзац шейпили, не размером
    /// (сменилось начертание или шрифт книги), абзац шейпится заново на месте —
    /// результат верен всегда, сэкономлено только когда звали правильно.
    sta_vector<Line> layout(const ShapedParagraph& shaped, float width,
                            const ParagraphStyle& style);

    /// Верстает абзац в строки шириной width — шейпинг и разбивка разом.
    ///
    /// Пустой абзац даёт пустой список: блок без текста не занимает полосы.
    sta_vector<Line> layout(const BookText& book, const Paragraph& paragraph, float width,
                            const ParagraphStyle& style);

    /// Высота строки при таком кегле — нужна пагинатору до вёрстки, чтобы
    /// прикинуть, влезет ли блок.
    float lineHeightFor(const ParagraphStyle& style) const;

    /// Средняя ширина знака шрифтом книги при этом кегле — то, чем читалка
    /// меряет длину строки, решая, сколько колонок показать. Меряется по
    /// панграмме языка книги (`textStyle().locale`) и запоминается: ответ
    /// зависит только от шрифта и кегля. Не вышло измерить — полкегля.
    float averageCharWidth(float fontSize) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace bukvitsa::typography
