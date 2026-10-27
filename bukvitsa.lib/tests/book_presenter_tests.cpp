// Тесты ведущего полосы (BookPresenter) на настоящей книге. Холст — поддельный:
// пишет журнал вызовов, и при каждом — букву места чтения и левую колонку
// разворота, какими их видит поток страниц в этот миг, так что порядок «что
// раньше, что позже» в переворотах, перевёрстке и прыжке проверяется без
// окна. Туда же пишет и слушатель места чтения: место ставится при нажатии,
// после всего остального. Простой — поддельный, как у проб досчёта главы:
// ожидания стоят, пока проба не сделает ход. Листы, их посадки и пул — у
// холста, и здесь их нет: газета и книга для ведущего одно и то же.
//
// Нарушенная проводка (начало и конец без книги, команда не полосы, прокрутка
// без сноски, второй холст, ведущий под холстом, просьба без холста)
// останавливает читалку —
// такие пробы идут в дочернем процессе: тот же исполняемый файл с флагом
// `--stop-probe=<имя>`. Смотрится код выхода и отчёт об остановке в stderr:
// остановиться проба обязана своей строкой, а не чужой.

#include <algorithm>
#include <coroutine>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Отчёт отладочной CRT пробы остановки — в stderr, а не окном.
#include <crtdbg.h>

#include "check.h"

// Последними: поток страниц ведёт к импорту модулей книги и wxl.core, роман
// проб и ведущий — к wxl.async.
#include "bukvitsa/reader/page_flow.h"
#include "novel.h"
#include "bukvitsa/reader/book_presenter.h"

import wxl.async;
import wxl.core;

using namespace bukvitsa;
using namespace bukvitsa::reader;
using namespace bukvitsa::reader::tests;
using wxl::async::cancellation_source;

/// Пробы остановки очереди картинок — у её проб (picture_queue_tests.cpp):
/// дочерний запуск один на исполняемый файл, и разбирает имя он здесь.
bool isPictureStopProbe(std::string_view probeName);
int runPictureStopProbe(std::string_view probeName);

/// Пробы остановки рабочего места — у его проб (lib_tests.cpp).
bool isWorkspaceStopProbe(std::string_view probeName);
int runWorkspaceStopProbe(std::string_view probeName);

namespace {

using Column = PageFlow::Column;

/// Ширины полосы на кегле по умолчанию: на первой строка длиннее 85 знаков —
/// разворот в две колонки, на второй короче — одна колонка.
constexpr float kTwoColumnWidth = 1400.0f;
constexpr float kOneColumnWidth = 700.0f;

/// Масштаб экрана в пробах меры — 125 %, не единица: так видно, что ведущий
/// его взял, а не оставил прежним.
constexpr float kScaledScreen = 1.25f;

/// Ход колеса на одну защёлку.
constexpr int kWheelNotch = 120;

/// Вид шире и крупнее умолчания — в единицах настроек: кегль в DIP,
/// интерлиньяж и поля в процентах, как у ползунков панели.
constexpr double kLargeFontSize = 24.0;
constexpr double kWideLeading = 160.0;
constexpr double kWideMargin = 10.0;

/// Насколько мера заходит за границу одной и двух колонок в пробе гистерезиса:
/// доли знака, как дрожит окно, которое тянут мышью.
constexpr float kHysteresisReach = 1.0f;

/// Чем кончается std::abort у CRT MSVC — так кончается и остановка читалки
/// (core::abort) после отчёта.
constexpr DWORD kAbortExitCode = 3;

/// Флаг дочернего запуска пробы остановки и имена проб.
constexpr std::string_view kStopProbeFlag = "--stop-probe=";
constexpr std::string_view kStartWithoutBook = "go-to-start-without-book";
constexpr std::string_view kEndWithoutBook = "go-to-end-without-book";
constexpr std::string_view kForeignCommand = "foreign-command";
constexpr std::string_view kScrollWithoutNote = "scroll-note-without-note";
constexpr std::string_view kSecondCanvas = "second-canvas";
constexpr std::string_view kCanvasOutlives = "presenter-dies-under-canvas";
constexpr std::string_view kNoCanvas = "request-without-canvas";
constexpr std::string_view kUnknownProbe = "no-such-probe";

/// Что пишет в отчёт дочерний запуск, получивший незнакомое имя пробы.
constexpr std::string_view kUnknownProbeReport = "незнакомая проба остановки";

/// Сколько ждать дочерний запуск пробы: остановка — дело миллисекунд, а
/// зависшая проба не должна вешать весь прогон.
constexpr DWORD kProbeTimeoutMs = 20000;

/// Код, которым снимается дочерний запуск: зависший или не вставший в задание.
constexpr UINT kKilledProbeExitCode = 0xDEAD;

/// Буфер канала отчёта: отчёт об остановке — одна строка, и с запасом.
constexpr DWORD kReportPipeBytes = 64 * 1024;

/// Исполнитель намерений, который их только считает.
class CountingActions final : public BookPresenter::Actions {
public:
    void togglePanel() override { ++panelToggles; }

    /// Сбой машинерии полосы в этих пробах — провал.
    void reportFailure(std::exception_ptr) noexcept override { check(false, "у полосы не должно было быть сбоя"); }

    int panelToggles = 0;
};

/// Что попало в журнал: вызов холста или перемена места чтения.
enum class Step {
    TurnStarting,
    TurnStarted,
    FinishTurns,
    Redraw,
    Restyle,
    ShowNote,
    HideNote,
    ScrollNote,
    Activate,
    PositionMoved,
};

const char* nameOf(Step journalStep) {
    switch (journalStep) {
        case Step::TurnStarting:
            return "turnStarting";
        case Step::TurnStarted:
            return "turnStarted";
        case Step::FinishTurns:
            return "finishTurns";
        case Step::Redraw:
            return "redraw";
        case Step::Restyle:
            return "restyle";
        case Step::ShowNote:
            return "showNote";
        case Step::HideNote:
            return "hideNote";
        case Step::ScrollNote:
            return "scrollNote";
        case Step::Activate:
            return "activate";
        case Step::PositionMoved:
            return "position";
    }
    return "?";
}

/// Строка журнала: что случилось и каким поток страниц был в этот миг.
struct Entry {
    Step journalStep = Step::Redraw;
    bool turnsForward = false;       ///< у переворота: вперёд ли
    bool entersReading = false;      ///< у activate: вход в чтение или уход
    uint32_t readingPlace = 0;       ///< буква места чтения потока
    Column leftColumn;               ///< левая колонка разворота
    bool chapterComplete = false;    ///< текущая глава досчитана
    bool spreadShown = false;        ///< разворот собран — есть что рисовать
};

/// Холст, который ничего не рисует, а записывает. Встаёт под ведущего
/// последним своим членом и уходит, умирая, — как полоса Reader. Слушает и
/// место чтения: его перемена ложится в тот же журнал, по порядку с вызовами.
class RecordingCanvas final : public PageCanvas {
public:
    explicit RecordingCanvas(BookPresenter& presenter)
        : presenter_(presenter),
          positionWatch_(presenter.position(), *this),
          canvasAttachment_(presenter.attachCanvas(*this)) {}

    void turnStarting(bool forward) override { recordTurn(Step::TurnStarting, forward); }
    void turnStarted(bool forward) override { recordTurn(Step::TurnStarted, forward); }
    void finishTurns() override { journal.push_back(entryOf(Step::FinishTurns)); }
    void redraw() noexcept override { journal.push_back(entryOf(Step::Redraw)); }
    void restyle() override { journal.push_back(entryOf(Step::Restyle)); }

    bool showNote(const PageFlow::NoteHit& noteHit) override {
        journal.push_back(entryOf(Step::ShowNote));
        shownNoteBody = noteHit.body;
        return noteShows;
    }

    void hideNote() override { journal.push_back(entryOf(Step::HideNote)); }

    void scrollNote(NoteScroll scroll) override {
        journal.push_back(entryOf(Step::ScrollNote));
        noteScrolls.push_back(scroll);
    }

    void activate(bool entersReading) override {
        Entry activateEntry = entryOf(Step::Activate);
        activateEntry.entersReading = entersReading;
        journal.push_back(activateEntry);
    }

    /// Шаги журнала по порядку.
    std::vector<Step> steps() const {
        std::vector<Step> recordedSteps;
        for (const Entry& journalEntry : journal) recordedSteps.push_back(journalEntry.journalStep);
        return recordedSteps;
    }

    /// Первая строка журнала с этим шагом; пусто — такого не было.
    std::optional<Entry> first(Step journalStep) const {
        for (const Entry& journalEntry : journal)
            if (journalEntry.journalStep == journalStep) return journalEntry;
        return std::nullopt;
    }

    std::vector<Entry> journal;

    /// Что отвечать на showNote: есть ли в теле что показать.
    bool noteShows = true;

    /// Тело последней показанной сноски.
    uint32_t shownNoteBody = ~uint32_t{0};

