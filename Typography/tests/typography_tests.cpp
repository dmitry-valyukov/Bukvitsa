// Проверка вёрстки на настоящих книгах. Как и у FB3, без фреймворка: ценность
// теста в том, что через него проходят реальные файлы.
//
// Пока проверяется первый слой — разворачивание дерева книги в хранилище
// блоков: порядок блоков, свёртка пробелов, прогоны стиля, прогоны позиций и
// тела сносок за потоком чтения.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <d2d1_1.h>
#include <dwrite.h>
#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>

// Пакет FB3 пробы — у слоя FB3: формат его, и описан он там один раз. Он
// тянет только стандартные заголовки и потому стоит до заголовков вёрстки.
#include "test_package.h"

// Заголовки вёрстки после всех стандартных: они ведут к импорту модуля книги,
// а стандартный заголовок после импорта MSVC уже не принимает.
#include "bukvitsa/typography/block.h"
#include "bukvitsa/typography/formula.h"
#include "bukvitsa/typography/glyph_painter.h"
#include "bukvitsa/typography/hyphenation.h"
#include "bukvitsa/typography/layout.h"
#include "bukvitsa/typography/note.h"
#include "bukvitsa/typography/page.h"

import wxl.core;
import bukvitsa.fb3;
import bukvitsa.mathml;

using namespace bukvitsa;

namespace {

/// Цвет для Direct2D записью `rgb`, как во всём дереве: такую запись редактор
/// узнаёт и ставит образец цвета.
constexpr D2D1_COLOR_F rgb(int r, int g, int b) {
    return {r / 255.0f, g / 255.0f, b / 255.0f, 1.0f};
}

int failures = 0;

void check(bool condition, std::string_view what) {
    std::printf("%s %.*s\n", condition ? "  ok  " : "FAILED", static_cast<int>(what.size()),
                what.data());
    if (!condition) ++failures;
}

const char* nameOf(typography::BlockKind kind) {
    switch (kind) {
    case typography::BlockKind::Paragraph:    return "абзац";
    case typography::BlockKind::Title:        return "заголовок";
    case typography::BlockKind::Subtitle:     return "подзаголовок";
    case typography::BlockKind::Epigraph:     return "эпиграф";
    case typography::BlockKind::Annotation:   return "аннотация";
    case typography::BlockKind::Verse:        return "стих";
    case typography::BlockKind::Preformatted: return "преформат";
    case typography::BlockKind::Quote:        return "цитата";
    case typography::BlockKind::Subscription: return "подпись";
    case typography::BlockKind::ListItem:     return "пункт";
    case typography::BlockKind::Image:        return "картинка";
    case typography::BlockKind::Separator:    return "разделитель";
    }
    return "?";
}

/// UTF-16 обратно в UTF-8 — только чтобы напечатать в консоль. Сюда идут и
/// пути из файловой системы, поэтому текст чинится: сломанное место выходит
/// знаком U+FFFD, видным в выводе.
std::string toUtf8(std::wstring_view text) {
    return std::string(unicode::repaired(text).to_utf8().chars());
}

/// Хранилище из одного абзаца — то, что дала бы книга из одного `<p>`: текст,
/// один прогон стиля на весь текст и один прогон позиций с нуля, то есть
/// позиция идёт вровень с индексом. Прогон стиля есть и у абзаца без
/// разметки: прогоны — разбиение текста, а не только отличия от умолчания.
typography::BookText paragraphOf(u16_view text, typography::FontStyle style = {}) {
    typography::BookText book;
    book.text.append(text);

    const auto length = static_cast<uint32_t>(text.size());
    typography::Block block;
    block.paragraph.text = {0, length};
    if (length > 0) {
        book.spans.push_back(typography::StyleSpan{0, length, style});
        book.offsets.push_back(typography::OffsetSpan{0, 0});
        block.paragraph.spans = {0, 1};
        block.paragraph.offsets = {0, 1};
    }

    book.blocks.push_back(block);
    book.flowCount = 1;
    return book;
}

/// Единственный абзац хранилища, построенного paragraphOf.
const typography::Paragraph& onlyParagraph(const typography::BookText& book) {
    return book.blocks.front().paragraph;
}

/// Вёрстка хранилища из одного абзаца. Строки в хранилище не смотрят, так
/// что оно может быть и временным.
sta_vector<typography::Line> layoutOf(typography::Engine& engine, const typography::BookText& book,
                                      float width, const typography::ParagraphStyle& style) {
    return engine.layout(book, onlyParagraph(book), width, style);
}

void testLayout(typography::Engine& engine, const typography::BookText& book, float width);
void showFirstLines(typography::Engine& engine, const typography::BookText& book, float width);
void testScaledShaping(typography::Engine& engine, const typography::BookText& book, float width);
void testPagination(typography::Engine& engine, const typography::BookText& book,
                    uint32_t characterCount);
void testChunkedPagination(typography::Engine& engine, const typography::BookText& book,
                           uint32_t characterCount);
void testEagerPagination(typography::Engine& engine, const typography::BookText& book,
                         uint32_t characterCount);
void testChapterFirstPage(typography::Engine& engine, const typography::BookText& book,
                          uint32_t characterCount);
void testSeparatorAtPageBottom(typography::Engine& engine);
void testClustersStayWhole(typography::Engine& engine);
void testHyphenation();
void testHyphenationRules();
void testHyphenationInLayout(typography::Engine& engine);

/// Формулы: MicroTeX с бэкендом Direct2D/DirectWrite. Стек проверяется
/// насквозь — разбор, метрики и настоящая растеризация в битмап WIC: пустая
/// картинка значила бы, что глифы или трансформации потерялись по дороге.
void testFormulas(IDWriteFactory* dwrite) {
    std::printf("\n=== формулы (MicroTeX) ===\n");

    typography::FormulaEngine formulas(dwrite, BUKVITSA_MICROTEX_RES);

    const std::unique_ptr<typography::Formula> emc =
        formulas.parse(L"E = mc^2", 20.0f, 600.0f);
    check(emc != nullptr, "E=mc^2 разобрана");
    if (!emc) return;

    std::printf("       ширина %.1f, высота %.1f, базовая линия %.1f\n", emc->width(),
                emc->height(), emc->baseline());
    check(emc->width() > 20.0f && emc->height() > 10.0f, "размеры формулы осмысленные");
    // У E=mc^2 нет свесов под линию, поэтому линия совпадает с низом бокса.
    check(emc->baseline() > 0.0f && emc->baseline() <= emc->height(),
          "базовая линия внутри бокса");

    const std::unique_ptr<typography::Formula> fraction = formulas.parse(
        L"\\frac{-b \\pm \\sqrt{b^2-4ac}}{2a} + \\int_0^\\infty e^{-x^2}\\,dx", 20.0f, 600.0f);
    check(fraction != nullptr, "дробь с корнем и интегралом разобрана");
    if (fraction) {
        // А у дроби свес есть: знаменатель ниже линии, и линия строго внутри.
        check(fraction->baseline() > 0.0f && fraction->baseline() < fraction->height(),
              "у дроби базовая линия строго внутри бокса");
    }

    const std::unique_ptr<typography::Formula> text =
        formulas.parse(L"m — \\text{масса тела}", 20.0f, 600.0f);
    check(text != nullptr, "кириллица в \\text{} разобрана");

    // MicroTeX разбирает частичным парсером и прощает почти всё — оборванные
    // скобки, незнакомые команды и окружения рисуются тем, что он понял.
    // Контракт parse: чужой мусор никогда не выходит наружу исключением.
    bool threw = false;
    try {
        formulas.parse(L"\\frac{оборванная", 20.0f, 600.0f);
        formulas.parse(L"\\nosuchcommand{x} \\begin{nosuch}y\\end{other}", 20.0f, 600.0f);
        formulas.parse(L"}}}{{{ &&& $ \\\\", 20.0f, 600.0f);
    } catch (...) {
        threw = true;
    }
    check(!threw, "мусор не выходит наружу исключением");

    // Знак вне BMP доходит до шрифта одной кодовой точкой. \Textit отдаёт
    // аргумент бэкенду строкой целиком, мимо разбора MicroTeX по wchar_t; ни
    // 📖, ни 字 у Georgia нет, и каждый из них — один глиф .notdef. Пара,
    // разорванная на половины, дала бы два, и формула вышла бы шире.
    {
        const std::unique_ptr<typography::Formula> astral =
            formulas.parse(L"\\Textit{\U0001F4D6}", 20.0f, 600.0f);
        const std::unique_ptr<typography::Formula> one =
            formulas.parse(L"\\Textit{\u5B57}", 20.0f, 600.0f);
        const std::unique_ptr<typography::Formula> two =
            formulas.parse(L"\\Textit{\u5B57\u5B57}", 20.0f, 600.0f);
        check(astral && one && two, "\\Textit со знаком вне BMP разобран");
        if (astral && one && two) {
            std::printf("       ширина: 📖 %.2f, 字 %.2f, 字字 %.2f\n", astral->width(),
                        one->width(), two->width());
            check(two->width() > one->width() + 1.0f,
                  "у .notdef есть ширина: сравнение что-то значит");
            check(std::fabs(astral->width() - one->width()) < 0.01f,
                  "знак вне BMP — один глиф, пара не разорвана");
        }
    }

    // Вся цепочка EPUB: MathML → TeX → MicroTeX. Формула квадратного
    // уравнения в том виде, в каком её пишут конвертеры издателей.
    {
        const std::optional<u8_view> mathml = unicode::checked(
            "<math display=\"block\"><mi>x</mi><mo>=</mo><mfrac>"
            "<mrow><mo>\xE2\x88\x92</mo><mi>b</mi><mo>\xC2\xB1</mo><msqrt>"
            "<msup><mi>b</mi><mn>2</mn></msup><mo>\xE2\x88\x92</mo><mn>4</mn><mi>a</mi>"
            "<mi>c</mi></msqrt></mrow><mrow><mn>2</mn><mi>a</mi></mrow></mfrac></math>");
        const std::optional<mathml::TexFormula> tex =
            mathml ? mathml::toTex(*mathml) : std::nullopt;
        check(tex.has_value() && tex->display, "MathML переведена в TeX, display распознан");
        if (tex) {
            const std::unique_ptr<typography::Formula> converted =
                formulas.parse(tex->tex, 20.0f, 600.0f);
            check(converted && converted->width() > 30.0f &&
                      converted->baseline() < converted->height(),
                  "переведённая формула разобрана MicroTeX и имеет свес дроби");
        }
    }

    // Растеризация: формула в битмап через тот же ID2D1DeviceContext, каким
    // рисуется страница. COM уже поднят STA-пулом; парного CoUninitialize
    // здесь нет и не должно быть — он валил бы COM под ногами остальных
    // тестов (Packaging API у книг), и падение было ровно таким.
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    Microsoft::WRL::ComPtr<IWICImagingFactory> wic;
    Microsoft::WRL::ComPtr<ID2D1Factory1> d2d;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&wic))) ||
        FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                 __uuidof(ID2D1Factory1), nullptr, &d2d))) {
        check(false, "фабрики WIC и Direct2D");
        return;
    }

    const UINT width = static_cast<UINT>(emc->width()) + 8;
    const UINT height = static_cast<UINT>(emc->height()) + 8;
    Microsoft::WRL::ComPtr<IWICBitmap> bitmap;
    wic->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnDemand,
                      &bitmap);

    Microsoft::WRL::ComPtr<ID2D1RenderTarget> target;
    const D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(d2d->CreateWicBitmapRenderTarget(bitmap.Get(), properties, &target))) {
        check(false, "цель отрисовки поверх WIC");
        return;
    }

    Microsoft::WRL::ComPtr<ID2D1DeviceContext> context;
    target.As(&context);

    context->BeginDraw();
    context->Clear(rgb(255, 255, 255));
    emc->draw(context.Get(), 4.0f, 4.0f);
    check(SUCCEEDED(context->EndDraw()), "отрисовка завершилась");

    // Не белые пиксели — доказательство, что глифы дошли до битмапа.
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
    bitmap->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data());
    size_t inked = 0;
    for (size_t i = 0; i < pixels.size(); i += 4) {
        if (pixels[i] != 0xFF || pixels[i + 1] != 0xFF || pixels[i + 2] != 0xFF) ++inked;
    }
    std::printf("       закрашено пикселей: %zu из %u\n", inked, width * height);
    check(inked > 50, "формула оставила след на битмапе");

    // Диагностический крючок: снимок битмапа в PNG, когда просят глазами.
    char shot[MAX_PATH]{};
    if (GetEnvironmentVariableA("BUKVITSA_FORMULA_SHOT", shot, MAX_PATH) > 0) {
        Microsoft::WRL::ComPtr<IWICStream> stream;
        Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
        Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
        wic->CreateStream(&stream);
        std::wstring wide(shot, shot + std::strlen(shot));
        if (SUCCEEDED(stream->InitializeFromFilename(wide.c_str(), GENERIC_WRITE)) &&
            SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
            SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
            SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
            SUCCEEDED(frame->WriteSource(bitmap.Get(), nullptr)) && SUCCEEDED(frame->Commit()) &&
            SUCCEEDED(encoder->Commit())) {
            std::printf("       снимок: %s\n", shot);
        }
    }
}

void testBook(typography::Engine& engine, const std::filesystem::path& path) {
    std::printf("\n=== %s ===\n", path.filename().string().c_str());

    const fb3::Document document(path);
    const typography::BookText book = typography::flatten(document);
    const std::span<const typography::Block> blocks = book.flow();

    std::printf("  блоков: %zu, из них в потоке чтения %zu\n", book.blocks.size(), blocks.size());
    check(!blocks.empty(), "книга развёрнута в блоки");

    // Блоки идут в порядке чтения: позиция каждого следующего не меньше.
    bool ordered = true;
    for (size_t i = 1; i < blocks.size(); ++i)
        if (blocks[i].charOffset < blocks[i - 1].charOffset) ordered = false;
    check(ordered, "блоки в порядке чтения");

    // Прогоны стиля покрывают текст целиком и не пересекаются: на этом стоит
    // весь шейпинг, который дальше режет абзац по ним. Тела сносок верстаются
    // тем же движком, поэтому проверяются все блоки, а не только поток.
    //
    // Прогоны позиций — по возрастанию начала, первый с нуля, последний внутри
    // текста: на этом стоит позиция каждого символа (charOffsetAt).
    bool spansCoverText = true;
    bool offsetsInOrder = true;
    for (const typography::Block& block : book.blocks) {
        const typography::Paragraph& paragraph = block.paragraph;

        uint32_t at = 0;
        for (const typography::StyleSpan& span : book.spansOf(paragraph)) {
            if (span.start != at) spansCoverText = false;
            at = span.start + span.length;
        }
        if (at != paragraph.text.count) spansCoverText = false;

        if (paragraph.text.count == 0) continue;

        const std::span<const typography::OffsetSpan> runs = book.offsetsOf(paragraph);
        if (runs.empty() || runs.front().start != 0 || runs.back().start >= paragraph.text.count)
            offsetsInOrder = false;
        for (size_t i = 1; i < runs.size(); ++i)
            if (runs[i].start <= runs[i - 1].start) offsetsInOrder = false;
    }
    check(spansCoverText, "прогоны стиля покрывают текст без дыр и перекрытий");
    check(offsetsInOrder, "прогоны позиций идут по порядку с начала абзаца и не выходят за текст");

    // Свёртка пробелов: ни двух подряд, ни пробела по краям — кроме
    // преформата, где значимо всё.
    bool collapsed = true;
    for (const typography::Block& block : book.blocks) {
        if (block.kind == typography::BlockKind::Preformatted) continue;
        const std::wstring_view text = book.textOf(block.paragraph).wchars();
        if (!text.empty() && (text.front() == L' ' || text.back() == L' ')) collapsed = false;
        if (text.find(L"  ") != std::wstring::npos) collapsed = false;
    }
    check(collapsed, "пробелы свёрнуты");

    std::printf("  начало книги:\n");
    for (size_t i = 0; i < blocks.size() && i < 6; ++i) {
        const typography::Block& block = blocks[i];
        // Обрезка по букве, а не по байтам UTF-8: `%.100s` резал букву пополам
        // прямо в выводе теста.
        const std::wstring_view whole = book.textOf(block.paragraph).wchars();
        const size_t cut = unicode::floor_grapheme_boundary(whole, 50);
        const std::string text = toUtf8(whole.substr(0, cut));

        std::string label = nameOf(block.kind);
        if (block.kind == typography::BlockKind::Title) label += std::to_string(block.level);

        std::printf("    [%s @%u] %s%s\n", label.c_str(), block.charOffset, text.c_str(),
                    cut < whole.size() ? "..." : "");
    }

    // Ширина полосы книжного разворота при кегле 20: около 60 знаков в строке,
    // как в бумажной книге. Вторая ширина — узкое окно, где абзац разбивается
    // куда чаще и вылезают все ошибки нарезки.
    testLayout(engine, book, 620.0f);
    testLayout(engine, book, 260.0f);
    showFirstLines(engine, book, 620.0f);
    testPagination(engine, book, document.characterCount());
    testChunkedPagination(engine, book, document.characterCount());
    testEagerPagination(engine, book, document.characterCount());
    testChapterFirstPage(engine, book, document.characterCount());
    testScaledShaping(engine, book, 620.0f);
}

/// Стиль блока — та же таблица, по которой потом верстает читалка. Здесь она
/// в тесте затем, чтобы проверять вёрстку на всех видах блоков сразу.
typography::ParagraphStyle styleFor(const typography::Block& block, float base) {
    typography::ParagraphStyle style;
    style.fontSize = base;
    style.lineHeight = 1.45f;

    switch (block.kind) {
    case typography::BlockKind::Title:
        style.fontSize = base * (block.level <= 1 ? 1.6f : 1.3f);
        style.alignment = typography::Alignment::Center;
        style.bold = true;
        break;
    case typography::BlockKind::Subtitle:
        style.alignment = typography::Alignment::Center;
        style.bold = true;
        break;
    case typography::BlockKind::Epigraph:
    case typography::BlockKind::Annotation:
        style.fontSize = base * 0.9f;
        style.italic = true;
        break;
    case typography::BlockKind::Verse:
        style.alignment = typography::Alignment::Left;
        break;
    case typography::BlockKind::Preformatted:
        style.alignment = typography::Alignment::Left;
        style.monospace = true;
        break;
    case typography::BlockKind::Subscription:
        style.alignment = typography::Alignment::Right;
        style.italic = true;
        break;
    default:
        style.firstLineIndent = base * 1.5f;
        break;
    }

    return style;
}

