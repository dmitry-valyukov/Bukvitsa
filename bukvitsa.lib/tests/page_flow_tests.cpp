// Тесты потока страниц (PageFlow) на настоящей книге: колонки по мере строки и
// гистерезис растяжки, место чтения сквозь перевёрстку, лента колонок через
// стык глав и у краёв книги, прыжок в далёкую главу, начало главы сквозь
// перевёрстку и прыжок, переворот назад с начала главы, хвост главы и «из M»,
// попадание в знак сноски — первый и каждый знак романа на четырёх мерах — и
// вёрстка её тела. Окна нет: DirectWrite и вёрстка работают и так.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string_view>
#include <vector>

#include "check.h"

// Последними: поток страниц ведёт к импорту модулей книги и wxl.core, роман
// проб — к ним же и к wxl.async, сноска — через те же заголовки вёрстки.
#include "bukvitsa/reader/page_flow.h"
#include "novel.h"
#include "bukvitsa/typography/note.h"

using namespace bukvitsa;
using namespace bukvitsa::reader;
using namespace bukvitsa::reader::tests;
using namespace std::chrono_literals;

namespace {

using Column = PageFlow::Column;

/// Мера полосы проб стыка глав: 1280 × 860 DIP — около окна первого запуска
/// при масштабе 100 % (окну задают внешний размер в пикселях, полоса получает
/// его клиентскую часть в DIP). На кегле 20 и 21 на ней разворот в две
/// колонки.
constexpr float kStripWidth = 1280.0f;
constexpr float kStripHeight = 860.0f;

/// Кегль на шаг крупнее — перевёрстка, как от Ctrl с колесом.
constexpr float kLargerFontSize = kFontSize + 1.0f;

/// Главы романа по индексу: 0 — титул, дальше I, II, … IX — индекс 9, в
/// колонцифре «Глава 10»; X — индекс 10; VIII перед IX — индекс 8.
constexpr size_t kNinthChapter = 9;
constexpr size_t kTenthChapter = 10;

/// Мера строки: колонку добавляют, пока строка длиннее 85 знаков и
/// пока следующая колонка не выйдет уже 43.
constexpr float kMaxLineChars = 85.0f;
constexpr float kMinLineChars = 43.0f;

/// Поток на книге с этого места, на полосе такой меры, видом по умолчанию.
void setUp(PageFlow& flow, not_null<Book> book, uint32_t at, float width, float height = kHeight) {
    flow.open(book, at);
    flow.setStyle(kFontSize, kLineHeight, kMargin);
    flow.resize(width, height);
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
void listChapters(std::string_view probe, const std::vector<size_t>& chapters) {
    for (const size_t chapter : chapters)
        std::printf("        %.*s: глава %zu\n", static_cast<int>(probe.size()), probe.data(),
                    chapter + 1);
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
    const uint32_t letter = middleLetter(*novel);

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
    setUp(flow, as_not_null(novel.get()), 0, kStripWidth, kStripHeight);
    const bool twoUp = flow.columns() == 2;
    flow.setStyle(kLargerFontSize, kLineHeight, kMargin);
    flow.relayout();
    check(twoUp && flow.columns() == 2,
          "полоса 1280 × 860: на кегле 20 и 21 — разворот в две колонки");

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

        flow.setStyle(kLargerFontSize, kLineHeight, kMargin);
        flow.relayout();
        if (flow.position() != letter || !atChapterStart(flow, chapter)) moved.push_back(chapter);
    }
    listChapters("прыжок", jumped);
    listChapters("перевёрстка", moved);
    check(jumped.empty(),
          "прыжок на первую букву главы — первая колонка этой главы, на каждом стыке");
    check(moved.empty(),
          "кегль 20 → 21 на начале главы — разворот с той же буквы той же главы, на каждом стыке");
}

/// Прыжок на первую букву главы из предыдущей — в первую колонку этой главы,
/// как бы ни была досчитана предыдущая: её хвост ещё идёт, досчитан до конца,
/// или искомая глава уже лежит правой колонкой разворота на стыке.
/// Ближайшая известная колонка здесь не ответ: на стыке это последняя колонка
/// предыдущей главы.
///
/// Недосчитанной предыдущая глава остаётся после прыжка на её начало, если в
/// ней больше страниц, чем досчитывает прыжок до видимого разворота. Так на
/// каждом стыке, кроме стыка I: титул — одна страница, и прыжок на него
/// досчитывает его до конца.
void testJumpToChapterStartFromNeighbour() {
    std::printf("\n=== поток страниц: прыжок на начало главы из предыдущей ===\n");
    const intrusive_ptr<Book> novel = openNovel();
    if (!novel) return;

    PageFlow flow;
    setUp(flow, as_not_null(novel.get()), 0, kStripWidth, kStripHeight);
    check(flow.columns() == 2, "полоса 1280 × 860 — разворот в две колонки");
    if (flow.columns() != 2) return;

    // Прыжок оглавлением: колонка по букве, книга — на неё.
    const auto jump = [&flow](uint32_t letter) { flow.show(flow.columnOf(letter)); };

    std::vector<size_t> untailed;   // стыки, где предыдущая досчитана уже прыжком
    std::vector<size_t> tailing;    // предыдущая ещё досчитывается
    std::vector<size_t> counted;    // предыдущая досчитана
    std::vector<size_t> ribbon;     // глава уже на ленте — правая колонка стыка
    for (size_t chapter = 1; chapter < novel->chapterCount(); ++chapter) {
        const uint32_t chapterStart = novel->chapterFirstChar(chapter);
        const uint32_t previousStart = novel->chapterFirstChar(chapter - 1);

        jump(previousStart);
        const bool tailRunning = flow.needsTail();
        jump(chapterStart);
        if (!tailRunning)
            untailed.push_back(chapter);
        else if (!atChapterStart(flow, chapter))
            tailing.push_back(chapter);

        jump(previousStart);
        int slices = 0;
        while (flow.advanceTail(50ms) && slices < 10000) ++slices;
        const bool complete = flow.isComplete();
        jump(chapterStart);
        if (!complete || !atChapterStart(flow, chapter)) counted.push_back(chapter);

        // Последняя буква предыдущей главы — её последняя колонка слева, первая
        // страница этой — справа.
        jump(chapterStart - 1);
        const PageFlow::Spread& seam = flow.spread();
        const bool onSeam = flow.anchor().chapter == chapter - 1 && seam.pages.size() == 2 &&
                            seam.ownColumns == 1 &&
                            seam.pages[1] == &novel->chapterAt(chapter).page(0);
        jump(chapterStart);
        if (!onSeam || !atChapterStart(flow, chapter)) ribbon.push_back(chapter);
    }
    listChapters("без недосчёта", untailed);
    listChapters("хвост идёт", tailing);
    listChapters("досчитана", counted);
    listChapters("на стыке", ribbon);
    check(untailed == std::vector<size_t>{1},
          "хвост предыдущей главы не досчитан на каждом стыке, кроме стыка I за титулом");
    check(tailing.empty(), "предыдущая глава досчитывается — прыжок в первую колонку искомой");
    check(counted.empty(), "предыдущая глава досчитана — прыжок в первую колонку искомой");
    check(ribbon.empty(), "искомая глава — правая колонка стыка — прыжок в её первую колонку");
}

/// Прыжок в IX и сразу в X, пока хвост IX не досчитан: место чтения — первая
/// буква X, и роман, открытый заново на этом месте, встаёт на первый разворот
/// X — место, которое читалка пишет в файл (`position()`), и открытие по нему
/// видят ту же главу.
void testJumpThenReopen() {
    std::printf("\n=== поток страниц: прыжок и открытие заново на том же месте ===\n");
    const intrusive_ptr<Book> novel = openNovel();
    if (!novel) return;

    check(novel->chapterCount() > kTenthChapter, "в романе есть главы IX и X");
    if (novel->chapterCount() <= kTenthChapter) return;

    PageFlow flow;
    setUp(flow, as_not_null(novel.get()), 0, kStripWidth, kStripHeight);
    flow.show(flow.columnOf(novel->chapterFirstChar(kNinthChapter)));
    check(atChapterStart(flow, kNinthChapter) && flow.needsTail(),
          "прыжок в IX — её первый разворот, хвост ещё досчитывается");

    flow.show(flow.columnOf(novel->chapterFirstChar(kTenthChapter)));
    const uint32_t saved = flow.position();
    check(atChapterStart(flow, kTenthChapter) && letterInChapter(*novel, kTenthChapter, saved),
          "сразу прыжок в X — её первый разворот, место чтения — её первая буква");

    // Открытие заново — другая книга из того же файла и свежий поток, как у
    // читалки после перезапуска.
    const intrusive_ptr<Book> reopenedNovel = openNovel();
    if (!reopenedNovel) return;
    PageFlow reopenedFlow;
    setUp(reopenedFlow, as_not_null(reopenedNovel.get()), saved, kStripWidth, kStripHeight);
    check(atChapterStart(reopenedFlow, kTenthChapter) && reopenedFlow.position() == saved,
          "открыли заново на этом месте — первый разворот X");
}

/// Переворот назад с первого разворота главы — на целый разворот по ленте: за
/// титулом в одну страницу — на самый первый разворот книги (титул слева,
/// начало главы I справа, колонцифра по титулу), за длинной главой — на её
/// последний полный разворот, обе колонки которого её.
void testTurnBackFromChapterStart() {
    std::printf("\n=== поток страниц: назад с начала главы ===\n");
    const intrusive_ptr<Book> novel = openNovel();
    if (!novel) return;

    check(novel->chapterCount() > kNinthChapter, "в романе есть глава IX");
    if (novel->chapterCount() <= kNinthChapter) return;

    PageFlow flow;
    setUp(flow, as_not_null(novel.get()), 0, kStripWidth, kStripHeight);
    check(flow.columns() == 2, "полоса 1280 × 860 — разворот в две колонки");
    if (flow.columns() != 2) return;

    flow.show(flow.columnOf(novel->chapterFirstChar(1)));
    const std::optional<Column> toTitle = flow.turnTarget(false);
    check(toTitle && *toTitle == Column{0, 0},
          "назад с начала главы I — самый первый разворот книги");

    // Разворот «титул и начало I» — только у титула в одну страницу; это
    // разбивка этой полосы, а не правило ленты, и проверяется отдельно.
    const typography::Chapter& title = novel->chapterAt(0);
    const bool titleOnePage = title.isComplete() && title.pageCount() == 1;
    check(titleOnePage, "титул на этой полосе — одна страница");
    if (toTitle && titleOnePage) {
        flow.show(*toTitle);
        const PageFlow::Spread& spread = flow.spread();
        check(flow.status().chapter == 0 && flow.status().first == 0 && spread.pages.size() == 2 &&
                  spread.ownColumns == 1 && spread.pages[1] == &novel->chapterAt(1).page(0),
              "титул слева, начало главы I справа, колонцифра — по титулу");
    }

    flow.show(flow.columnOf(novel->chapterFirstChar(kNinthChapter)));
    const std::optional<Column> toEighth = flow.turnTarget(false);
    const size_t eighthPages = novel->chapterAt(kNinthChapter - 1).pageCount();
    check(eighthPages >= 2 && toEighth && *toEighth == Column{kNinthChapter - 1, eighthPages - 2},
          "назад с начала IX — последний полный разворот VIII");
    if (toEighth) {
        flow.show(*toEighth);
        const PageFlow::Status status = flow.status();
        check(status.chapter == kNinthChapter - 1 && status.own == 2 && status.total == eighthPages,
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

    // Первый знак сноски в потоке чтения — туда и прыгаем.
    const std::optional<NoteMark> noteMark = firstNoteMark(*novel);
    check(noteMark.has_value(), "в романе есть сноски");
    if (!noteMark) return;

    // Тело знака — в хранилище, за потоком чтения: всплывашке есть что верстать.
    const typography::BookText& text = novel->text();
    check(noteMark->noteBody < text.noteBodies.size() &&
              text.noteBodies[noteMark->noteBody].count > 0 &&
              text.noteBodies[noteMark->noteBody].first >= text.flowCount,
          "у знака есть тело — блоки за потоком чтения");

    PageFlow flow;
    setUp(flow, as_not_null(novel.get()), 0, 1400.0f);
    flow.show(flow.columnOf(noteMark->markLetter));

    // Знак ищем в строках разворота, а щёлкаем в его середину на высоте строки.
    const std::optional<MarkPlace> markPlace = markPlaceOf(flow, noteMark->noteBody);
    check(markPlace.has_value(), "знак сноски — на развороте, куда прыгнули");
    if (!markPlace) return;

    check(!flow.noteAt(PageFlow::Point{flow.columnLeft(0) + 1.0f, 1.0f}),
          "щелчок в верхнее поле — мимо");

    const std::optional<PageFlow::NoteHit> hit = flow.noteAt(markPlace->tapPoint);
    check(hit && hit->body == noteMark->noteBody, "щелчок по знаку — его сноска");
    if (!hit) return;
    check(std::abs(hit->anchor.x - markPlace->popupAnchor.x) < 0.5f &&
              std::abs(hit->anchor.y - markPlace->popupAnchor.y) < 0.5f,
          "всплывашка встаёт под знаком");

    // Всплывашка верстает тело по номеру из щелчка тем же движком и из того же
    // хранилища — здесь то же самое без окна.
    const typography::NoteLayout body =
        typography::layoutNote(novel->engine(), text, text.noteBodies[hit->body], 300.0f, kFontSize);
    check(!body.lines.empty() && body.height > 0.0f, "тело сноски по номеру из щелчка верстается");
}

/// Мера полосы, на которой обходятся знаки сносок.
struct StripMeasure {
    float width;
    float height;
};

/// Сколько промахов печатать подробно: остальные только считаются.
constexpr size_t kReportedMisses = 12;

/// Держит ли строка первую единицу знака: строка того же абзаца — её первая
/// буква та же буква книги, что у абзаца на её месте, — и знак начинается в
/// ней. Строки не знают своего абзаца, а буква книги у каждой своя.
bool lineHoldsMark(const typography::BookText& text, const typography::Paragraph& paragraph,
                   const typography::Line& line, const typography::NoteAnchor& note) {
    if (line.textStart >= paragraph.text.count) return false;
    if (text.charOffsetAt(paragraph, line.textStart) != line.charOffset) return false;
    return note.position >= line.textStart && note.position - line.textStart < line.textLength;
}

/// Каждый знак сноски романа ловится щелчком в свою середину — на развороте
/// 1400, в одной колонке 700, на полосе окна первого запуска и на его
/// клиентской части. Знак ищется на своей строке — той, где начинается его
/// текст, — так что провалом выходит и знак, которого вёрстка не положила на
/// строку (нет PlacedNote с его телом), и знак, чья зона щелчка разошлась с
/// его местом (noteAt в середине знака — пусто или чужое тело).
void testEveryNoteMarkIsHit() {
    std::printf("\n=== поток страниц: каждый знак сноски ловится ===\n");
    const intrusive_ptr<Book> novel = openNovel();
    if (!novel) return;

    const typography::BookText& text = novel->text();

    // Последняя мера — клиентская часть окна 1280 × 860: окну задают внешний
    // размер, а полоса берёт клиентскую область (BookView::applySize). Рамка
    // WS_OVERLAPPEDWINDOW при 96 DPI — по 8 пикселей по бокам и снизу и 31
    // сверху, с заголовком.
    constexpr StripMeasure kMeasures[] = {
        {1400.0f, kHeight},
        {700.0f, kHeight},
        {kStripWidth, kStripHeight},
        {kStripWidth - 16.0f, kStripHeight - 39.0f},
    };

    for (const StripMeasure& measure : kMeasures) {
        const auto started = std::chrono::steady_clock::now();
        PageFlow flow;
        setUp(flow, as_not_null(novel.get()), 0, measure.width, measure.height);

        size_t marks = 0;
        size_t missed = 0;
        for (const typography::Block& block : text.flow()) {
            const typography::Paragraph& paragraph = block.paragraph;
            for (const typography::NoteAnchor& note : text.notesOf(paragraph)) {
                ++marks;
                flow.show(flow.columnOf(text.charOffsetAt(paragraph, note.position)));

                // Своя строка знака — на развороте, куда прыгнули.
                const PageFlow::Spread& spread = flow.spread();
                const typography::PlacedLine* markLine = nullptr;
                size_t markColumn = 0;
                for (size_t column = 0; column < spread.pages.size() && !markLine; ++column) {
                    for (const typography::PlacedLine& placed : spread.pages[column]->lines) {
                        if (!lineHoldsMark(text, paragraph, *placed.line, note)) continue;
                        markLine = &placed;
                        markColumn = column;
                        break;
                    }
                }

                const typography::PlacedNote* placedMark = nullptr;
                if (markLine) {
                    for (const typography::PlacedNote& placedNote : markLine->line->notes) {
                        if (placedNote.body != note.body) continue;
                        placedMark = &placedNote;
                        break;
                    }
                }

                // Щелчок — в середину знака на высоте строки, как в
                // testNoteMarkIsHit.
                std::optional<PageFlow::NoteHit> hit;
                float middle = 0.0f;
                if (placedMark) {
                    const float baseline = flow.verticalMargin() + markLine->baseline;
                    middle = flow.columnLeft(markColumn) + markLine->x + placedMark->x +
                             placedMark->width * 0.5f;
                    hit = flow.noteAt(
                        PageFlow::Point{middle, baseline - markLine->line->ascent * 0.5f});
                }
                if (hit && hit->body == note.body) continue;

                if (++missed > kReportedMisses) continue;

                const u8_text markText =
                    text.textOf(paragraph).substr(note.position, note.length).to_utf8();
                std::printf("        знак %zu «%.*s» (тело %u), мера %.0f × %.0f: глава %zu, ",
                            marks, static_cast<int>(markText.chars().size()),
                            markText.chars().data(), note.body, measure.width, measure.height,
                            flow.anchor().chapter + 1);
                if (!markLine) {
                    std::printf("своей строки на развороте нет\n");
                    continue;
                }

                const typography::Line& line = *markLine->line;
                std::printf("колонка %zu, строка [%u, %u) шириной %.1f, знак [%u, %u)\n",
                            markColumn, line.textStart, line.textStart + line.textLength,
                            line.width, note.position, note.position + note.length);
                if (!placedMark) {
                    std::printf("          PlacedNote с этим телом нет; прогоны строки:");
                    for (const typography::GlyphRun& run : line.runs)
                        std::printf(" [%u, %u) x %.1f ш %.1f", run.textStart,
                                    run.textStart + run.textLength, run.originX, run.width);
                    std::printf("\n");
                    continue;
                }
                std::printf("          x %.1f, ширина %.1f, середина %.1f; noteAt — %s %u\n",
                            flow.columnLeft(markColumn) + markLine->x + placedMark->x,
                            placedMark->width, middle, hit ? "тело" : "пусто",
                            hit ? hit->body : 0u);
            }
        }

        // Время — чтобы видеть цену пробы: обход прыгает к каждому знаку романа
        // и верстает главы заново на каждой мере.
        const auto elapsed = std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - started).count();
        std::printf("  мера %.0f × %.0f: знаков %zu, мимо %zu, %.0f мс\n", measure.width,
                    measure.height, marks, missed, elapsed);
        char what[160];
        std::snprintf(what, sizeof(what),
                      "каждый знак сноски ловится щелчком в свою середину — мера %.0f × %.0f",
                      measure.width, measure.height);
        check(marks > 0 && missed == 0, what);
    }
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
    testEveryNoteMarkIsHit();
}
