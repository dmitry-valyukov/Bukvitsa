// Тесты досчёта главы (ChapterTail) на настоящей книге. Простой — поддельный:
// его ожидания стоят в очереди, пока проба не сделает ход руками (wake), как
// стояли бы ожидания простоя окна. Так видно, что бывает между порциями:
// отменённый кадр — перевёрсткой или уходом владельца — на своём ходу
// кончается отменой, не тронув ни потока страниц, ни получателя, а новый кадр
// или разрушение держателя останавливают его сразу. Задачу кадра держит
// держатель, и жив ли кадр, видно по счётчику кадров задач: стоящий досчёт —
// это два кадра, его и его ожидания простоя. Порция — по времени, и сколько
// их уйдёт на главу, зависит от машины: пробы смотрят на состояние, не на счёт
// порций.

#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>
#include <utility>

#include "check.h"

// Последними: поток страниц ведёт к импорту модулей книги и wxl.core, роман
// проб и досчёт — к wxl.async.
#include "bukvitsa/reader/page_flow.h"
#include "novel.h"
#include "bukvitsa/reader/chapter_tail.h"

import wxl.async;
import wxl.core;

using namespace bukvitsa;
using namespace bukvitsa::reader;
using namespace bukvitsa::reader::tests;
using wxl::async::cancellation_source;

namespace {

/// Ширина полосы проб — та же, что у проб хвоста в потоке страниц.
constexpr float kWidth = 1400.0f;

/// Чем падает получатель в пробе сбоя — по тексту проба узнаёт, что до
/// получателя сбоев дошёл именно этот сбой.
constexpr char kReceiverFailure[] = "получатель упал";

/// Поток на начале самой длинной главы, свёрстанный до разворота.
void setUp(PageFlow& flow, not_null<Book> book) {
    flow.open(book, book->chapterFirstChar(longestChapter(*book)));
    flow.setStyle(kFontSize, kLineHeight, kMargin);
    flow.resize(kWidth, kHeight);
    flow.relayout();
}

/// Сколько кадров задач живо сверх того, что было до пробы.
struct FrameCount {
    size_t before = wxl::async::live_frames();
    size_t extra() const { return wxl::async::live_frames() - before; }
};

/// Кадров у стоящего досчёта: его и его ожидания простоя.
constexpr size_t kWaitingTailFrames = 2;

/// Получатель сбоев досчёта на время пробы: что ему отдали.
class FailureTrap : private noncopyable {
public:
    /// Получатель для держателя — пишет сюда же.
    function<void(std::exception_ptr)> receiver() {
        return [this](std::exception_ptr error) { caughtFailure_ = std::move(error); };
    }