/// Вёрстка всей книги на заданной ширине полосы. Проверяются те свойства, без
/// которых страница не построится: строки не шире полосы, идут по порядку и
/// покрывают текст, у каждой есть высота.
void testLayout(typography::Engine& engine, const typography::BookText& book, float width) {
    const std::span<const typography::Block> blocks = book.flow();
    const auto started = std::chrono::steady_clock::now();

    size_t lineCount = 0;
    size_t glyphCount = 0;
    bool inOrder = true;
    bool haveHeight = true;

    float worstOverflow = 0.0f;
    std::wstring worstLine;
    const char* worstKind = "";
    size_t worstRuns = 0, worstBlockLength = 0, worstSpans = 0;
    uint32_t worstStart = 0;
    std::wstring worstNeighbours;

    for (const typography::Block& block : blocks) {
        if (block.paragraph.text.count == 0)
            continue;

        const typography::ParagraphStyle style = styleFor(block, 20.0f);
        const auto lines = engine.layout(book, block.paragraph, width, style);
        const std::wstring_view text = book.textOf(block.paragraph).wchars();

        uint32_t reached = 0;
        for (const typography::Line& line : lines) {
            ++lineCount;
            for (const typography::GlyphRun& run : line.runs)
                glyphCount += run.glyphIndices.size();

            // Полторы десятых доли пикселя запаса: выключка по формату
            // распределяет добавку в float и вправе промахнуться на последний
            // разряд.
            if (line.width > width + 0.15f && line.width - width > worstOverflow) {
                worstOverflow = line.width - width;
                worstLine = text.substr(line.textStart, line.textLength);
                worstStart = line.textStart;
                worstNeighbours = text.substr(line.textStart >= 6 ? line.textStart - 6 : 0,
                                              line.textLength + 12);
                worstKind = nameOf(block.kind);
                worstRuns = line.runs.size();
                worstBlockLength = block.paragraph.text.count;
                worstSpans = block.paragraph.spans.count;
            }
            if (line.textStart < reached) inOrder = false;
            if (line.height <= 0.0f) haveHeight = false;

            reached = line.textStart + line.textLength;
        }

        if (reached > block.paragraph.text.count) inOrder = false;
    }

    const auto elapsed = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - started).count();

    std::printf("  вёрстка полосы %.0f: %zu строк, %zu глифов, %.0f мс\n", width, lineCount,
                glyphCount, elapsed);

    check(lineCount > 0, "книга разбита на строки");

    // Сноски. Знак сноски — это то, по чему читатель щёлкает, поэтому он
    // обязан существовать (в FB3 <note> сплошь и рядом пуст, и знак ставит
    // вёрстка), обязан попасть на строку и обязан вести к телу сноски.
    size_t anchors = 0, placed = 0, resolved = 0;
    std::wstring firstMarker;

    for (const typography::Block& block : blocks) {
        const std::span<const typography::NoteAnchor> notes = book.notesOf(block.paragraph);
        anchors += notes.size();

        if (firstMarker.empty() && !notes.empty())
            firstMarker = book.textOf(block.paragraph)
                              .wchars()
                              .substr(notes.front().position, notes.front().length);

        const typography::ParagraphStyle style = styleFor(block, 20.0f);
        for (const typography::Line& line : engine.layout(book, block.paragraph, width, style)) {
            placed += line.notes.size();
            for (const typography::PlacedNote& note : line.notes)
                if (note.body < book.noteBodies.size() && book.noteBodies[note.body].count > 0)
                    ++resolved;
        }
    }

    if (anchors > 0) {
        std::printf("  сносок: %zu, на строках: %zu, с телом: %zu, первый знак: «%.40s»\n", anchors,
                    placed, resolved, toUtf8(firstMarker).c_str());
        // Не «столько же»: знак, который автор написал сам, бывает длиной в
        // целый абзац и тогда достаётся каждой строке, через которую прошёл, —
        // щёлкнуть по нему можно в любой из них.
        check(placed >= anchors, "каждый знак сноски попал на строку");
        check(resolved == placed, "каждый знак сноски ведёт к телу");
    }

    // Ни одна строка не шире полосы — на этом стоит вся страница. Даже слово,
    // которое не разрывается нигде, вёрстка обязана разбить: строка, вылезшая
    // за поле, не только некрасива, но и не поместится в окно.
    if (worstOverflow > 0.0f)
        std::printf("    шире полосы на %.1f [%s, прогонов %zu, блок %zu символов, %zu стилей,\n"
                    "      строка с %u длиной %zu]:\n"
                    "      строка:  |%s|\n"
                    "      с краями: %s\n",
                    worstOverflow, worstKind, worstRuns, worstBlockLength, worstSpans, worstStart,
                    worstLine.size(), toUtf8(worstLine).c_str(), toUtf8(worstNeighbours).c_str());

    check(worstOverflow == 0.0f, "ни одна строка не шире полосы");
    check(inOrder, "строки идут по порядку и не выходят за абзац");
    check(haveHeight, "у каждой строки есть высота");
}

/// Первые строки книги как текст — самый прямой способ увидеть, что вёрстка
/// разбила абзац там, где надо.
void showFirstLines(typography::Engine& engine, const typography::BookText& book, float width) {
    int shown = 0;

    for (const typography::Block& block : book.flow()) {
        if (block.paragraph.text.count < 200 || shown > 0)
            continue;

        const typography::ParagraphStyle style = styleFor(block, 20.0f);
        const auto lines = engine.layout(book, block.paragraph, width, style);

        std::printf("  абзац на полосе %.0f (отступ %.0f):\n", width, style.firstLineIndent);
        for (size_t i = 0; i < lines.size() && i < 8; ++i) {
            const typography::Line& line = lines[i];
            const std::wstring text(
                book.textOf(block.paragraph).wchars().substr(line.textStart, line.textLength));
            std::printf("    %6.1f |%s\n", line.width, toUtf8(text).c_str());
        }
        ++shown;
    }
}

/// Пагинация: раскладка строк по страницам и поиск страницы по позиции чтения.
///
/// Меряется отдельно от вёрстки не из любопытства: `setStyle` — это то, что
/// происходит на каждое нажатие Ctrl+«+», и вопрос «почему смена кегля
/// подтормаживает» решается тем, какая доля этого времени приходится на
/// шейпинг, а какая на саму раскладку.
void testPagination(typography::Engine& engine, const typography::BookText& book,
                    uint32_t characterCount) {
    const std::span<const typography::Block> blocks = book.flow();

    typography::PageStyle style;
    style.width = 620.0f;
    style.height = 800.0f;
    style.fontSize = 20.0f;

    const auto started = std::chrono::steady_clock::now();
    typography::Chapter paginator(engine, book, blocks, characterCount);
    paginator.setStyle(style);
    const auto firstElapsed = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - started).count();

    // Второй setStyle с другим кеглем — ровно то, что делает Ctrl+«+».
    style.fontSize = 21.0f;
    const auto restarted = std::chrono::steady_clock::now();
    paginator.setStyle(style);
    const auto againElapsed = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - restarted).count();

    std::printf("  пагинация: %zu страниц, %.0f мс; смена кегля: %.0f мс\n",
                paginator.pageCount(), firstElapsed, againElapsed);

    // Среднее на абзац: вся книга накоплена и свёрстана разом (setStyle),
    // делим на число блоков. Первый прогон — полная пагинация, с шейпингом;
    // второй (смена кегля) — только разбивка на строки. Счётчик рядом с
    // числом, чтобы видеть, что мерилось не пустое место.
    std::printf("  абзацев: %zu; на абзац: %.1f мкс полная, %.1f мкс перевёрстка\n",
                blocks.size(), firstElapsed * 1000.0 / static_cast<double>(blocks.size()),
                againElapsed * 1000.0 / static_cast<double>(blocks.size()));

    check(paginator.pageCount() > 0, "книга разложена по страницам");

    // Страницы идут по порядку и не теряют текст между собой: разрыв здесь
    // означал бы, что читатель, листая, пропустил кусок книги.
    bool ordered = true;
    bool nonEmpty = true;
    for (size_t i = 0; i < paginator.pageCount(); ++i) {
        const typography::Page& page = paginator.page(i);
        if (page.lines.empty() && page.images.empty()) nonEmpty = false;
        if (i > 0 && page.firstCharOffset < paginator.page(i - 1).firstCharOffset) ordered = false;
    }
    check(ordered, "страницы идут по порядку");
    check(nonEmpty, "пустых страниц нет");

    // Обратный переход: по позиции чтения находится страница, на которой эта
    // позиция и лежит. На этом стоит сохранение места при смене кегля.
    bool roundTrip = true;
    for (size_t i = 0; i < paginator.pageCount(); ++i) {
        const typography::Page& page = paginator.page(i);
        if (paginator.pageForCharOffset(page.firstCharOffset) != i) roundTrip = false;
    }
    check(roundTrip, "по позиции чтения находится своя страница");

    // Каждая глава верхнего уровня начинается с новой страницы: символ, с
    // которого начинается её первый блок, обязан быть началом какой-то
    // страницы, а не серединой чужой. Подсекции (startsSection > 1) сюда не
    // входят — они текут внутри своей главы.
    size_t chapters = 0;
    size_t chaptersChecked = 0;
    bool chaptersStartPages = true;
    for (const typography::Block& block : blocks) {
        if (block.startsSection != 1) continue;
        ++chapters;

        // Главу, начинающуюся не текстом (картинкой, разделителем), этот тест
        // проверить не может: пагинатору здесь не дан размер картинок, и
        // картинка на полосу ничего не кладёт, а значит и страницы собой не
        // начинает. В самой читалке размер есть — там такая глава страницу
        // начинает; здесь ограничиваемся текстовыми началами, а их
        // большинство.
        if (block.paragraph.text.count == 0) continue;
        ++chaptersChecked;

        // Начало страницы — это первый поставленный на неё символ, а его
        // позиция у текстового блока — из прогонов позиций (после свёрнутых
        // пробелов), а не charOffset узла: у заголовка с отбивкой перед текстом
        // они разные.
        const uint32_t firstChar = book.charOffsetAt(block.paragraph, 0);
        const size_t at = paginator.pageForCharOffset(firstChar);
        if (paginator.page(at).firstCharOffset != firstChar) chaptersStartPages = false;
    }
    std::printf("  глав верхнего уровня: %zu (текстовых проверено: %zu)\n", chapters, chaptersChecked);
    check(chaptersStartPages, "каждая глава начинается с новой страницы");

    // Блоки, отданные наружу: на них стоят оглавление, поиск и переход по
    // закладке. Проверяется, что это те же блоки, что верстались, и что
    // позиции в них не убывают -- то, на что опирается поиск места по книге.
    const std::span<const typography::Block> exposed = paginator.blocks();
    bool offsetsRise = true;
    for (size_t i = 1; i < exposed.size(); ++i) {
        if (exposed[i].charOffset < exposed[i - 1].charOffset) offsetsRise = false;
    }
    check(exposed.size() == blocks.size(), "пагинатор отдаёт те же блоки, из которых верстал");
    check(offsetsRise, "позиции блоков идут по возрастанию");
}

/// Порционная вёрстка: та же книга, разложенная не разом, а порциями.
///
/// Проверяется главное обещание порций: страницы выходят те же самые, а уже
/// набранные не переписываются задним числом. На втором стоит то, ради чего
/// порции и заведены, — читалка показывает страницу, не дожидаясь конца
/// счёта, и показанное потом не должно оказаться неправдой.
void testChunkedPagination(typography::Engine& engine, const typography::BookText& book,
                           uint32_t characterCount) {
    typography::PageStyle style;
    style.width = 620.0f;
    style.height = 800.0f;
    style.fontSize = 20.0f;

    typography::Chapter paginator(engine, book, book.flow(), characterCount);
    paginator.setStyle(style);

    // Снимок целой вёрстки. Именно снимок, а не ссылки: следующая вёрстка
    // перевернёт под ними страницы.
    struct Snapshot {
        uint32_t first;
        uint32_t last;
        size_t lines;
        size_t images;
    };
    std::vector<Snapshot> whole;
    for (size_t i = 0; i < paginator.pageCount(); ++i) {
        const typography::Page& page = paginator.page(i);
        whole.push_back({page.firstCharOffset, page.lastCharOffset, page.lines.size(),
                         page.images.size()});
    }

    // Совпала ли набранная порциями книга с той, что вышла разом.
    const auto matches = [&] {
        if (paginator.pageCount() != whole.size()) return false;
        for (size_t i = 0; i < whole.size(); ++i) {
            const typography::Page& page = paginator.page(i);
            if (page.firstCharOffset != whole[i].first || page.lastCharOffset != whole[i].last ||
                page.lines.size() != whole[i].lines || page.images.size() != whole[i].images)
                return false;
        }
        return true;
    };

    // Всё, что набрано к этой минуте, обязано совпадать с целой вёрсткой:
    // набранная страница окончательна и задним числом не переписывается.
    bool settled = true;
    bool grows = true;
    size_t seen = 0;
    const auto watch = [&] {
        if (paginator.pageCount() < seen) grows = false;
        for (size_t i = 0; i < seen && i < whole.size(); ++i) {
            const typography::Page& page = paginator.page(i);
            if (page.firstCharOffset != whole[i].first || page.lines.size() != whole[i].lines)
                settled = false;
        }
        seen = paginator.pageCount();
    };

    // Порция в ноль: срок вышел ещё до начала, значит каждая считает ровно
    // один блок. Так меряется то, чем порция может превысить свой срок, —
    // цена самого дорогого блока книги; разорвать его нечем.
    double worstBlock = 0.0;
    paginator.beginLayout(style);
    check(!paginator.isComplete() || book.flow().empty(), "начатая вёрстка ещё не досчитана");

    while (true) {
        const auto started = std::chrono::steady_clock::now();
        const bool more = paginator.advance(std::chrono::milliseconds(0));
        worstBlock = std::max(worstBlock, std::chrono::duration<double, std::milli>(
                                              std::chrono::steady_clock::now() - started).count());
        watch();
        if (!more) break;
    }
    check(matches(), "книга, свёрстанная по блоку за раз, — та же самая");

    // А теперь порция та, что берёт читалка.
    const auto slice = std::chrono::milliseconds(50);
    double worstChunk = 0.0;
    double wholeElapsed = 0.0;
    int chunks = 0;
    seen = 0;
    paginator.beginLayout(style);

    while (true) {
        const auto started = std::chrono::steady_clock::now();
        const bool more = paginator.advance(slice);
        const double elapsed = std::chrono::duration<double, std::milli>(
                                   std::chrono::steady_clock::now() - started).count();
        worstChunk = std::max(worstChunk, elapsed);
        wholeElapsed += elapsed;
        ++chunks;
        watch();
        if (!more) break;
    }

    std::printf("  порциями: %d порций по 50 мс, худшая %.0f мс из %.0f, дороже всех блок %.0f\n",
                chunks, worstChunk, wholeElapsed, worstBlock);

    check(paginator.isComplete(), "книга досчитана");
    check(grows, "число страниц только растёт");
    check(settled, "набранная страница больше не меняется");
    check(matches(), "порциями выходит ровно та же книга, что и разом");

    // Обещание порции: она кончается на первом блоке после срока. Проверяется
    // это числом порций — без срока порция была бы одна на всю книгу, — а не
    // самими миллисекундами: они зависят от сборки и от того, чем ещё занята
    // машина, и сравнивать два замера между собой значило бы ловить чужую
    // загрузку.
    check(chunks > 1 || wholeElapsed < 50.0, "книга, не влезшая в срок, разбита на порции");

    // И грубый предел сверху — на случай, если срок перестанут проверять
    // вовсе: тогда порция стала бы всей книгой.
    check(worstChunk <= 50.0 + worstBlock * 3.0 + 25.0, "порция не растягивается на всю книгу");
}

/// Энергичный досчёт: когда читатель идёт туда, куда фоновые порции ещё не
/// дошли, набор доводится ровно до нужного места и не дальше. На этом стоит
/// листание назад и прыжок по закладке — ждать порций там нечего, но и считать
/// главу целиком незачем.
void testEagerPagination(typography::Engine& engine, const typography::BookText& book,
                         uint32_t characterCount) {
    typography::PageStyle style;
    style.width = 620.0f;
    style.height = 800.0f;
    style.fontSize = 20.0f;

    typography::Chapter paginator(engine, book, book.flow(), characterCount);
    paginator.setStyle(style);
    if (paginator.pageCount() < 8) {
        check(true, "книга слишком коротка для проверки досчёта — пропущено");
        return;
    }

    // Место чтения где-то в середине — так оно и приходит из читалки: первым
    // символом страницы, на которой читатель стоял.
    const uint32_t at = paginator.page(paginator.pageCount() / 2).firstCharOffset;

    typography::Chapter second(engine, book, book.flow(), characterCount);
    second.beginLayout(style);
    const bool more = second.advanceTo(at);
    const size_t reached = second.pageForCharOffset(at);

    check(second.page(reached).firstCharOffset <= at, "досчитано до места чтения");
    check(second.pageCount() > reached, "страница с этим символом окончательна");

    second.advanceToPage(reached + 2);
    check(second.pageCount() > reached + 2, "досчитано и до всего разворота");

    std::printf("  энергичный досчёт: %zu страниц из %zu, книга %s\n", second.pageCount(),
                paginator.pageCount(), more ? "ещё не досчитана" : "досчитана целиком");
}
/// Первая колонка главы, свёрстанной своим пагинатором, не должна быть пустой.
///
/// Читалка режет книгу на главы верхнего уровня и верстает каждую отдельным
/// Chapter. На стыке лента показывает первую колонку следующей главы — и если
/// пагинатор отдаёт её пустой, читатель видит пустую страницу. Тест
/// воспроизводит стык на настоящей книге: берёт главу из середины и проверяет,
/// что её страница 0 непуста и начинается ровно с её первого блока.
void testChapterFirstPage(typography::Engine& engine, const typography::BookText& book,
                          uint32_t characterCount) {
    const std::span<const typography::Block> blocks = book.flow();

    std::vector<size_t> starts;
    for (size_t i = 0; i < blocks.size(); ++i)
        if (i == 0 || blocks[i].startsSection == 1) starts.push_back(i);
    if (starts.empty()) {
        check(true, "нет глав верхнего уровня — пропущено");
        return;
    }

    // Полоса — узкая колонка книжного разворота, как у читалки: там стык и виден.
    typography::PageStyle style;
    style.width = 300.0f;
    style.height = 800.0f;
    style.fontSize = 20.0f;

    // Проходим каждую главу верхнего уровня и ищем ту, у которой первая колонка
    // выходит пустой: читатель увидел бы её на стыке пустой страницей.
    size_t emptyChapters = 0;
    size_t firstEmpty = starts.size();
    for (size_t c = 0; c < starts.size(); ++c) {
        const size_t first = starts[c];
        const size_t last = c + 1 < starts.size() ? starts[c + 1] : blocks.size();

        typography::Chapter chapter(engine, book, blocks.subspan(first, last - first),
                                    characterCount);
        chapter.beginLayout(style);
        chapter.advanceToPage(static_cast<size_t>(-1));

        // Глава без страниц — не беда: лента колонок её перешагивает (секция
        // из одного разделителя). Беда — глава, у которой страницы есть, а
        // первая пуста: её читатель увидит пустой колонкой на стыке.
        if (chapter.pageCount() > 0 && chapter.page(0).lines.empty() &&
            chapter.page(0).images.empty()) {
            if (firstEmpty == starts.size()) firstEmpty = c;
            ++emptyChapters;
        }
    }

    if (emptyChapters > 0) {
        // Показываем первую провинившуюся главу с её блоками — по ним видно, чем
        // она начинается.
        const size_t first = starts[firstEmpty];
        const size_t last =
            firstEmpty + 1 < starts.size() ? starts[firstEmpty + 1] : blocks.size();
        std::printf("\n=== пустая первая колонка: глава %zu из %zu (блоки %zu..%zu) ===\n",
                    firstEmpty, starts.size(), first, last);
        for (size_t i = first; i < last && i < first + 6; ++i) {
            const typography::Block& b = blocks[i];
            std::printf("    [%s @%u sect%u lvl%u] %.70s\n", nameOf(b.kind), b.charOffset,
                        static_cast<unsigned>(b.startsSection), static_cast<unsigned>(b.level),
                        toUtf8(book.textOf(b.paragraph).wchars()).c_str());
        }
    }

    std::printf("  глав верхнего уровня: %zu, с пустой первой колонкой: %zu\n", starts.size(),
                emptyChapters);
    check(emptyChapters == 0, "ни одна глава не начинается пустой колонкой");
}

/// Разделитель, пришедшийся на низ полосы, не должен зацикливать набор.
///
/// Отбивка разделителя занимает место, но страницы не начинает: страница
/// остаётся пустой, а места на ней уже нет. Строка, которая в остаток не
/// влезла, требует новой страницы — а закрывать нечего, и набор до
/// исправления ходил по кругу вечно.
void testSeparatorAtPageBottom(typography::Engine& engine) {
    // Разделитель, за ним абзац: разделитель ничем в массивах не владеет, и
    // встать перед абзацем ему ничто не мешает.
    const u16_view line = u"Строка, которой уже не хватает места на полосе.";
    typography::BookText book = paragraphOf(line);

    typography::Block separator;
    separator.kind = typography::BlockKind::Separator;
    book.blocks.insert(book.blocks.begin(), separator);
    book.flowCount = 2;

    typography::PageStyle style;
    style.width = 400.0f;
    style.fontSize = 20.0f;
    // Полоса ровно в одну строку: отбивка разделителя — полтора кегля — и
    // строка на неё вместе уже не помещаются.
    style.height = 30.0f;

    typography::Chapter paginator(engine, book, book.flow(), static_cast<uint32_t>(line.size()));
    paginator.setStyle(style);

    std::printf("\n=== узкая полоса ===\n");
    check(paginator.pageCount() >= 1, "разделитель у низа полосы не зацикливает набор");
}

