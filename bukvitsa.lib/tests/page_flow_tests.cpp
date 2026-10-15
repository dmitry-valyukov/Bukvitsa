// Тесты потока страниц (PageFlow) на настоящей книге: колонки по мере строки и
// гистерезис растяжки, место чтения сквозь перевёрстку, лента колонок через
// стык глав и у краёв книги, прыжок в далёкую главу, начало главы сквозь
// перевёрстку и прыжок, переворот назад с начала главы, хвост главы и «из M»,
// попадание в знак сноски и вёрстка её тела. Окна нет: DirectWrite и вёрстка
// работают и так.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "check.h"

// Последними: поток страниц ведёт к импорту модулей книги и wxl.core, сноска —
// через те же заголовки вёрстки.
#include "bukvitsa/reader/page_flow.h"
#include "bukvitsa/typography/note.h"

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

/// Окно читалки при первом запуске. На кегле 20 и 21 в нём разворот в две
/// колонки — пробы стыка глав идут на нём.
constexpr float kWindowWidth = 1280.0f;
constexpr float kWindowHeight = 860.0f;

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
/// свои, и пробы не должны видеть друг друга через них. Документ разбора
/// живёт только на время открытия, как у рабочего места. Пусто — роман не
/// открылся, и проба кончается, не начавшись.
intrusive_ptr<Book> openNovel() {
    intrusive_ptr<Book> book;
    try {
        book = make_refcounted<Book>(kNovel, fb3::Document{bytesOf(kNovel)}, dwriteFactory());
    } catch (const std::exception&) {
        book = nullptr;
    }
    check(book && book->chapterCount() > 3, "роман открыт, в нём главы");
    if (book && book->chapterCount() <= 3) book = nullptr;
    return book;
}

/// Поток на книге с этого места, в окне такой ширины, видом по умолчанию.
void setUp(PageFlow& flow, not_null<Book> book, uint32_t at, float width) {
    flow.open(book, at);
    flow.setStyle(kFontSize, kLineHeight, kMargin);
    flow.resize(width, kHeight);
    flow.relayout();
}