    /// Роды прокрутки сноски по порядку просьб.
    std::vector<NoteScroll> noteScrolls;

private:
    Entry entryOf(Step journalStep) const {
        const PageFlow& flow = presenter_.flow();
        Entry journalEntry;
        journalEntry.journalStep = journalStep;
        journalEntry.readingPlace = flow.position();
        journalEntry.leftColumn = flow.anchor();
        journalEntry.chapterComplete = flow.isComplete();
        journalEntry.spreadShown = !flow.spread().pages.empty();
        return journalEntry;
    }

    void recordTurn(Step journalStep, bool forward) {
        Entry turnEntry = entryOf(journalStep);
        turnEntry.turnsForward = forward;
        journal.push_back(turnEntry);
    }

    /// Слушатель места чтения ведущего — член noexcept: событию поля некому
    /// отдать исключение.
    void positionMoved(uint32_t) noexcept { journal.push_back(entryOf(Step::PositionMoved)); }

    BookPresenter& presenter_;
    change_watch<observable<uint32_t const>, &RecordingCanvas::positionMoved> positionWatch_;

    /// Последним: под ведущим холст стоит, когда построен весь.
    BookPresenter::CanvasAttachment canvasAttachment_;
};

/// Журнал — ровно эти шаги по порядку; нет — проба печатает, какие были.
void checkSteps(const RecordingCanvas& canvas, std::initializer_list<Step> expectedSteps,
                std::string_view what) {
    const std::vector<Step> recordedSteps = canvas.steps();
    const bool sameSteps = std::equal(recordedSteps.begin(), recordedSteps.end(),
                                      expectedSteps.begin(), expectedSteps.end());
    check(sameSteps, what);
    if (sameSteps) return;
    std::printf("        было:");
    for (const Step recordedStep : recordedSteps) std::printf(" %s", nameOf(recordedStep));
    std::printf("\n");
}

/// Всё, что ведущему даёт владелец, — в порядке жизни: простой раньше
/// ведущего (досчёт держит ссылку на него), холст позже (встаёт под ведущего
/// и уходит раньше него).
struct Bench {
    ParkingIdle idle;
    Settings settings;
    ThemeList themes{std::vector<Skin>{}};   // встроенные темы и системные обложки
    CountingActions actions;
    cancellation_source ownerSource;
    BookPresenter presenter{settings, themes, idle, actions, ownerSource.token()};
    RecordingCanvas canvas{presenter};

    const PageFlow& flow() const { return presenter.flow(); }

