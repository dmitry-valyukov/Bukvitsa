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
using wxl::async::task;

namespace {

const std::filesystem::path kNovel = std::filesystem::path(BUKVITSA_TESTDATA_DIR) /
                                     L"Turgenev_I._Spisokshkolnoy._Otcyi_I_Deti.fb3";

/// Полоса проб — та же, что у проб хвоста в потоке страниц.
constexpr float kWidth = 1400.0f;
constexpr float kHeight = 900.0f;

/// Сколько ходов очереди проба делает самое большее: глава романа досчитывается
/// за единицы порций, а зависшая проба хуже упавшей.
constexpr int kMaxTurns = 10000;

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

/// Поток на начале самой длинной главы, свёрстанный до разворота: хвост у неё
/// заведомо есть.
void setUp(PageFlow& flow, not_null<Book> book) {
    size_t longest = 0;
    for (size_t index = 1; index < book->chapterCount(); ++index)
        if (chapterLength(*book, index) > chapterLength(*book, longest)) longest = index;

    flow.open(book, book->chapterFirstChar(longest));
    flow.setStyle(20.0f, 1.45f, 0.075f);
    flow.resize(kWidth, kHeight);
    flow.relayout();
}

/// Простой, который будит проба: ожидания стоят в очереди, пока она не
/// сделает ход (wake), и ход будит первое. Отмена, как у ожидания простоя окна,
/// никого не будит: под отменённым токеном ожидание не встаёт вовсе, а стоящее
/// кончается отменой на том ходу, который его будит, — и планировщика при этом
/// не трогает.
class ParkingIdle final : public IdleScheduler {
public:
    task<> idle(cancellation_token stop) override {
        stop.throw_if_canceled();
        co_await Parking{*this};
        stop.throw_if_canceled();
    }

    /// Ход очереди: будит первое стоящее ожидание; false — будить некого.
    /// Ручка уходит из очереди раньше, чем кадр возобновлён: кадр может
    /// кончиться внутри этого вызова, и его уже ничто не должно помнить.
    bool wake() {
        if (parkedWaiters_.empty()) return false;
        auto const waiter = parkedWaiters_.front();
        parkedWaiters_.pop_front();
        waiter.resume();
        return true;
    }

    /// Сколько ожиданий стоит.
    size_t parked() const { return parkedWaiters_.size(); }

    /// Сколько раз вставали в ожидание.
    int parkings() const { return parkings_; }

private:
    /// Ожидание в кадре корутины. Кадр, разрушенный, пока оно стоит, уносит
    /// его с собой, и оно снимает себя с очереди — иначе wake() возобновил бы
    /// уничтоженный кадр. Так же снимает себя ожидание простоя окна.
    class Parking {
    public:
        explicit Parking(ParkingIdle& owner) noexcept : owner_(owner) {}
        Parking(const Parking&) = delete;
        Parking& operator=(const Parking&) = delete;

        ~Parking() {
            if (standing_) std::erase(owner_.parkedWaiters_, waiter_);
        }

        bool await_ready() const noexcept { return false; }

        void await_suspend(std::coroutine_handle<> waiter) noexcept {
            waiter_ = waiter;
            owner_.parkedWaiters_.push_back(waiter);
            ++owner_.parkings_;
            standing_ = true;
        }

        void await_resume() noexcept { standing_ = false; }

    private:
        ParkingIdle& owner_;
        std::coroutine_handle<> waiter_;
        bool standing_ = false;
    };