/// Книга FB3 из одного тела: остальные части пакета — минимум, который
/// требует формат.
std::string fb3Of(std::string_view body) { return fb3::tests::fb3Package({.body = body}); }

/// Книга FB3 из одного тела и двух картинок: связи rIdA и rIdB ведут к своим
/// частям, а rIdLost — к части, которой в пакете нет. Байты картинок — не
/// картинки: вёрстке они не нужны, ей нужен номер части.
std::string fb3WithImages(std::string_view body) {
    static const std::string bodyRelationships = fb3::tests::relationships(
        R"(<Relationship Id="rIdA" Type="http://www.fictionbook.org/FictionBook3/relationships/image" Target="img/a.png"/>)"
        R"(<Relationship Id="rIdB" Type="http://www.fictionbook.org/FictionBook3/relationships/image" Target="img/b.png"/>)"
        R"(<Relationship Id="rIdLost" Type="http://www.fictionbook.org/FictionBook3/relationships/image" Target="img/lost.png"/>)");
    static constexpr fb3::tests::PartBytes images[] = {{"fb3/img/a.png", "картинка A"}, {"fb3/img/b.png", "картинка B"}};
    return fb3::tests::fb3Package({.body = body, .bodyRelationships = bodyRelationships, .otherParts = images});
}

/// Буквы из нескольких кодовых точек — то, на чём проверяется всякое место,
/// где вёрстка сама выбирает позицию в тексте.
struct CorpusLetter {
    const char* name;
    u16_view text;
    char32_t filler;   ///< буква той же письменности: прогон длиннее ёмкости должен быть одним
    bool symbol = false;   ///< символ, а не буква письменности: слово он кончает
};

constexpr CorpusLetter kLetters[] = {
    {"и + краткая", u"\x0438\x0306", U'\x0436'},
    {"е + диерезис + акут", u"\x0435\x0308\x0301", U'\x0436'},
    {"иероглиф вне BMP", u"\xD842\xDFB7", U'\x4E2D'},
    {"деванагари кша", u"\x0915\x094D\x0937", U'\x0915'},
    {"хангыль L V T", u"\x1100\x1161\x11A8", U'\xAC00'},
    {"флаг", u"\xD83C\xDDEF\xD83C\xDDF5", 0, true},
    {"эмодзи с цветом кожи", u"\xD83D\xDC4D\xD83C\xDFFD", 0, true},
    {"семья через ZWJ", u"\xD83D\xDC68\x200D\xD83D\xDC69\x200D\xD83D\xDC67", 0, true},
};

bool isLetterStart(u16_view text, uint32_t at) {
    return at >= text.size() || unicode::floor_grapheme_boundary(text, at) == at;
}

/// Ни строка, ни прогон глифов не начинаются внутри буквы.
bool linesStartAtLetters(u16_view text, std::span<const typography::Line> lines,
                         const char* what) {
    bool whole = !lines.empty();
    for (const typography::Line& line : lines) {
        if (!isLetterStart(text, line.textStart)) {
            std::printf("       %s: строка начинается внутри буквы на %u\n", what, line.textStart);
            whole = false;
        }
        for (const typography::GlyphRun& run : line.runs) {
            if (!isLetterStart(text, run.textStart)) {
                std::printf("       %s: прогон начинается внутри буквы на %u\n", what, run.textStart);
                whole = false;
            }
        }
    }
    return whole;
}

/// Где у второго глифа строки начало относительно первого: знак, посаженный
/// на букву, стоит там, куда его поставил шрифт, и никуда не уезжает.
float markOffset(const typography::GlyphRun& run) {
    return run.advances[0] + run.offsets[1].advanceOffset - run.offsets[0].advanceOffset;
}

/// Вёрстка не рвёт букву — закон. Буква здесь — графемный кластер Unicode:
/// «й» из «и» и знака краткой, флаг из двух региональных индикаторов, семья
/// эмодзи через ZWJ. Разрез между её кодовыми точками кодировку не портит,
/// а букву рвёт: знак уходит к шейперу без основы и повисает, конъюнкт
/// распадается, пара становится двумя прямоугольниками.
///
/// Места, где вёрстка сама выбирает позицию в тексте: деление прогона
/// длиннее ёмкости кластерной карты, рубка слова шире полосы, разрядка и
/// равномерное сжатие строки, которые двигают глифы.
void testClustersStayWhole(typography::Engine& engine) {
    std::printf("\n=== буква не режется ===\n");

    // Разбор книги: разметка внутри буквы, пробел — основа знака в начале
    // абзаца, сноска без знака между буквой и её знаком.
    {
        const fb3::Document document(fb3Of(R"(<?xml version="1.0" encoding="UTF-8"?>
<fb3-body xmlns="http://www.fictionbook.org/FictionBook3/body" xmlns:xlink="http://www.w3.org/1999/xlink" id="00000000-0000-0000-0000-000000000002">
<section id="s1">
<p>ж&#x438;<em>&#x306;</em>ж</p>
<p> &#x301;ж</p>
<p>ж&#x438;<note href="n1" xlink:role="footnote"/>&#x306;ж</p>
</section>
<notes show="0"><notebody id="n1"><p>сноска</p></notebody></notes>
</fb3-body>)"));
        const typography::BookText book = typography::flatten(document);
        const std::span<const typography::Block> blocks = book.flow();
        check(blocks.size() == 3, "книга из трёх абзацев развёрнута в три блока");

        if (blocks.size() == 3) {
            for (const typography::Block& block : blocks) {
                const u16_view text = book.textOf(block.paragraph);
                for (const typography::StyleSpan& span : book.spansOf(block.paragraph))
                    check(isLetterStart(text, span.start),
                          "прогон стиля начинается с буквы, а не с её знака");
                for (const typography::NoteAnchor& note : book.notesOf(block.paragraph))
                    check(isLetterStart(text, note.position), "знак сноски стоит между буквами");
            }

            check(book.textOf(blocks[0].paragraph) == L"\x0436\x0438\x0306\x0436",
                  "знак, размеченный отдельно, остаётся при своей букве");
            check(book.textOf(blocks[1].paragraph).wchars().starts_with(L" \x0301"),
                  "пробел — основа знака в начале абзаца — не выбрасывается");
            check(book.textOf(blocks[2].paragraph) == L"\x0436\x0438\x0306" L"1" L"\x0436",
                  "знак сноски, поставленный вёрсткой, встаёт после буквы, а не внутри неё");
        }
    }

    typography::ParagraphStyle style;
    style.fontSize = 20.0f;

    // Прогон без пробелов делится на восьмую долю ёмкости раньше её конца —
    // на 3500-й единице. Буква начинается на единицу раньше, и разрез
    // приходится внутрь неё. Филлер — той же письменности, иначе прогон
    // кончился бы сам на смене письменности.
    for (const CorpusLetter& letter : kLetters) {
        if (!letter.filler) continue;

        u16_text text;
        for (int i = 0; i < 3499; ++i) text.push_back(letter.filler);
        text += letter.text;
        for (int i = 0; i < 1000; ++i) text.push_back(letter.filler);

        check(linesStartAtLetters(text, layoutOf(engine, paragraphOf(text), 400.0f, style),
                                  letter.name),
              std::string("длинный прогон делится между буквами: ") + letter.name);
    }

    // Полоса в два кегля рубит слово кусками по полкегля — уже любой буквы.
    u16_text word;
    for (int round = 0; round < 3; ++round)
        for (const CorpusLetter& letter : kLetters)
            word += letter.text;

    check(linesStartAtLetters(word, layoutOf(engine, paragraphOf(word), 40.0f, style), "рубка"),
          "слово шире полосы рубится между буквами");

    // Знаки сажает на букву не всякий шрифт: Georgia, шрифт читалки по
    // умолчанию, рисует знак ударения отдельным глифом со своей шириной.
    // Проверки сдвига знака берут Segoe UI, где знак посажен на букву.
    const typography::TextStyle previous = engine.textStyle();
    typography::TextStyle segoe = previous;
    segoe.fontFamily = L"Segoe UI";
    engine.setTextStyle(segoe);

    // Разрядка раздвигает буквы, а не глифы: знак ударения остаётся над своей
    // буквой.
    typography::ParagraphStyle ragged = style;
    ragged.alignment = typography::Alignment::Left;

    typography::FontStyle spacing;
    spacing.spaced = true;

    const auto plainLines =
        layoutOf(engine, paragraphOf(u"\x0436\x0301\x0436\x0301"), 1000.0f, ragged);
    const auto spacedLines =
        layoutOf(engine, paragraphOf(u"\x0436\x0301\x0436\x0301", spacing), 1000.0f, ragged);
    const bool twoGlyphs = !plainLines.empty() && !spacedLines.empty() &&
                           plainLines[0].runs.size() == 1 && spacedLines[0].runs.size() == 1 &&
                           plainLines[0].runs[0].advances.size() == 4 &&
                           spacedLines[0].runs[0].advances.size() == 4;
    check(twoGlyphs, "«ж» и знак ударения — два глифа, иначе разрядку не на чем проверить");
    if (twoGlyphs) {
        const float before = markOffset(plainLines[0].runs[0]);
        const float after = markOffset(spacedLines[0].runs[0]);
        std::printf("       знак от буквы: %.3f без разрядки, %.3f с разрядкой\n", before, after);
        check(std::abs(before - after) < 0.01f, "разрядка не отодвигает знак от буквы");
    }

    // Сжатие переполненной строки: одна буква со знаком шире полосы на десять
    // процентов — больше, чем DirectWrite сжимает сам, так что остаток
    // снимает равномерное сжатие. Ширины сжимаются, глиф буквы — нет, и знак
    // обязан остаться там, где его посадил шрифт.
    const typography::BookText single = paragraphOf(u"\x0436\x0301");
    const auto natural = layoutOf(engine, single, 1000.0f, ragged);
    const bool attached = !natural.empty() && natural[0].runs.size() == 1 &&
                          natural[0].runs[0].advances.size() == 2 &&
                          natural[0].runs[0].advances[1] == 0.0f;
    check(attached, "в Segoe UI знак ударения посажен на «ж»: глиф без ширины");
    if (attached) {
        const typography::GlyphRun& wide = natural[0].runs[0];
        const auto squeezed = layoutOf(engine, single, wide.advances[0] * 0.9f, style);
        const bool oneRun = squeezed.size() == 1 && squeezed[0].runs.size() == 1 &&
                            squeezed[0].runs[0].advances.size() == 2;
        check(oneRun, "переполненная строка из одной буквы — одна строка из двух глифов");
        if (oneRun) {
            const typography::GlyphRun& narrow = squeezed[0].runs[0];
            std::printf("       ширина буквы %.3f -> %.3f, знак от буквы %.3f -> %.3f\n",
                        wide.advances[0], narrow.advances[0], markOffset(wide), markOffset(narrow));
            check(narrow.advances[0] < wide.advances[0] - 0.5f, "строка и правда сжата");
            check(std::abs(markOffset(narrow) - markOffset(wide)) < 0.01f,
                  "сжатие строки не сдвигает знак с буквы");
        }
    }

    engine.setTextStyle(previous);
}
/// Смена кегля через сохранённый шейпинг должна давать ровно то же, что
/// шейпинг заново на новом кегле.
///
/// Это проверка допущения, на котором стоит весь кэш: метрики DirectWrite в
/// естественном режиме измерения линейны по кеглю, поэтому строку при кегле 26
/// можно получить из шейпинга при кегле 20 умножением. Если допущение неверно,
/// разойдутся переломы — и разойдутся заметно, а не в последнем разряде.
void testScaledShaping(typography::Engine& engine, const typography::BookText& book, float width) {
    size_t compared = 0;
    size_t sameBreaks = 0;
    float worstWidth = 0.0f;

    for (const typography::Block& block : book.flow()) {
        if (block.paragraph.text.count == 0)
            continue;

        const typography::ParagraphStyle atTwenty = styleFor(block, 20.0f);
        const typography::ParagraphStyle atTwentySix = styleFor(block, 26.0f);

        const typography::ShapedParagraphPtr shaped = engine.shape(book, block.paragraph, atTwenty);
        const auto scaled = engine.layout(*shaped, width, atTwentySix);
        const auto fresh = engine.layout(book, block.paragraph, width, atTwentySix);

        ++compared;
        if (scaled.size() != fresh.size())
            continue;

        bool same = true;
        for (size_t i = 0; i < scaled.size(); ++i) {
            if (scaled[i].textStart != fresh[i].textStart ||
                scaled[i].textLength != fresh[i].textLength)
                same = false;
            worstWidth = std::max(worstWidth, std::abs(scaled[i].width - fresh[i].width));
        }
        if (same)
            ++sameBreaks;
    }

    std::printf("  кегль из кэша: %zu абзацев, совпало %zu, худший разброс ширины %.4f\n",
                compared, sameBreaks, worstWidth);

    check(compared == 0 || sameBreaks == compared,
          "вёрстка из сохранённого шейпинга совпадает с вёрсткой заново");
    check(worstWidth < 0.05f, "ширины строк совпадают");
}

/// Слово, ожидание и итог — одной строкой отчёта. С длиной и в UTF-8: `%ls`
/// в локали «C» на кириллице обрывает печать молча.
void reportHyphenation(u8_view word, u8_view expected, u8_view got) {
    std::string line(word.chars());
    line += ": ждали ";
    line += expected.chars();
    line += ", вышло ";
    line += got.chars();
    std::printf("       %.*s\n", static_cast<int>(line.size()), line.data());
}

/// Слово, размеченное переносами, — «до-сто-при-ме-ча-тель-ность».
///
/// Разметку получает только слово из букв алфавита образцов, а там каждая
/// единица UTF-16 — целая кодовая точка из BMP; слово без разметки отдаётся
/// как есть.
u8_text hyphenated(u16_view word) {
    const typography::HyphenPoints points = typography::hyphenate(word);
    if (points == 0) return word.to_utf8();

    const std::u16string_view units = word.plain();
    u8_text marked;
    for (size_t at = 0; at < units.size(); ++at) {
        if ((points >> at) & 1) marked.push_back(U'-');
        marked.push_back(static_cast<char32_t>(units[at]));
    }
    return marked;
}

bool hyphenatedIs(u16_view word, u8_view expected) {
    const u8_text marked = hyphenated(word);
    if (marked.chars() == expected.chars()) return true;

    reportHyphenation(word.to_utf8(), expected, marked);
    return false;
}

/// Переносы по образцам Лянга.
///
/// Главная проверка — корпус: слова книг из FB3/testdata с переносами,
/// посчитанными независимой реализацией того же алгоритма прямо по образцам
/// (Typography/tools/gen-hyphen-patterns.py). Движок обязан совпасть с ней
/// слово в слово: любое расхождение значит ошибку укладки таблицы или её
/// обхода, а не разночтение образцов.
void testHyphenation() {
    std::printf("\n=== переносы ===\n");

    // Слова, ради которых образцы и выбраны: правило «гласная — согласная —
    // гласная» разорвало бы их по-другому и неверно.
    check(hyphenatedIs(u"достопримечательность", u8"до-сто-при-ме-ча-тель-ность"),
          "достопримечательность");
    check(hyphenatedIs(u"подъезд", u8"подъ-езд"), "приставка не отрывается от ъ");
    check(hyphenatedIs(u"разбить", u8"раз-бить"), "морфемная граница");
    check(hyphenatedIs(u"ванна", u8"ван-на"), "удвоенная согласная");
    check(hyphenatedIs(u"асбест", u8"ас-бест"), "слово из списка исключений");
    check(hyphenatedIs(u"associate", u8"as-so-ciate"), "английское исключение");
    check(hyphenatedIs(u"hyphenation", u8"hy-phen-ation"), "английское слово");

    check(hyphenatedIs(u"мама", u8"ма-ма"), "две буквы с каждой стороны — можно");
    check(typography::hyphenate(u"оса") == 0, "одну букву не отрывают");
    check(typography::hyphenate(u"под") == 0, "трёх букв мало");

    // Слово, где есть хоть что-то помимо букв алфавита, образцы не размечают
    // вовсе — и это то, отчего их место не может попасть внутрь буквы. Знаки
    // над буквами снимает со слова hyphenPointsOf (testHyphenationRules), и
    // сюда оно приходит уже из одних основ.
    check(typography::hyphenate(u"досто\x0301примечательность") == 0, "слово со знаком ударения");
    check(typography::hyphenate(u"страница5") == 0, "слово с цифрой");
    check(typography::hyphenate(u"привет\xD83D\xDC4D") == 0, "слово с эмодзи");
    check(typography::hyphenate(u"") == 0, "пустое слово");

    for (const CorpusLetter& letter : kLetters) {
        u16_text word;
        word += u"досто";
        word += letter.text;
        word += u"примечательность";
        check(typography::hyphenate(word) == 0, letter.name);
    }

    // Корпус.
    const std::filesystem::path corpus =
        std::filesystem::path{BUKVITSA_TYPOGRAPHY_DATA_DIR} / "hyphen_corpus.txt";

    std::ifstream file(corpus);
    if (!file) {
        const std::string path = toUtf8(corpus.native());
        std::printf("FAILED не открылся корпус %.*s\n", static_cast<int>(path.size()), path.data());
        ++failures;
        return;
    }

    size_t words = 0, wrong = 0;
    for (std::string line; std::getline(file, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        ++words;
        const auto expected = unicode::checked(line);
        if (!expected) {
            std::printf("FAILED строка корпуса %zu — не UTF-8\n", words);
            ++wrong;
            continue;
        }

        u16_text word;
        for (const char32_t code : unicode::code_points(*expected))
            if (code != U'-') word.push_back(code);

        const u8_text got = hyphenated(word);
        if (got.chars() != expected->chars() && ++wrong <= 10)
            reportHyphenation(word.to_utf8(), *expected, got);
    }

    std::printf("  корпус: %zu слов\n", words);
    check(words > 1000, "корпус не пуст");
    check(wrong == 0, "каждое слово корпуса размечено как эталон");
}

/// Абзац, размеченный так, как места переноса получит разбивка: «до|сто|при».
/// Черта, а не дефис: дефисы бывают в самом тексте, и место переноса рядом с
/// ними не должно с ними путаться.
u8_text markedParagraph(u16_view text, std::span<const typography::NoteAnchor> notes) {
    const sta_vector<uint8_t> hyphens = typography::hyphenPointsOf(text, notes);

    u8_text marked;
    size_t at = 0;
    for (const char32_t code : unicode::code_points(text)) {
        if (hyphens[at] != 0) marked.push_back(U'|');
        marked.push_back(code);
        at += static_cast<size_t>(unicode::utf16_size(code));
    }
    return marked;
}

bool markedIs(u16_view text, u8_view expected,
              std::span<const typography::NoteAnchor> notes = {}) {
    const u8_text marked = markedParagraph(text, notes);
    if (marked.chars() == expected.chars()) return true;

    reportHyphenation(text.to_utf8(), expected, marked);
    return false;
}

/// Сколько мест переноса в абзаце — там, где важно не какие они, а есть ли.
size_t hyphenCount(u16_view text) {
    const sta_vector<uint8_t> hyphens = typography::hyphenPointsOf(text, {});
    return static_cast<size_t>(std::count_if(hyphens.begin(), hyphens.end(),
                                             [](uint8_t value) { return value != 0; }));
}

/// Правила набора поверх образцов. Проверяются там, откуда места переноса
/// берёт разбивка, — в hyphenPointsOf: hyphenate() о правилах не знает и
/// знать не должен, его сверяют с эталоном, который считает одни образцы.
void testHyphenationRules() {
    std::printf("\n=== правила переносов ===\n");

    // Слово с цифрой или с точкой внутри не переносится вовсе; точка на краю
    // слова — пунктуация.
    check(markedIs(u"страница5", u8"страница5"), "слово с цифрой не переносится");
    check(markedIs(u"страница.", u8"стра|ни|ца."), "точка в конце слова — пунктуация");
    check(markedIs(u".страница", u8".стра|ни|ца"), "точка в начале слова — пунктуация");
    check(markedIs(u"т.е.", u8"т.е."), "сокращение с точкой внутри");
    check(markedIs(u"см.страницу", u8"см.страницу"), "точка внутри отменяет перенос всего слова");

    // Дефис и апостроф — внутри слова: цифра за ними отменяет перенос и до них.
    check(markedIs(u"Москвич-412", u8"Москвич-412"), "цифра за дефисом-минусом");
    check(markedIs(u"Москвич\x2010" u"412", u8"Москвич\u2010412"), "цифра за дефисом U+2010");
    check(markedIs(u"Москвич\x2011" u"412", u8"Москвич\u2011412"), "цифра за неразрывным дефисом");
    check(markedIs(u"Москвич\x2019" u"412", u8"Москвич\u2019412"), "цифра за апострофом U+2019");
    check(markedIs(u"Москвич'412", u8"Москвич'412"), "цифра за апострофом U+0027");
    check(markedIs(u"достопримечательность'переносимость",
                   u8"до|сто|при|ме|ча|тель|ность'пе|ре|но|си|мость"),
          "апостроф U+0027 делит слово на части");
    check(markedIs(u"достопримечательность\x2019" u"переносимость",
                   u8"до|сто|при|ме|ча|тель|ность\u2019пе|ре|но|си|мость"),
          "апостроф U+2019 делит слово на части");
    check(markedIs(u"достопримечательность\x02BC" u"переносимость",
                   u8"до|сто|при|ме|ча|тель|ность\u02BCпе|ре|но|си|мость"),
          "апостроф-буква U+02BC делит слово на части, а не отменяет его перенос");

    // Буква, которой образцы не знают, — слово другого языка: перенос его
    // отменяется целиком, а не по куски вокруг неё.
    check(markedIs(u"\x040E" u"збекистон", u8"\u040Eзбекистон"), "«Ў» в начале слова");
    check(markedIs(u"\x00DC" u"bersetzung", u8"\u00DCbersetzung"), "«Ü» одной кодовой точкой");
    check(markedIs(u"M\x00E4" u"dchenhandelsschule", u8"M\u00E4dchenhandelsschule"),
          "«ä» одной кодовой точкой посреди слова");
    check(markedIs(u"Ma\x0308" u"dchenhandelsschule", u8"Ma\u0308dchenhandelsschule"),
          "«a» с диерезисом — буква, образцам незнакомая");

    // Чужая буква — буква любой письменности, и за пределом таблицы признаков
    // тоже: расширенная латиница, вьетнамская буква, буква вне BMP отменяют
    // перенос всего слова, а не режут его на куски. Знак за буквой — откуда
    // бы он ни был — делает её чужой, как и ZWJ, приставший к ней.
    check(markedIs(u"Mu\x1E25" u"ammad", u8"Mu\u1E25ammad"), "«ḥ» из расширенной латиницы");
    check(markedIs(u"Nguy\x1EC5" u"n-переносимость", u8"Nguy\u1EC5n-переносимость"),
          "вьетнамская «ễ» отменяет перенос и за дефисом");
    check(markedIs(u"п\xD835\xDD1E" u"ереносимость", u8"п\U0001D51Eереносимость"),
          "буква вне BMP");
    check(markedIs(u"достопри\x1DC4" u"мечательность", u8"достопри\u1DC4мечательность"),
          "знак выше таблицы за буквой алфавита");
    check(markedIs(u"достопри\x200D" u"мечательность", u8"достопри\u200Dмечательность"),
          "ZWJ внутри слова — не знак ударения");

    // Пробелы, пунктуация и символы за пределом таблицы слово кончают:
    // перенос по обе стороны от них остаётся.
    check(markedIs(u"переносимость\x2014" u"переносимость\x2E3A" u"переносимость\x3001"
                   u"переносимость\xFEFF" u"переносимость\xFFFC" u"переносимость\x2026",
                   u8"пе|ре|но|си|мость\u2014пе|ре|но|си|мость\u2E3Aпе|ре|но|си|мость\u3001"
                   u8"пе|ре|но|си|мость\uFEFFпе|ре|но|си|мость\uFFFCпе|ре|но|си|мость\u2026"),
          "тире, двойное тире, запятая CJK, ZWNBSP, знак объекта и многоточие кончают слово");
    check(markedIs(u"переносимость\xD83D\xDE00" u"переносимость",
                   u8"пе|ре|но|си|мость\U0001F600пе|ре|но|си|мость"),
          "эмодзи кончает слово");

    // Буква ли кодовая точка, решает её общая категория L: ниже U+0530 — по
    // таблице переносов, выше — unicode::is_letter, по таблице графем wxl.
    // Буква любой письменности, и модификаторная (Lm) тоже, отменяет перенос
    // слова, к которому приставлена, — и та, что стоит среди символов и
    // пунктуации (ⁱ, ℓ, 々). Валюта — не буква и слово кончает.
    check(markedIs(u"\x03B1" u"переносимость", u8"\u03B1переносимость"),
          "греческая «α» в таблице — чужая буква");
    check(markedIs(u"переносимость\x10D0", u8"переносимость\u10D0"),
          "грузинская «ა» выше таблицы — чужая буква");
    check(markedIs(u"\x0561" u"переносимость", u8"\u0561переносимость"),
          "армянская «ա» выше таблицы — чужая буква");
    check(markedIs(u"переносимость\x02B0", u8"переносимость\u02B0"),
          "модификаторная «ʰ» (Lm) в таблице — чужая буква");
    check(markedIs(u"переносимость\x1D43", u8"переносимость\u1D43"),
          "модификаторная «ᵃ» (Lm) выше таблицы — чужая буква");
    check(markedIs(u"переносимость\x2071", u8"переносимость\u2071"),
          "надстрочная «ⁱ» (Lm) среди знаков пунктуации — чужая буква");
    check(markedIs(u"\x2113" u"переносимость", u8"\u2113переносимость"),
          "«ℓ» среди буквоподобных символов — чужая буква");
    check(markedIs(u"переносимость\x3005", u8"переносимость\u3005"),
          "знак повтора «々» (Lm) среди пунктуации CJK — чужая буква");
    check(markedIs(u"переносимость\x20AC" u"переносимость",
                   u8"пе|ре|но|си|мость\u20ACпе|ре|но|си|мость"),
          "евро — не буква и кончает слово");

    // Известное ограничение: выше таблицы не буква — граница, какой бы она
    // ни была. Цифра чужой письменности не отменяет перенос, как 0–9, а
    // делит слово; знак частного использования — лигатура из конвертера —
    // режет слово на части, которые размечаются порознь; знак без основы за
    // мягким переносом не прозрачен, и часть до мягкого переноса закрывается
    // без него и получает места по образцам. Пробы держат это поведение,
    // чтобы смена его не прошла незамеченной.
    check(markedIs(u"переносимость\x0663" u"переносимость",
                   u8"пе|ре|но|си|мость\u0663пе|ре|но|си|мость"),
          "известное ограничение: арабо-индийская «٣» делит слово");
    {
        u16_text text;
        text.push_back(U'\xE000');
        text += u"переносимость";
        check(hyphenCount(text) == 4, "известное ограничение: U+E000 (Co) перед словом — граница");
    }
    check(markedIs(u"переносимость\x00AD\x1DC4" u"переносимость",
                   u8"пе|ре|но|си|мость\u00AD\u1DC4пе|ре|но|си|мость"),
          "известное ограничение: знак выше таблицы за мягким переносом делит слово");

    // Символы, что стоят в книгах вплотную к слову, — граница, а не буква:
    // слово по обе стороны от них переносится.
    struct BookSymbol {
        const char* name;
        char32_t code;
    };
    constexpr BookSymbol kBookSymbols[] = {
        {"№", U'\x2116'}, {"₽", U'\x20BD'}, {"≤", U'\x2264'}, {"→", U'\x2192'},
        {"♦", U'\x2666'}, {"✓", U'\x2713'}, {"…", U'\x2026'}, {"—", U'\x2014'},
    };
    for (const BookSymbol& symbol : kBookSymbols) {
        u16_text text;
        text += u"переносимость";
        text.push_back(symbol.code);
        text += u"переносимость";
        check(hyphenCount(text) == 8, std::string("слово рядом с «") + symbol.name + "» переносится");
    }

    // Выше таблицы роль решает unicode::is_letter: буква любой письменности —
    // чужая, и слово, к которому она приставлена, не переносится; всё не буква
    // — граница. Не-буквы — те, что стоят среди букв и легко сошли бы за них:
    // свободная кодовая точка (Cn) в блоке греческих букв, не-символ,
    // комбинирующая кириллическая буква — знак (Mn), а не буква, и символы (So)
    // рядом с иероглифами. Стоит кодовая точка в начале слова: знак за буквой
    // ушёл бы в её графему, и решала бы буква.
    struct AboveTable {
        char32_t code;
        bool letter;
    };
    constexpr AboveTable kAboveTable[] = {
        {U'\x2C00', true},    // глаголица, Lu
        {U'\x10000', true},   // линейное письмо Б, Lo
        {U'\x1FFF', false},   // свободная, Cn
        {U'\x1FFFE', false},  // не-символ, Cn
        {U'\x2DFF', false},   // комбинирующая кириллическая буква, Mn
        {U'\x2E80', false},   // ключ CJK, So
        {U'\x2FFF', false},   // знак описания иероглифа, So
    };
    for (const AboveTable& point : kAboveTable) {
        u16_text text;
        text.push_back(point.code);
        text += u"переносимость";

        char what[128];
        std::snprintf(what, sizeof what, "выше таблицы роль решает is_letter: U+%04X — %s",
                      static_cast<unsigned>(point.code), point.letter ? "чужая буква" : "граница");
        check(hyphenCount(text) == (point.letter ? 0u : 4u), what);
    }

    // Разложенные «й» и «ё» — «и» с краткой и «е» с диерезисом. Свести их к
    // одной кодовой точке нечем, и видеть в «й» гласную «и» образцам нельзя:
    // «во-йна» — неверное место. Такое слово не переносится вовсе.
    check(markedIs(u"вои\x0306на", u8"вои\u0306на"), "«война» с разложенной «й»");
    check(markedIs(u"леи\x0306тенант", u8"леи\u0306тенант"), "«лейтенант» с разложенной «й»");
    check(markedIs(u"е\x0308лочка", u8"е\u0308лочка"), "«ёлочка» с разложенной «ё»");
    check(markedIs(u"лейтенант", u8"лей|те|нант"), "та же «й» одной кодовой точкой переносится");

    // Заглавная не первой буквой — аббревиатура или имя, которого образцы не
    // видели. Первой буквой — норма, и у части после дефиса тоже.
    check(markedIs(u"юнеско", u8"юне|ско"), "строчными слово переносится");
    check(markedIs(u"ЮНЕСКО", u8"ЮНЕСКО"), "прописными — нет");
    check(markedIs(u"МосКва", u8"МосКва"), "заглавная посреди слова");
    check(markedIs(u"ПетерБург", u8"ПетерБург"), "заглавная посреди слова, где образцы рвали бы");
    check(markedIs(u"самолЁт", u8"самолЁт"), "«Ё» посреди слова — тоже заглавная");
    check(markedIs(u"самолёт", u8"са|мо|лёт"), "«самолёт» строчными переносится");
    check(markedIs(u"McDonald", u8"McDonald"), "латинская заглавная посреди слова");
    check(markedIs(u"mcdonald", u8"mcdon|ald"), "то же строчными переносится");
    check(markedIs(u"Петербург", u8"Пе|тер|бург"), "заглавная первой буквой");
    check(typography::hyphenPointsOf(u"Москва", {}) == typography::hyphenPointsOf(u"москва", {}),
          "«Москва» размечена как «москва»");
    check(markedIs(u"Санкт-Петербург", u8"Санкт-Пе|тер|бург"),
          "части через дефис размечаются порознь, и у второй заглавная — первая буква");
    check(markedIs(u"Петропавловск-Камчатский", u8"Пет|ро|пав|ловск-Кам|чат|ский"),
          "места в обеих частях");

    // Латиница и кириллица в одной части — образцы одного языка к буквам
    // другого не применяются.
    check(markedIs(u"мет\x0070ового", u8"метpового"), "латинская «p» среди кириллицы");
    check(markedIs(u"метрового", u8"мет|ро|во|го"), "то же слово кириллицей переносится");

    // Часть без гласной — не слог, а обрубок. Образцы такие места дают сами,
    // и отбрасывает их не hyphenate(), а правило.
    check(hyphenatedIs(u"чкнуть", u8"чк-нуть"), "образцы рвут «чк-нуть»");
    check(markedIs(u"чкнуть", u8"чкнуть"), "«чк» без гласной не остаётся на строке");
    check(markedIs(u"чкнувши", u8"чкнув|ши"),
          "из двух мест остаётся то, где гласные по обе стороны");
    check(markedIs(u"ссьма", u8"ссьма"), "«ссь» без гласной не остаётся на строке");
    check(markedIs(u"флэнжер", u8"флэн|жер"), "«фл» без гласной, хотя гласная сразу за местом");
    check(markedIs(u"англ.", u8"англ."), "«гл» без гласной не уходит на следующую");
    check(markedIs(u"поэт", u8"по|эт"), "гласная справа — сразу за местом");
    check(markedIs(u"стклянку", u8"стклян|ку"), "место с гласными по обе стороны остаётся");
    check(markedIs(u"тёмный", u8"тём|ный"), "«ё» — гласная");
    check(markedIs(u"crystal", u8"crys|tal"), "«y» в английском — гласная");

    // Знак ударения прозрачен: образцы видят основы букв, а место встаёт на
    // границу буквы — за знаком, а не между ним и основой.
    check(markedIs(u"досто\x0301примечательность", u8"до|сто\u0301|при|ме|ча|тель|ность"),
          "знак ударения не отменяет перенос, и место встаёт за ним");
    check(markedIs(u"досто\x0300примечательность", u8"до|сто\u0300|при|ме|ча|тель|ность"),
          "и знак тупого ударения");
    check(markedIs(u"\x0301достопримечательность", u8"\u0301до|сто|при|ме|ча|тель|ность"),
          "знак без основы в начале абзаца слову не мешает");

    // Хвост переноса считается буквами, без знаков: в «доро́га» место перед
    // «ро́га» уносит четыре буквы, а не пять единиц, место перед «га» — две,
    // и такой перенос разбивка берёт дороже.
    const sta_vector<uint8_t> stressed = typography::hyphenPointsOf(u"доро\x0301га", {});
    check(stressed.size() == 7 && stressed[2] == 4 && stressed[5] == 2,
          "знак ударения не считается буквой хвоста");

    // Длиннее предела часть не размечается вовсе — и считаются в ней буквы, а
    // не знаки над ними.
    u16_text longest;
    for (int at = 0; at < 3; ++at) longest += u"достопримечательность";
    check(longest.size() == typography::kMaxHyphenatedWord + 1 && hyphenCount(longest) == 0,
          "часть длиннее предела не переносится вовсе");
    check(hyphenCount(u16_view(longest).substr(0, longest.size() - 1)) > 0,
          "часть ровно в предел переносится");
    u16_text stressedLongest;
    for (int at = 0; at < 3; ++at) stressedLongest += u"досто\x0301примечательность";
    check(hyphenCount(u16_view(stressedLongest).substr(0, stressedLongest.size() - 1)) > 0,
          "знаки ударения в длину части не входят");

    // Мягкий перенос из книги отменяет места по образцам в своей части: их
    // ставит автор, и места оттуда отдаёт разбивке DirectWrite.
    check(markedIs(u"достопри\x00ADмечательность", u8"достопри\u00ADмечательность"),
          "в слове с мягким переносом мест по образцам нет");
    check(markedIs(u"достопри\x00AD\x0301мечательность", u8"достопри\u00AD\u0301мечательность"),
          "и знак без основы за мягким переносом часть не делит");
    check(markedIs(u"северо-за\x00ADпадный", u8"се|ве|ро-за\u00ADпадный"),
          "мягкий перенос отменяет места только своей части");
    check(markedIs(u"за\x00ADпадно-сибирский", u8"за\u00ADпадно-си|бир|ский"),
          "мягкий перенос в первой части не трогает вторую");
    check(markedIs(u"\x00ADдостопримечательность", u8"\u00ADдо|сто|при|ме|ча|тель|ность"),
          "мягкий перенос в начале слова мест не отменяет");
    check(markedIs(u"северо-\x00ADзападный", u8"се|ве|ро-\u00ADза|пад|ный"),
          "мягкий перенос в начале части мест не отменяет");

    // Знак сноски — слово сам по себе: цифра вплотную к слову слово не
    // продолжает, а текст знака размечается как всякий другой.
    auto const digitMark = typography::NoteAnchor{7, 1, 0};
    check(markedIs(u"маркизу5", u8"мар|ки|зу5", std::span(&digitMark, 1)),
          "знак сноски после слова не отменяет его переносов");
    auto const leadingMark = typography::NoteAnchor{0, 1, 0};
    check(markedIs(u"5маркизу", u8"5мар|ки|зу", std::span(&leadingMark, 1)),
          "и знак сноски перед словом");
    auto const wordMark = typography::NoteAnchor{0, 21, 0};
    check(markedIs(u"достопримечательность", u8"до|сто|при|ме|ча|тель|ность",
                   std::span(&wordMark, 1)),
          "текст знака сноски переносится");

    // Буква из нескольких кодовых точек — разложенные «й» и «ё», иероглиф,
    // конъюнкт, слог хангыля — для образцов чужая, и слово с ней не
    // переносится вовсе. Символ — флаг, эмодзи — слово кончает, и места по
    // обе стороны от него есть — всегда между буквами, из скольких бы кодовых
    // точек буква ни состояла.
    for (const CorpusLetter& letter : kLetters) {
        u16_text word;
        word += u"досто";
        word += letter.text;
        word += u"примечательность";

        const sta_vector<uint8_t> hyphens = typography::hyphenPointsOf(word, {});
        size_t points = 0;
        bool whole = true;
        for (uint32_t at = 0; at < hyphens.size(); ++at) {
            if (hyphens[at] == 0) continue;
            ++points;
            if (!isLetterStart(word, at)) whole = false;
        }
        if (letter.symbol)
            check(points > 0 && whole, std::string("места переноса между буквами: ") + letter.name);
        else
            check(points == 0, std::string("слово с чужой буквой без мест: ") + letter.name);
    }
}

/// Кончается ли строка дефисом переноса. Глиф спрашивается у того же шрифта,
/// каким набран последний прогон: вёрстка берёт типографский дефис, а в шрифте
/// без него — минус-дефис, и тест обязан признать оба.
bool endsWithHyphen(const typography::Line& line) {
    if (line.runs.empty()) return false;

    const typography::GlyphRun& run = line.runs.back();
    if (run.glyphIndices.empty() || run.fontFace == nullptr) return false;

    for (const UINT32 code : {0x2010u, 0x002Du}) {
        UINT16 glyph = 0;
        if (SUCCEEDED(run.fontFace->GetGlyphIndices(&code, 1, &glyph)) && glyph != 0 &&
            run.glyphIndices.back() == glyph)
            return true;
    }
    return false;
}

/// Переносы в вёрстке: разметка доходит до строк и печатает дефис, мягкий
/// перенос из книги не виден, пока строка на нём не кончилась, а по
/// неразрывному пробелу строка не рвётся вовсе.
void testHyphenationInLayout(typography::Engine& engine) {
    std::printf("\n=== переносы в вёрстке ===\n");

    typography::ParagraphStyle style;

    // Длинные слова в узкую полосу: без переносов строка была бы одним словом
    // и дырой в пол-полосы.
    u16_text text;
    for (int at = 0; at < 12; ++at) text += u"достопримечательность ";

    const auto lines = layoutOf(engine, paragraphOf(text), 200.0f, style);
    size_t hyphenated = 0;
    for (const typography::Line& line : lines)
        if (endsWithHyphen(line)) ++hyphenated;

    std::printf("  строк %zu, из них с переносом %zu\n", lines.size(), hyphenated);
    check(hyphenated > 0, "слово переносится, и дефис нарисован");

    // Мягкий перенос из книги невидим, пока строка на нём не кончилась.
    const auto plain = layoutOf(engine, paragraphOf(u"несколько"), 1000.0f, style);
    const auto soft = layoutOf(engine, paragraphOf(u"не­сколько"), 1000.0f, style);
    const bool measured = plain.size() == 1 && soft.size() == 1;
    check(measured && std::abs(plain[0].width - soft[0].width) < 0.01f,
          "мягкий перенос не занимает места посреди строки");

    const auto printLines = [](const auto& lines) {
        for (const typography::Line& line : lines)
            std::printf("       строка с %u длиной %u, ширина %.1f%s\n", line.textStart,
                        line.textLength, line.width, endsWithHyphen(line) ? ", дефис" : "");
    };

    // Он же — место разрыва: слово из одних согласных образцы не разорвут, и
    // единственный перенос в нём тот, что стоит в книге. Полоса такая, что
    // половина слова в неё помещается, а слово целиком — нет; набор с
    // выключкой влево, чтобы строке без пробелов не приходилось тянуться.
    typography::ParagraphStyle ragged = style;
    ragged.alignment = typography::Alignment::Left;

    const float half = layoutOf(engine, paragraphOf(u"ффффф"), 1000.0f, ragged).front().width;

    const auto broken = layoutOf(engine, paragraphOf(u"ффффф­ффффф"), half * 1.5f, ragged);
    const bool atSoftHyphen = broken.size() == 2 && endsWithHyphen(broken[0]) &&
                              broken[0].textLength == 6 && broken[1].textStart == 6;
    if (!atSoftHyphen) printLines(broken);
    check(atSoftHyphen, "строка кончилась на мягком переносе, и дефис появился");

    // Слово шире полосы, но с переносами, которые делят его на помещающиеся
    // куски, переносится, а не рубится: каждая строка, кроме последней,
    // кончается дефисом.
    const auto split = layoutOf(engine, paragraphOf(u"достопримечательность"), 90.0f, ragged);
    bool hyphenatedNotChopped = split.size() > 1;
    for (size_t at = 0; at + 1 < split.size(); ++at)
        if (!endsWithHyphen(split[at])) hyphenatedNotChopped = false;
    if (!hyphenatedNotChopped) printLines(split);
    check(hyphenatedNotChopped, "слово шире полосы переносится по слогам, а не рубится");

    // Неразрывный пробел: строка не вправе начаться сразу после него, какой бы
    // ширины ни была полоса.
    u16_text tied;
    for (int at = 0; at < 20; ++at) tied += u"аа бб вв ";
    const typography::BookText paragraph = paragraphOf(tied);

    bool kept = true;
    for (float width = 40.0f; width <= 400.0f; width += 7.0f)
        for (const typography::Line& line : layoutOf(engine, paragraph, width, style))
            if (line.textStart > 0 && tied.plain()[line.textStart - 1] == u' ') {
                std::printf("       полоса %.0f: строка начата после неразрывного пробела\n", width);
                kept = false;
            }

    check(kept, "по неразрывному пробелу строка не рвётся");
}

/// Дефис переноса в над- и подстрочном стоит рядом со своими буквами, а не на
/// базовой линии. Шейпер этого глифа не видел — его дописывает строка, — и
/// сдвиг прогона ему передаётся отдельно. Так кончаются строки длинных `<sup>`
/// и знаков сносок с текстом (n_3 в nightmare_example).
void testHyphenShift(typography::Engine& engine) {
    std::printf("\n=== дефис в над- и подстрочном ===\n");

    typography::ParagraphStyle reference;
    reference.alignment = typography::Alignment::Left;
    typography::ParagraphStyle larger = reference;
    larger.fontSize = reference.fontSize * 1.3f;
    // По формату строка с переносом идёт через JustifyGlyphAdvances, и сдвиг
    // дефиса должен пройти и через него: выключка возвращает смещения глифов
    // заново.
    typography::ParagraphStyle justified = reference;
    justified.alignment = typography::Alignment::Justify;

    struct Shift {
        bool hyphenated = true;  ///< при каждом кегле и выключке есть строка с переносом
        bool beside = true;      ///< у каждой такой дефис сдвинут ровно как её буквы
        float letters = 0.0f;    ///< сдвиг букв последней из них
        float hyphen = 0.0f;     ///< и её дефиса
    };

    // Слово шейпится при опорном кегле, а верстается и при нём, и при другом:
    // при другом сдвиг приходит умножением, и дефису он должен прийти тем же
    // умножением, что буквам. Полоса — две пятых слова: оно переносится, а
    // слоги в неё влезают. Эталон — смещение первой буквы строки: в нём, кроме
    // сдвига формата, сидит и вертикальная посадка глифа шрифтом (GPOS), и
    // проба полагается на то, что у кириллицы Georgia она нулевая, — тогда
    // смещение буквы и есть сдвиг формата.
    const auto shiftOf = [&](int8_t script) {
        typography::FontStyle font;
        font.script = script;
        const typography::BookText book = paragraphOf(u"достопримечательность", font);
        const typography::ShapedParagraphPtr shaped =
            engine.shape(book, onlyParagraph(book), reference);

        Shift shift;
        for (const typography::ParagraphStyle& style : {reference, larger, justified}) {
            const float word = engine.layout(*shaped, 1000.0f, style).front().width;

            size_t hyphens = 0;
            for (const typography::Line& line : engine.layout(*shaped, word * 0.4f, style)) {
                if (!endsWithHyphen(line)) continue;

                const typography::GlyphRun& run = line.runs.back();
                shift.letters = run.offsets.front().ascenderOffset;
                shift.hyphen = run.offsets.back().ascenderOffset;
                shift.beside = shift.beside && std::abs(shift.hyphen - shift.letters) < 0.01f;
                ++hyphens;
            }

            std::printf("  сдвиг %+d, кегль %.0f, %s: переносов %zu, буквы %.2f, дефис %.2f\n", script,
                        style.fontSize,
                        style.alignment == typography::Alignment::Justify ? "по формату" : "влево",
                        hyphens, shift.letters, shift.hyphen);
            shift.hyphenated = shift.hyphenated && hyphens > 0;
        }
        return shift;
    };

    const Shift super = shiftOf(1);
    const Shift sub = shiftOf(-1);
    const Shift plain = shiftOf(0);

    check(super.hyphenated && sub.hyphenated && plain.hyphenated,
          "слово в узкой полосе переносится в каждом прогоне, кегле и выключке");
    check(super.beside && super.letters > 0.0f,
          "дефис надстрочного поднят вровень со своими буквами");
    check(sub.beside && sub.letters < 0.0f,
          "дефис подстрочного опущен вровень со своими буквами");
    check(plain.beside && plain.hyphen == 0.0f, "дефис обычного прогона — на базовой линии");
}

/// Серии переносов на живом тексте: все абзацы «Отцов и детей» в самой узкой
/// колонке, какую строит читалка, и в ещё более узкой — ни одна серия не
/// длиннее двух строк. Штраф за двойной перенос такого не обеспечивал: серии в
/// пять строк оставались при любом его весе.
void testHyphenSeries(typography::Engine& engine, const std::filesystem::path& testdata) {
    const std::filesystem::path bookPath = testdata / "Turgenev_I._Spisokshkolnoy._Otcyi_I_Deti.fb3";
    if (!std::filesystem::exists(bookPath)) {
        std::printf("\n=== серии переносов: нет «Отцов и детей», пропущено ===\n");
        return;
    }

    std::printf("\n=== серии переносов ===\n");
    const fb3::Document document(bookPath);
    const typography::BookText book = typography::flatten(document);

    for (const float width : {460.0f, 300.0f}) {
        size_t lines = 0, hyphens = 0, longest = 0;
        for (const typography::Block& block : book.flow()) {
            if (block.kind != typography::BlockKind::Paragraph || block.paragraph.text.count == 0)
                continue;

            size_t row = 0;
            for (const typography::Line& line :
                 engine.layout(book, block.paragraph, width, styleFor(block, 20.0f))) {
                ++lines;
                row = endsWithHyphen(line) ? row + 1 : 0;
                hyphens += row != 0;
                longest = std::max(longest, row);
            }
        }

        std::printf("  полоса %.0f: строк %zu, переносов %zu, подряд не больше %zu\n", width, lines,
                    hyphens, longest);
        check(longest <= 2, "больше двух переносов подряд не бывает");
    }
}

/// Диагностический снимок, когда набор просят показать глазами: одни и те же
/// абзацы «Отцов и детей» в узкой колонке без переносов и с ними, рядом, в PNG
/// по пути из BUKVITSA_HYPHEN_SHOT. Без переменной не делает ничего.
void shootHyphenation(typography::Engine& engine, const std::filesystem::path& testdata) {
    wchar_t shot[MAX_PATH]{};
    const DWORD shotLength = GetEnvironmentVariableW(L"BUKVITSA_HYPHEN_SHOT", shot, MAX_PATH);
    const std::filesystem::path bookPath = testdata / "Turgenev_I._Spisokshkolnoy._Otcyi_I_Deti.fb3";
    if (shotLength == 0 || shotLength >= MAX_PATH || !std::filesystem::exists(bookPath)) return;

    std::printf("\n=== снимок переносов ===\n");

    const fb3::Document document(bookPath);
    const typography::BookText book = typography::flatten(document);

    // Обычные абзацы подряд с первого длинного: заголовки выключены по центру
    // и о переносах ничего не скажут.
    std::vector<const typography::Block*> chosen;
    for (const typography::Block& block : book.flow()) {
        if (block.kind != typography::BlockKind::Paragraph) continue;
        if (chosen.empty() && block.paragraph.text.count < 400) continue;
        chosen.push_back(&block);
        if (chosen.size() == 9) break;
    }

    // Колонка — самая узкая из тех, что строит читалка: две колонки идут, пока
    // строка не короче 43 знаков, и на деле приходят около 45. Средний знак
    // меряется по той же панграмме, что у читалки, тем же шрифтом и кеглем.
    typography::ParagraphStyle measure;
    measure.alignment = typography::Alignment::Left;
    const u16_view pangram = u"съешь же ещё этих мягких французских булок, да выпей чаю";
    const float pangramWidth =
        layoutOf(engine, paragraphOf(pangram), 10000.0f, measure).front().width;

    const float kColumn = std::round(45.0f * pangramWidth / static_cast<float>(pangram.size()));
    constexpr float kGap = 40.0f, kMargin = 24.0f;
    std::printf("  колонка: 45 знаков, %.0f DIP\n", kColumn);

    const typography::TextStyle original = engine.textStyle();

    struct Column {
        std::vector<sta_vector<typography::Line>> paragraphs;
        float height = 0.0f;
        size_t lines = 0, hyphens = 0, hyphensInARow = 0;
    };

    Column columns[2];
    for (int side = 0; side < 2; ++side) {
        typography::TextStyle textStyle = original;
        textStyle.hyphenation = side == 1;
        engine.setTextStyle(textStyle);

        Column& column = columns[side];
        size_t row = 0;
        for (const typography::Block* block : chosen) {
            column.paragraphs.push_back(
                engine.layout(book, block->paragraph, kColumn, styleFor(*block, 20.0f)));
            for (const typography::Line& line : column.paragraphs.back()) {
                column.height += line.height;
                ++column.lines;
                row = endsWithHyphen(line) ? row + 1 : 0;
                column.hyphens += row != 0;
                column.hyphensInARow = std::max(column.hyphensInARow, row);
            }
        }
    }
    engine.setTextStyle(original);

    std::printf("  без переносов: %zu строк\n", columns[0].lines);
    std::printf("  с переносами:  %zu строк, переносов %zu, подряд не больше %zu\n", columns[1].lines,
                columns[1].hyphens, columns[1].hyphensInARow);

    Microsoft::WRL::ComPtr<IWICImagingFactory> wic;
    Microsoft::WRL::ComPtr<ID2D1Factory1> d2d;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
        FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), nullptr, &d2d)))
        return;

    const auto width = static_cast<UINT>(kMargin * 2 + kColumn * 2 + kGap);
    const auto height = static_cast<UINT>(kMargin * 2 + std::max(columns[0].height, columns[1].height));

    Microsoft::WRL::ComPtr<IWICBitmap> bitmap;
    Microsoft::WRL::ComPtr<ID2D1RenderTarget> target;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> context;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> ink, rule;
    if (FAILED(wic->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnDemand,
                                 &bitmap)) ||
        FAILED(d2d->CreateWicBitmapRenderTarget(
            bitmap.Get(),
            D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                         D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                                           D2D1_ALPHA_MODE_PREMULTIPLIED)),
            &target)) ||
        FAILED(target.As(&context)))
        return;

    context->CreateSolidColorBrush(rgb(32, 32, 32), &ink);
    context->CreateSolidColorBrush(rgb(216, 208, 192), &rule);
    context->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

    // Знаки сносок — той же кистью, что текст: снимок о переносах, а не о
    // цвете знаков.
    auto const brushes = typography::TextBrushes{not_null<ID2D1Brush>{ink.Get()},
                                                 not_null<ID2D1Brush>{ink.Get()}};

    context->BeginDraw();
    context->Clear(rgb(251, 248, 241));
    for (int side = 0; side < 2; ++side) {
        const float left = kMargin + static_cast<float>(side) * (kColumn + kGap);
        const float bottom = static_cast<float>(height) - kMargin;

        // Края полосы: по ним видно, что выключка по формату ровная и ничего
        // не вылезло.
        context->DrawLine({left, kMargin}, {left, bottom}, rule.Get(), 0.5f);
        context->DrawLine({left + kColumn, kMargin}, {left + kColumn, bottom}, rule.Get(), 0.5f);

        float y = kMargin;
        for (const auto& lines : columns[side].paragraphs)
            for (const typography::Line& line : lines) {
                typography::drawLine(context.Get(), line, left, y + line.ascent, brushes);
                y += line.height;
            }
    }
    if (FAILED(context->EndDraw())) return;

    Microsoft::WRL::ComPtr<IWICStream> stream;
    Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
    Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
    if (SUCCEEDED(wic->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromFilename(shot, GENERIC_WRITE)) &&
        SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
        SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
        SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
        SUCCEEDED(frame->WriteSource(bitmap.Get(), nullptr)) && SUCCEEDED(frame->Commit()) &&
        SUCCEEDED(encoder->Commit())) {
        const std::string path = toUtf8(std::wstring_view(shot, shotLength));
        std::printf("  снимок: %.*s\n", static_cast<int>(path.size()), path.data());
    }
}

