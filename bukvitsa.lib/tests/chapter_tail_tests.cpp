// Тесты досчёта главы (ChapterTail) на настоящей книге. Простой — поддельный:
// его ожидания стоят в очереди, пока проба не сделает ход руками (wake), как
// стояли бы ожидания простоя окна. Так видно, что бывает между порциями:
// отменённый кадр — перевёрсткой, уходом владельца или разрушением держателя —
// на своём ходу кончается отменой, не тронув ни потока страниц, ни получателя.
// Кадр досчёта владеет собой (detached_task), и жив ли он, видно по счётчику
// таких кадров. Порция — по времени, и сколько их уйдёт на главу, зависит от
// машины: пробы смотрят на состояние, не на счёт порций.

#include <coroutine>
#include <cstdio>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>

#include "check.h"

// Последними: поток страниц ведёт к импорту модулей книги и wxl.core, досчёт —
// к wxl.async.
#include "bukvitsa/reader/page_flow.h"
#include "bukvitsa/reader/chapter_tail.h"

import wxl.async;
import wxl.core;

using namespace bukvitsa;
using namespace bukvitsa::reader;
using bukvitsa::reader::tests::check;
using wxl::async::cancellation_source;
using wxl::async::cancellation_token;
using wxl::async::detached_task;
using wxl::async::operation_canceled_exception;
using wxl::async::task;