    /// Мера полосы этой ширины и книга на этой букве; журнал — с чистого
    /// листа. Мера раньше книги: открытие верстает без гистерезиса колонок, и
    /// их число решает одна ширина.
    void openAt(not_null<Book> book, uint32_t openLetter, float width) {
        presenter.resize(width, kHeight, 1.0f);
        presenter.open(book, openLetter);
        canvas.journal.clear();
    }
};

/* ---------------- переворот ---------------- */

/// Переворот вперёд и назад: холст берёт уходящий разворот до шага книги,
/// рисует новый после него, пускает переворот и лишь потом сдвигается место
/// чтения — на начало новой левой колонки.
void testTurnOrder() {
    std::printf("\n=== ведущий: порядок шагов переворота ===\n");
    const intrusive_ptr<Book> novel = openNovel();
    if (!novel) return;

    Bench bench;
    bench.openAt(as_not_null(novel.get()), middleLetter(*novel), kTwoColumnWidth);
    check(bench.flow().columns() == 2, "полоса — разворот в две колонки");
    if (bench.flow().columns() != 2) return;

    const Column outgoingColumn = bench.flow().anchor();
    const uint32_t outgoingPlace = bench.flow().position();

    bench.presenter.perform(Command::TurnForward);
    checkSteps(bench.canvas,
               {Step::TurnStarting, Step::Redraw, Step::TurnStarted, Step::PositionMoved},
               "вперёд: turnStarting → redraw → turnStarted → место чтения");

    const Column incomingColumn = bench.flow().anchor();
    check(incomingColumn == Column{outgoingColumn.chapter, outgoingColumn.index + 2},
          "книга шагнула на разворот вперёд");

    const std::vector<Entry>& forwardJournal = bench.canvas.journal;
    check(forwardJournal.size() == 4 && forwardJournal[0].turnsForward &&
              forwardJournal[2].turnsForward,
          "обе точки переворота знают, что он вперёд");
    check(forwardJournal.size() == 4 && forwardJournal[0].leftColumn == outgoingColumn &&
              forwardJournal[0].readingPlace == outgoingPlace,
          "turnStarting — пока книга на уходящем развороте");
    check(forwardJournal.size() == 4 && forwardJournal[1].leftColumn == incomingColumn &&
              forwardJournal[2].leftColumn == incomingColumn,
          "redraw и turnStarted — уже на новом развороте");
    check(placeAtLeftColumn(bench.flow()) &&
              bench.presenter.position().get() == bench.flow().position(),
          "место чтения — начало новой левой колонки");

    bench.canvas.journal.clear();
    bench.presenter.perform(Command::TurnBackward);
    checkSteps(bench.canvas,
               {Step::TurnStarting, Step::Redraw, Step::TurnStarted, Step::PositionMoved},
               "назад: тот же порядок шагов");

    const std::vector<Entry>& backwardJournal = bench.canvas.journal;
    check(backwardJournal.size() == 4 && !backwardJournal[0].turnsForward &&
              !backwardJournal[2].turnsForward,
          "обе точки переворота знают, что он назад");
    check(backwardJournal.size() == 4 && backwardJournal[0].leftColumn == incomingColumn &&
              backwardJournal[1].leftColumn == outgoingColumn,
          "назад — с нового разворота на прежний");
    check(bench.flow().anchor() == outgoingColumn &&
              bench.presenter.position().get() == bench.flow().position(),
          "книга вернулась на прежний разворот, место — его начало");
}

/// Газета (одна колонка) и книга (две) у ведущего не различаются: порядок
/// шагов один, вид переворота холст решает сам по числу колонок.
void testTurnSameInBothModes() {
    std::printf("\n=== ведущий: газета и книга — один порядок ===\n");
    const intrusive_ptr<Book> novel = openNovel();
    if (!novel) return;

    Bench bench;
    bench.openAt(as_not_null(novel.get()), middleLetter(*novel), kOneColumnWidth);
    check(bench.flow().columns() == 1, "узкая полоса — одна колонка");
    if (bench.flow().columns() != 1) return;

    const Column outgoingColumn = bench.flow().anchor();
    bench.presenter.perform(Command::TurnForward);
    checkSteps(bench.canvas,
               {Step::TurnStarting, Step::Redraw, Step::TurnStarted, Step::PositionMoved},
               "газета: turnStarting → redraw → turnStarted → место чтения");
    check(bench.flow().anchor() == Column{outgoingColumn.chapter, outgoingColumn.index + 1},
          "газета шагнула на одну колонку");
    check(bench.canvas.journal.size() == 4 &&
              bench.canvas.journal[0].leftColumn == outgoingColumn &&
              bench.canvas.journal[1].leftColumn == bench.flow().anchor(),
          "газета: уходящий берётся до шага, новый рисуется после");
}

/// Край книги: листать некуда — ни одного вызова холста, место на месте.
void testTurnAtBookEdges() {
    std::printf("\n=== ведущий: края книги ===\n");
    const intrusive_ptr<Book> novel = openNovel();
    if (!novel) return;

    Bench bench;
    bench.openAt(as_not_null(novel.get()), 0, kTwoColumnWidth);

    bench.presenter.perform(Command::TurnBackward);
    checkSteps(bench.canvas, {}, "назад с начала книги — холст не тронут");
    check(bench.presenter.position().get() == 0, "место — начало книги");

    bench.presenter.perform(Command::GoToEnd);
    checkSteps(bench.canvas, {Step::FinishTurns, Step::Redraw, Step::PositionMoved},
               "конец книги — прыжок");
    check(bench.flow().anchor().chapter == novel->chapterCount() - 1, "книга — в последней главе");

    const uint32_t endPlace = bench.presenter.position().get();
    bench.canvas.journal.clear();
    bench.presenter.perform(Command::TurnForward);
    checkSteps(bench.canvas, {}, "вперёд с конца книги — холст не тронут");
    check(bench.presenter.position().get() == endPlace, "место не сдвинулось");

    bench.presenter.perform(Command::GoToStart);
    checkSteps(bench.canvas, {Step::FinishTurns, Step::Redraw, Step::PositionMoved},
               "начало книги — прыжок");
    check(bench.flow().anchor() == Column{0, 0} && bench.presenter.position().get() == 0,
          "книга — на самом первом развороте, место — ноль");
}

/// Переворот через границу главы: в недосчитанную главу вошли — её досчёт
/// заводится тем же переворотом.
void testTurnIntoChapterStartsTail() {
    std::printf("\n=== ведущий: переворот в соседнюю главу — досчёт ===\n");
    const intrusive_ptr<Book> novel = openNovel();
    if (!novel) return;

    // Следующая глава — самая длинная: войдя в неё, хвост заведомо досчитывать.
    const size_t nextChapter = longestChapter(*novel);
    check(nextChapter >= 2, "перед самой длинной главой есть глава с текстом");
    if (nextChapter < 2) return;

    Bench bench;
    bench.openAt(as_not_null(novel.get()), novel->chapterFirstChar(nextChapter) - 1,
                 kTwoColumnWidth);
    check(bench.flow().anchor().chapter == nextChapter - 1, "книга — в главе перед стыком");

    // Глава перед стыком досчитана: кадр досчёта кончился, и новый может
    // завести только переворот.
    wakeAll(bench.idle);
    check(bench.flow().isComplete() && bench.idle.parked() == 0, "глава перед стыком досчитана");

    bench.presenter.perform(Command::TurnForward);
    check(bench.flow().anchor().chapter == nextChapter && bench.flow().needsTail(),
          "переворот ввёл в недосчитанную главу");
    check(bench.idle.parked() == 1, "переворот завёл её досчёт");
}

/* ---------------- перевёрстка и прыжок ---------------- */

/// Перевёрстка посреди серии переворотов: всё, что летит, — в конечное
/// состояние до вёрстки, рисунок — после; место чтения не двигается и о себе
/// не объявляет — иначе растяжка и кегль уводили бы место назад.
void testRelayoutMidSeries() {
    std::printf("\n=== ведущий: перевёрстка посреди серии ===\n");
    const intrusive_ptr<Book> novel = openNovel();
    if (!novel) return;

    Bench bench;
    bench.openAt(as_not_null(novel.get()), middleLetter(*novel), kTwoColumnWidth);

    // Глава досчитана до конца: перевёрстка заводит её заново, и по этому
    // видно, какой вызов холста был до вёрстки, а какой после.
    wakeAll(bench.idle);
    check(bench.flow().isComplete(), "глава досчитана");

    // Два переворота в воздухе — поддельный холст их не сажает.
    bench.presenter.perform(Command::TurnForward);
    bench.presenter.perform(Command::TurnForward);
    const uint32_t seriesPlace = bench.presenter.position().get();
    bench.canvas.journal.clear();

    bench.settings.fontSize.set(kFontSizeDefault + kFontSizeStep);
    checkSteps(bench.canvas, {Step::FinishTurns, Step::Redraw}, "кегль: finishTurns → redraw");

    const std::optional<Entry> finishEntry = bench.canvas.first(Step::FinishTurns);
    const std::optional<Entry> redrawEntry = bench.canvas.first(Step::Redraw);
    check(finishEntry && finishEntry->chapterComplete,
          "finishTurns — до вёрстки: глава ещё прежняя");
    check(redrawEntry && !redrawEntry->chapterComplete && redrawEntry->spreadShown,
          "redraw — после вёрстки: глава свёрстана заново");
    check(bench.presenter.position().get() == seriesPlace &&
              bench.flow().position() == seriesPlace &&
              letterInLeftColumn(bench.flow(), seriesPlace),
          "место чтения то же, буква — в левой колонке");
    check(bench.idle.parked() == 1, "досчёт новой вёрстки заведён");
}

/// Прыжок по оглавлению посреди серии: перевороты — в конечное состояние
/// раньше, чем книга начнёт искать колонку места (columnOf), — пока она на
/// прежнем развороте; рисунок — после шага, место — начало колонки с буквой.
void testJumpMidSeries() {
    std::printf("\n=== ведущий: прыжок посреди серии ===\n");
    const intrusive_ptr<Book> novel = openNovel();
    if (!novel) return;

    Bench bench;
    bench.openAt(as_not_null(novel.get()), middleLetter(*novel), kTwoColumnWidth);

    // Досчёт главы, с которой прыгают, кончен: новый может завести только
    // сам прыжок.
    wakeAll(bench.idle);
    bench.presenter.perform(Command::TurnForward);
    const Column inFlightColumn = bench.flow().anchor();
    bench.canvas.journal.clear();

    const size_t distantChapter = novel->chapterCount() - 2;
    const uint32_t jumpLetter = novel->chapterFirstChar(distantChapter) +
                                chapterLength(*novel, distantChapter) * 2 / 5;
    bench.presenter.goToCharOffset(jumpLetter);

    checkSteps(bench.canvas, {Step::FinishTurns, Step::Redraw, Step::PositionMoved},
               "прыжок: finishTurns → redraw → место чтения");
    const std::optional<Entry> finishEntry = bench.canvas.first(Step::FinishTurns);
    const std::optional<Entry> redrawEntry = bench.canvas.first(Step::Redraw);
    check(finishEntry && finishEntry->leftColumn == inFlightColumn,
          "finishTurns — пока книга на прежнем развороте");
    check(redrawEntry && redrawEntry->leftColumn.chapter == distantChapter,
          "redraw — уже в далёкой главе");
    check(letterInLeftColumn(bench.flow(), jumpLetter) && placeAtLeftColumn(bench.flow()) &&
              bench.presenter.position().get() <= jumpLetter,
          "место — начало колонки с буквой, буква в левой колонке");
    check(bench.flow().needsTail() && bench.idle.parked() == 1,
          "в новой главе досчитывать хвост — прыжок завёл досчёт");

    wakeAll(bench.idle);
    check(bench.flow().isComplete(), "досчёт довёл новую главу до конца");
}

/// Прыжок без книги: переходить некуда — ни одного вызова холста, место не
/// объявлено и не сдвинуто.
void testJumpWithoutBook() {
    std::printf("\n=== ведущий: прыжок без книги ===\n");
    Bench bench;
    bench.presenter.resize(kTwoColumnWidth, kHeight, 1.0f);
    bench.canvas.journal.clear();

    // Буква любая: книги, где её искать, нет.
    bench.presenter.goToCharOffset(1000);
    checkSteps(bench.canvas, {}, "без книги прыжок не трогает холста и места не объявляет");
    check(bench.presenter.position().get() == 0 && bench.flow().position() == 0,
          "место не сдвинулось");
}

/// Кегль с клавиш — в настройки, в пределах; перевёрстка — от поля настроек;
/// так же интерлиньяж и поля.
void testFontSizeRelayout() {
    std::printf("\n=== ведущий: кегль — перевёрстка ===\n");
    const intrusive_ptr<Book> novel = openNovel();
    if (!novel) return;

    Bench bench;
    const uint32_t readingLetter = middleLetter(*novel);
    bench.openAt(as_not_null(novel.get()), readingLetter, kTwoColumnWidth);
    check(bench.idle.parked() == 1, "после открытия досчёт стоит в простое");

    bench.presenter.perform(Command::FontLarger);
    check(bench.settings.fontSize.get() == kFontSizeDefault + kFontSizeStep,
          "крупнее — кегль в настройках на шаг больше");
    checkSteps(bench.canvas, {Step::FinishTurns, Step::Redraw}, "крупнее: finishTurns → redraw");
    check(bench.flow().fontSize() == static_cast<float>(kFontSizeDefault + kFontSizeStep),
          "поток страниц свёрстан новым кеглем");

    // Прежний кадр досчёта отменён, и задача нового, присвоенная поверх,
    // останавливает его сразу: стоит один — новый. Без отмены прежний считал бы
    // новую вёрстку, а нового бы не было.
    check(bench.idle.parked() == 1, "перевёрстка отменила прежний досчёт и завела новый");

    bench.canvas.journal.clear();
    bench.presenter.perform(Command::FontSmaller);
    check(bench.settings.fontSize.get() == kFontSizeDefault, "мельче — обратно");
    checkSteps(bench.canvas, {Step::FinishTurns, Step::Redraw}, "мельче: finishTurns → redraw");

    bench.presenter.perform(Command::FontLarger);
    bench.presenter.perform(Command::FontLarger);
    bench.canvas.journal.clear();
    bench.presenter.perform(Command::FontReset);
    check(bench.settings.fontSize.get() == kFontSizeDefault, "сброс — кегль по умолчанию");
    checkSteps(bench.canvas, {Step::FinishTurns, Step::Redraw}, "сброс: finishTurns → redraw");
    check(bench.presenter.position().get() == readingLetter &&
              letterInLeftColumn(bench.flow(), readingLetter),
          "кегль туда и обратно — та же буква в левой колонке");

    bench.settings.fontSize.set(kFontSizeMax);
    bench.canvas.journal.clear();
    bench.presenter.perform(Command::FontLarger);
    check(bench.settings.fontSize.get() == kFontSizeMax, "крупнее предела — кегль на пределе");
    checkSteps(bench.canvas, {}, "на пределе — перевёрстки нет");

    // Интерлиньяж и поля — те же поля настроек, что у ползунков панели, в
    // процентах; поток страниц берёт множитель и долю.
    bench.settings.lineHeight.set(kWideLeading);
    checkSteps(bench.canvas, {Step::FinishTurns, Step::Redraw},
               "интерлиньяж: finishTurns → redraw");
    check(bench.flow().lineHeight() == static_cast<float>(kWideLeading) / 100.0f,
          "поток свёрстан новым интерлиньяжем");

    bench.canvas.journal.clear();
    bench.settings.margin.set(kWideMargin);
    checkSteps(bench.canvas, {Step::FinishTurns, Step::Redraw}, "поля: finishTurns → redraw");
    check(bench.flow().marginFraction() == static_cast<float>(kWideMargin) / 100.0f,
          "поток свёрстан новыми полями");
}

/// Вид — по настройкам с первой минуты: ведущий, построенный на неумолчных
/// кегле, интерлиньяже и полях, верстает ими, не дожидаясь их перемены.
void testStyleFromSettings() {
    std::printf("\n=== ведущий: вид по настройкам с первой минуты ===\n");

    ParkingIdle idle;
    Settings settings;
    settings.fontSize.set(kLargeFontSize);
    settings.lineHeight.set(kWideLeading);
    settings.margin.set(kWideMargin);
    ThemeList themes;
    CountingActions actions;
    cancellation_source ownerSource;
    const BookPresenter presenter{settings, themes, idle, actions, ownerSource.token()};

    check(presenter.flow().fontSize() == static_cast<float>(kLargeFontSize) &&
              presenter.flow().lineHeight() == static_cast<float>(kWideLeading) / 100.0f &&
              presenter.flow().marginFraction() == static_cast<float>(kWideMargin) / 100.0f,
          "поток страниц взял кегль, интерлиньяж и поля из настроек");
}

/// Новая мера: поток берёт размер и масштаб, полоса перевёрстывается; пустая
/// мера — ни вёрстки, ни рисунка.
void testResizeRelayout() {
    std::printf("\n=== ведущий: мера полосы ===\n");
    const intrusive_ptr<Book> novel = openNovel();
    if (!novel) return;

    Bench bench;
    const uint32_t readingLetter = middleLetter(*novel);
    bench.openAt(as_not_null(novel.get()), readingLetter, kTwoColumnWidth);

    bench.presenter.resize(kOneColumnWidth, kHeight, kScaledScreen);
    checkSteps(bench.canvas, {Step::FinishTurns, Step::Redraw}, "новая мера: finishTurns → redraw");
    check(bench.flow().width() == kOneColumnWidth && bench.flow().columns() == 1 &&
              bench.presenter.scale() == kScaledScreen,
          "поток взял ширину, ведущий — масштаб");
    check(bench.presenter.position().get() == readingLetter &&
              letterInLeftColumn(bench.flow(), readingLetter),
          "место чтения то же");

    bench.canvas.journal.clear();
    bench.presenter.resize(0.0f, 0.0f, kScaledScreen);
    checkSteps(bench.canvas, {}, "пустая мера — холст не тронут");
}

/// Растяжка окна через границу одной и двух колонок — с гистерезисом: окно
/// тянут мышью, граница меры проходит под курсором, и без него колонки
/// защёлкали бы. Перевёрстке не от окна гистерезис не положен — там, где
/// растяжка держит две колонки, открытие на той же мере даёт одну.
void testWindowResizeKeepsColumns() {
    std::printf("\n=== ведущий: гистерезис колонок при растяжке ===\n");
    const intrusive_ptr<Book> novel = openNovel();
    const intrusive_ptr<Book> boundaryNovel = openNovel();
    if (!novel || !boundaryNovel) return;

    Bench bench;
    const uint32_t readingLetter = middleLetter(*novel);

    // Граница одной и двух колонок — делением пополам на своём потоке страниц
    // и своей книге: кэш глав и текущая глава у книги свои, и поиск границы не
    // должен трогать ведущего. Вид — тот же, что у ведущего.
    PageFlow boundaryFlow;
    boundaryFlow.open(as_not_null(boundaryNovel.get()), middleLetter(*boundaryNovel));
    boundaryFlow.setStyle(bench.flow().fontSize(), bench.flow().lineHeight(),
                          bench.flow().marginFraction());
    float oneColumnSide = kOneColumnWidth;
    float twoColumnSide = kTwoColumnWidth;
    boundaryFlow.resize(oneColumnSide, kHeight);
    boundaryFlow.relayout();
    const int narrowColumns = boundaryFlow.columns();
    boundaryFlow.resize(twoColumnSide, kHeight);
    boundaryFlow.relayout();
    check(narrowColumns == 1 && boundaryFlow.columns() == 2,
          "граница одной и двух колонок — между узкой и широкой полосой");
    while (twoColumnSide - oneColumnSide > 0.25f) {
        const float middleWidth = (oneColumnSide + twoColumnSide) * 0.5f;
        boundaryFlow.resize(middleWidth, kHeight);
        boundaryFlow.relayout();
        (boundaryFlow.columns() >= 2 ? twoColumnSide : oneColumnSide) = middleWidth;
    }
    const float belowBoundary = oneColumnSide - kHysteresisReach;
    const float aboveBoundary = twoColumnSide + kHysteresisReach;

    boundaryFlow.resize(belowBoundary, kHeight);
    boundaryFlow.relayout(false);
    check(boundaryFlow.columns() == 1, "под границей без гистерезиса — одна колонка");

    bench.openAt(as_not_null(novel.get()), readingLetter, aboveBoundary);
    check(bench.flow().columns() == 2, "над границей — две колонки");

    bench.presenter.resize(belowBoundary, kHeight, 1.0f);
    check(bench.flow().columns() == 2,
          "растяжка окна под границу на долю знака — две колонки остаются");

    bench.presenter.open(as_not_null(novel.get()), readingLetter);
    check(bench.flow().columns() == 1,
          "открытие на той же мере верстает не от окна — одна колонка");
}

/* ---------------- экран и холст ---------------- */

/// Уход из чтения и вход: холст уходит и входит сам, а входя, получает
/// нынешний разворот.
void testActive() {
    std::printf("\n=== ведущий: вход в чтение и уход ===\n");
    const intrusive_ptr<Book> novel = openNovel();
    if (!novel) return;

    Bench bench;
    bench.openAt(as_not_null(novel.get()), middleLetter(*novel), kTwoColumnWidth);

    bench.presenter.setActive(false);
    checkSteps(bench.canvas, {Step::Activate}, "уход: activate(false)");
    check(!bench.canvas.journal.empty() && !bench.canvas.journal[0].entersReading,
          "уход — не вход");

    bench.canvas.journal.clear();
    bench.presenter.setActive(true);
    checkSteps(bench.canvas, {Step::Activate, Step::Redraw}, "вход: activate(true) → redraw");
    check(!bench.canvas.journal.empty() && bench.canvas.journal[0].entersReading, "вход — вход");
}

/// Холст ушёл — досчёт главы отменён: досчитав, он попросил бы перерисовки у
/// холста, которого нет. Под новым холстом досчёт заводится снова. Сноска,
/// открытая на ушедшем холсте, ушла с ним.
void testTailEndsWithCanvas() {
    std::printf("\n=== ведущий: уход холста отменяет досчёт и сноску ===\n");
    const intrusive_ptr<Book> novel = openNovel();
    if (!novel) return;

    ParkingIdle idle;
    Settings settings;
    ThemeList themes;
    CountingActions actions;
    cancellation_source ownerSource;
    BookPresenter presenter{settings, themes, idle, actions, ownerSource.token()};
    std::optional<RecordingCanvas> stripCanvas;
    stripCanvas.emplace(presenter);

    presenter.resize(kTwoColumnWidth, kHeight, 1.0f);
    presenter.open(as_not_null(novel.get()), middleLetter(*novel));
    check(idle.parked() == 1 && presenter.flow().needsTail(), "досчёт главы стоит в простое");
    const size_t pagesBefore = presenter.flow().pageCount();

    // Без отмены ход очереди довёл бы досчёт до получателя, а тот — до
    // холста, которого нет: остановка посреди прогона.
    stripCanvas.reset();
    wakeAll(idle);
    check(idle.parked() == 0 && presenter.flow().needsTail() &&
              presenter.flow().pageCount() == pagesBefore,
          "холст ушёл — прежний кадр досчёта ушёл отменой, глава не тронута");

    stripCanvas.emplace(presenter);
    presenter.resize(kOneColumnWidth, kHeight, 1.0f);
    stripCanvas->journal.clear();
    wakeAll(idle);
    check(presenter.flow().isComplete(),
          "под новым холстом досчёт заведён снова и доведён до конца");
    checkSteps(*stripCanvas, {Step::Redraw}, "досчитав, он перерисовал разворот на новом холсте");

    const std::optional<NoteMark> noteMark = firstNoteMark(*novel);
    check(noteMark.has_value(), "в романе есть сноски");
    if (!noteMark) return;
    presenter.goToCharOffset(noteMark->markLetter);
    const std::optional<PageFlow::Point> markPoint =
        markPointOf(presenter.flow(), noteMark->noteBody);
    check(markPoint.has_value(), "знак сноски — на развороте, где его буква");
    if (!markPoint) return;
    presenter.tap(*markPoint, PointerButton::Left);
    check(presenter.noteOpen(), "сноска открыта на прежнем холсте");

    // Считай ведущий сноску открытой и под новым холстом, первый щелчок по
    // полю ушёл бы на прятанье того, чего на этом холсте нет, а не на листание.
    stripCanvas.reset();
    stripCanvas.emplace(presenter);
    check(!presenter.noteOpen(), "холст ушёл — сноска ушла с ним");
    presenter.tap(marginPoint(presenter.flow(), 0.9f), PointerButton::Left);
    check(!stripCanvas->first(Step::HideNote) && stripCanvas->first(Step::TurnStarting),
          "щелчок под новым холстом листает, а не прячет ушедшую сноску");
}

/* ---------------- ввод ---------------- */

/// Колесо с Ctrl — кегль, без — листание: от себя назад, на себя вперёд; без
/// хода колесо не берётся.
void testWheel() {
    std::printf("\n=== ведущий: колесо ===\n");
    const intrusive_ptr<Book> novel = openNovel();
    if (!novel) return;

    Bench bench;
    bench.openAt(as_not_null(novel.get()), middleLetter(*novel), kTwoColumnWidth);

    check(bench.presenter.wheel(kWheelNotch, true) &&
              bench.settings.fontSize.get() == kFontSizeDefault + kFontSizeStep,
          "Ctrl, от себя — кегль крупнее");
    check(bench.presenter.wheel(-kWheelNotch, true) &&
              bench.settings.fontSize.get() == kFontSizeDefault,
          "Ctrl, на себя — мельче");
    checkSteps(bench.canvas, {Step::FinishTurns, Step::Redraw, Step::FinishTurns, Step::Redraw},
               "кегль колесом — две перевёрстки, ни одного переворота");

    bench.canvas.journal.clear();
    check(bench.presenter.wheel(-kWheelNotch, false), "на себя — колесо взято");
    check(bench.canvas.first(Step::TurnStarting) &&
              bench.canvas.first(Step::TurnStarting)->turnsForward,
          "на себя — переворот вперёд");

    bench.canvas.journal.clear();
    check(bench.presenter.wheel(kWheelNotch, false), "от себя — колесо взято");
    check(bench.canvas.first(Step::TurnStarting) &&
              !bench.canvas.first(Step::TurnStarting)->turnsForward,
          "от себя — переворот назад");

    bench.canvas.journal.clear();
    check(!bench.presenter.wheel(0, false) && !bench.presenter.wheel(0, true),
          "без хода колесо не берётся");
    checkSteps(bench.canvas, {}, "без хода — холст не тронут");
}

/// Щелчок по третям: левая — назад, правая — вперёд, середина — ничего;
/// правая кнопка — ящики, холст не тронут.
void testTapThirds() {
    std::printf("\n=== ведущий: щелчок по третям ===\n");
    const intrusive_ptr<Book> novel = openNovel();
    if (!novel) return;

    Bench bench;
    bench.openAt(as_not_null(novel.get()), middleLetter(*novel), kTwoColumnWidth);

    bench.presenter.tap(marginPoint(bench.flow(), 0.9f), PointerButton::Left);
    check(bench.canvas.first(Step::TurnStarting) &&
              bench.canvas.first(Step::TurnStarting)->turnsForward,
          "правая треть — вперёд");

    bench.canvas.journal.clear();
    bench.presenter.tap(marginPoint(bench.flow(), 0.1f), PointerButton::Left);
    check(bench.canvas.first(Step::TurnStarting) &&
              !bench.canvas.first(Step::TurnStarting)->turnsForward,
          "левая треть — назад");

    bench.canvas.journal.clear();
    bench.presenter.tap(marginPoint(bench.flow(), 0.5f), PointerButton::Left);
    checkSteps(bench.canvas, {}, "середина — ничего");

    bench.presenter.tap(marginPoint(bench.flow(), 0.9f), PointerButton::Right);
    check(bench.actions.panelToggles == 1, "правая кнопка — ящики");
    checkSteps(bench.canvas, {}, "правая кнопка страницы не листает");
}

/// Под мастером (предпросмотр, wizardOpen в контексте карты) из щелчка
/// остаётся листание левой кнопкой: ящики не открываются.
void testTapUnderWizard() {
    std::printf("\n=== ведущий: щелчок под мастером ===\n");
    const intrusive_ptr<Book> novel = openNovel();
    if (!novel) return;

    Bench bench;
    bench.openAt(as_not_null(novel.get()), middleLetter(*novel), kTwoColumnWidth);
    const Skin previewSkin = defaultSkin();
    bench.presenter.setPreview(&previewSkin);
    bench.canvas.journal.clear();

    bench.presenter.tap(marginPoint(bench.flow(), 0.9f), PointerButton::Right);
    check(bench.actions.panelToggles == 0, "правая кнопка под мастером ящиков не открывает");
    checkSteps(bench.canvas, {}, "и не листает");

    bench.presenter.tap(marginPoint(bench.flow(), 0.9f), PointerButton::Left);
    check(bench.canvas.first(Step::TurnStarting) &&
              bench.canvas.first(Step::TurnStarting)->turnsForward,
          "левая кнопка под мастером листает");

    bench.presenter.setPreview(nullptr);
    bench.canvas.journal.clear();
    bench.presenter.tap(marginPoint(bench.flow(), 0.9f), PointerButton::Right);
    check(bench.actions.panelToggles == 1, "мастер ушёл — правая кнопка снова ящики");
}

/// Сноска: щелчок по знаку показывает её раньше листания, щелчок мимо —
/// закрывает, не листая; клавиши прокрутки уходят холсту, не пряча её и не
/// листая; тема, прыжок, переворот, открытие книги и Escape её прячут; тело,
/// из которого нечего набрать, открытой сноски не делает (noteOpen в
/// контексте карты).
void testNote() {
    std::printf("\n=== ведущий: сноска ===\n");
    const intrusive_ptr<Book> novel = openNovel();
    if (!novel) return;

    const std::optional<NoteMark> noteMark = firstNoteMark(*novel);
    check(noteMark.has_value(), "в романе есть сноски");
    if (!noteMark) return;

    Bench bench;
    bench.openAt(as_not_null(novel.get()), noteMark->markLetter, kTwoColumnWidth);
    const std::optional<PageFlow::Point> openedMarkPoint =
        markPointOf(bench.flow(), noteMark->noteBody);
    check(openedMarkPoint.has_value(), "знак сноски — на развороте, где его буква");
    if (!openedMarkPoint) return;

    bench.presenter.tap(*openedMarkPoint, PointerButton::Left);
    checkSteps(bench.canvas, {Step::ShowNote}, "щелчок по знаку — сноска, не листание");
    check(bench.presenter.noteOpen() && bench.canvas.shownNoteBody == noteMark->noteBody,
          "открыта сноска этого знака");

    // Прокрутка — холсту, своим родом на каждую команду: ни прятанья, ни
    // листания, место чтения стоит.
    bench.canvas.journal.clear();
    for (const Command scrollCommand :
         {Command::ScrollNoteLineDown, Command::ScrollNoteLineUp, Command::ScrollNotePageDown,
          Command::ScrollNotePageUp, Command::ScrollNoteToStart, Command::ScrollNoteToEnd})
        bench.presenter.perform(scrollCommand);
    checkSteps(bench.canvas,
               {Step::ScrollNote, Step::ScrollNote, Step::ScrollNote, Step::ScrollNote,
                Step::ScrollNote, Step::ScrollNote},
               "клавиши прокрутки — холсту: сноска не спрятана, книга не листнута");
    check(bench.canvas.noteScrolls ==
              std::vector<NoteScroll>{NoteScroll::LineDown, NoteScroll::LineUp, NoteScroll::PageDown,
                                      NoteScroll::PageUp, NoteScroll::ToStart, NoteScroll::ToEnd},
          "каждая команда — своим родом прокрутки");
    check(bench.presenter.noteOpen(), "после прокрутки сноска открыта");

    bench.canvas.journal.clear();
    bench.presenter.tap(marginPoint(bench.flow(), 0.9f), PointerButton::Left);
    checkSteps(bench.canvas, {Step::HideNote},
               "щелчок мимо знака — сноска закрыта, книга не листнута");
    check(!bench.presenter.noteOpen(), "сноски больше нет");

    bench.canvas.journal.clear();
    bench.presenter.tap(*openedMarkPoint, PointerButton::Left);
    bench.presenter.perform(Command::DismissNote);
    checkSteps(bench.canvas, {Step::ShowNote, Step::HideNote}, "Escape закрывает сноску");
    check(!bench.presenter.noteOpen(), "после Escape сноски нет");

    bench.canvas.journal.clear();
    bench.presenter.tap(*openedMarkPoint, PointerButton::Left);
    bench.presenter.setTheme(1);
    checkSteps(bench.canvas, {Step::ShowNote, Step::HideNote, Step::Restyle, Step::Redraw},
               "тема прячет сноску прежде перекраски");

    // Прыжок — в конец книги: место заведомо сдвинется, и о нём будет слышно.
    bench.canvas.journal.clear();
    bench.presenter.tap(*openedMarkPoint, PointerButton::Left);
    bench.presenter.perform(Command::GoToEnd);
    checkSteps(bench.canvas,
               {Step::ShowNote, Step::FinishTurns, Step::HideNote, Step::Redraw,
                Step::PositionMoved},
               "прыжок: finishTurns → сноска → redraw → место");
    check(!bench.presenter.noteOpen(), "после прыжка сноски нет");

    // Обратно к знаку: глава могла сверстаться заново — точку знака ищем снова.
    bench.presenter.goToCharOffset(noteMark->markLetter);
    const std::optional<PageFlow::Point> markPointAfterEnd =
        markPointOf(bench.flow(), noteMark->noteBody);
    check(markPointAfterEnd.has_value(), "вернулись — знак снова на развороте");
    if (!markPointAfterEnd) return;

    bench.canvas.journal.clear();
    bench.presenter.tap(*markPointAfterEnd, PointerButton::Left);
    bench.presenter.perform(Command::TurnForward);
    checkSteps(bench.canvas,
               {Step::ShowNote, Step::HideNote, Step::TurnStarting, Step::Redraw, Step::TurnStarted,
                Step::PositionMoved},
               "переворот прячет сноску первым делом");
    check(!bench.presenter.noteOpen(), "после переворота сноски нет");

    bench.presenter.goToCharOffset(noteMark->markLetter);
    const std::optional<PageFlow::Point> markPointAfterTurn =
        markPointOf(bench.flow(), noteMark->noteBody);
    check(markPointAfterTurn.has_value(), "и снова знак на развороте");
    if (!markPointAfterTurn) return;
    bench.canvas.noteShows = false;
    bench.canvas.journal.clear();
    bench.presenter.tap(*markPointAfterTurn, PointerButton::Left);
    check(!bench.presenter.noteOpen(), "тело, из которого нечего набрать, — сноска не открыта");
    bench.presenter.tap(marginPoint(bench.flow(), 0.5f), PointerButton::Left);
    checkSteps(bench.canvas, {Step::ShowNote}, "и прятать потом нечего");

    // Открытие книги — на букве в глубине самой длинной главы: место
    // заведомо сдвинется.
    bench.canvas.noteShows = true;
    bench.canvas.journal.clear();
    bench.presenter.tap(*markPointAfterTurn, PointerButton::Left);
    bench.presenter.open(as_not_null(novel.get()), middleLetter(*novel));
    checkSteps(bench.canvas,
               {Step::ShowNote, Step::HideNote, Step::FinishTurns, Step::Redraw,
                Step::PositionMoved},
               "открытие книги прячет сноску первым делом");
}

/* ---------------- книга, досчёт, тема ---------------- */

/// Знак под указателем — тот же ответ, что дал бы щелчок: над знаком — да, и
/// щелчок там открывает сноску; в поле — нет; под мастером — нет, и щелчок
/// там сноски не открывает; без книги — нет. Вопрос к потоку страниц: холста
/// он не зовёт и без холста отвечает.
void testPointsAtNoteMark() {
    std::printf("\n=== ведущий: знак сноски под указателем ===\n");
    const intrusive_ptr<Book> novel = openNovel();
    if (!novel) return;

    const std::optional<NoteMark> noteMark = firstNoteMark(*novel);
    check(noteMark.has_value(), "в романе есть сноски");
    if (!noteMark) return;

    Bench bench;
    bench.openAt(as_not_null(novel.get()), noteMark->markLetter, kTwoColumnWidth);
    const std::optional<PageFlow::Point> markPoint = markPointOf(bench.flow(), noteMark->noteBody);
    check(markPoint.has_value(), "знак сноски — на развороте, где его буква");
    if (!markPoint) return;

    check(bench.presenter.pointsAtNoteMark(*markPoint), "над знаком — да");
    check(!bench.presenter.pointsAtNoteMark(marginPoint(bench.flow(), 0.5f)), "в поле — нет");
    checkSteps(bench.canvas, {}, "вопрос холста не зовёт");

    bench.presenter.tap(*markPoint, PointerButton::Left);
    checkSteps(bench.canvas, {Step::ShowNote}, "где «да», щелчок открывает сноску");
    bench.presenter.perform(Command::DismissNote);

    const Skin previewSkin = defaultSkin();
    bench.presenter.setPreview(&previewSkin);
    bench.canvas.journal.clear();
    check(!bench.presenter.pointsAtNoteMark(*markPoint), "под мастером над знаком — нет");

    // Щелчок под мастером — последним: он может перелистнуть, и знак уйдёт с
    // разворота.
    bench.presenter.tap(*markPoint, PointerButton::Left);
    check(!bench.canvas.first(Step::ShowNote) && !bench.presenter.noteOpen(),
          "и щелчок под мастером по знаку сноски не открывает");

    // Без книги — нет: с холстом он не зовётся, без холста не нужен.
    Bench emptyBench;
    emptyBench.presenter.resize(kTwoColumnWidth, kHeight, 1.0f);
    emptyBench.canvas.journal.clear();
    check(!emptyBench.presenter.pointsAtNoteMark(*markPoint), "без книги — нет");
    checkSteps(emptyBench.canvas, {}, "и холста не зовёт");

    ParkingIdle loneIdle;
    Settings loneSettings;
    auto loneThemes = ThemeList{std::vector<Skin>{}};
    CountingActions loneActions;
    cancellation_source loneSource;
    auto const canvaslessPresenter =
        BookPresenter{loneSettings, loneThemes, loneIdle, loneActions, loneSource.token()};
    check(!canvaslessPresenter.pointsAtNoteMark(*markPoint),
          "без холста и без книги — нет, и без остановки");
}

/// Открытие ставит место на ту букву, с которой открыли, и верстает: досчёт
/// заводится, холст — в конечное состояние до вёрстки, рисунок — после.
void testOpenPlacesAndLaysOut() {
    std::printf("\n=== ведущий: открытие книги ===\n");
    const intrusive_ptr<Book> novel = openNovel();
    if (!novel) return;

    Bench bench;
    bench.presenter.resize(kTwoColumnWidth, kHeight, 1.0f);
    check(!bench.presenter.isOpen() && bench.presenter.book() == nullptr, "до открытия книги нет");
    bench.canvas.journal.clear();

    const uint32_t openLetter = middleLetter(*novel);
    bench.presenter.open(as_not_null(novel.get()), openLetter);
    checkSteps(bench.canvas, {Step::FinishTurns, Step::Redraw, Step::PositionMoved},
               "открытие: finishTurns → redraw → место чтения");
    const std::optional<Entry> finishEntry = bench.canvas.first(Step::FinishTurns);
    const std::optional<Entry> redrawEntry = bench.canvas.first(Step::Redraw);
    check(finishEntry && !finishEntry->spreadShown, "finishTurns — до вёрстки");
    check(redrawEntry && redrawEntry->spreadShown, "redraw — по свёрстанному развороту");
    check(bench.presenter.isOpen() && bench.presenter.book() == novel.get(), "книга открыта");
    check(bench.presenter.position().get() == openLetter &&
              letterInLeftColumn(bench.flow(), openLetter),
          "место — та самая буква, она в левой колонке");
    check(bench.flow().anchor().chapter == longestChapter(*novel), "глава — та, где буква");
    check(bench.idle.parked() == 1, "досчёт главы заведён");
}

/// Хвост главы: «из …» до досчёта, «из M» после, и одна перерисовка — когда
/// глава досчитана.
void testTailRedrawsOnce() {
    std::printf("\n=== ведущий: хвост главы ===\n");
    const intrusive_ptr<Book> novel = openNovel();
    if (!novel) return;

    Bench bench;
    bench.openAt(as_not_null(novel.get()), middleLetter(*novel), kTwoColumnWidth);
    check(bench.flow().needsTail() && !bench.flow().status().total, "после открытия — «из …»");

    wakeAll(bench.idle);
    check(bench.flow().isComplete() && bench.flow().status().total == bench.flow().pageCount(),
          "досчитано — «из M»");
    checkSteps(bench.canvas, {Step::Redraw}, "досчитав, ведущий перерисовал разворот один раз");
    check(bench.canvas.first(Step::Redraw) && bench.canvas.first(Step::Redraw)->chapterComplete,
          "перерисовка — по досчитанной главе");
}

/// Тема и предпросмотр: перекраска — restyle и redraw; та же тема — ничего;
/// номер — по кругу списка; у предпросмотра бумага и кривые свои.
void testThemeAndPreview() {
    std::printf("\n=== ведущий: тема и предпросмотр ===\n");
    const intrusive_ptr<Book> novel = openNovel();
    if (!novel) return;

    Bench bench;
    bench.openAt(as_not_null(novel.get()), middleLetter(*novel), kTwoColumnWidth);

    bench.presenter.setTheme(1);
    checkSteps(bench.canvas, {Step::Restyle, Step::Redraw}, "тема: restyle → redraw");
    check(bench.presenter.theme().get() == 1 && &bench.presenter.paper() == &kThemes[1] &&
              bench.presenter.pageSkin() == nullptr,
          "бумага второй темы, обложки нет");

    bench.canvas.journal.clear();
    bench.presenter.setTheme(1);
    checkSteps(bench.canvas, {}, "та же тема — холст не тронут");

    bench.presenter.setTheme(bench.themes.count());
    check(bench.presenter.theme().get() == 0, "за последней обложкой — первая тема");

    bench.presenter.perform(Command::NextTheme);
    check(bench.presenter.theme().get() == 1, "следующая тема — клавишей");

    bench.presenter.setTheme(kThemeCount);
    check(bench.presenter.activeSkin() != nullptr &&
              bench.presenter.pageSkin() == bench.presenter.activeSkin() &&
              &bench.presenter.paper() == &kSkinTheme,
          "обложка: страница по её снимку и кривым, бумага обложек");

    Skin previewSkin = defaultSkin();
    previewSkin.name = u16_text{u"Проба"};
    bench.canvas.journal.clear();
    bench.presenter.setPreview(&previewSkin);
    checkSteps(bench.canvas, {Step::Restyle, Step::Redraw}, "предпросмотр: restyle → redraw");
    check(bench.presenter.previewing() &&
              bench.presenter.pageSkin() != bench.presenter.activeSkin() &&
              bench.presenter.pageSkin()->name == previewSkin.name,
          "под мастером страница — по кривым предпросмотра");
    check(bench.presenter.theme().get() == kThemeCount, "тема под предпросмотром та же");

    bench.canvas.journal.clear();
    bench.presenter.setPreview(nullptr);
    checkSteps(bench.canvas, {Step::Restyle, Step::Redraw}, "предпросмотр снят: restyle → redraw");
    check(!bench.presenter.previewing() &&
              bench.presenter.pageSkin() == bench.presenter.activeSkin(),
          "страница — снова по обложке темы");

    bench.canvas.journal.clear();
    bench.presenter.skinsChanged(kThemeCount);
    checkSteps(bench.canvas, {Step::Restyle, Step::Redraw},
               "список обложек сменился, номер тот же — вид пересобран");

    bench.canvas.journal.clear();
    bench.presenter.skinsChanged(0);
    checkSteps(bench.canvas, {Step::Restyle, Step::Redraw},
               "номер другой — перекраска одна, слушателем темы");
    check(bench.presenter.theme().get() == 0, "встали на новый номер");
}

/* ---------------- остановка на нарушенной проводке ---------------- */

/// Как кончился дочерний запуск пробы: код выхода, что он написал в stderr, и
/// не пришлось ли снимать его по сроку.
struct ProbeOutcome {
    int exitCode = -1;            ///< -1 — запустить не удалось
    std::string reportText;       ///< stderr пробы — отчёт об остановке, UTF-8
    bool timedOut = false;
};

/// Описатель ядра на время запуска пробы: закрывается сам на всяком выходе,
/// так что ранний возврат ничего не оставляет открытым.
class KernelHandle : private noncopyable {
public:
    KernelHandle() = default;
    explicit KernelHandle(HANDLE ownedHandle) noexcept : ownedHandle_(ownedHandle) {}
    ~KernelHandle() { reset(); }