/// Базовые линии идут сверху вниз, и вся сноска ниже последней из них.
bool notePlacedDownwards(const typography::NoteLayout& note) {
    float previous = 0.0f;
    for (const typography::NoteLine& placed : note.lines) {
        if (placed.baseline <= previous) return false;
        previous = placed.baseline;
    }
    return note.height > previous;
}

/// Раскладка всплывающей сноски — то, что читалка показывает по щелчку на
/// знаке: строки, их базовые линии и высота, без окна.
void testNoteLayout(typography::Engine& engine, const std::filesystem::path& testdata) {
    std::printf("\n=== сноска ===\n");

    // Абзац из ста слов — несколько строк в любой из колонок ниже.
    std::string hundred;
    for (int i = 0; i < 100; ++i) hundred += "слово ";

    const std::string body = std::string(R"(<?xml version="1.0" encoding="UTF-8"?>
<fb3-body xmlns="http://www.fictionbook.org/FictionBook3/body" xmlns:xlink="http://www.w3.org/1999/xlink" id="00000000-0000-0000-0000-000000000003">
<section id="s1"><p>Текст книги.</p></section>
<notes show="0">
<notebody id="empty"/>
<notebody id="blank"><p> </p></notebody>
<notebody id="word"><p>Слово.</p></notebody>
<notebody id="hundred"><p>)") + hundred + R"(</p></notebody>
<notebody id="two"><p>)" + hundred + R"(</p><p>Второй абзац.</p></notebody>
</notes>
</fb3-body>)";
    const fb3::Document document{fb3Of(body)};
    check(document.noteBody("empty") && document.noteBody("blank") && document.noteBody("word") &&
              document.noteBody("hundred") && document.noteBody("two"),
          "тела сносок находятся по id");

    // Тела развёрнуты в хранилище по номеру в списке тел документа — тому же,
    // что несёт якорь знака.
    const typography::BookText book = typography::flatten(document);
    check(book.noteBodies.size() == document.noteBodies().size(), "у каждого тела — свои блоки");

    constexpr float kFontSize = 20.0f;
    constexpr float kWidth = 300.0f;
    const auto noteOf = [&](std::string_view id, float width) {
        const std::span<const fb3::Node* const> bodies = document.noteBodies();
        const auto found = std::find(bodies.begin(), bodies.end(), document.noteBody(id));
        if (found == bodies.end() || book.noteBodies.size() != bodies.size())
            return typography::NoteLayout{};
        const auto index = static_cast<size_t>(found - bodies.begin());
        return typography::layoutNote(engine, book, book.noteBodies[index], width, kFontSize);
    };

    // Пустая сноска не занимает места: всплывашка по ней не открывается.
    const typography::NoteLayout emptyNote = noteOf("empty", kWidth);
    const typography::NoteLayout blankNote = noteOf("blank", kWidth);
    const typography::NoteLayout noWidth = noteOf("word", 0.0f);
    check(emptyNote.lines.empty() && emptyNote.height == 0.0f,
          "пустая сноска — ни строки, высота ноль");
    check(blankNote.lines.empty() && blankNote.height == 0.0f,
          "сноска из одних пробелов — ни строки, высота ноль");
    check(noWidth.lines.empty() && noWidth.height == 0.0f,
          "колонка без ширины — ни строки, высота ноль");

    // Высота растёт с текстом.
    const typography::NoteLayout word = noteOf("word", kWidth);
    const typography::NoteLayout hundredWords = noteOf("hundred", kWidth);
    std::printf("  слово: строк %zu, высота %.1f; сто слов: строк %zu, высота %.1f\n",
                word.lines.size(), word.height, hundredWords.lines.size(), hundredWords.height);
    check(word.lines.size() == 1 && notePlacedDownwards(word),
          "одно слово — одна строка, сноска ниже её базовой линии");
    check(hundredWords.lines.size() > 1 && notePlacedDownwards(hundredWords) &&
              hundredWords.height > word.height,
          "сто слов — строки сверху вниз, сноска выше, чем из одного слова");

    // Отбивка между абзацами больше шага строк внутри абзаца. Первая строка
    // второго абзаца — та, что начинается с начала его текста.
    const typography::NoteLayout two = noteOf("two", kWidth);
    size_t second = 0;
    for (size_t i = 1; i < two.lines.size() && second == 0; ++i)
        if (two.lines[i].line.textStart == 0) second = i;

    float lineStep = 0.0f;
    for (size_t i = 1; i < second; ++i)
        lineStep = std::max(lineStep, two.lines[i].baseline - two.lines[i - 1].baseline);
    const float paragraphStep =
        second > 1 ? two.lines[second].baseline - two.lines[second - 1].baseline : 0.0f;

    std::printf("  шаг строк %.1f, шаг через границу абзацев %.1f\n", lineStep, paragraphStep);
    check(second > 1 && paragraphStep > lineStep,
          "отбивка между абзацами больше, чем между строками одного абзаца");

    // Уже колонка — больше строк.
    const typography::NoteLayout narrow = noteOf("hundred", kWidth / 2.0f);
    check(narrow.lines.size() > hundredWords.lines.size() && narrow.height > hundredWords.height,
          "колонка вдвое уже — строк больше, сноска выше");

    // Сноски настоящей книги: каждая, на которую ссылается текст.
    // Без книги проба не прошла бы, а промолчала: отсутствие файла — провал.
    const std::filesystem::path bookPath = testdata / "nightmare_example.fb3";
    if (!std::filesystem::exists(bookPath)) {
        check(false, "nightmare_example.fb3 на месте — по нему раскладываются сноски книги");
        return;
    }

    const fb3::Document nightmare{bookPath};
    const typography::BookText stored = typography::flatten(nightmare);
    std::vector<uint32_t> targets;
    for (const typography::Block& block : stored.flow())
        for (const typography::NoteAnchor& anchor : stored.notesOf(block.paragraph))
            if (std::find(targets.begin(), targets.end(), anchor.body) == targets.end())
                targets.push_back(anchor.body);

    bool allPlaced = true;
    for (const uint32_t target : targets) {
        const typography::NoteLayout note = typography::layoutNote(
            engine, stored, stored.noteBodies[target], kWidth, kFontSize);
        if (note.lines.empty() || !notePlacedDownwards(note)) allPlaced = false;
    }

    std::printf("  nightmare_example: сносок %zu\n", targets.size());
    check(!targets.empty() && allPlaced, "каждая сноска книги раскладывается в строки сверху вниз");
}