/// Поток на книге с этого места в окне читалки, видом по умолчанию.
void setUpWindow(PageFlow& flow, not_null<Book> book, uint32_t at) {
    flow.open(book, at);
    flow.setStyle(kFontSize, kLineHeight, kMargin);
    flow.resize(kWindowWidth, kWindowHeight);
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

/// Буква лежит в тексте этой главы.
bool letterInChapter(const Book& book, size_t chapter, uint32_t letter) {
    const uint32_t first = book.chapterFirstChar(chapter);
    return letter >= first && letter - first < chapterLength(book, chapter);
}

/// Книга стоит на первой колонке главы: разворот начинается её первой
/// страницей, место чтения — начало этой страницы, колонцифра — по этой главе.
bool atChapterStart(const PageFlow& flow, size_t chapter) {
    const typography::Chapter& paginator = flow.book()->paginator();
    const PageFlow::Status status = flow.status();
    return flow.anchor() == Column{chapter, 0} && paginator.pageCount() != 0 &&
           flow.position() == paginator.page(0).firstCharOffset &&
           !flow.spread().pages.empty() && flow.spread().pages[0] == &paginator.page(0) &&
           status.chapter == chapter && status.first == 0;
}

/// Главы, на которых проба не сошлась, — номерами колонцифры.
void listChapters(const char* probe, const std::vector<size_t>& chapters) {
    for (const size_t chapter : chapters) std::printf("        %s: глава %zu\n", probe, chapter + 1);
}

/// Колонки — по длине строки в знаках, ширина — по полям.
void testColumnsFollowLineLength() {
    std::printf("\n=== поток страниц: колонки по длине строки ===\n");
    const intrusive_ptr<Book> novel = openNovel();
    if (!novel) return;

    typography::Engine& engine = novel->engine();
    const float normal = engine.averageCharWidth(20.0f);
    const float large = engine.averageCharWidth(40.0f);
    check(normal > 0.0f && std::abs(large / normal - 2.0f) < 0.1f,
          "мера знака по кеглю: вдвое крупнее — вдвое шире");

    PageFlow flow;
    setUp(flow, as_not_null(novel.get()), 0, 700.0f);
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
    const intrusive_ptr<Book> novel = openNovel();
    if (!novel) return;

    PageFlow flow;
    setUp(flow, as_not_null(novel.get()), 0, 600.0f);

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
    const intrusive_ptr<Book> novel = openNovel();
    if (!novel) return;

    // Буква в середине длинной главы — заведомо не начало колонки.
    const size_t chapter = longestChapter(*novel);
    const uint32_t letter =
        novel->chapterFirstChar(chapter) + chapterLength(*novel, chapter) * 3 / 5;

    PageFlow flow;
    setUp(flow, as_not_null(novel.get()), letter, 1400.0f);
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
    const intrusive_ptr<Book> novel = openNovel();
    if (!novel) return;

    // Следующая глава — самая длинная: войдя в неё, хвост заведомо досчитывать.
    const size_t next = longestChapter(*novel);
    check(next >= 2, "перед самой длинной главой есть глава с текстом");
    if (next < 2) return;
    const size_t chapter = next - 1;

    PageFlow flow;
    setUp(flow, as_not_null(novel.get()), 0, 1400.0f);
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
    const intrusive_ptr<Book> novel = openNovel();
    if (!novel) return;

    PageFlow flow;
    setUp(flow, as_not_null(novel.get()), 0, 1400.0f);
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
    const intrusive_ptr<Book> novel = openNovel();
    if (!novel) return;

    PageFlow flow;
    setUp(flow, as_not_null(novel.get()), 0, 1400.0f);

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

/// Начало главы сквозь перевёрстку: прыжок на первую букву главы ставит
/// разворот на её первую колонку, и кегль 20 → 21 оставляет его там же — на
/// той же букве. Конец предыдущей главы лежит вплотную к ней, и буква,
/// отнесённая не к той главе, показала бы слева последнюю колонку предыдущей.
/// Каждый стык романа — от титула до последней главы.
void testChapterStartSurvivesRelayout() {
    std::printf("\n=== поток страниц: начало главы сквозь перевёрстку ===\n");
    const intrusive_ptr<Book> novel = openNovel();
    if (!novel) return;

    PageFlow flow;
    setUpWindow(flow, as_not_null(novel.get()), 0);
    const bool twoUp = flow.columns() == 2;
    flow.setStyle(kFontSize + 1.0f, kLineHeight, kMargin);
    flow.relayout();
    check(twoUp && flow.columns() == 2, "окно читалки: на кегле 20 и 21 — разворот в две колонки");

    std::vector<size_t> jumped;   // прыжок встал не на первую колонку главы
    std::vector<size_t> moved;    // перевёрстка увела разворот с начала главы
    for (size_t chapter = 1; chapter < novel->chapterCount(); ++chapter) {
        flow.setStyle(kFontSize, kLineHeight, kMargin);
        flow.relayout();

        flow.show(flow.columnOf(novel->chapterFirstChar(chapter)));
        const uint32_t letter = flow.position();
        if (!atChapterStart(flow, chapter) || !letterInChapter(*novel, chapter, letter)) {
            jumped.push_back(chapter);
            continue;
        }

        flow.setStyle(kFontSize + 1.0f, kLineHeight, kMargin);
        flow.relayout();
        if (flow.position() != letter || !atChapterStart(flow, chapter)) moved.push_back(chapter);
    }
    listChapters("прыжок", jumped);
    listChapters("перевёрстка", moved);
    check(jumped.empty(), "прыжок на первую букву главы — первая колонка этой главы, на каждом стыке");
    check(moved.empty(),
          "кегль 20 → 21 на начале главы — разворот с той же буквы той же главы, на каждом стыке");
}

/// Прыжок на первую букву главы из предыдущей — в первую колонку этой главы,
/// как бы ни была досчитана предыдущая: её хвост ещё идёт, досчитан до конца,
/// или искомая глава уже лежит правой колонкой разворота на стыке.
/// Ближайшая известная колонка здесь не ответ: на стыке это последняя колонка
/// предыдущей главы.
void testJumpToChapterStartFromNeighbour() {
    std::printf("\n=== поток страниц: прыжок на начало главы из предыдущей ===\n");
    const intrusive_ptr<Book> novel = openNovel();
    if (!novel) return;

    PageFlow flow;
    setUpWindow(flow, as_not_null(novel.get()), 0);
    check(flow.columns() == 2, "окно читалки — разворот в две колонки");
    if (flow.columns() != 2) return;

    // Прыжок оглавлением: колонка по букве, книга — на неё.
    const auto jump = [&flow](uint32_t letter) { flow.show(flow.columnOf(letter)); };

    std::vector<size_t> tailing;   // предыдущая ещё досчитывается
    std::vector<size_t> counted;   // предыдущая досчитана
    std::vector<size_t> ribbon;    // глава уже на ленте — правая колонка стыка
    size_t undercounted = 0;       // стыков, где хвост предыдущей и правда не досчитан
    for (size_t chapter = 1; chapter < novel->chapterCount(); ++chapter) {
        const uint32_t start = novel->chapterFirstChar(chapter);
        const uint32_t before = novel->chapterFirstChar(chapter - 1);

        jump(before);
        if (flow.needsTail()) ++undercounted;
        jump(start);
        if (!atChapterStart(flow, chapter)) tailing.push_back(chapter);

        jump(before);
        int slices = 0;
        while (flow.advanceTail(50ms) && slices < 10000) ++slices;
        const bool complete = flow.isComplete();
        jump(start);
        if (!complete || !atChapterStart(flow, chapter)) counted.push_back(chapter);

        // Последняя буква предыдущей главы — её последняя колонка слева, начало
        // этой — справа.
        jump(start - 1);
        const bool seam = flow.anchor().chapter == chapter - 1 &&
                          flow.spread().pages.size() == 2 && flow.spread().ownColumns == 1;
        jump(start);
        if (!seam || !atChapterStart(flow, chapter)) ribbon.push_back(chapter);
    }
    listChapters("хвост идёт", tailing);
    listChapters("досчитана", counted);
    listChapters("на стыке", ribbon);
    check(undercounted > 0, "на части стыков хвост предыдущей главы ещё не досчитан");
    check(tailing.empty(), "предыдущая глава досчитывается — прыжок в первую колонку искомой");
    check(counted.empty(), "предыдущая глава досчитана — прыжок в первую колонку искомой");
    check(ribbon.empty(), "искомая глава — правая колонка стыка — прыжок в её первую колонку");
}

/// Прыжок в IX и сразу в X, пока хвост IX не досчитан: место чтения — первая
/// буква X, и роман, открытый заново на этом месте, встаёт на первый разворот
/// X — запись места в файл и открытие по ней видят ту же главу.
void testJumpThenReopen() {
    std::printf("\n=== поток страниц: прыжок и открытие заново на том же месте ===\n");
    const intrusive_ptr<Book> novel = openNovel();
    if (!novel) return;

    // Главы романа: титул, затем I, II, … — IX девятая, X десятая.
    constexpr size_t kNinth = 9;
    constexpr size_t kTenth = 10;
    check(novel->chapterCount() > kTenth, "в романе есть главы IX и X");
    if (novel->chapterCount() <= kTenth) return;

    PageFlow flow;
    setUpWindow(flow, as_not_null(novel.get()), 0);
    flow.show(flow.columnOf(novel->chapterFirstChar(kNinth)));
    check(atChapterStart(flow, kNinth) && flow.needsTail(),
          "прыжок в IX — её первый разворот, хвост ещё досчитывается");

    flow.show(flow.columnOf(novel->chapterFirstChar(kTenth)));
    const uint32_t saved = flow.position();
    check(atChapterStart(flow, kTenth) && letterInChapter(*novel, kTenth, saved),
          "сразу прыжок в X — её первый разворот, место чтения — её первая буква");

    // Открытие заново — другая книга из того же файла и свежий поток, как у
    // читалки после перезапуска.
    const intrusive_ptr<Book> again = openNovel();
    if (!again) return;
    PageFlow reopened;
    setUpWindow(reopened, as_not_null(again.get()), saved);
    check(atChapterStart(reopened, kTenth) && reopened.position() == saved,
          "открыли заново на записанном месте — первый разворот X");
}

/// Переворот назад с первого разворота главы — на целый разворот по ленте: за
/// титулом в одну страницу — на самый первый разворот книги (титул слева,
/// начало главы I справа, колонцифра по титулу), за длинной главой — на её
/// последний полный разворот, обе колонки которого её.
void testTurnBackFromChapterStart() {
    std::printf("\n=== поток страниц: назад с начала главы ===\n");
    const intrusive_ptr<Book> novel = openNovel();
    if (!novel) return;

    constexpr size_t kNinth = 9;
    check(novel->chapterCount() > kNinth, "в романе есть глава IX");
    if (novel->chapterCount() <= kNinth) return;

    PageFlow flow;
    setUpWindow(flow, as_not_null(novel.get()), 0);
    check(flow.columns() == 2, "окно читалки — разворот в две колонки");
    if (flow.columns() != 2) return;

    flow.show(flow.columnOf(novel->chapterFirstChar(1)));
    const std::optional<Column> toTitle = flow.turnTarget(false);
    check(toTitle && *toTitle == Column{0, 0}, "назад с начала главы I — самый первый разворот книги");
    if (toTitle) {
        flow.show(*toTitle);
        const PageFlow::Spread& spread = flow.spread();
        check(flow.status().chapter == 0 && flow.status().first == 0 && spread.pages.size() == 2 &&
                  spread.ownColumns == 1 && spread.pages[1] == &novel->chapterAt(1).page(0),
              "титул слева, начало главы I справа, колонцифра — по титулу");
    }

    flow.show(flow.columnOf(novel->chapterFirstChar(kNinth)));
    const std::optional<Column> toEighth = flow.turnTarget(false);
    const size_t eighth = novel->chapterAt(kNinth - 1).pageCount();
    check(eighth >= 2 && toEighth && *toEighth == Column{kNinth - 1, eighth - 2},
          "назад с начала IX — последний полный разворот VIII");
    if (toEighth) {
        flow.show(*toEighth);
        const PageFlow::Status status = flow.status();
        check(status.chapter == kNinth - 1 && status.own == 2 && status.total == eighth,
              "обе колонки — VIII, «из M» — число её страниц");
    }
}

/// Хвост главы порциями: «из M» появляется, показанное не меняется.
void testTailCompletesChapter() {
    std::printf("\n=== поток страниц: хвост главы и «из M» ===\n");
    const intrusive_ptr<Book> novel = openNovel();
    if (!novel) return;

    const size_t chapter = longestChapter(*novel);
    PageFlow flow;
    setUp(flow, as_not_null(novel.get()), novel->chapterFirstChar(chapter), 1400.0f);
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
    const intrusive_ptr<Book> novel = openNovel();
    if (!novel) return;

    // Первый знак сноски в потоке чтения — туда и прыгаем. Знак — текст
    // абзаца, как всё остальное, и его место в книге дают прогоны позиций.
    const typography::BookText& text = novel->text();
    std::optional<uint32_t> wanted;   // номер тела; пусто — знака не нашлось
    uint32_t mark = 0;
    for (const typography::Block& block : text.flow()) {
        const std::span<const typography::NoteAnchor> notes = text.notesOf(block.paragraph);
        if (notes.empty()) continue;
        wanted = notes.front().body;
        mark = text.charOffsetAt(block.paragraph, notes.front().position);
        break;
    }
    check(wanted.has_value(), "в романе есть сноски");
    if (!wanted) return;

    // Тело знака — в хранилище, за потоком чтения: всплывашке есть что верстать.
    check(*wanted < text.noteBodies.size() && text.noteBodies[*wanted].count > 0 &&
              text.noteBodies[*wanted].first >= text.flowCount,
          "у знака есть тело — блоки за потоком чтения");

    PageFlow flow;
    setUp(flow, as_not_null(novel.get()), 0, 1400.0f);
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
                if (note.body != *wanted) continue;
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

    check(!flow.noteAt(PageFlow::Point{flow.columnLeft(0) + 1.0f, 1.0f}),
          "щелчок в верхнее поле — мимо");

    const std::optional<PageFlow::NoteHit> hit = flow.noteAt(*at);
    check(hit && hit->body == *wanted, "щелчок по знаку — его сноска");
    if (!hit) return;
    check(std::abs(hit->anchor.x - under.x) < 0.5f && std::abs(hit->anchor.y - under.y) < 0.5f,
          "всплывашка встаёт под знаком");

    // Всплывашка верстает тело по номеру из щелчка тем же движком и из того же
    // хранилища — здесь то же самое без окна.
    const typography::NoteLayout body =
        typography::layoutNote(novel->engine(), text, text.noteBodies[hit->body], 300.0f, kFontSize);
    check(!body.lines.empty() && body.height > 0.0f, "тело сноски по номеру из щелчка верстается");
}

}  // namespace

void runPageFlowTests() {
    testColumnsFollowLineLength();
    testColumnHysteresis();
    testAnchorSurvivesRelayout();
    testSpreadAcrossChapters();
    testRibbonStopsAtBookEdges();
    testJumpToDistantChapter();
    testChapterStartSurvivesRelayout();
    testJumpToChapterStartFromNeighbour();
    testJumpThenReopen();
    testTurnBackFromChapterStart();
    testTailCompletesChapter();
    testNoteMarkIsHit();
}