    HANDLE get() const noexcept { return ownedHandle_; }

    /// Куда вызов Windows кладёт новый описатель; прежний закрывается.
    HANDLE* put() noexcept {
        reset();
        return &ownedHandle_;
    }

    void reset() noexcept {
        if (ownedHandle_) ::CloseHandle(ownedHandle_);
        ownedHandle_ = nullptr;
    }

private:
    HANDLE ownedHandle_ = nullptr;
};

/// Список атрибутов запуска с одним атрибутом — описателем, который наследует
/// дочерний: ему достаётся ровно этот, а не всё наследуемое, что родитель
/// держит в эту минуту.
class InheritedHandleList : private noncopyable {
public:
    explicit InheritedHandleList(HANDLE onlyInheritedHandle)
        : inheritedHandle_(onlyInheritedHandle) {
        // Первый вызов — только за размером списка: он и должен не удаться.
        SIZE_T listBytes = 0;
        ::InitializeProcThreadAttributeList(nullptr, 1, 0, &listBytes);
        listStorage_.resize(listBytes);
        if (!::InitializeProcThreadAttributeList(attributeList(), 1, 0, &listBytes)) return;
        listInitialized_ = true;
        // Windows держит указатель на сам описатель, пока список жив, —
        // потому описатель — член списка, а не временный.
        listReady_ = ::UpdateProcThreadAttribute(attributeList(), 0,
                                                 PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                                 &inheritedHandle_, sizeof inheritedHandle_,
                                                 nullptr, nullptr) != FALSE;
    }