/// Докуда строка ставит перья и шаги, от левого края полосы: самое левое перо
/// глифа (место пера и сдвиг выключки) и самый правый конец шагов.
struct PenReach {
    float left = 0.0f;
    float right = 0.0f;
};

PenReach penReach(const typography::Line& line) {
    PenReach reach;
    for (const typography::GlyphRun& run : line.runs) {
        float pen = run.originX;
        for (size_t glyph = 0; glyph < run.glyphIndices.size(); ++glyph) {
            const float glyphStart = pen + run.offsets[glyph].advanceOffset;
            reach.left = std::min(reach.left, glyphStart);
            reach.right = std::max(reach.right, glyphStart);
            pen += run.advances[glyph];
        }
        reach.right = std::max(reach.right, pen);
    }
    return reach;
}

/// Кусок строки для отчёта: табуляция и перевод строки — видимыми знаками, не
/// длиннее 60 единиц и по границе буквы, как и всякая обрезка в выводе проб.
std::string reportedText(std::wstring_view text) {
    const size_t cut = unicode::floor_grapheme_boundary(text, std::min<size_t>(text.size(), 60));
    std::wstring shown;
    for (const wchar_t unit : text.substr(0, cut))
        shown += unit == L'\t' ? L'→' : unit == L'\n' ? L'↵' : unit;
    return toUtf8(shown) + (cut < text.size() ? "…" : "");
}