    /// Текст пойманного сбоя; пусто — сбоя не было.
    std::string message() const {
        try {
            if (caughtFailure_) std::rethrow_exception(caughtFailure_);
        } catch (const std::exception& failure) {
            return failure.what();
        } catch (...) {
            // Чужое исключение — не тот сбой, которого ждёт проба: пусто.
        }
        return {};
    }

private:
    std::exception_ptr caughtFailure_;
};

/// Получатель сбоев для проб, где сбоя быть не должно: сбой там — провал пробы.
function<void(std::exception_ptr)> failureIsAFault() {
    return [](std::exception_ptr) { check(false, "у досчёта не должно было быть сбоя"); };
}

/// Хвост доходит до конца главы, получатель зовётся один раз, кадр уходит
/// сам и опускает знак: в другой недосчитанной главе досчёт заводится снова.
void testTailRunsToEnd() {
    std::printf("\n=== досчёт главы: хвост до конца ===\n");
    const intrusive_ptr<Book> novel = openNovel();
    if (!novel) return;

    PageFlow flow;
    setUp(flow, as_not_null(novel.get()));
    ParkingIdle idle;
    cancellation_source ownerSource;
    int completed = 0;
    auto tail = ChapterTail{flow, idle, ownerSource.token(), [&completed] { ++completed; }, failureIsAFault()};
    auto const frames = FrameCount{};

    const size_t pagesBefore = flow.pageCount();
    tail.start();
    check(flow.needsTail() && idle.parked() == 1 && frames.extra() == kWaitingTailFrames,
          "досчёт встал в ожидание простоя ещё внутри start()");
    check(flow.pageCount() == pagesBefore && completed == 0, "до хода очереди поток не тронут");

    tail.start();
    check(idle.parkings() == 1 && frames.extra() == kWaitingTailFrames, "идущий досчёт второй раз не заводится");

    wakeAll(idle);
    check(flow.isComplete() && !flow.needsTail() && flow.status().total == flow.pageCount(),
          "глава досчитана в простое — «из M» известно");
    check(completed == 1, "получатель позван один раз");
    check(idle.parked() == 0 && frames.extra() == 0, "кадр ушёл сам");

    const int parkings = idle.parkings();
    tail.start();
    check(idle.parked() == 0 && idle.parkings() == parkings && frames.extra() == 0,
          "досчитанной главе досчёт не заводится");

    // Прыжок в другую длинную главу: её хвост не досчитан, и start() обязан
    // завести кадр — значит, прежний кадр, досчитав, опустил знак.
    const size_t other = longestChapter(*novel, novel->currentChapter());
    flow.show(flow.columnOf(novel->chapterFirstChar(other)));
    check(flow.needsTail(), "в другой длинной главе хвост ещё не досчитан");
    tail.start();
    check(idle.parked() == 1 && frames.extra() == kWaitingTailFrames, "знак опущен — в новой главе заведён кадр");

    wakeAll(idle);
    check(flow.isComplete() && completed == 2 && frames.extra() == 0,
          "и новая глава досчитана до конца");
}

/// Перевёрстка посреди хвоста: отмена никого не будит, прежний кадр стоит до
/// хода очереди; новый кадр, заведённый поверх, останавливает его сразу — ни
/// потока, ни получателя прежний не трогает — и доходит до конца по новой мере.
void testRelayoutCancelsTail() {
    std::printf("\n=== досчёт главы: перевёрстка посреди хвоста ===\n");
    const intrusive_ptr<Book> novel = openNovel();
    if (!novel) return;

    PageFlow flow;
    setUp(flow, as_not_null(novel.get()));
    ParkingIdle idle;
    cancellation_source ownerSource;
    int completed = 0;
    auto tail = ChapterTail{flow, idle, ownerSource.token(), [&completed] { ++completed; }, failureIsAFault()};
    auto const frames = FrameCount{};

    tail.start();
    tail.restart();
    check(idle.parked() == 1 && frames.extra() == kWaitingTailFrames,
          "отмена никого не будит: прежний кадр стоит до хода очереди");

    // Как перевёрстка полосы: новая мера, глава начисто до разворота, досчёт
    // заново. Прежний кадр отменён и для держателя не существует — задача
    // нового присваивается поверх и останавливает его.
    flow.resize(kWidth - 400.0f, kHeight);
    flow.relayout();
    const size_t pagesRelaid = flow.pageCount();
    tail.start();
    check(flow.needsTail() && idle.parked() == 1 && frames.extra() == kWaitingTailFrames,
          "после перевёрстки стоит один кадр — новый; прежний остановлен");
    check(flow.pageCount() == pagesRelaid && completed == 0, "прежний не тронул ни потока, ни получателя");

    wakeAll(idle);
    check(flow.isComplete() && !flow.needsTail(), "новый кадр досчитал главу по новой мере");
    check(completed == 1 && frames.extra() == 0, "получатель позван один раз, кадр ушёл сам");
}

/// Уход владельца: его токен отменён — стоящее ожидание кончается отменой на
/// ходу очереди, кадр уходит, поток страниц и получатель не тронуты; после
/// этого start() кадра не заводит и простоя не спрашивает, и перевёрстка
/// этого не меняет.
void testOwnerGoneEndsTail() {
    std::printf("\n=== досчёт главы: уход владельца ===\n");
    const intrusive_ptr<Book> novel = openNovel();
    if (!novel) return;

    PageFlow flow;
    setUp(flow, as_not_null(novel.get()));
    ParkingIdle idle;
    cancellation_source ownerSource;
    int completed = 0;
    auto tail = ChapterTail{flow, idle, ownerSource.token(), [&completed] { ++completed; }, failureIsAFault()};
    auto const frames = FrameCount{};

    tail.start();
    const size_t pagesBefore = flow.pageCount();
    ownerSource.cancel();
    check(idle.parked() == 1 && frames.extra() == kWaitingTailFrames,
          "отмена никого не будит: ожидание стоит до хода очереди");

    idle.wake();
    check(idle.parked() == 0 && frames.extra() == 0, "ход очереди кончил кадр отменой");
    check(flow.needsTail() && flow.pageCount() == pagesBefore && completed == 0,
          "поток не тронут, получатель не позван");

    // Простой под отменённым токеном и сам не встал бы — проверяется, что
    // держатель его даже не спрашивает: кадр не рождается вовсе.
    const int callsBefore = idle.idleCalls();
    tail.start();
    check(idle.idleCalls() == callsBefore && frames.extra() == 0,
          "под отменённым владельцем кадр не заводится");

    tail.restart();
    tail.start();
    check(idle.idleCalls() == callsBefore && frames.extra() == 0 && completed == 0,
          "и после перевёрстки: новый источник рождается отменённым");
}

/// Держатель разрушен, пока кадр стоит: задача кадра — его поле, и кадр уходит
/// вместе с ним, сразу, не дожидаясь хода очереди и не тронув потока страниц.
void testHolderGoneEndsTail() {
    std::printf("\n=== досчёт главы: держатель разрушен ===\n");
    const intrusive_ptr<Book> novel = openNovel();
    if (!novel) return;

    PageFlow flow;
    setUp(flow, as_not_null(novel.get()));
    ParkingIdle idle;
    cancellation_source ownerSource;
    int completed = 0;
    auto const frames = FrameCount{};
    {
        auto tail = ChapterTail{flow, idle, ownerSource.token(), [&completed] { ++completed; }, failureIsAFault()};
        tail.start();
    }
    check(idle.parked() == 0 && frames.extra() == 0, "кадр ушёл вместе с держателем, и его ожидание снято");
    check(flow.needsTail() && completed == 0, "поток и получатель не тронуты");
}

/// Перевёрстка из получателя «глава досчитана» — restart(), затем start() —
/// задачи кадра, который этого получателя зовёт, не меняет: он на стеке, и
/// новый круг идёт в нём же, под новым источником, — до конца новой главы.
void testRestartFromReceiverGoesAnotherRound() {
    std::printf("\n=== досчёт главы: перевёрстка из получателя ===\n");
    const intrusive_ptr<Book> novel = openNovel();
    if (!novel) return;

    PageFlow flow;
    setUp(flow, as_not_null(novel.get()));
    ParkingIdle idle;
    cancellation_source ownerSource;
    int completed = 0;
    ChapterTail* holder = nullptr;
    const size_t other = longestChapter(*novel, novel->currentChapter());
    auto tail = ChapterTail{flow, idle, ownerSource.token(),
                            [&] {
                                if (++completed > 1) return;
                                // Как прыжок полосы в другую недосчитанную главу.
                                flow.show(flow.columnOf(novel->chapterFirstChar(other)));
                                holder->restart();
                                holder->start();
                            },
                            failureIsAFault()};
    holder = &tail;
    auto const frames = FrameCount{};

    tail.start();
    wakeAll(idle);

    check(completed == 2 && flow.isComplete() && !flow.needsTail(),
          "из получателя досчёт пошёл на новый круг и досчитал другую главу");
    check(idle.parked() == 0 && frames.extra() == 0, "кадр ушёл сам");
}

/// Перевёрстка из получателя без нового start(): кадр, вернувшись из
/// получателя, кончается, знаки опущены, и start() снаружи заводит новый кадр.
void testRestartFromReceiverWithoutStart() {
    std::printf("\n=== досчёт главы: перевёрстка из получателя без start() ===\n");
    const intrusive_ptr<Book> novel = openNovel();
    if (!novel) return;

    PageFlow flow;
    setUp(flow, as_not_null(novel.get()));
    ParkingIdle idle;
    cancellation_source ownerSource;
    int completed = 0;
    ChapterTail* holder = nullptr;
    const size_t other = longestChapter(*novel, novel->currentChapter());
    auto tail = ChapterTail{flow, idle, ownerSource.token(),
                            [&] {
                                ++completed;
                                if (completed > 1) return;
                                flow.show(flow.columnOf(novel->chapterFirstChar(other)));
                                holder->restart();
                            },
                            failureIsAFault()};
    holder = &tail;
    auto const frames = FrameCount{};

    tail.start();
    wakeAll(idle);
    check(completed == 1 && flow.needsTail(), "получатель перевёрстал, другая глава ещё не досчитана");
    check(idle.parked() == 0 && frames.extra() == 0, "без start() кадр кончился, вернувшись из получателя");

    tail.start();
    check(idle.parked() == 1 && frames.extra() == kWaitingTailFrames, "знаки опущены — start() заводит новый кадр");

    wakeAll(idle);
    check(completed == 2 && flow.isComplete() && frames.extra() == 0, "новый кадр досчитал другую главу");
}

/// Перевёрстка из получателя сбоев (restart, затем start): тот же кадр идёт на
/// новый круг, второго кадра под бегущим не заводится.
void testRestartFromFailureReceiverGoesAnotherRound() {
    std::printf("\n=== досчёт главы: перевёрстка из получателя сбоев ===\n");
    const intrusive_ptr<Book> novel = openNovel();
    if (!novel) return;

    PageFlow flow;
    setUp(flow, as_not_null(novel.get()));
    ParkingIdle idle;
    cancellation_source ownerSource;
    int completed = 0;
    int failed = 0;
    ChapterTail* holder = nullptr;
    auto tail = ChapterTail{flow, idle, ownerSource.token(), [&completed] { ++completed; },
                            [&](std::exception_ptr) {
                                ++failed;
                                holder->restart();
                                holder->start();
                            }};
    holder = &tail;
    auto const frames = FrameCount{};

    tail.start();
    idle.wake(ParkingIdle::Woken::failure);
    check(failed == 1 && completed == 0, "сбой дошёл до получателя сбоев один раз");
    check(idle.parked() == 1 && frames.extra() == kWaitingTailFrames,
          "досчёт, попрошенный из получателя сбоев, — новый круг того же кадра: один кадр, одно ожидание");

    wakeAll(idle);
    check(completed == 1 && flow.isComplete() && frames.extra() == 0, "кадр досчитал главу и ушёл");
}

/// Сбой в цикле досчёта — ошибка программы, а не конец: кадр отдаёт его
/// получателю сбоев сразу и опускает знак, так что следующий start() заводит
/// новый кадр без перевёрстки. Порцию потока страниц сломать нечем — падает
/// простой, на неотменённом ходу.
void testLoopFailureLowersMark() {
    std::printf("\n=== досчёт главы: сбой в цикле ===\n");
    const intrusive_ptr<Book> novel = openNovel();
    if (!novel) return;

    PageFlow flow;
    setUp(flow, as_not_null(novel.get()));
    ParkingIdle idle;
    cancellation_source ownerSource;
    int completed = 0;
    auto trap = FailureTrap{};
    auto tail = ChapterTail{flow, idle, ownerSource.token(), [&completed] { ++completed; }, trap.receiver()};
    auto const frames = FrameCount{};

    tail.start();
    idle.wake(ParkingIdle::Woken::failure);
    check(trap.message() == kIdleFailure, "сбой цикла дошёл до получателя сбоев сразу — тот самый");
    check(idle.parked() == 0 && frames.extra() == 0 && completed == 0,
          "кадр ушёл, получатель не позван");

    tail.start();
    check(flow.needsTail() && idle.parked() == 1 && frames.extra() == kWaitingTailFrames,
          "знак опущен сбоем — start() заводит новый кадр без перевёрстки");

    wakeAll(idle);
    check(flow.isComplete() && completed == 1 && frames.extra() == 0, "новый кадр досчитал главу");
}

/// Сбой получателя уходит из кадра сразу получателю сбоев, ничего не копя.
void testReceiverFailureGoesToHandler() {
    std::printf("\n=== досчёт главы: сбой получателя — получателю сбоев ===\n");
    const intrusive_ptr<Book> novel = openNovel();
    if (!novel) return;

    PageFlow flow;
    setUp(flow, as_not_null(novel.get()));
    ParkingIdle idle;
    cancellation_source ownerSource;
    auto trap = FailureTrap{};
    auto tail = ChapterTail{flow, idle, ownerSource.token(),
                            [] { throw std::runtime_error(kReceiverFailure); }, trap.receiver()};
    auto const frames = FrameCount{};

    tail.start();
    wakeAll(idle);

    check(flow.isComplete(), "глава досчитана, получатель упал");
    check(trap.message() == kReceiverFailure,
          "сбой получателя дошёл до получателя сбоев сразу — тот самый");
    check(idle.parked() == 0 && frames.extra() == 0, "кадр ушёл");
}

}  // namespace

void runChapterTailTests() {
    testTailRunsToEnd();
    testRelayoutCancelsTail();
    testOwnerGoneEndsTail();
    testHolderGoneEndsTail();
    testRestartFromReceiverGoesAnotherRound();
    testRestartFromReceiverWithoutStart();
    testRestartFromFailureReceiverGoesAnotherRound();
    testLoopFailureLowersMark();
    testReceiverFailureGoesToHandler();
}