    ~InheritedHandleList() {
        if (listInitialized_) ::DeleteProcThreadAttributeList(attributeList());
    }

    /// Собран ли список; нет — запускать пробу не с чем.
    bool ready() const noexcept { return listReady_; }

    LPPROC_THREAD_ATTRIBUTE_LIST attributeList() noexcept {
        return reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(listStorage_.data());
    }

private:
    HANDLE inheritedHandle_;
    std::vector<std::byte> listStorage_;
    bool listInitialized_ = false;
    bool listReady_ = false;
};

/// Дочерний запуск этой пробы: тот же исполняемый файл с флагом пробы. Окна и
/// консоли у дочернего нет; наследует он один описатель — пишущий конец
/// канала, его stdout и stderr — и стоит в задании, которое закрывается
/// вместе с родителем: пережить прогон, как бы тот ни кончился, проба не
/// может. Не ушёл за срок — снимается.
ProbeOutcome runStopProbe(std::string_view probeName) {
    ProbeOutcome probeOutcome;

    std::wstring testExecutable(32768, L'\0');
    const DWORD pathLength = ::GetModuleFileNameW(nullptr, testExecutable.data(),
                                                  static_cast<DWORD>(testExecutable.size()));
    if (pathLength == 0 || pathLength >= testExecutable.size()) return probeOutcome;
    testExecutable.resize(pathLength);

    // Задание заводится до запуска: не заведётся — снимать ещё некого.
    KernelHandle probeJob{::CreateJobObjectW(nullptr, nullptr)};
    if (!probeJob.get()) return probeOutcome;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jobLimits{};
    jobLimits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!::SetInformationJobObject(probeJob.get(), JobObjectExtendedLimitInformation, &jobLimits,
                                   sizeof jobLimits))
        return probeOutcome;