/// Id тел сносок по номеру — со ссылок на них: у тела в модели книги своего id
/// нет, а ссылка несёт и id, и номер тела. На тело без единой ссылки id не
/// найдётся, и отчёт назовёт его номером.
void collectNoteIds(const fb3::Node& node, std::vector<std::string>& ids) {
    if (node.kind() == fb3::NodeKind::NoteRef) {
        const fb3::NoteRefData* ref = node.noteRef();
        if (ref && ref->body && *ref->body < ids.size() && ids[*ref->body].empty())
            ids[*ref->body] = std::string(ref->targetId.chars());
    }
    for (const fb3::Node& child : node.children()) collectNoteIds(child, ids);
}

/// Сноска ставит перья и шаги там, где её свёрстали, — внутри колонки. У
/// каждой строки каждого тела сносок nightmare_example проверяется то, по чему
/// drawLine расставит глифы: перо каждого глифа (место пера и сдвиг выключки)
/// и конец шагов каждого прогона — внутри колонки, ширина прогона — сумма его
/// шагов, ширина строки — сумма её прогонов. Чернил проба не мерит: глиф
/// вправе выступить за свой шаг (курсивная прописная в конце строки,
/// курсивное «р» в начале), и этот выступ вбирает поле всплывашки вокруг
/// колонки. Тела нарочно пёстрые: в n_3 вперемешку над- и подстрочные
/// вставки, курсив, разрядка, капитель и моноширный, а в преформате — строки с
/// отступом в табуляциях за переводом строки; в n_2 и e_2 — слово в 25 664
/// знака, которое вёрстка рубит по колонке, как то же слово в потоке
/// (testLayout на полосе 260); строки, выключенные до самого края, есть у
/// каждой колонки. Самую дальнюю строку проба называет id тела, местом в
/// абзаце и текстом — по ним видно, где она в книге.
void testNotePensInsideColumn(typography::Engine& engine, const std::filesystem::path& testdata) {
    std::printf("\n=== сноска: перья и шаги внутри колонки ===\n");

    // Без книги проба не прошла бы, а промолчала: отсутствие файла — провал.
    const std::filesystem::path bookPath = testdata / "nightmare_example.fb3";
    if (!std::filesystem::exists(bookPath)) {
        check(false, "nightmare_example.fb3 на месте — по нему проверяются перья и шаги сносок");
        return;
    }

    const fb3::Document nightmare{bookPath};
    const typography::BookText book = typography::flatten(nightmare);

    std::vector<std::string> noteIds(nightmare.noteBodies().size());
    collectNoteIds(nightmare.body(), noteIds);
    for (const fb3::Node* noteBody : nightmare.noteBodies()) collectNoteIds(*noteBody, noteIds);

    // Кегль книги по умолчанию: сноска набирается от него мельче.
    constexpr float kFontSize = 20.0f;
    // Допуск — как у пробы полосы (testLayout): выключка раздаёт добавку во
    // float и вправе промахнуться на последний разряд.
    constexpr float kSlack = 0.15f;

    // Колонка самой широкой всплывашки (460 DIP без двух рамок и двух полей) и
    // узкая, где строк больше и переносов чаще.
    for (const float column : {426.0f, 260.0f}) {
        size_t lineCount = 0;
        size_t edgeLines = 0;
        float leftmost = 0.0f;
        float rightmost = 0.0f;
        size_t rightmostBody = 0;
        size_t rightmostLine = 0;
        uint32_t rightmostStart = 0;
        std::wstring rightmostText;
        bool widthsAgree = true;

        for (size_t body = 0; body < book.noteBodies.size(); ++body) {
            const typography::NoteLayout note =
                typography::layoutNote(engine, book, book.noteBodies[body], column, kFontSize);

            // Строки идут блок за блоком, и первая строка блока — с начала его
            // текста: по ней видно, какому абзацу строка принадлежит. Блоки без
            // текста строк не дают.
            const std::span<const typography::Block> blocks = book.blocksOf(book.noteBodies[body]);
            size_t blockIndex = 0;
            bool blockStarted = false;

            for (size_t index = 0; index < note.lines.size(); ++index) {
                const typography::Line& line = note.lines[index].line;
                ++lineCount;

                if (line.textStart == 0) {
                    if (blockStarted) ++blockIndex;
                    while (blockIndex < blocks.size() && blocks[blockIndex].paragraph.text.count == 0)
                        ++blockIndex;
                    blockStarted = true;
                }

                float runsWidth = 0.0f;
                for (const typography::GlyphRun& run : line.runs) {
                    float advances = 0.0f;
                    for (const float advance : run.advances) advances += advance;
                    if (std::abs(advances - run.width) > 0.01f) widthsAgree = false;
                    runsWidth += run.width;
                }
                if (std::abs(runsWidth - line.width) > 0.01f) widthsAgree = false;

                const PenReach reach = penReach(line);
                leftmost = std::min(leftmost, reach.left);
                if (reach.right > column - 0.5f) ++edgeLines;
                if (reach.right > rightmost) {
                    rightmost = reach.right;
                    rightmostBody = body;
                    rightmostLine = index;
                    rightmostStart = line.textStart;
                    rightmostText = blockIndex < blocks.size()
                                        ? std::wstring(book.textOf(blocks[blockIndex].paragraph)
                                                           .wchars()
                                                           .substr(line.textStart, line.textLength))
                                        : std::wstring();
                }
            }
        }

        // Без единого тела отчёт о самой дальней строке называет номер 0, а
        // проверка ниже провалится сама: читать список id за его концом нельзя.
        const std::string rightmostId =
            rightmostBody < noteIds.size() && !noteIds[rightmostBody].empty() ? noteIds[rightmostBody]
                                                                               : std::string("без id");
        std::printf("  колонка %.0f: строк %zu, у края %zu; перья и шаги от %.2f до %.2f\n"
                    "    самая дальняя — тело %s (номер %zu), строка %zu, с %u в абзаце: «%s»\n",
                    column, lineCount, edgeLines, leftmost, rightmost,
                    rightmostId.c_str(), rightmostBody, rightmostLine,
                    rightmostStart, reportedText(rightmostText).c_str());
        check(lineCount > 0 && edgeLines > 0, "у тел сносок есть строки, выключенные до края колонки");
        check(leftmost >= -kSlack && rightmost <= column + kSlack,
              "перья и шаги каждой строки сноски — внутри колонки");
        check(widthsAgree, "ширина прогона — сумма его шагов, ширина строки — сумма её прогонов");
    }
}

/// Позиция символа в книге по прогонам позиций там, где она не идёт вровень с
/// индексом в тексте: свёрнутые пробелы, пара вне BMP, знак сноски, который
/// поставила вёрстка. Книга маленькая, но развёрнута настоящим flatten.
void testCharOffsets() {
    std::printf("\n=== позиции символов ===\n");

    // Девять сносок в третьем абзаце — затем, чтобы знак в четвёртом вышел из
    // двух цифр.
    std::string nine;
    for (int i = 0; i < 9; ++i) nine += R"(ж<note href="n1" xlink:role="footnote"/>)";

    const std::string body = std::string(R"(<?xml version="1.0" encoding="UTF-8"?>
<fb3-body xmlns="http://www.fictionbook.org/FictionBook3/body" xmlns:xlink="http://www.w3.org/1999/xlink" id="00000000-0000-0000-0000-000000000004">
<section id="s1">
<p>a  b</p>
<p>x&#x1F600;y</p>
<p>)") + nine + R"(</p>
<p>x<note href="n1" xlink:role="footnote"/>y</p>
</section>
<notes show="0"><notebody id="n1"><p>сноска</p></notebody></notes>
</fb3-body>)";
    const fb3::Document document{fb3Of(body)};
    const typography::BookText book = typography::flatten(document);
    const std::span<const typography::Block> blocks = book.flow();

    check(blocks.size() == 4, "четыре абзаца — четыре блока потока");
    if (blocks.size() != 4) return;

    const auto at = [&](size_t block, uint32_t index) {
        return book.charOffsetAt(blocks[block].paragraph, index);
    };

    // «a␠␠b»: свёрнутый пробел стоит на месте первого из двух, «b» — на своём.
    const uint32_t a = at(0, 0);
    check(book.textOf(blocks[0].paragraph) == L"a b", "два пробела свёрнуты в один");
    check(a == blocks[0].charOffset, "первый символ абзаца стоит там же, где его узел");
    check(at(0, 1) == a + 1 && at(0, 2) == a + 3, "позиции после свёртки: 0, 1, 3");
    check(book.offsetsOf(blocks[0].paragraph).size() == 2,
          "свёртка — одна точка разрыва, прогонов позиций два");

    // Пара: обе половины несут одну позицию, следующий символ — на одну дальше.
    const uint32_t x = at(1, 0);
    check(blocks[1].paragraph.text.count == 4, "символ вне BMP — две единицы текста");
    check(at(1, 1) == x + 1 && at(1, 2) == x + 1 && at(1, 3) == x + 2,
          "обе половины пары — одна позиция, следующий символ — на одну дальше");

    // Знак десятой сноски: цифры поставила вёрстка, в книге их нет, и обе
    // стоят там, где стоит ссылка, — на позиции текста за ней.
    const typography::Paragraph& marked = blocks[3].paragraph;
    const uint32_t m = at(3, 0);
    check(book.textOf(marked) == L"x10y", "знак десятой сноски — из двух цифр");
    check(at(3, 1) == m + 1 && at(3, 2) == m + 1 && at(3, 3) == m + 1,
          "цифры знака стоят на одной позиции, текст за ним — на своей");

    const std::span<const typography::NoteAnchor> anchors = book.notesOf(marked);
    check(anchors.size() == 1 && anchors[0].position == 1 && anchors[0].length == 2 &&
              anchors[0].body == 0,
          "якорь — обе цифры знака, тело — первое");

    // Тело сноски — за потоком, своим блоком.
    check(book.flowCount == 4 && book.noteBodies.size() == 1 && book.noteBodies[0].first == 4 &&
              book.noteBodies[0].count == 1 && book.blocks.size() == 5,
          "тело сноски — один блок сразу за потоком");
}

/// Роль прогона — знак сноски или текст книги — от разметки до прогона
/// глифов: по ней вывод красит знак. Знак с якорем начинает свой прогон
/// стиля, каким бы ни было начертание перед ним, и место на строке у каждого
/// знака своё; роль — только у знака с живым якорем.
void testNoteMarkRole(typography::Engine& engine) {
    std::printf("\n=== роль знака сноски ===\n");

    const std::string body = R"(<?xml version="1.0" encoding="UTF-8"?>
<fb3-body xmlns="http://www.fictionbook.org/FictionBook3/body" xmlns:xlink="http://www.w3.org/1999/xlink" id="00000000-0000-0000-0000-000000000005">
<section id="s1">
<p>слово<note href="n1" xlink:role="footnote">1</note> дальше</p>
<p>x<sup>2</sup><note href="n1" xlink:role="footnote">3</note>y</p>
<p>z<note href="n1" xlink:role="footnote">4</note><note href="n2" xlink:role="footnote">5</note>w</p>
<p>q<a xlink:href="#s1">ссылка</a>r</p>
<p>u<note href="nowhere" xlink:role="footnote">6</note>v</p>
<p>e<note href="n1" xlink:role="footnote">f<img src="rIdA"/>g</note>h</p>
<p>a<note href="n1" xlink:role="footnote"/>b</p>
<p>c<note href="n1" xlink:role="footnote"/><note href="n2" xlink:role="footnote"/>d</p>
<p>e<note href="n1" xlink:role="footnote"/><note href="n2" xlink:role="footnote">9</note>f</p>
<p>j<note href="nowhere" xlink:role="footnote"/><note href="n2" xlink:role="footnote">9</note>k</p>
<p>h<note href="n1" xlink:role="footnote"><em>4</em>2</note>i</p>
<p>g<note href="n1" xlink:role="footnote"/></p>
<p>t<note href="n1" xlink:role="footnote"><note href="n2" xlink:role="footnote">8</note>7</note>u</p>
</section>
<notes show="0">
<notebody id="n1"><p>первая</p></notebody>
<notebody id="n2"><p>вторая</p></notebody>
</notes>
</fb3-body>)";
    // Пакет с картинками: картинка внутри знака должна быть целой, со своей
    // частью, — битая блока не заводит и абзаца не режет.
    auto const document = fb3::Document{fb3WithImages(body)};
    const typography::BookText book = typography::flatten(document);
    const std::span<const typography::Block> blocks = book.flow();

    // Шестой абзац картинка внутри знака делит на три блока.
    check(blocks.size() == 15, "тринадцать абзацев и картинка — пятнадцать блоков потока");
    if (blocks.size() != 15) return;

    using typography::TextRole;
    const auto spansOf = [&](size_t block) { return book.spansOf(blocks[block].paragraph); };
    const auto isSpan = [](const typography::StyleSpan& span, uint32_t start, uint32_t length,
                           TextRole role, int8_t script) {
        return span.start == start && span.length == length && span.style.role == role &&
               span.style.script == script;
    };

    // Знак — свой прогон стиля: надстрочный, с ролью знака; текст вокруг — с
    // ролью текста.
    const std::span<const typography::StyleSpan> plain = spansOf(0);
    check(book.textOf(blocks[0].paragraph) == L"слово1 дальше" && plain.size() == 3 &&
              isSpan(plain[0], 0, 5, TextRole::Body, 0) &&
              isSpan(plain[1], 5, 1, TextRole::NoteMark, 1) &&
              isSpan(plain[2], 6, 7, TextRole::Body, 0),
          "знак сноски — свой прогон стиля: надстрочный, роль знака");

    // `<sup>` вплотную к знаку набран так же, но значит другое: прогоны два,
    // и роль знака — только у знака.
    const std::span<const typography::StyleSpan> raised = spansOf(1);
    check(book.textOf(blocks[1].paragraph) == L"x23y" && raised.size() == 4 &&
              isSpan(raised[1], 1, 1, TextRole::Body, 1) &&
              isSpan(raised[2], 2, 1, TextRole::NoteMark, 1),
          "<sup> вплотную к знаку — отдельный прогон с ролью текста");

    // Два знака вплотную — начертание одно, но прогонов два: знак с якорем
    // начинает свой. Якорей тоже два, каждый на свою цифру.
    const std::span<const typography::StyleSpan> paired = spansOf(2);
    const std::span<const typography::NoteAnchor> pairedMarks =
        book.notesOf(blocks[2].paragraph);
    check(book.textOf(blocks[2].paragraph) == L"z45w" && paired.size() == 4 &&
              isSpan(paired[1], 1, 1, TextRole::NoteMark, 1) &&
              isSpan(paired[2], 2, 1, TextRole::NoteMark, 1) && pairedMarks.size() == 2 &&
              pairedMarks[0].position == 1 && pairedMarks[0].length == 1 &&
              pairedMarks[1].position == 2 && pairedMarks[1].length == 1 &&
              pairedMarks[0].body != pairedMarks[1].body,
          "два знака вплотную — по прогону стиля на якорь");

    // Ссылка роли не получает: перехода по ней нет, и цвет ссылки обманывал бы.
    bool linkIsText = !spansOf(3).empty();
    for (const typography::StyleSpan& span : spansOf(3))
        if (span.style.role != TextRole::Body) linkIsText = false;
    check(linkIsText, "<a> — роль текста");

    // Битый знак щелчка не держит: якоря нет, и роли знака тоже — он просто
    // надстрочный.
    const std::span<const typography::StyleSpan> broken = spansOf(4);
    check(book.textOf(blocks[4].paragraph) == L"u6v" && broken.size() == 3 &&
              isSpan(broken[1], 1, 1, TextRole::Body, 1) &&
              book.notesOf(blocks[4].paragraph).empty(),
          "знак без тела — надстрочный с ролью текста, без якоря");

    // До прогона глифов роль доходит без потерь: роль знака — ровно у тех
    // прогонов, что лежат в знаке.
    typography::ParagraphStyle ragged;
    ragged.alignment = typography::Alignment::Left;
    const auto rolesFollowMarks = [&](size_t block) {
        const typography::Paragraph& paragraph = blocks[block].paragraph;
        const auto lines = engine.layout(book, paragraph, 1000.0f, ragged);
        bool follows = lines.size() == 1;
        for (const typography::Line& line : lines) {
            for (const typography::GlyphRun& run : line.runs) {
                bool inMark = false;
                for (const typography::NoteAnchor& note : book.notesOf(paragraph))
                    if (run.textStart < note.position + note.length &&
                        note.position < run.textStart + run.textLength)
                        inMark = true;
                if (inMark != (run.style.role == TextRole::NoteMark)) follows = false;
            }
        }
        return follows;
    };
    check(rolesFollowMarks(0) && rolesFollowMarks(1) && rolesFollowMarks(2) &&
              rolesFollowMarks(3) && rolesFollowMarks(4) && rolesFollowMarks(5) &&
              rolesFollowMarks(7) && rolesFollowMarks(8) && rolesFollowMarks(9) &&
              rolesFollowMarks(10) && rolesFollowMarks(11) && rolesFollowMarks(12) &&
              rolesFollowMarks(13) && rolesFollowMarks(14),
          "роль знака — ровно у прогонов глифов, лежащих в знаке с телом");

    const auto raisedLines = engine.layout(book, blocks[1].paragraph, 1000.0f, ragged);
    bool supApart = false;
    for (const typography::Line& line : raisedLines)
        for (const typography::GlyphRun& run : line.runs)
            if (run.textStart == 1 && run.textLength == 1 && run.style.role == TextRole::Body &&
                run.style.script == 1)
                supApart = true;
    check(supApart, "<sup> вплотную к знаку — свой прогон глифов с ролью текста");

    // Свои прогоны — свои места на строке: знаки стоят рядом, не налезая друг
    // на друга, и щелчок в середину каждого берёт его, хотя зоны щелчка у них
    // перекрываются.
    const auto pairedLines = engine.layout(book, blocks[2].paragraph, 1000.0f, ragged);
    bool apart = pairedLines.size() == 1 && pairedLines[0].notes.size() == 2;
    if (apart) {
        const typography::Line& line = pairedLines[0];
        const typography::PlacedNote& first = line.notes[0];
        const typography::PlacedNote& second = line.notes[1];
        const float reach = ragged.fontSize * 0.33f;
        apart = first.width > 0.0f && second.width > 0.0f && first.body != second.body &&
                first.x + first.width <= second.x + 1e-3f &&
                typography::noteUnder(line, first.x + first.width * 0.5f, reach) == &first &&
                typography::noteUnder(line, second.x + second.width * 0.5f, reach) == &second &&
                typography::noteUnder(line, second.x + second.width + 2.0f * reach, reach) ==
                    nullptr;
    }
    check(apart, "два знака вплотную — два места на строке, и щелчок различает их");

    // Картинка внутри знака сменила блок, пока знак набирался: якорь выброшен,
    // и роли знака не достаётся ни тексту до картинки, ни тексту после.
    bool orphanIsText = blocks[6].kind == typography::BlockKind::Image &&
                        book.textOf(blocks[5].paragraph) == L"ef" &&
                        book.textOf(blocks[7].paragraph) == L"gh";
    for (const size_t block : {size_t{5}, size_t{7}}) {
        if (!book.notesOf(blocks[block].paragraph).empty()) orphanIsText = false;
        for (const typography::StyleSpan& span : spansOf(block))
            if (span.style.role != TextRole::Body) orphanIsText = false;
    }
    check(orphanIsText, "знак с картинкой внутри — ни якоря, ни прогона с ролью знака");

    // Знаки, которые ставит вёрстка (пустой `<note/>`): цифры по порядку
    // книги, у каждого с якорем — свой прогон с ролью знака.
    const auto anchorIs = [&](size_t block, size_t index, uint32_t position, uint32_t length) {
        const std::span<const typography::NoteAnchor> anchors =
            book.notesOf(blocks[block].paragraph);
        return index < anchors.size() && anchors[index].position == position &&
               anchors[index].length == length;
    };
    const std::span<const typography::StyleSpan> placedMark = spansOf(8);
    check(book.textOf(blocks[8].paragraph) == L"a1b" && placedMark.size() == 3 &&
              isSpan(placedMark[1], 1, 1, TextRole::NoteMark, 1) && anchorIs(8, 0, 1, 1),
          "знак вёрстки — свой прогон с ролью знака и якорь на нём");

    const std::span<const typography::StyleSpan> placedPair = spansOf(9);
    check(book.textOf(blocks[9].paragraph) == L"c23d" && placedPair.size() == 4 &&
              isSpan(placedPair[1], 1, 1, TextRole::NoteMark, 1) &&
              isSpan(placedPair[2], 2, 1, TextRole::NoteMark, 1) && anchorIs(9, 0, 1, 1) &&
              anchorIs(9, 1, 2, 1),
          "два знака вёрстки подряд — по прогону и по якорю на каждый");

    // Знак вёрстки встаёт перед первой буквой знака с текстом за ним — уже
    // после входа в этот `<note>`. Якорь второго знака отсчитывается от его
    // собственной первой буквы, за знаком вёрстки: якоря не перекрываются, и
    // каждый накрывает ровно свой текст. Разрез зовущего после знаков вёрстки
    // возвращается, и текст второго знака начинает свой прогон.
    const std::span<const typography::StyleSpan> beforeText = spansOf(10);
    check(book.textOf(blocks[10].paragraph) == L"e49f" && beforeText.size() == 4 &&
              isSpan(beforeText[1], 1, 1, TextRole::NoteMark, 1) &&
              isSpan(beforeText[2], 2, 1, TextRole::NoteMark, 1) &&
              book.notesOf(blocks[10].paragraph).size() == 2 && anchorIs(10, 0, 1, 1) &&
              anchorIs(10, 1, 2, 1),
          "знак вёрстки перед знаком с текстом — свои прогоны и якоря встык, без перекрытия");

    // Перед знаком с текстом — битый пустой `<note/>`: его цифра — без якоря и
    // без роли знака, а у второго знака якорь — только на своём тексте. Без
    // возвращённого разреза текст второго знака слился бы с цифрой битого.
    const std::span<const typography::StyleSpan> afterBroken = spansOf(11);
    check(book.textOf(blocks[11].paragraph) == L"j59k" && afterBroken.size() == 4 &&
              isSpan(afterBroken[1], 1, 1, TextRole::Body, 1) &&
              isSpan(afterBroken[2], 2, 1, TextRole::NoteMark, 1) &&
              book.notesOf(blocks[11].paragraph).size() == 1 && anchorIs(11, 0, 2, 1),
          "битый знак вёрстки перед знаком с текстом — его цифра без роли, якорь только у текста");

    // Знак с разметкой внутри — несколько прогонов, и роль у каждого.
    const std::span<const typography::StyleSpan> styledMark = spansOf(12);
    check(book.textOf(blocks[12].paragraph) == L"h42i" && styledMark.size() == 4 &&
              isSpan(styledMark[1], 1, 1, TextRole::NoteMark, 1) && styledMark[1].style.italic &&
              isSpan(styledMark[2], 2, 1, TextRole::NoteMark, 1) && !styledMark[2].style.italic &&
              anchorIs(12, 0, 1, 2),
          "<em> внутри знака — роль у обоих его прогонов");

    // Знак в конце абзаца ставит закрытие блока: буквы за ним уже не будет.
    const std::span<const typography::StyleSpan> closing = spansOf(13);
    check(book.textOf(blocks[13].paragraph) == L"g6" && closing.size() == 2 &&
              isSpan(closing[1], 1, 1, TextRole::NoteMark, 1) && anchorIs(13, 0, 1, 1),
          "знак вёрстки в конце абзаца — прогон с ролью знака и якорь");

    // Знак внутри знака до первой буквы внешнего: буква внутреннего внешнему
    // не своя, и внешний начинается со своей первой буквы, за внутренним, —
    // якоря не перекрываются. Якоря идут в порядке ссылок: внешний первым.
    const std::span<const typography::StyleSpan> nested = spansOf(14);
    check(book.textOf(blocks[14].paragraph) == L"t87u" && nested.size() == 4 &&
              isSpan(nested[1], 1, 1, TextRole::NoteMark, 1) &&
              isSpan(nested[2], 2, 1, TextRole::NoteMark, 1) &&
              book.notesOf(blocks[14].paragraph).size() == 2 && anchorIs(14, 0, 2, 1) &&
              anchorIs(14, 1, 1, 1),
          "знак внутри знака до его первой буквы — внешний со своей буквы, без перекрытия");
}