namespace {

const std::filesystem::path kNovel = std::filesystem::path(BUKVITSA_TESTDATA_DIR) /
                                     L"Turgenev_I._Spisokshkolnoy._Otcyi_I_Deti.fb3";

/// Полоса проб — та же, что у проб хвоста в потоке страниц.
constexpr float kWidth = 1400.0f;
constexpr float kHeight = 900.0f;

/// Вид по умолчанию — тот, что у настроек.
constexpr float kFontSize = 20.0f;
constexpr float kLineHeight = 1.45f;
constexpr float kMargin = 0.075f;

/// Сколько ходов очереди проба делает самое большее: глава романа досчитывается
/// за единицы порций, а зависшая проба хуже упавшей.
constexpr int kMaxTurns = 10000;

/// Чем падают получатель и простой в пробах сбоя — по тексту проба узнаёт,
/// что до обработчика дошёл именно этот сбой.
constexpr char kReceiverFailure[] = "получатель упал";
constexpr char kIdleFailure[] = "простой упал";

std::string bytesOf(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

/// Роман, открытый заново для каждой пробы: кэш глав и текущая глава у книги
/// свои. Пусто — роман не открылся, и проба кончается, не начавшись.
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

/// Длина главы в символах книги.
uint32_t chapterLength(const Book& book, size_t index) {
    const uint32_t end = index + 1 < book.chapterCount() ? book.chapterFirstChar(index + 1)
                                                         : book.characterCount();
    return end - book.chapterFirstChar(index);
}

/// Самая длинная глава, кроме этой (номер вне книги — без исключений): хвост
/// у неё заведомо есть.
size_t longestChapter(const Book& book, size_t except) {
    size_t longest = except == 0 ? 1 : 0;
    for (size_t index = 0; index < book.chapterCount(); ++index)
        if (index != except && chapterLength(book, index) > chapterLength(book, longest))
            longest = index;
    return longest;
}

/// Поток на начале самой длинной главы, свёрстанный до разворота.
void setUp(PageFlow& flow, not_null<Book> book) {
    flow.open(book, book->chapterFirstChar(longestChapter(*book, book->chapterCount())));
    flow.setStyle(kFontSize, kLineHeight, kMargin);
    flow.resize(kWidth, kHeight);
    flow.relayout();
}

/// Простой, который будит проба: ожидания стоят в очереди, пока она не
/// сделает ход (wake), и ход будит первое. Отмена, как у ожидания простоя окна,
/// никого не будит: под отменённым токеном ожидание не встаёт вовсе, а стоящее
/// кончается отменой на том ходу, который его будит. Уход очереди (разрушение
/// планировщика) кончает стоящие ожидания отменой и при живом токене. Ни в
/// одном из этих исходов ожидание планировщика не трогает.
class ParkingIdle final : public IdleScheduler {
public:
    /// Чем кончается стоящее ожидание: ходом очереди, сбоем на этом ходу или
    /// уходом очереди.
    enum class Woken { turn, failure, closing };

    /// Уход очереди: стоящие ожидания кончаются отменой, как у простоя окна на
    /// выходе, — кадры не остаются стоять на разрушенном планировщике, если
    /// проба провалилась посреди хвоста.
    ~ParkingIdle() {
        while (wake(Woken::closing)) {
        }
    }

    task<> idle(cancellation_token stop) override {
        // Счёт — до всякой проверки: по нему видно, спрашивал ли держатель
        // простоя вообще.
        ++idleCalls_;
        stop.throw_if_canceled();
        auto const woken = co_await Parking{*this};
        if (woken == Woken::closing) throw operation_canceled_exception{};
        stop.throw_if_canceled();
        if (woken == Woken::failure) throw std::runtime_error(kIdleFailure);
    }

    /// Ход очереди: будит первое стоящее ожидание; false — будить некого.
    /// Ожидание уходит из очереди раньше, чем кадр возобновлён: кадр может
    /// кончиться внутри этого вызова вместе с ожиданием.
    bool wake(Woken how = Woken::turn) {
        if (parkedWaits_.empty()) return false;
        Parking* const parking = parkedWaits_.front();
        parkedWaits_.pop_front();
        parking->woken = how;
        auto const waiter = parking->waiter;
        waiter.resume();
        return true;
    }

    /// Сколько ожиданий стоит.
    size_t parked() const { return parkedWaits_.size(); }

    /// Сколько раз вставали в ожидание.
    int parkings() const { return parkings_; }

    /// Сколько раз спрашивали простоя — и под отменённым токеном тоже.
    int idleCalls() const { return idleCalls_; }

private:
    /// Ожидание в кадре корутины. Кадр досчёта владеет собой и стоящим не
    /// разрушается — уходит только через ход, — так что указатель в очереди
    /// живёт, пока ожидание в ней.
    struct Parking {
        ParkingIdle& owner;
        std::coroutine_handle<> waiter;
        Woken woken = Woken::turn;

        explicit Parking(ParkingIdle& scheduler) noexcept : owner(scheduler) {}
        Parking(const Parking&) = delete;
        Parking& operator=(const Parking&) = delete;

        bool await_ready() const noexcept { return false; }

        void await_suspend(std::coroutine_handle<> here) noexcept {
            waiter = here;
            owner.parkedWaits_.push_back(this);
            ++owner.parkings_;
        }

        Woken await_resume() const noexcept { return woken; }
    };

    std::deque<Parking*> parkedWaits_;
    int parkings_ = 0;
    int idleCalls_ = 0;
};

/// Ходы очереди, пока кто-нибудь ждёт простоя.
void wakeAll(ParkingIdle& idle) {
    for (int turns = 0; turns < kMaxTurns && idle.wake(); ++turns) {
    }
}

/// Сколько кадров detached_task живо сверх того, что было до пробы.
struct FrameCount {
    size_t before = detached_task::live_frames();
    size_t extra() const { return detached_task::live_frames() - before; }
};

/// Что поймал обработчик сбоев detached_task, пока его подменяла проба.
std::exception_ptr caughtFailure;

/// Подменяет обработчик сбоев detached_task на время пробы — по умолчанию он
/// завершает процесс — и возвращает прежний.
class FailureTrap : private noncopyable {
public:
    FailureTrap() : previousHandler_(wxl::async::on_detached_task_failure()) {
        caughtFailure = nullptr;
        wxl::async::on_detached_task_failure() = [](std::exception_ptr error) noexcept {
            caughtFailure = std::move(error);
        };
    }

    ~FailureTrap() {
        wxl::async::on_detached_task_failure() = previousHandler_;
        caughtFailure = nullptr;
    }

    /// Текст пойманного сбоя; пусто — сбоя не было.
    std::string message() const {
        try {
            if (caughtFailure) std::rethrow_exception(caughtFailure);
        } catch (const std::exception& failure) {
            return failure.what();
        } catch (...) {
            // Чужое исключение — не тот сбой, которого ждёт проба: пусто.
        }
        return {};
    }

private:
    wxl::async::detached_task_failure_handler previousHandler_;
};

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
    auto tail = ChapterTail{flow, idle, ownerSource.token(), [&completed] { ++completed; }};
    auto const frames = FrameCount{};

    const size_t pagesBefore = flow.pageCount();
    tail.start();
    check(flow.needsTail() && idle.parked() == 1 && frames.extra() == 1,
          "досчёт встал в ожидание простоя ещё внутри start()");
    check(flow.pageCount() == pagesBefore && completed == 0, "до хода очереди поток не тронут");

    tail.start();
    check(idle.parkings() == 1 && frames.extra() == 1, "идущий досчёт второй раз не заводится");

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
    check(idle.parked() == 1 && frames.extra() == 1, "знак опущен — в новой главе заведён кадр");

    wakeAll(idle);
    check(flow.isComplete() && completed == 2 && frames.extra() == 0,
          "и новая глава досчитана до конца");
}

/// Перевёрстка посреди хвоста: отмена никого не будит, прежний кадр на своём
/// ходу кончается отменой и не трогает ни потока, ни получателя; новый кадр,
/// заведённый рядом, доходит до конца по новой мере.
void testRelayoutCancelsTail() {
    std::printf("\n=== досчёт главы: перевёрстка посреди хвоста ===\n");
    const intrusive_ptr<Book> novel = openNovel();
    if (!novel) return;

    PageFlow flow;
    setUp(flow, as_not_null(novel.get()));
    ParkingIdle idle;
    cancellation_source ownerSource;
    int completed = 0;
    auto tail = ChapterTail{flow, idle, ownerSource.token(), [&completed] { ++completed; }};
    auto const frames = FrameCount{};

    tail.start();
    tail.restart();
    check(idle.parked() == 1 && frames.extra() == 1,
          "отмена никого не будит: прежний кадр стоит до хода очереди");

    // Как перевёрстка полосы: новая мера, глава начисто до разворота, досчёт
    // заново. Прежний кадр отменён и для держателя не существует — новый
    // заводится рядом с ним.
    flow.resize(kWidth - 400.0f, kHeight);
    flow.relayout();
    tail.start();
    check(flow.needsTail() && idle.parked() == 2 && frames.extra() == 2,
          "после перевёрстки заведён новый кадр");

    const size_t pagesBefore = flow.pageCount();
    idle.wake();
    check(idle.parked() == 1 && frames.extra() == 1, "прежний кадр на своём ходу ушёл");
    check(flow.pageCount() == pagesBefore && flow.needsTail() && completed == 0,
          "и не тронул ни потока, ни получателя");

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
    auto tail = ChapterTail{flow, idle, ownerSource.token(), [&completed] { ++completed; }};
    auto const frames = FrameCount{};

    tail.start();
    const size_t pagesBefore = flow.pageCount();
    ownerSource.cancel();
    check(idle.parked() == 1 && frames.extra() == 1,
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

/// Держатель разрушен, пока кадр стоит: кадр уходит на своём ходу отменой и
/// разрушенного держателя не трогает — как на выходе приложения.
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
        auto tail = ChapterTail{flow, idle, ownerSource.token(), [&completed] { ++completed; }};
        tail.start();
    }
    check(idle.parked() == 1 && frames.extra() == 1, "кадр пережил держателя и стоит");

    const size_t pagesBefore = flow.pageCount();
    idle.wake();
    check(idle.parked() == 0 && frames.extra() == 0, "на своём ходу кадр ушёл отменой");
    check(flow.pageCount() == pagesBefore && completed == 0, "поток и получатель не тронуты");
}

/// Сбой в цикле досчёта — ошибка программы, а не конец: кадр отдаёт его
/// обработчику сбоев сразу и опускает знак, так что следующий start() заводит
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
    auto const trap = FailureTrap{};
    auto tail = ChapterTail{flow, idle, ownerSource.token(), [&completed] { ++completed; }};
    auto const frames = FrameCount{};