    // Отчёт пробы — строка-другая, буфер канала в 64 КБ вмещает его
    // заведомо: пишущий не встанет на полном канале, и читать можно после его
    // ухода.
    SECURITY_ATTRIBUTES pipeSecurity{};
    pipeSecurity.nLength = sizeof pipeSecurity;
    pipeSecurity.bInheritHandle = TRUE;
    KernelHandle reportRead;
    KernelHandle reportWrite;
    if (!::CreatePipe(reportRead.put(), reportWrite.put(), &pipeSecurity, kReportPipeBytes))
        return probeOutcome;
    // Конец отчёта наступает, когда закрыты все пишущие концы, а описатели
    // ушедшего дочернего закрываются вместе с ним; читающий конец не
    // наследуется из гигиены — он родителя, и только.
    if (!::SetHandleInformation(reportRead.get(), HANDLE_FLAG_INHERIT, 0)) return probeOutcome;

    InheritedHandleList inheritedHandles{reportWrite.get()};
    if (!inheritedHandles.ready()) return probeOutcome;

    const std::wstring probeArgument = std::wstring(kStopProbeFlag.begin(), kStopProbeFlag.end()) +
                                       std::wstring(probeName.begin(), probeName.end());
    std::wstring commandLine = L"\"" + testExecutable + L"\" " + probeArgument;
    STARTUPINFOEXW childStartup{};
    childStartup.StartupInfo.cb = sizeof childStartup;
    childStartup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    childStartup.StartupInfo.hStdInput = nullptr;
    childStartup.StartupInfo.hStdOutput = reportWrite.get();
    childStartup.StartupInfo.hStdError = reportWrite.get();
    childStartup.lpAttributeList = inheritedHandles.attributeList();
    // Остановленным: в задание дочерний встаёт прежде, чем сделает хоть шаг.
    PROCESS_INFORMATION childLaunch{};
    const BOOL childStarted = ::CreateProcessW(
        testExecutable.c_str(), commandLine.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
        &childStartup.StartupInfo, &childLaunch);