/// Пробел в конце абзаца, который свёртка не тронула (так бывает в
/// преформате), — место разрыва, и разбивка берёт его, когда строка с ним
/// вылезает за поле, а без него помещается. Разрыв пробел съедает, но строки
/// за концом текста при этом не бывает: последней остаётся та, что кончилась
/// на пробеле.
void testTrailingSpace(typography::Engine& engine) {
    std::printf("\n=== пробел в конце абзаца ===\n");

    typography::ParagraphStyle ragged;
    ragged.alignment = typography::Alignment::Left;

    // Цифры: ни переноса, ни разрыва между ними нет, и слово — один бокс.
    const float word = layoutOf(engine, paragraphOf(u"12345"), 1000.0f, ragged).front().width;
    const typography::BookText spaced = paragraphOf(u"12345    ");
    const auto lines = layoutOf(engine, spaced, word + 1.0f, ragged);

    std::printf("  слово %.1f, полоса %.1f, строк %zu\n", word, word + 1.0f, lines.size());
    check(lines.size() == 1 && lines[0].textStart == 0 && lines[0].textLength == 5 &&
              lines[0].charOffset == 0,
          "одна строка — слово, хвостовой пробел съеден разрывом");

    bool inside = true;
    for (const typography::Line& line : lines)
        if (line.textStart >= onlyParagraph(spaced).text.count) inside = false;
    check(inside, "строки за концом текста нет");
}

/// Сумма шагов первых count глифов строки — по порядку прогонов: так стоит
/// отступ, у которого на каждый пробел свой глиф.
float leadingAdvance(const typography::Line& line, size_t count) {
    float advance = 0.0f;
    for (const typography::GlyphRun& run : line.runs) {
        for (const float glyphAdvance : run.advances) {
            if (count == 0) return advance;
            advance += glyphAdvance;
            --count;
        }
    }
    return advance;
}

/// Отступ строки — пробелы, с которых она начинается в начале абзаца или за
/// обязательным разрывом, — разбивка мерит так же, как нарезка строк рисует,
/// а выключка его не тянет. Нарезка начинает строку за разрывом
/// с отступа, и разбивка, съевшая бы его как клей, выпустила бы строку за
/// полосу на его ширину — так бывает у строки преформата с табуляциями перед
/// «Предварительно» в n_3 nightmare_example на колонке сноски 260. Выключка,
/// которой отдан отступ, увела бы в него почти весь запас строки с длинным
/// отступом и парой пробелов между словами. Отступ бывает не только в
/// преформате: свёртка складывает лишь пробелы ASCII, и в
/// начале абзаца и за `<br/>` доходят неразрывный пробел, круглая шпация и
/// прочие пробелы Юникода. Полоса — та, что у сноски: по формату, кегль 17,
/// без абзацного отступа.
void testIndentAfterBreak(typography::Engine& engine) {
    std::printf("\n=== отступ строки ===\n");

    typography::ParagraphStyle justified;
    justified.fontSize = 17.0f;
    justified.lineHeight = 1.35f;
    typography::ParagraphStyle ragged = justified;
    ragged.alignment = typography::Alignment::Left;
    constexpr float kColumn = 260.0f;
    constexpr float kSlack = 0.15f;

    const auto insideColumn = [&](std::span<const typography::Line> lines, float column) {
        bool inside = !lines.empty();
        for (const typography::Line& line : lines) {
            const PenReach reach = penReach(line);
            if (reach.left < -kSlack || reach.right > column + kSlack) inside = false;
        }
        return inside;
    };
    const auto widest = [](std::span<const typography::Line> lines) {
        float reached = 0.0f;
        for (const typography::Line& line : lines) reached = std::max(reached, penReach(line).right);
        return reached;
    };
    const auto indentOf = [](char32_t code, int count) {
        u16_text indent;
        for (int at = 0; at < count; ++at) indent.push_back(code);
        return indent;
    };

    // Отступ за переводом строки — в 12 табуляций, как у преформата в глубине
    // разметки, в 24 пробела, в 6 неразрывных пробелов и в 2 круглые шпации, —
    // и он же в начале абзаца. За ним — текст на несколько строк: строка с
    // отступом выключается по формату, и выключка его не тянет — он не шире,
    // чем в той же строке на полосе, где ничего не выключается и не сжимается.
    struct Indent {
        const char* name;
        char32_t code;
        int count;
    };
    for (const Indent& indent : {Indent{"12 табуляций", U'\t', 12}, Indent{"24 пробела", U' ', 24},
                                 Indent{"6 неразрывных пробелов", U'\x00A0', 6},
                                 Indent{"2 круглые шпации", U'\x2003', 2}}) {
        for (const bool afterBreak : {true, false}) {
            u16_text indentedText;
            if (afterBreak) indentedText += u"Строка\n";
            indentedText += indentOf(indent.code, indent.count);
            indentedText += u"Предварительно форматированный текст, заголовок и ещё слова до края";
            const typography::BookText indented = paragraphOf(indentedText);
            const auto lines = layoutOf(engine, indented, kColumn, justified);
            const auto naturalLines = layoutOf(engine, indented, 10000.0f, ragged);
            const size_t line = afterBreak ? 1 : 0;
            const uint32_t indentStart = afterBreak ? 7 : 0;

            const bool found = lines.size() > line + 1 && naturalLines.size() > line;
            const float justifiedIndent =
                found ? leadingAdvance(lines[line], static_cast<size_t>(indent.count)) : 0.0f;
            const float naturalIndent =
                found ? leadingAdvance(naturalLines[line], static_cast<size_t>(indent.count)) : 0.0f;
            std::printf("  %s %s: строк %zu, самая широкая %.2f; отступ по формату %.2f, сам по себе %.2f\n",
                        indent.name, afterBreak ? "за переводом строки" : "в начале абзаца", lines.size(),
                        widest(lines), justifiedIndent, naturalIndent);
            check(insideColumn(lines, kColumn), "строка с отступом — внутри полосы");
            check(found && lines[line].textStart == indentStart,
                  "отступ остаётся в строке: она начинается с его первого знака");
            check(found && justifiedIndent <= naturalIndent + 0.01f, "выключка отступ не тянет");
        }
    }

    // Отступ почти во всю полосу и короткое слово за ним — строка шире полосы
    // на полтора процента. Другого решения у разбивки нет: строку из одного
    // отступа нечем растянуть. Проба сторожит, что отступ не исключён из
    // сжатия: DirectWrite сжимает и буквы, и, если отступу сжиматься нельзя,
    // всё переполнение ложится на три буквы слова — на машине они выходили
    // 0,86 своей ширины, а со сжимаемым отступом остаются около 1,0 (замер на машине: 0,996).
    // Порог — не уже 0,92.
    {
        const u16_view shortWord = u"сон";
        u16_text spacedText;
        spacedText += u"а\n";
        spacedText += indentOf(U' ', 60);
        spacedText += shortWord;
        const typography::BookText spaced = paragraphOf(spacedText);
        const auto naturalLines = layoutOf(engine, spaced, 10000.0f, ragged);
        const float naturalWidth = naturalLines.size() == 2 ? naturalLines[1].width : 0.0f;
        const float column = naturalWidth / 1.015f;
        const auto spacedLines = layoutOf(engine, spaced, column, justified);

        // Шаги глифов строки по порядку прогонов: отступ — первые 60, за ними буквы.
        const auto advancesOf = [](const typography::Line& line) {
            std::vector<float> advances;
            for (const typography::GlyphRun& run : line.runs)
                advances.insert(advances.end(), run.advances.begin(), run.advances.end());
            return advances;
        };
        bool lettersKept = spacedLines.size() == 2 && naturalLines.size() == 2;
        float narrowest = 1.0f;
        float narrowestAdvance = 0.0f;   // шаг самой сжатой буквы, DIP — для отчёта
        if (lettersKept) {
            const std::vector<float> squeezed = advancesOf(spacedLines[1]);
            const std::vector<float> natural = advancesOf(naturalLines[1]);
            lettersKept = squeezed.size() == natural.size() && natural.size() > 60;
            for (size_t glyph = 60; lettersKept && glyph < natural.size(); ++glyph) {
                if (natural[glyph] <= 0.0f) continue;
                const float ratio = squeezed[glyph] / natural[glyph];
                if (ratio < narrowest) {
                    narrowest = ratio;
                    narrowestAdvance = squeezed[glyph];
                }
            }
            lettersKept = lettersKept && narrowest >= 0.92f;
        }
        std::printf("  отступ почти во всю полосу: строка %.2f на полосе %.2f, строк %zu, буквы — от %.3f своей ширины (шаг %.2f)\n",
                    naturalWidth, column, spacedLines.size(), narrowest, narrowestAdvance);
        check(lettersKept && insideColumn(spacedLines, column),
              "строку с отступом почти во всю полосу сжатие берёт вместе с отступом: буквы не уже 0,92 своей ширины");
    }

    // Отступ шире полосы рубится, как слово шире полосы, — по буквам. Что он
    // и правда шире, проверяется его естественной шириной: при узкой
    // табуляции проба прошла бы впустую. Шире полосы — значит, строк не меньше
    // трёх: «а», хотя бы один кусок отступа и строка, где он кончается.
    u16_text wideText;
    wideText += u"а\n";
    wideText += indentOf(U'\t', 40);
    wideText += u"текст дальше";
    const typography::BookText wide = paragraphOf(wideText);
    const auto wideNatural = layoutOf(engine, wide, 10000.0f, ragged);
    const float wideIndent = wideNatural.size() == 2 ? leadingAdvance(wideNatural[1], 40) : 0.0f;
    const auto wideLines = layoutOf(engine, wide, kColumn, justified);
    std::printf("  отступ шире полосы: сам по себе %.2f, строк %zu, самая широкая %.2f\n", wideIndent,
                wideLines.size(), widest(wideLines));
    check(wideIndent > kColumn && wideLines.size() >= 3, "отступ из 40 табуляций шире полосы");
    check(insideColumn(wideLines, kColumn) && linesStartAtLetters(wideText, wideLines, "отступ шире полосы"),
          "отступ шире полосы разрублен по буквам, строки внутри полосы");

    // Пробел — основа знака за переводом строки: отступ из одного него и
    // слово за знаком. Полоса — между шириной слова и шириной слова с пробелом:
    // без пробела слово вошло бы, с ним — нет, и место разрыва между отступом и
    // словом было бы разрывом посреди буквы. Строка выходит шире полосы на долю
    // пробела, и её снимает равномерное сжатие.
    const u16_view markedWord = u"ббббббббб";
    const u16_view markedText = u"а\n \x0301ббббббббб";
    const u16_view markedLine = u" \x0301ббббббббб";
    const float wordWidth = layoutOf(engine, paragraphOf(markedWord), 1000.0f, ragged).front().width;
    const float markedWidth = layoutOf(engine, paragraphOf(markedLine), 1000.0f, ragged).front().width;
    const float between = (wordWidth + markedWidth) * 0.5f;
    const typography::BookText marked = paragraphOf(markedText);
    const auto markedLines = layoutOf(engine, marked, between, justified);
    std::printf("  пробел — основа знака: слово %.2f, с пробелом %.2f, полоса %.2f, строк %zu\n", wordWidth,
                markedWidth, between, markedLines.size());
    check(markedWidth > wordWidth && markedLines.size() == 2 && markedLines[1].textStart == 2 &&
              linesStartAtLetters(markedText, markedLines, "пробел — основа знака") &&
              insideColumn(markedLines, between),
          "отступ из пробела — основы знака не отделяется от знака, и строка внутри полосы");

    // Пробел — основа знака посреди строки: разрыв перед знаком разрешён
    // правилами переноса строк (UAX #14, LB10), а начать строку со знака без
    // основы значит разорвать букву. Других мест разрыва здесь нет, и строка
    // остаётся одна, шире полосы: буква важнее полосы.
    const u16_view midText = u"аааааа \x0301бббб";
    const auto midLines = layoutOf(engine, paragraphOf(midText), 60.0f, typography::ParagraphStyle{});
    std::printf("  знак за пробелом посреди строки: строк %zu, самая широкая %.2f\n", midLines.size(),
                widest(midLines));
    check(linesStartAtLetters(midText, midLines, "знак за пробелом"),
          "строка не начинается со знака, основа которого — пробел на прежней строке");

    // Растягивает ли DirectWrite табуляцию посреди выключенной строки — не
    // проверка, а сведение для читающего отчёт: у отступа возможности растяжения
    // сняты, и ответ на нём не виден.
    const u16_view tabbedText = u"аа\tбб вв гг дд ее жж зз ии кк лл мм нн оо пп рр сс тт уу фф хх цц чч";
    const typography::BookText tabbed = paragraphOf(tabbedText);
    const auto tabbedJustified = layoutOf(engine, tabbed, 150.0f, justified);
    const auto tabbedRagged = layoutOf(engine, tabbed, 150.0f, ragged);
    const auto tabWidth = [](std::span<const typography::Line> lines) {
        if (lines.empty()) return -1.0f;
        for (const typography::GlyphRun& run : lines.front().runs)
            if (run.textStart == 2 && run.textLength == 1) return run.width;
        return -1.0f;
    };
    std::printf("  табуляция посреди первой строки: влево %.2f, по формату %.2f (-1 — не отдельный прогон)\n",
                tabWidth(tabbedRagged), tabWidth(tabbedJustified));
}

