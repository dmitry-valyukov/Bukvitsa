// Тесты досчёта главы (ChapterTail) на настоящей книге. Простой — поддельный:
// его ожидание стоит, пока проба не сделает ход очереди руками (wake), как
// стояло бы ожидание простоя окна. Так видно, что держатель делает между
// порциями: перевёрстка снимает стоящий кадр, уход владельца кончает ожидание
// отменой, не трогая потока страниц. Порция — по времени, и сколько их уйдёт
// на главу, зависит от машины: пробы смотрят на состояние, не на счёт порций.

#include <coroutine>
#include <cstdio>
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

/// Простой, который будит проба: ожидание стоит, пока она не сделает ход
/// очереди (wake). Отмена, как у ожидания простоя окна, никого не будит: под
/// отменённым токеном ожидание не встаёт вовсе, а стоящее кончается отменой на
/// том ходу, который его будит. Встать может одно ожидание: досчёт у потока
/// страниц один.
class ParkingIdle final : public IdleScheduler {
public:
    task<> idle(cancellation_token stop) override {
        stop.throw_if_canceled();
        co_await Parking{*this};
        stop.throw_if_canceled();
    }

    /// Ход очереди: будит стоящее ожидание; false — будить некого.
    bool wake() {
        if (!parkedWaiter_) return false;
        std::exchange(parkedWaiter_, {}).resume();
        return true;
    }

    /// Стоит ли кто-нибудь в ожидании.
    bool parked() const { return static_cast<bool>(parkedWaiter_); }

    /// Сколько раз вставали в ожидание.
    int parkings() const { return parkings_; }

    /// Сколько стоявших ожиданий ушло вместе с разрушенным кадром.
    int dropped() const { return dropped_; }

private:
    /// Ожидание в кадре корутины. Кадр, разрушенный, пока оно стоит, уносит
    /// его с собой, и оно снимает себя с планировщика — иначе wake() возобновил
    /// бы уничтоженный кадр. Так же снимает себя ожидание простоя окна.
    class Parking {
    public:
        explicit Parking(ParkingIdle& owner) noexcept : owner_(owner) {}
        Parking(const Parking&) = delete;
        Parking& operator=(const Parking&) = delete;

        ~Parking() {
            if (!standing_) return;
            owner_.parkedWaiter_ = {};
            ++owner_.dropped_;
        }

        bool await_ready() const noexcept { return false; }

        void await_suspend(std::coroutine_handle<> waiter) noexcept {
            // Молча, пока правило держится: ожидание встаёт на каждую порцию.
            if (owner_.parkedWaiter_) check(false, "второе ожидание встало поверх первого");
            owner_.parkedWaiter_ = waiter;
            ++owner_.parkings_;
            standing_ = true;
        }

        void await_resume() noexcept { standing_ = false; }

    private:
        ParkingIdle& owner_;
        bool standing_ = false;
    };

    std::coroutine_handle<> parkedWaiter_;
    int parkings_ = 0;
    int dropped_ = 0;
};

/// Ходы очереди, пока кто-нибудь ждёт простоя. Сколько сделано.
int wakeAll(ParkingIdle& idle) {
    int turns = 0;
    while (turns < kMaxTurns && idle.wake()) ++turns;
    return turns;
}

/// Хвост доходит до конца главы, получатель зовётся один раз, кадр кончается
/// сам — его не снимают.
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

    const size_t before = flow.pageCount();
    tail.start();
    check(flow.needsTail() && idle.parked() && idle.parkings() == 1,
          "досчёт встал в ожидание простоя ещё внутри start()");
    check(flow.pageCount() == before && completed == 0, "до хода очереди поток не тронут");

    tail.start();
    check(idle.parkings() == 1, "идущий досчёт второй раз не заводится");

    wakeAll(idle);
    check(flow.isComplete() && !flow.needsTail() && flow.status().total == flow.pageCount(),
          "глава досчитана в простое — «из M» известно");
    check(completed == 1, "получатель позван один раз");
    check(!idle.parked() && idle.dropped() == 0, "кадр кончился сам, его не снимали");

    const int parkings = idle.parkings();
    tail.start();
    check(!idle.parked() && idle.parkings() == parkings, "досчитанной главе досчёт не заводится");
}