    // Свой пишущий конец — прочь: иначе конец отчёта не наступил бы и с
    // уходом дочернего.
    reportWrite.reset();
    if (!childStarted) return probeOutcome;
    const KernelHandle childProcess{childLaunch.hProcess};
    KernelHandle childThread{childLaunch.hThread};

    // Вне задания или не пущенный — не проба: снять и провал.
    if (!::AssignProcessToJobObject(probeJob.get(), childProcess.get()) ||
        ::ResumeThread(childThread.get()) == static_cast<DWORD>(-1)) {
        ::TerminateProcess(childProcess.get(), kKilledProbeExitCode);
        ::WaitForSingleObject(childProcess.get(), INFINITE);
        return probeOutcome;
    }
    childThread.reset();

    if (::WaitForSingleObject(childProcess.get(), kProbeTimeoutMs) == WAIT_TIMEOUT) {
        ::TerminateProcess(childProcess.get(), kKilledProbeExitCode);
        ::WaitForSingleObject(childProcess.get(), INFINITE);
        probeOutcome.timedOut = true;
    }

    char reportChunk[4096];
    DWORD chunkBytes = 0;
    while (::ReadFile(reportRead.get(), reportChunk, sizeof reportChunk, &chunkBytes, nullptr) &&
           chunkBytes > 0)
        probeOutcome.reportText.append(reportChunk, chunkBytes);

