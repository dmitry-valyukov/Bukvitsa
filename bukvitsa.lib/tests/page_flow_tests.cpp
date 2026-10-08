// Тесты потока страниц (PageFlow) на настоящей книге: колонки по мере строки и
// гистерезис растяжки, место чтения сквозь перевёрстку, лента колонок через
// стык глав и у краёв книги, прыжок в далёкую главу, хвост главы и «из M»,
// попадание в знак сноски. Окна нет: DirectWrite и вёрстка работают и так.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "check.h"

// Последним: поток страниц ведёт к импорту модулей книги и wxl.core.
#include "bukvitsa/reader/page_flow.h"

using namespace bukvitsa;
using namespace bukvitsa::reader;
using bukvitsa::reader::tests::check;
using namespace std::chrono_literals;

namespace {

using Column = PageFlow::Column;

const std::filesystem::path kNovel = std::filesystem::path(BUKVITSA_TESTDATA_DIR) /
                                     L"Turgenev_I._Spisokshkolnoy._Otcyi_I_Deti.fb3";

/// Высота полосы во всех пробах — окно ноутбука; ширина у каждой своя.
constexpr float kHeight = 900.0f;

/// Вид по умолчанию — тот, что у настроек.
constexpr float kFontSize = 20.0f;
constexpr float kLineHeight = 1.45f;
constexpr float kMargin = 0.075f;

/// Мера строки: колонку добавляют, пока строка длиннее 85 знаков и
/// пока следующая колонка не выйдет уже 43.
constexpr float kMaxLineChars = 85.0f;
constexpr float kMinLineChars = 43.0f;

std::string bytesOf(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

/// Роман, открытый заново для каждой пробы: кэш глав и текущая глава у книги
/// свои, и пробы не должны видеть друг друга через них.
std::shared_ptr<Book> openNovel() {
    std::shared_ptr<Book> book;
    try {
        book = std::make_shared<Book>(kNovel, bytesOf(kNovel), dwriteFactory());
    } catch (const std::exception&) {
        book.reset();
    }
    check(book && book->chapterCount() > 3, "роман открыт, в нём главы");
    if (book && book->chapterCount() <= 3) book.reset();
    return book;
}

/// Поток на книге с этого места, в окне такой ширины, видом по умолчанию.
void setUp(PageFlow& flow, std::shared_ptr<Book> book, uint32_t at, float width) {
    flow.open(std::move(book), at);
    flow.setStyle(kFontSize, kLineHeight, kMargin);
    flow.resize(width, kHeight);
    flow.relayout();
}

/// Первая ширина окна (шагом в 50 DIP), на которой поток выбирает столько
/// колонок; поток остаётся свёрстанным на ней. Ноль — такой ширины нет.
float widthForColumns(PageFlow& flow, int wanted) {
    for (float width = 600.0f; width <= 4000.0f; width += 50.0f) {
        flow.resize(width, kHeight);
        flow.relayout();
        if (flow.columns() == wanted) return width;
    }
    return 0.0f;
}

/// Длина главы в символах книги.
uint32_t chapterLength(const Book& book, size_t index) {
    const uint32_t end = index + 1 < book.chapterCount() ? book.chapterFirstChar(index + 1)
                                                         : book.characterCount();
    return end - book.chapterFirstChar(index);
}

/// Самая длинная глава: у неё хвост заведомо есть.
size_t longestChapter(const Book& book) {
    size_t longest = 0;
    for (size_t index = 1; index < book.chapterCount(); ++index)
        if (chapterLength(book, index) > chapterLength(book, longest)) longest = index;
    return longest;
}

/// Буква лежит в левой колонке разворота: колонка начинается не позже неё, а
/// следующая колонка той же главы — позже.
bool letterInLeftColumn(const PageFlow& flow, uint32_t letter) {
    const typography::Chapter& chapter = flow.book()->paginator();
    const size_t left = flow.page();
    if (chapter.pageCount() == 0 || chapter.page(left).firstCharOffset > letter) return false;
    return left + 1 >= chapter.pageCount() || chapter.page(left + 1).firstCharOffset > letter;
}

/// Колонки — по длине строки в знаках, ширина — по полям.
void testColumnsFollowLineLength() {
    std::printf("\n=== поток страниц: колонки по длине строки ===\n");
    const std::shared_ptr<Book> novel = openNovel();
    if (!novel) return;

    typography::Engine& engine = novel->engine();
    const float normal = engine.averageCharWidth(20.0f);
    const float large = engine.averageCharWidth(40.0f);
    check(normal > 0.0f && std::abs(large / normal - 2.0f) < 0.1f,
          "мера знака по кеглю: вдвое крупнее — вдвое шире");

    PageFlow flow;
    setUp(flow, novel, 0, 700.0f);
    check(flow.columns() == 1, "узкое окно — одна колонка");

    // Правило меры на сетке окон и кеглей: колонок ровно столько, сколько
    // велит длина строки, — не меньше и не больше.
    bool rule = true;
    for (const float size : {12.0f, 16.0f, 20.0f, 28.0f, 40.0f}) {
        flow.setStyle(size, kLineHeight, kMargin);
        for (float width = 500.0f; width <= 3200.0f; width += 150.0f) {
            flow.resize(width, kHeight);
            flow.relayout();
            const int count = flow.columns();
            const bool enough = flow.lineChars(count) <= kMaxLineChars ||
                                flow.lineChars(count + 1) < kMinLineChars;
            const bool needed = count == 1 || (flow.lineChars(count - 1) > kMaxLineChars &&
                                               flow.lineChars(count) >= kMinLineChars);
            rule = rule && enough && needed;
        }
    }
    check(rule, "колонок столько, сколько велит мера строки");

    // Та же ширина, другой кегль: мелкий набор делится на колонки раньше.
    flow.resize(1600.0f, kHeight);
    flow.setStyle(14.0f, kLineHeight, kMargin);
    flow.relayout();
    const int onFineType = flow.columns();
    flow.setStyle(28.0f, kLineHeight, kMargin);
    flow.relayout();
    check(onFineType > flow.columns(),
          "то же окно: на мелком кегле колонок больше, чем на крупном");

    // Ширину колонок задают поля: две колонки занимают всё место между ними,
    // и корешок — ровно посередине полосы.
    flow.setStyle(kFontSize, kLineHeight, kMargin);
    const float twoUp = widthForColumns(flow, 2);
    check(twoUp > 0.0f && std::abs(flow.spine() - twoUp * 0.5f) < 0.5f,
          "разворот в две колонки — корешок посередине");
}

/// Гистерезис числа колонок — на растяжке окна, и только на ней.
void testColumnHysteresis() {
    std::printf("\n=== поток страниц: гистерезис колонок при растяжке ===\n");
    const std::shared_ptr<Book> novel = openNovel();
    if (!novel) return;

    PageFlow flow;
    setUp(flow, novel, 0, 600.0f);

    // Граница одной и двух колонок — делением пополам, без гистерезиса.
    float one = 600.0f;
    float two = 3200.0f;
    bool bracket = flow.columns() == 1;
    flow.resize(two, kHeight);
    flow.relayout();
    bracket = bracket && flow.columns() >= 2;
    check(bracket, "между узким и широким окном есть граница одной и двух колонок");
    if (!bracket) return;

    while (two - one > 0.25f) {
        const float middle = (one + two) * 0.5f;
        flow.resize(middle, kHeight);
        flow.relayout();
        (flow.columns() >= 2 ? two : one) = middle;
    }

    // Окно тянут мышью туда-сюда через границу на DIP с небольшим — доли
    // знака. Перейдя на две колонки, полоса на них и держится.
    const float below = one - 1.0f;
    const float above = two + 1.0f;
    flow.resize(below, kHeight);
    flow.relayout();
    std::vector<int> seen;
    for (int pass = 0; pass < 3; ++pass) {
        flow.resize(above, kHeight);
        flow.relayout(true);
        seen.push_back(flow.columns());
        flow.resize(below, kHeight);
        flow.relayout(true);
        seen.push_back(flow.columns());
    }
    check(std::ranges::all_of(seen, [](int count) { return count == 2; }),
          "растяжка через границу меры: колонки не дёргаются на долю знака");

    // Кегль и поля просят новый вид — и получают ровно его, без гистерезиса.
    flow.relayout(false);
    check(flow.columns() == 1, "перевёрстка не от окна: та же ширина — одна колонка");

    // Сузили на знаки, а не на доли — колонка уходит и при растяжке.
    flow.resize(above, kHeight);
    flow.relayout(true);
    flow.resize(one * 0.9f, kHeight);
    flow.relayout(true);
    check(flow.columns() == 1, "окно уже на несколько знаков — одна колонка и при растяжке");
}

/// Перевёрстка не двигает место чтения: буква читателя — якорь, колонка
/// ищется по ней.
void testAnchorSurvivesRelayout() {
    std::printf("\n=== поток страниц: перевёрстка не двигает место чтения ===\n");
    const std::shared_ptr<Book> novel = openNovel();
    if (!novel) return;

    // Буква в середине длинной главы — заведомо не начало колонки.
    const size_t chapter = longestChapter(*novel);
    const uint32_t letter =
        novel->chapterFirstChar(chapter) + chapterLength(*novel, chapter) * 3 / 5;

    PageFlow flow;
    setUp(flow, novel, letter, 1400.0f);
    check(flow.anchor().chapter == chapter && flow.position() == letter &&
              letterInLeftColumn(flow, letter),
          "открыли на букве — она в левой колонке разворота");

    // Три растяжки мышью туда-обратно: прежде каждая уводила на развороты назад.
    bool kept = true;
    for (int pass = 0; pass < 3; ++pass) {
        for (const float width : {904.0f, 1508.0f, 1400.0f}) {
            flow.resize(width, kHeight);
            flow.relayout(true);
            kept = kept && flow.position() == letter && letterInLeftColumn(flow, letter);
        }
    }
    check(kept, "три растяжки — та же буква, и она в левой колонке");

    // Колонку помним по её первой букве: страницы главы перевёрстка заводит
    // заново, и указатель на прежнюю ничего не скажет.
    const size_t page = flow.page();
    const bool shown = !flow.spread().pages.empty();
    const uint32_t columnStart = shown ? flow.spread().pages[0]->firstCharOffset : 0;

    flow.setStyle(24.0f, kLineHeight, kMargin);
    flow.relayout();
    check(flow.position() == letter && letterInLeftColumn(flow, letter),
          "кегль крупнее — та же буква в левой колонке");

    flow.setStyle(kFontSize, kLineHeight, kMargin);
    flow.relayout();
    check(shown && flow.position() == letter && flow.page() == page &&
              !flow.spread().pages.empty() &&
              flow.spread().pages[0]->firstCharOffset == columnStart,
          "кегль туда и обратно — та же колонка");
}

/// Разворот на стыке глав: правая колонка — начало следующей.
void testSpreadAcrossChapters() {
    std::printf("\n=== поток страниц: разворот на стыке глав ===\n");
    const std::shared_ptr<Book> novel = openNovel();
    if (!novel) return;

    // Следующая глава — самая длинная: войдя в неё, хвост заведомо досчитывать.
    const size_t next = longestChapter(*novel);
    check(next >= 2, "перед самой длинной главой есть глава с текстом");
    if (next < 2) return;
    const size_t chapter = next - 1;

    PageFlow flow;
    setUp(flow, novel, 0, 1400.0f);
    const float width = widthForColumns(flow, 2);
    check(width > 0.0f, "есть окно под разворот в две колонки");
    if (width <= 0.0f) return;

    // Последняя колонка главы — левой: правую занимает начало следующей.
    flow.show(flow.columnOf(novel->chapterFirstChar(next) - 1));
    const PageFlow::Spread& spread = flow.spread();
    check(flow.anchor().chapter == chapter && novel->currentChapter() == chapter,
          "левая колонка — в главе перед стыком");
    check(spread.pages.size() == 2 && spread.ownColumns == 1,
          "на развороте две колонки, своя у главы одна");
    check(spread.pages.size() == 2 && spread.pages[1] == &novel->chapterAt(next).page(0),
          "правая колонка — первая страница следующей главы");
    check(flow.status().chapter == chapter && flow.status().own == 1,
          "колонцифра — по левой главе, одна своя колонка");

    const Column last = flow.anchor();
    const std::optional<Column> ahead = flow.turnTarget(true);
    check(ahead && *ahead == Column{next, 1},
          "вперёд через стык — разворот со второй колонки следующей главы");
    check(flow.anchor() == last && novel->currentChapter() == chapter,
          "выбор цели книгу не двигает");
    if (!ahead) return;

    flow.show(*ahead);
    check(novel->currentChapter() == next &&
              flow.position() == novel->chapterAt(next).page(1).firstCharOffset,
          "переворот через границу: соседняя глава — текущая, место — начало колонки");
    check(flow.needsTail() && !flow.isComplete(), "в новую главу вошли — её хвост досчитывать");

    const std::optional<Column> behind = flow.turnTarget(false);
    check(behind && *behind == last, "назад — на тот же стык");
}

/// Лента у краёв книги: дальше первого и последнего разворота листать некуда.
void testRibbonStopsAtBookEdges() {
    std::printf("\n=== поток страниц: лента у краёв книги ===\n");
    const std::shared_ptr<Book> novel = openNovel();
    if (!novel) return;

    PageFlow flow;
    setUp(flow, novel, 0, 1400.0f);
    check(!flow.turnTarget(false), "назад с начала книги — некуда");
    check(flow.turnTarget(true).has_value(), "вперёд с начала книги — есть куда");

    flow.show(flow.columnOf(novel->characterCount()));
    check(flow.anchor().chapter == novel->chapterCount() - 1, "конец книги — в последней главе");
    check(!flow.turnTarget(true), "вперёд с конца книги — некуда");
    check(flow.turnTarget(false).has_value(), "назад с конца книги — есть куда");
    check(flow.isComplete() && !flow.needsTail(),
          "последняя глава досчитана до конца — хвоста нет");
}

/// Прыжок по закладке в далёкую главу — разрыв ленты.
void testJumpToDistantChapter() {
    std::printf("\n=== поток страниц: прыжок в далёкую главу ===\n");
    const std::shared_ptr<Book> novel = openNovel();
    if (!novel) return;

    PageFlow flow;
    setUp(flow, novel, 0, 1400.0f);

    const size_t distant = novel->chapterCount() - 2;
    const uint32_t letter =
        novel->chapterFirstChar(distant) + chapterLength(*novel, distant) * 2 / 5;

    const Column target = flow.columnOf(letter);
    check(target.chapter == distant, "колонка — в той главе, где буква");

    flow.show(target);
    check(flow.anchor() == target && novel->currentChapter() == distant,
          "книга встала на эту колонку");
    check(letterInLeftColumn(flow, letter), "буква — в левой колонке разворота");
    check(!flow.spread().pages.empty() &&
              flow.position() == flow.spread().pages[0]->firstCharOffset,
          "место чтения — начало колонки, куда прыгнули");
    check(flow.needsTail(), "в новой главе хвост ещё досчитывать");
}

/// Хвост главы порциями: «из M» появляется, показанное не меняется.
void testTailCompletesChapter() {
    std::printf("\n=== поток страниц: хвост главы и «из M» ===\n");
    const std::shared_ptr<Book> novel = openNovel();
    if (!novel) return;

    const size_t chapter = longestChapter(*novel);
    PageFlow flow;
    setUp(flow, novel, novel->chapterFirstChar(chapter), 1400.0f);
    check(flow.needsTail() && !flow.isComplete() && !flow.status().total,
          "после перевёрстки глава свёрстана до разворота — «из …»");

    const uint32_t position = flow.position();
    const size_t page = flow.page();
    const typography::Page* left = flow.spread().pages.empty() ? nullptr : flow.spread().pages[0];
    const size_t before = flow.pageCount();

    int slices = 0;
    while (flow.advanceTail(50ms) && slices < 10000) ++slices;

    check(flow.isComplete() && !flow.needsTail(), "хвост досчитан порциями");
    check(flow.pageCount() > before && flow.status().total == flow.pageCount(),
          "«из M» — число страниц главы");
    check(left && flow.position() == position && flow.page() == page &&
              !flow.spread().pages.empty() && flow.spread().pages[0] == left,
          "хвост показанного не меняет");
}

/// Знак сноски ловится щелчком там, где его рисуют.
void testNoteMarkIsHit() {
    std::printf("\n=== поток страниц: попадание в знак сноски ===\n");
    const std::shared_ptr<Book> novel = openNovel();
    if (!novel) return;

    // Первый знак сноски в книге — туда и прыгаем.
    const fb3::Node* wanted = nullptr;
    uint32_t mark = 0;
    for (const typography::Block& block : novel->blocks()) {
        if (block.paragraph.notes.empty()) continue;
        const typography::NoteAnchor& note = block.paragraph.notes.front();
        if (note.position >= block.paragraph.charOffsets.size()) continue;
        wanted = note.target;
        mark = block.paragraph.charOffsets[note.position];
        break;
    }
    check(wanted != nullptr, "в романе есть сноски");
    if (!wanted) return;

    PageFlow flow;
    setUp(flow, novel, 0, 1400.0f);
    flow.show(flow.columnOf(mark));

    // Знак ищем в строках разворота, а щёлкаем в его середину на высоте строки.
    std::optional<PageFlow::Point> at;
    PageFlow::Point under;
    const PageFlow::Spread& spread = flow.spread();
    for (size_t column = 0; column < spread.pages.size() && !at; ++column) {
        for (const typography::PlacedLine& placed : spread.pages[column]->lines) {
            const typography::Line& line = *placed.line;
            const float baseline = flow.verticalMargin() + placed.baseline;
            const float x = flow.columnLeft(column) + placed.x;
            for (const typography::PlacedNote& note : line.notes) {
                if (note.target != wanted) continue;
                const float middle = x + note.x + note.width * 0.5f;
                at = PageFlow::Point{middle, baseline - line.ascent * 0.5f};
                under = PageFlow::Point{middle, baseline + line.descent};
                break;
            }
            if (at) break;
        }
    }
    check(at.has_value(), "знак сноски — на развороте, куда прыгнули");
    if (!at) return;

    const PageFlow::NoteHit hit = flow.noteAt(*at);
    check(hit.target == wanted, "щелчок по знаку — его сноска");
    check(std::abs(hit.anchor.x - under.x) < 0.5f && std::abs(hit.anchor.y - under.y) < 0.5f,
          "всплывашка встаёт под знаком");
    check(flow.noteAt(PageFlow::Point{flow.columnLeft(0) + 1.0f, 1.0f}).target == nullptr,
          "щелчок в верхнее поле — мимо");
}

}  // namespace

void runPageFlowTests() {
    testColumnsFollowLineLength();
    testColumnHysteresis();
    testAnchorSurvivesRelayout();
    testSpreadAcrossChapters();
    testRibbonStopsAtBookEdges();
    testJumpToDistantChapter();
    testTailCompletesChapter();
    testNoteMarkIsHit();
}