    tail.start();
    idle.wake(ParkingIdle::Woken::failure);
    check(trap.message() == kIdleFailure, "сбой цикла дошёл до обработчика сразу — тот самый");
    check(idle.parked() == 0 && frames.extra() == 0 && completed == 0,
          "кадр ушёл, получатель не позван");

    tail.start();
    check(flow.needsTail() && idle.parked() == 1 && frames.extra() == 1,
          "знак опущен сбоем — start() заводит новый кадр без перевёрстки");

    wakeAll(idle);
    check(flow.isComplete() && completed == 1 && frames.extra() == 0, "новый кадр досчитал главу");
}

/// Сбой получателя уходит из кадра сразу в обработчик сбоев, ничего не копя.
void testReceiverFailureGoesToHandler() {
    std::printf("\n=== досчёт главы: сбой получателя — обработчику ===\n");
    const intrusive_ptr<Book> novel = openNovel();
    if (!novel) return;

    PageFlow flow;
    setUp(flow, as_not_null(novel.get()));
    ParkingIdle idle;
    cancellation_source ownerSource;
    auto const trap = FailureTrap{};
    auto tail = ChapterTail{flow, idle, ownerSource.token(),
                            [] { throw std::runtime_error(kReceiverFailure); }};
    auto const frames = FrameCount{};

    tail.start();
    wakeAll(idle);

    check(flow.isComplete(), "глава досчитана, получатель упал");
    check(trap.message() == kReceiverFailure,
          "сбой получателя дошёл до обработчика сразу — тот самый");
    check(idle.parked() == 0 && frames.extra() == 0, "кадр ушёл");
}

}  // namespace

void runChapterTailTests() {
    testTailRunsToEnd();
    testRelayoutCancelsTail();
    testOwnerGoneEndsTail();
    testHolderGoneEndsTail();
    testLoopFailureLowersMark();
    testReceiverFailureGoesToHandler();
}