    DWORD childExitCode = 0;
    if (::GetExitCodeProcess(childProcess.get(), &childExitCode))
        probeOutcome.exitCode = static_cast<int>(childExitCode);
    return probeOutcome;
}

/// Проба остановилась своей строкой: кодом abort, в срок и с отчётом этого
/// места. Чужая остановка — другой текст — не зачёт.
void checkStops(std::string_view probeName, std::string_view expectedReport) {
    const ProbeOutcome probeOutcome = runStopProbe(probeName);
    const bool stoppedHere = !probeOutcome.timedOut &&
                             probeOutcome.exitCode == static_cast<int>(kAbortExitCode) &&
                             probeOutcome.reportText.find(expectedReport) != std::string::npos;
    check(stoppedHere, probeName);
    if (stoppedHere) return;
    std::printf("        код выхода: %d%s, отчёт: %s\n", probeOutcome.exitCode,
                probeOutcome.timedOut ? " (снят по сроку)" : "", probeOutcome.reportText.c_str());
}

/// Начало и конец книги без книги, команда не полосы и прокрутка без открытой
/// сноски — ошибка проводки: карта их так не отдаёт, и ведущий останавливает
/// читалку, а не пропускает.
/// Так же — второй холст поверх первого, ведущий, умирающий под холстом, и
/// просьба к холсту, которого нет. Незнакомое имя пробы дочерний запуск не
/// пропускает молча.
void testWiringFaultsStop() {
    std::printf("\n=== ведущий: нарушенная проводка — остановка ===\n");
    checkStops(kStartWithoutBook, "BookPresenter::perform: начало или конец книги без книги");
    checkStops(kEndWithoutBook, "BookPresenter::perform: начало или конец книги без книги");
    checkStops(kForeignCommand, "BookPresenter::perform: команда не полосы");
    checkStops(kScrollWithoutNote, "BookPresenter::perform: прокрутка сноски без сноски");
    checkStops(kSecondCanvas, "BookPresenter: второй холст поверх первого");
    checkStops(kCanvasOutlives, "BookPresenter: ведущий умирает, а холст под ним остался");
    checkStops(kNoCanvas, "BookPresenter: просьба к холсту, которого нет");

    const ProbeOutcome unknownOutcome = runStopProbe(kUnknownProbe);
    check(!unknownOutcome.timedOut && unknownOutcome.exitCode == 1 &&
              unknownOutcome.reportText.find(kUnknownProbeReport) != std::string::npos,
          "незнакомая проба — провал дочернего запуска, а не остановка");
}

/// Проба остановки в дочернем запуске. Остановилась — сюда не вернулась; не
/// остановилась — кончается сразу (_Exit), ничего не разрушая: иначе
/// остановку дал бы разбор того, что проба нарочно оставила сломанным, а не
/// проверяемое место. Исключение — тоже не остановка: оно значит, что
/// проверки нет, а сбой случился дальше.
int runStopProbeChild(std::string_view probeName) {
    // Остановка — это ожидаемый исход пробы, и окна о ней CRT показывать не
    // должна: отладочная сборка открыла бы окно abort() и ждала бы ответа, а
    // отчёт об ошибке ушёл бы в Windows.
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);

    // Сбой, а не остановка (обращение по пустому указателю и прочее), — тоже
    // без окна: Windows ждала бы ответа до срока пробы, и провал пришёл бы
    // снятием, а не своим кодом.
    ::SetErrorMode(::GetErrorMode() | SEM_NOGPFAULTERRORBOX);

#ifdef _DEBUG
    // Отладочная CRT сообщает о своих assert и ошибках окном с кнопками и ждёт
    // ответа, а у пробы отвечать некому. Сообщение — в stderr, то есть в канал
    // отчёта: в провале его увидит родитель.
    for (const int reportType : {_CRT_ASSERT, _CRT_ERROR}) {
        if (_CrtSetReportMode(reportType, _CRTDBG_MODE_FILE) == -1 ||
            _CrtSetReportFile(reportType, _CRTDBG_FILE_STDERR) == _CRTDBG_HFILE_ERROR) {
            std::fprintf(stderr, "проба остановки: отчёт CRT не отдан в stderr\n");
            return 1;
        }
    }
#endif

    if (isPictureStopProbe(probeName)) return runPictureStopProbe(probeName);
    if (isWorkspaceStopProbe(probeName)) return runWorkspaceStopProbe(probeName);

    const bool knownProbe =
        probeName == kStartWithoutBook || probeName == kEndWithoutBook ||
        probeName == kForeignCommand || probeName == kScrollWithoutNote ||
        probeName == kSecondCanvas || probeName == kCanvasOutlives || probeName == kNoCanvas;
    if (!knownProbe) {
        std::fprintf(stderr, "%.*s: %.*s\n", static_cast<int>(kUnknownProbeReport.size()),
                     kUnknownProbeReport.data(), static_cast<int>(probeName.size()),
                     probeName.data());
        return 1;
    }

    ParkingIdle idle;
    Settings settings;
    ThemeList themes;
    CountingActions actions;
    cancellation_source ownerSource;
    std::optional<BookPresenter> presenter;
    presenter.emplace(settings, themes, idle, actions, ownerSource.token());

    // Холсты пробы — на всю пробу: разрушить их значило бы снять с ведущего
    // то, что проба нарочно на нём оставила.
    std::optional<RecordingCanvas> firstCanvas;
    std::optional<RecordingCanvas> secondCanvas;
    try {
        if (probeName == kStartWithoutBook) {
            presenter->perform(Command::GoToStart);
        } else if (probeName == kEndWithoutBook) {
            presenter->perform(Command::GoToEnd);
        } else if (probeName == kForeignCommand) {
            presenter->perform(Command::ShowLibrary);
        } else if (probeName == kScrollWithoutNote) {
            // Холст есть: остановиться проба обязана на сноске, а не на
            // просьбе без холста.
            firstCanvas.emplace(*presenter);
            presenter->perform(Command::ScrollNotePageDown);
        } else if (probeName == kSecondCanvas) {
            firstCanvas.emplace(*presenter);
            secondCanvas.emplace(*presenter);
        } else if (probeName == kCanvasOutlives) {
            firstCanvas.emplace(*presenter);
            presenter.reset();
        } else if (probeName == kNoCanvas) {
            presenter->setActive(true);
        }
    } catch (...) {
        std::_Exit(1);
    }

    // Ведущий вернулся — значит, не остановился: проба провалена.
    std::_Exit(0);
}

}  // namespace

void checkStopProbe(std::string_view probeName, std::string_view expectedReport) {
    checkStops(probeName, expectedReport);
}

void runBookPresenterTests() {
    testTurnOrder();
    testTurnSameInBothModes();
    testTurnAtBookEdges();
    testTurnIntoChapterStartsTail();
    testRelayoutMidSeries();
    testJumpMidSeries();
    testJumpWithoutBook();
    testFontSizeRelayout();
    testStyleFromSettings();
    testResizeRelayout();
    testWindowResizeKeepsColumns();
    testActive();
    testTailEndsWithCanvas();
    testWheel();
    testTapThirds();
    testTapUnderWizard();
    testNote();
    testPointsAtNoteMark();
    testOpenPlacesAndLaysOut();
    testTailRedrawsOnce();
    testThemeAndPreview();
    testWiringFaultsStop();
}

std::optional<int> stopProbeExitCode(int argc, char* argv[]) {
    if (argc < 2) return std::nullopt;

    // Других аргументов у тестов нет: незнакомый — провал запуска, а не
    // молчаливый обычный прогон.
    const std::string_view launchArgument = argv[1];
    if (!launchArgument.starts_with(kStopProbeFlag)) {
        std::fprintf(stderr, "незнакомый аргумент запуска: %s\n", argv[1]);
        return 1;
    }
    return runStopProbeChild(launchArgument.substr(kStopProbeFlag.size()));
}