    std::deque<std::coroutine_handle<>> parkedWaiters_;
    int parkings_ = 0;
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

/// Хвост доходит до конца главы, получатель зовётся один раз, кадр уходит
/// сам.
void testTailRunsToEnd() {
    std::printf("\n=== досчёт главы: хвост до конца ===\n");
    const intrusive_ptr<Book> novel = openNovel();
    if (!novel) return;

    PageFlow flow;
    setUp(flow, as_not_null(novel.get()));
    ParkingIdle idle;
    cancellation_source owner;
    int completed = 0;
    auto tail = ChapterTail{flow, idle, owner.token(), [&completed] { ++completed; }};
    auto const frames = FrameCount{};

    const size_t before = flow.pageCount();
    tail.start();
    check(flow.needsTail() && idle.parked() == 1 && frames.extra() == 1,
          "досчёт встал в ожидание простоя ещё внутри start()");
    check(flow.pageCount() == before && completed == 0, "до хода очереди поток не тронут");

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
    cancellation_source owner;
    int completed = 0;
    auto tail = ChapterTail{flow, idle, owner.token(), [&completed] { ++completed; }};
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

    const size_t before = flow.pageCount();
    idle.wake();
    check(idle.parked() == 1 && frames.extra() == 1, "прежний кадр на своём ходу ушёл");
    check(flow.pageCount() == before && flow.needsTail() && completed == 0,
          "и не тронул ни потока, ни получателя");

    wakeAll(idle);
    check(flow.isComplete() && !flow.needsTail(), "новый кадр досчитал главу по новой мере");
    check(completed == 1 && frames.extra() == 0, "получатель позван один раз, кадр ушёл сам");
}

/// Уход владельца: его токен отменён — стоящее ожидание кончается отменой на
/// ходу очереди, кадр уходит, поток страниц и получатель не тронуты; после
/// этого start() кадра не заводит, и перевёрстка этого не меняет.
void testOwnerGoneEndsTail() {
    std::printf("\n=== досчёт главы: уход владельца ===\n");
    const intrusive_ptr<Book> novel = openNovel();
    if (!novel) return;

    PageFlow flow;
    setUp(flow, as_not_null(novel.get()));
    ParkingIdle idle;
    cancellation_source owner;
    int completed = 0;
    auto tail = ChapterTail{flow, idle, owner.token(), [&completed] { ++completed; }};
    auto const frames = FrameCount{};

    tail.start();
    const size_t before = flow.pageCount();
    owner.cancel();
    check(idle.parked() == 1 && frames.extra() == 1,
          "отмена никого не будит: ожидание стоит до хода очереди");

    idle.wake();
    check(idle.parked() == 0 && frames.extra() == 0, "ход очереди кончил кадр отменой");
    check(flow.needsTail() && flow.pageCount() == before && completed == 0,
          "поток не тронут, получатель не позван");

    tail.start();
    check(idle.parkings() == 1 && frames.extra() == 0,
          "под отменённым владельцем кадр не заводится");

    tail.restart();
    tail.start();
    check(idle.parkings() == 1 && frames.extra() == 0 && completed == 0,
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
    cancellation_source owner;
    int completed = 0;
    auto const frames = FrameCount{};
    {
        auto tail = ChapterTail{flow, idle, owner.token(), [&completed] { ++completed; }};
        tail.start();
    }
    check(idle.parked() == 1 && frames.extra() == 1, "кадр пережил держателя и стоит");

    const size_t before = flow.pageCount();
    idle.wake();
    check(idle.parked() == 0 && frames.extra() == 0, "на своём ходу кадр ушёл отменой");
    check(flow.pageCount() == before && completed == 0, "поток и получатель не тронуты");
}

/// Что поймал обработчик сбоев detached_task, пока проба его подменяла.
std::exception_ptr caughtFailure;

/// Сбой досчёта — ошибка программы, а не конец: кадр отдаёт его обработчику
/// сбоев detached_task сразу, ничего не копя. Проба подменяет обработчик на
/// своё время (по умолчанию он завершает процесс) и возвращает прежний.
void testTailFailureGoesToHandler() {
    std::printf("\n=== досчёт главы: сбой получателя — обработчику ===\n");
    const intrusive_ptr<Book> novel = openNovel();
    if (!novel) return;

    PageFlow flow;
    setUp(flow, as_not_null(novel.get()));
    ParkingIdle idle;
    cancellation_source owner;
    auto tail = ChapterTail{flow, idle, owner.token(),
                            [] { throw std::runtime_error("получатель упал"); }};
    auto const frames = FrameCount{};

    auto& handler = wxl::async::on_detached_task_failure();
    auto const previous = handler;
    caughtFailure = nullptr;
    handler = [](std::exception_ptr error) noexcept { caughtFailure = std::move(error); };

    tail.start();
    wakeAll(idle);

    handler = previous;

    bool fromReceiver = false;
    try {
        if (caughtFailure) std::rethrow_exception(caughtFailure);
    } catch (const std::runtime_error&) {
        fromReceiver = true;
    } catch (...) {
    }
    caughtFailure = nullptr;

    check(flow.isComplete(), "глава досчитана, получатель упал");
    check(fromReceiver, "сбой получателя дошёл до обработчика сбоев сразу");
    check(idle.parked() == 0 && frames.extra() == 0, "кадр ушёл");
}

}  // namespace

void runChapterTailTests() {
    testTailRunsToEnd();
    testRelayoutCancelsTail();
    testOwnerGoneEndsTail();
    testHolderGoneEndsTail();
    testTailFailureGoesToHandler();
}