/// Перевёрстка посреди хвоста: стоящий кадр снят тут же, вместе с ожиданием,
/// а не на ходу очереди; новый кадр после неё заводится и доходит до конца.
void testRelayoutDropsTail() {
    std::printf("\n=== досчёт главы: перевёрстка посреди хвоста ===\n");
    const intrusive_ptr<Book> novel = openNovel();
    if (!novel) return;

    PageFlow flow;
    setUp(flow, as_not_null(novel.get()));
    ParkingIdle idle;
    cancellation_source owner;
    int completed = 0;
    auto tail = ChapterTail{flow, idle, owner.token(), [&completed] { ++completed; }};

    tail.start();
    const size_t before = flow.pageCount();
    tail.restart();
    check(!idle.parked() && idle.dropped() == 1,
          "перевёрстка сняла стоящий кадр — его ожидание ушло из простоя");
    check(flow.pageCount() == before && completed == 0,
          "снятый кадр не считал и не звал получателя");

    // Как перевёрстка полосы: новая мера, глава начисто до разворота, досчёт
    // заново. Отменённый, но не размотанный кадр выглядел бы здесь идущим, и
    // start() не завёл бы нового.
    flow.resize(kWidth - 400.0f, kHeight);
    flow.relayout();
    tail.start();
    check(flow.needsTail() && idle.parked() && idle.parkings() == 2,
          "после перевёрстки заведён новый кадр");

    wakeAll(idle);
    check(flow.isComplete() && !flow.needsTail(), "новый кадр досчитал главу по новой мере");
    check(completed == 1 && idle.dropped() == 1,
          "получатель позван один раз, новый кадр кончился сам");
}

/// Уход владельца: его токен отменён — стоящее ожидание кончается отменой на
/// ходу очереди, кадр кончается ею сам; поток страниц и получатель не тронуты.
/// Под отменённым токеном start() заводит кадр, который не встаёт в ожидание,
/// а тут же кончается отменой.
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

    tail.start();
    const size_t before = flow.pageCount();
    owner.cancel();
    check(idle.parked(), "отмена никого не будит: ожидание стоит до хода очереди");

    check(idle.wake(), "ход очереди будит отменённое ожидание");
    check(!idle.parked() && idle.dropped() == 0, "кадр кончился отменой сам, его не снимали");
    check(flow.needsTail() && flow.pageCount() == before && completed == 0,
          "поток не тронут, получатель не позван");

    tail.start();
    check(!idle.parked() && idle.parkings() == 1,
          "под отменённым токеном новый кадр в ожидание не встаёт");
    check(flow.pageCount() == before && completed == 0, "и кончается отменой, ничего не тронув");

    tail.restart();
    tail.start();
    check(!idle.parked() && idle.parkings() == 1 && completed == 0,
          "перевёрстка после ухода владельца заводит источник уже отменённым");
}

/// Сбой досчёта — ошибка программы, не конец: его отдаёт наверх ближайший
/// start(), а не уносит с собой кадр.
void testTailFailureGoesUp() {
    std::printf("\n=== досчёт главы: сбой не теряется ===\n");
    const intrusive_ptr<Book> novel = openNovel();
    if (!novel) return;

    PageFlow flow;
    setUp(flow, as_not_null(novel.get()));
    ParkingIdle idle;
    cancellation_source owner;
    auto tail = ChapterTail{flow, idle, owner.token(),
                            [] { throw std::runtime_error("получатель упал"); }};

    tail.start();
    wakeAll(idle);
    check(flow.isComplete() && !idle.parked(), "глава досчитана, получатель упал внутри кадра");

    bool thrown = false;
    try {
        tail.start();
    } catch (const std::runtime_error&) {
        thrown = true;
    }
    check(thrown, "сбой кадра вышел из следующего start()");

    thrown = false;
    try {
        tail.start();
    } catch (...) {
        thrown = true;
    }
    check(!thrown && !idle.parked(), "сбой отдан один раз, кадр отпущен");
}

}  // namespace

void runChapterTailTests() {
    testTailRunsToEnd();
    testRelayoutDropsTail();
    testOwnerGoneEndsTail();
    testTailFailureGoesUp();
}