/// Строка, кончившаяся обязательным разрывом (`<br/>` свёртка делает
/// переводом строки), набирается как последняя: выключка по формату не тянет
/// её до полосы, а сжимает, только если она переполнена. Короткая она по
/// смыслу — перед разрывом стоит клей бесконечной растяжимости, и разбивка
/// так её и считает; растяни её выключка — пробелы в ней разъехались бы, как в
/// строке преформата в сноске nightmare.
void testLineBeforeForcedBreak(typography::Engine& engine) {
    std::printf("\n=== строка до обязательного разрыва ===\n");

    typography::ParagraphStyle justified;   // по формату — по умолчанию
    typography::ParagraphStyle ragged;
    ragged.alignment = typography::Alignment::Left;

    // Полоса широкая: обе строки в ней коротки, и строку, дотянутую до края,
    // видно сразу. В первой строке пробел между словами — выключке есть что
    // раздвинуть, и строка, отданная ей, вышла бы шире естественной.
    constexpr float kWidth = 1000.0f;
    const typography::BookText book = paragraphOf(u"одно слово\nдальше текст абзаца");

    // Естественная ширина — у тех же строк, набранных влево: их выключка не
    // трогает, а переломы у обоих наборов одни — обязательный разрыв и конец
    // абзаца.
    const auto lines = layoutOf(engine, book, kWidth, justified);
    const auto natural = layoutOf(engine, book, kWidth, ragged);
    const bool sameBreaks = lines.size() == 2 && natural.size() == 2 &&
                            lines[0].textStart == 0 && lines[1].textStart == natural[1].textStart;

    if (sameBreaks)
        std::printf("  по формату %.1f и %.1f, влево %.1f и %.1f, полоса %.0f\n", lines[0].width,
                    lines[1].width, natural[0].width, natural[1].width, kWidth);
    // Различает ширина: выключка дотянула бы строку до полосы. Начало у полосы
    // не различает — растянутая по формату строка начинается там же, — это
    // сведение.
    check(sameBreaks && std::abs(lines[0].width - natural[0].width) < 0.01f &&
              lines[0].width < kWidth * 0.5f,
          "строка до обязательного разрыва — естественной ширины");
    check(sameBreaks && !lines[0].runs.empty() && lines[0].runs.front().originX == 0.0f,
          "строка до обязательного разрыва — у начала полосы");
    check(sameBreaks && std::abs(lines[1].width - natural[1].width) < 0.01f,
          "последняя строка абзаца — как была");

    // Переполненная строка до обязательного разрыва сжимается в полосу: полоса
    // на 0,3 % уже её естественной ширины, и разбивке выгоднее сжать три
    // пробела на доли пикселя, чем рвать раньше и тянуть оставшиеся до края.
    const typography::BookText tight = paragraphOf(u"одно слово два три\nдальше текст абзаца");
    const auto tightNatural = layoutOf(engine, tight, kWidth, ragged);
    const float tightWidth = tightNatural.size() == 2 ? tightNatural[0].width : 0.0f;
    const float tightColumn = tightWidth / 1.003f;
    const auto tightLines = layoutOf(engine, tight, tightColumn, justified);
    const bool wholeLine = tightNatural.size() == 2 && tightLines.size() == 2 &&
                           tightLines[1].textStart == tightNatural[1].textStart;

    std::printf("  переполненная: естественная %.2f, полоса %.2f, строк %zu, первая %.2f\n", tightWidth,
                tightColumn, tightLines.size(), tightLines.empty() ? 0.0f : tightLines[0].width);
    check(wholeLine && tightLines[0].width <= tightColumn + 0.05f && tightLines[0].width < tightWidth,
          "переполненная строка до обязательного разрыва сжата в полосу");
}

/// Тела сносок настоящих книг в хранилище: лежат за потоком чтения по
/// порядку документа, глав не открывают, у каждого якоря непустое тело, и
/// тело верстается тем же хранилищем, без разбора заново.
void testNoteBodies(typography::Engine& engine, const std::filesystem::path& testdata) {
    std::printf("\n=== тела сносок в хранилище ===\n");

    // Возвращает число якорей во всей книге — и в потоке, и в самих телах.
    const auto checkBook = [&](const char* name, size_t expectedBodies) -> size_t {
        const std::filesystem::path path = testdata / name;
        if (!std::filesystem::exists(path)) {
            std::printf("  нет %s: пропущено\n", name);
            return 0;
        }

        const fb3::Document document(path);
        const typography::BookText book = typography::flatten(document);

        size_t anchors = 0;
        bool anchorsResolve = true;
        for (const typography::Block& block : book.blocks)
            for (const typography::NoteAnchor& anchor : book.notesOf(block.paragraph)) {
                ++anchors;
                if (anchor.body >= book.noteBodies.size() ||
                    book.noteBodies[anchor.body].count == 0)
                    anchorsResolve = false;
            }

        bool outsideChapters = true;
        for (size_t i = book.flowCount; i < book.blocks.size(); ++i)
            if (book.blocks[i].startsSection != 0) outsideChapters = false;

        // Тела идут подряд, одно за другим, сразу за потоком и до конца блоков.
        bool bodiesInOrder = true;
        uint32_t next = book.flowCount;
        for (const typography::Range& body : book.noteBodies) {
            if (body.first != next) bodiesInOrder = false;
            next = body.first + body.count;
        }
        if (next != book.blocks.size()) bodiesInOrder = false;

        std::printf("  %s: блоков %zu, в потоке %u, тел %zu, якорей %zu\n", name,
                    book.blocks.size(), book.flowCount, book.noteBodies.size(), anchors);
        check(book.noteBodies.size() == expectedBodies, "тел сносок столько, сколько в книге");
        check(book.flowCount < book.blocks.size(), "тела сносок лежат за потоком чтения");
        check(bodiesInOrder, "тела идут подряд за потоком, по порядку документа");
        check(anchors > 0 && anchorsResolve, "каждый якорь ведёт к непустому телу");
        check(outsideChapters, "блоки тел глав не открывают");

        if (!book.noteBodies.empty()) {
            const typography::NoteLayout first =
                typography::layoutNote(engine, book, book.noteBodies[0], 300.0f, 20.0f);
            check(!first.lines.empty() && notePlacedDownwards(first),
                  "первое тело верстается в строки прямо из хранилища");
        }
        return anchors;
    };

    // У «Отцов и детей» все 155 знаков — текст с телом, без картинок внутри,
    // так что якорь есть у каждого.
    const size_t turgenev = checkBook("Turgenev_I._Spisokshkolnoy._Otcyi_I_Deti.fb3", 155);
    if (turgenev > 0) check(turgenev == 155, "у каждой из 155 ссылок «Отцов и детей» свой якорь");

    checkBook("nightmare_example.fb3", 6);
}

/// Ссылки на сноски из испорченного файла. Номер тела ставит разбор FB3 по id
/// из файла, и ссылка на id, которого среди тел нет, — на секцию, на пустой
/// id, без href вовсе — остаётся без номера: книга открывается, а знак такой
/// ссылки — надстрочный текст без якоря. Два тела с одним id — ссылка ведёт к
/// первому, второе остаётся в списке тел. Номер, который дошёл до якоря, —
/// всегда тело из списка, и оно верстается.
void testBrokenNoteRefs(typography::Engine& engine) {
    std::printf("\n=== битые ссылки на сноски ===\n");

    const std::string body = R"(<?xml version="1.0" encoding="UTF-8"?>
<fb3-body xmlns="http://www.fictionbook.org/FictionBook3/body" xmlns:xlink="http://www.w3.org/1999/xlink" id="00000000-0000-0000-0000-000000000006">
<section id="s1">
<p>a<note href="#nowhere" xlink:role="footnote">1</note>b</p>
<p>c<note xlink:role="footnote">2</note>d</p>
<p>e<note href="#" xlink:role="footnote">3</note>f</p>
<p>g<note href="#s1" xlink:role="footnote">4</note>h</p>
<p>i<note href="#twice" xlink:role="footnote">5</note>j</p>
<p>k<note href="#n2" xlink:role="footnote"/>l</p>
</section>
<notes show="0">
<notebody id="twice"><p>первое</p></notebody>
<notebody id="twice"><p>второе</p></notebody>
<notebody><p>без id</p></notebody>
<notebody id="n2"><p>тело с битой ссылкой<note href="#gone" xlink:role="footnote">6</note> внутри</p></notebody>
</notes>
</fb3-body>)";
    auto const document = fb3::Document{fb3Of(body)};

    // Разбор: у каждой ссылки номер либо пуст, либо — тело из списка; пуст он
    // ровно у тех, чьего id среди тел нет.
    size_t refs = 0;
    size_t numbered = 0;
    bool inList = true;
    const auto walk = [&](const fb3::Node& node, auto& self) -> void {
        if (const fb3::NoteRefData* data = node.noteRef()) {
            ++refs;
            if (data->body) {
                ++numbered;
                if (*data->body >= document.noteBodies().size()) inList = false;
            }
        }
        for (const fb3::Node& child : node.children()) self(child, self);
    };
    // Тела сносок — тоже в дереве тела книги, так что обход одного корня
    // видит и ссылку внутри тела.
    walk(document.body(), walk);

    check(document.noteBodies().size() == 4, "тела все четыре: и с одним id на двоих, и без id");
    check(refs == 7 && numbered == 2 && inList,
          "номер — у двух ссылок с телом, у пяти битых его нет, номер — тело из списка");

    // Вёрстка: битый знак — текст, якорь — только у ссылки с телом.
    const typography::BookText book = typography::flatten(document);
    const std::span<const typography::Block> blocks = book.flow();
    check(blocks.size() == 6 && book.noteBodies.size() == 4,
          "шесть абзацев потока и четыре тела за ним");
    if (blocks.size() != 6 || book.noteBodies.size() != 4) return;

    bool brokenAreText = true;
    const wchar_t* const brokenTexts[] = {L"a1b", L"c2d", L"e3f", L"g4h"};
    for (size_t i = 0; i < std::size(brokenTexts); ++i) {
        const typography::Paragraph& paragraph = blocks[i].paragraph;
        if (book.textOf(paragraph) != brokenTexts[i] || !book.notesOf(paragraph).empty())
            brokenAreText = false;
        for (const typography::StyleSpan& span : book.spansOf(paragraph))
            if (span.style.role != typography::TextRole::Body) brokenAreText = false;
    }
    check(brokenAreText, "знак ссылки без тела — текст: ни якоря, ни роли знака");

    const std::span<const typography::NoteAnchor> twice = book.notesOf(blocks[4].paragraph);
    check(book.textOf(blocks[4].paragraph) == L"i5j" && twice.size() == 1 && twice[0].body == 0,
          "id у двух тел — ссылка ведёт к первому");

    const std::span<const typography::NoteAnchor> marked = book.notesOf(blocks[5].paragraph);
    check(book.textOf(blocks[5].paragraph) == L"k1l" && marked.size() == 1 && marked[0].body == 3,
          "пустой знак с телом — знак вёрстки и якорь к своему телу");

    const typography::Range last = book.noteBodies[3];
    check(last.count == 1 &&
              book.textOf(book.blocks[last.first].paragraph) == L"тело с битой ссылкой6 внутри" &&
              book.notesOf(book.blocks[last.first].paragraph).empty(),
          "битая ссылка внутри тела — тоже текст без якоря");

    // Каждый якорь книги ведёт к телу из списка, и тело верстается.
    bool anchorsLayOut = true;
    for (const typography::Block& block : book.blocks)
        for (const typography::NoteAnchor& anchor : book.notesOf(block.paragraph)) {
            if (anchor.body >= book.noteBodies.size()) {
                anchorsLayOut = false;
                continue;
            }
            const typography::NoteLayout note =
                typography::layoutNote(engine, book, book.noteBodies[anchor.body], 300.0f, 20.0f);
            if (note.lines.empty()) anchorsLayOut = false;
        }
    check(anchorsLayOut, "каждый якорь — тело из списка, и оно верстается");
}

/// Картинка, чей src не ведёт к части, — битая ссылка, как ссылка на сноску
/// без тела: у разбора номера части у неё нет, у вёрстки нет блока — места
/// она не занимает, абзац, в котором стоит, не режет, и чужая картинка, первая
/// в книге, на её место не встаёт. Целые картинки — на своих местах.
void testBrokenImageRefs() {
    std::printf("\n=== битые ссылки на картинки ===\n");

    const std::string body = R"(<?xml version="1.0" encoding="UTF-8"?>
<fb3-body xmlns="http://www.fictionbook.org/FictionBook3/body" xmlns:xlink="http://www.w3.org/1999/xlink" id="00000000-0000-0000-0000-000000000007">
<section id="s1">
<p>до</p>
<img src="nowhere"/>
<p>между</p>
<img src="rIdB"/>
<p>x<img src="rIdLost"/>y</p>
<img src="rIdA"/>
<p>после<img src=""/></p>
<p>e<note href="#n1" xlink:role="footnote">f<img src="gone"/>g</note>h</p>
</section>
<notes show="0"><notebody id="n1"><p>тело</p></notebody></notes>
</fb3-body>)";
    auto const document = fb3::Document{fb3WithImages(body)};

    // Части — только те две, что есть в пакете; связь без части в список не идёт.
    const auto partOf = [&](std::string_view id) -> std::optional<uint32_t> {
        for (uint32_t i = 0; i < document.images().size(); ++i)
            if (document.images()[i].relationshipId == id) return i;
        return std::nullopt;
    };
    check(document.images().size() == 2 && partOf("rIdA") && partOf("rIdB") && !partOf("rIdLost"),
          "частей две, связь без части в список не вошла");

    // Разбор: номер части — только у src, за которым часть есть, и это её номер.
    size_t images = 0;
    size_t numbered = 0;
    bool ownParts = true;
    const auto walk = [&](const fb3::Node& node, auto& self) -> void {
        if (const fb3::ImageData* data = node.image()) {
            ++images;
            if (data->partIndex) {
                ++numbered;
                if (*data->partIndex != partOf(data->relationshipId.chars())) ownParts = false;
            } else if (partOf(data->relationshipId.chars())) {
                ownParts = false;
            }
        }
        for (const fb3::Node& child : node.children()) self(child, self);
    };
    walk(document.body(), walk);
    check(images == 6 && numbered == 2 && ownParts,
          "номер части — у двух картинок из шести, и он — их часть, а не первая в книге");

    // Вёрстка: блоков у битых нет, абзац с битой картинкой не разрезан.
    const typography::BookText book = typography::flatten(document);
    const std::span<const typography::Block> blocks = book.flow();
    check(blocks.size() == 7, "семь блоков: пять абзацев и две картинки");
    if (blocks.size() != 7) return;

    const auto isText = [&](size_t at, std::wstring_view text) {
        return blocks[at].kind == typography::BlockKind::Paragraph &&
               book.textOf(blocks[at].paragraph) == text;
    };
    const auto isImage = [&](size_t at, std::string_view id) {
        return blocks[at].kind == typography::BlockKind::Image && partOf(id) &&
               blocks[at].imageIndex == *partOf(id);
    };
    check(isText(0, L"до") && isText(1, L"между"),
          "src в никуда — ни блока, ни подставленной первой картинки");
    check(isImage(2, "rIdB") && isImage(4, "rIdA"), "целые картинки — на своих местах, со своими частями");
    check(isText(3, L"xy"), "битая картинка в строке абзац не режет");
    check(isText(5, L"после"), "пустой src — тоже без блока");

    // Битая картинка внутри знака сноски блока не меняет: знак цел, его якорь —
    // обе буквы вокруг картинки, и роль знака — у них.
    const std::span<const typography::NoteAnchor> marked = book.notesOf(blocks[6].paragraph);
    bool markWhole = isText(6, L"efgh") && marked.size() == 1 && marked[0].position == 1 &&
                     marked[0].length == 2 && marked[0].body == 0;
    for (const typography::StyleSpan& span : book.spansOf(blocks[6].paragraph)) {
        const bool inMark = span.start >= 1 && span.start + span.length <= 3;
        if ((span.style.role == typography::TextRole::NoteMark) != inMark) markWhole = false;
    }
    check(markWhole, "битая картинка в знаке сноски — знак цел, якорь длины 2");
}

/// Римские знаки сносок — до 3999: дальше у римских записи нет, и знак идёт
/// арабскими цифрами, а не тысячами «m».
void testRomanMarkers() {
    std::printf("\n=== римские знаки сносок ===\n");

    std::string paragraphs;
    for (int i = 0; i < 4000; ++i)
        paragraphs += R"(<p>a<note href="#n1" xlink:role="footnote" autotext="i"/></p>)";
    const std::string body = std::string(R"(<?xml version="1.0" encoding="UTF-8"?>
<fb3-body xmlns="http://www.fictionbook.org/FictionBook3/body" xmlns:xlink="http://www.w3.org/1999/xlink" id="00000000-0000-0000-0000-000000000008">
<section id="s1">)") + paragraphs + R"(</section>
<notes show="0"><notebody id="n1"><p>тело</p></notebody></notes>
</fb3-body>)";
    auto const document = fb3::Document{fb3Of(body)};
    const typography::BookText book = typography::flatten(document);
    const std::span<const typography::Block> blocks = book.flow();

    check(blocks.size() == 4000, "четыре тысячи абзацев");
    if (blocks.size() != 4000) return;
    check(book.textOf(blocks[3].paragraph) == L"aiv" &&
              book.textOf(blocks[3998].paragraph) == L"ammmcmxcix",
          "знаки по 3999 — римские");
    check(book.textOf(blocks[3999].paragraph) == L"a4000", "четырёхтысячный — арабскими");
}

}  // namespace

int main() {
    // Без буфера: тест, упавший в глубине COM или чужого кода, обязан
    // оставить на экране всё, что успел сказать, — иначе падение немое.
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    Microsoft::WRL::ComPtr<IDWriteFactory> dwrite;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(dwrite.GetAddressOf())))) {
        std::printf("FAILED не удалось создать фабрику DirectWrite\n");
        return 1;
    }

    typography::Engine engine(dwrite.Get());

    testFormulas(dwrite.Get());
    testSeparatorAtPageBottom(engine);
    testClustersStayWhole(engine);
    testCharOffsets();
    testNoteMarkRole(engine);
    testBrokenNoteRefs(engine);
    testBrokenImageRefs();
    testRomanMarkers();
    testHyphenation();
    testHyphenationRules();
    testHyphenationInLayout(engine);
    testHyphenShift(engine);
    testTrailingSpace(engine);
    testIndentAfterBreak(engine);
    testLineBeforeForcedBreak(engine);

    const std::filesystem::path testdata{BUKVITSA_TESTDATA_DIR};
    testHyphenSeries(engine, testdata);
    shootHyphenation(engine, testdata);
    testNoteLayout(engine, testdata);
    testNoteBodies(engine, testdata);
    testNotePensInsideColumn(engine, testdata);

    for (const char* name : {"anathomy_tutorial_example.fb3", "nightmare_example.fb3",
                             "hardcore_file_structure.fb3",
                             "Prokofev_R._Stellar9._Stellar_Prometeyi67901532.fb3"}) {
        const std::filesystem::path path = testdata / name;

        if (!std::filesystem::exists(path)) {
            std::printf("\n=== %s: нет файла, пропущен ===\n", name);
            continue;
        }

        try {
            testBook(engine, path);
        } catch (const std::exception& error) {
            std::printf("FAILED %s: %s\n", name, error.what());
            ++failures;
        }
    }

    std::printf("\n%s\n", failures == 0 ? "OK" : "ЕСТЬ ОШИБКИ");
    return failures;
}
