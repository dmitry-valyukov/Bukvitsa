// Тесты очереди картинок книги (PictureQueue) на настоящей книге с
// картинками — официальном примере nightmare.
//
// Правила очереди проверяются на ней самой: стенд держит поток страниц,
// поддельный простой и очередь, а получатель очереди — свой счётчик стенда.
// Так в счёт попадают только перерисовки по готовым картинкам: у ведущего
// тот же холст перерисовывает и досчёт главы, и его перерисовка пришла бы
// в счёт на первом же ходе простоя. Проводку ведущего (смена книги, уход
// холста, замена устройства) проверяют пробы через ведущего — по состояниям
// картинок, без счёта перерисовок.
//
// Раскодирование — настоящее, на рабочем потоке спящей петли проб: он один и
// берёт операции по порядку, так что поток, занятый пробой (HeldWorker),
// держит раскодирование «в пути» сколько нужно пробе. Простой — поддельный:
// ожидание стоит, пока проба не сделает ход, а отменённое кончается отменой
// только на том ходу, который его будит (ParkingIdle::wake): так подделка
// моделирует ход очереди потока — у окна отменённое ожидание простоя
// кончается на ближайшем ходу его очереди, здесь — на wake().
//
// Битмапы устройства делает контекст Direct2D без окна — на программном
// устройстве D3D11 (WARP): у каждого такого контекста своё устройство, и
// замена устройства проверяется двумя.
//
// Нарушенный договор книги (раскодирование не ждущей картинки, ответ или
// отмена без просьбы, «картинка» без пикселей, контекст чужого устройства)
// останавливает читалку — такие пробы идут в дочернем процессе, общим
// запуском проб остановки ведущего (`checkStopProbe`).

#include <objbase.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wincodec.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <optional>
#include <semaphore>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "check.h"

// Последними: поток страниц ведёт к импорту модулей книги и wxl.core, роман
// проб, ведущий, очередь и раскодирование — к wxl.async.
#include "bukvitsa/reader/page_flow.h"
#include "novel.h"
#include "bukvitsa/reader/book_presenter.h"
#include "bukvitsa/reader/picture_queue.h"
#include "bukvitsa/reader/imaging.h"

import wxl.async;
import wxl.core;

using namespace bukvitsa;
using namespace bukvitsa::reader;
using namespace bukvitsa::reader::tests;
using Microsoft::WRL::ComPtr;
using wxl::async::cancellation_source;
using wxl::async::sta_loop;
using wxl::async::task;

/// Общий запуск проб остановки — у проб ведущего (book_presenter_tests.cpp).
void checkStopProbe(std::string_view probeName, std::string_view expectedReport);

namespace {

const std::filesystem::path kPictureBook = std::filesystem::path(BUKVITSA_TESTDATA_DIR) /
                                           L"nightmare_example.fb3";

/// Полоса в одну колонку: разворот — одна страница, и соседи — ровно по
/// странице с каждой стороны.
constexpr float kStripWidth = 700.0f;

/// Сколько разворотов разведчик проходит самое большее, ища нужный.
constexpr int kMaxScoutTurns = 2000;

/// Глава романа, у начала которой проба соседа назад смотрит через границу:
/// не первая, и перед ней непустая.
constexpr size_t kBoundaryChapter = 2;

/// Имена проб остановки очереди.
constexpr std::string_view kBeginTwice = "picture-begin-twice";
constexpr std::string_view kAcceptUnasked = "picture-accept-unasked";
constexpr std::string_view kAbandonUnasked = "picture-abandon-unasked";
constexpr std::string_view kReadyWithoutPixels = "picture-ready-without-pixels";
constexpr std::string_view kForeignDevice = "picture-foreign-device";

/// Книга с картинками, открытая заново для каждой пробы: состояние картинок
/// у книги своё, и пробы не должны видеть друг друга через него. Пусто — не
/// открылась.
intrusive_ptr<Book> openPictureBook() {
    intrusive_ptr<Book> book;
    try {
        book = make_refcounted<Book>(kPictureBook, fb3::Document{bytesOf(kPictureBook)}, dwriteFactory());
    } catch (const std::exception& failure) {
        std::printf("  книга с картинками не открылась: %s\n", failure.what());
        book = nullptr;
    }
    check(book && book->pictureCount() > 0 && book->picturesInFlow(), "nightmare открыт, картинки в потоке");
    if (book && !book->picturesInFlow()) book = nullptr;
    return book;
}

/// Вид полосы — тот, что у настроек по умолчанию: у ведущего он оттуда же.
void styleAsSettings(PageFlow& flow) {
    const Settings defaults;
    flow.setStyle(static_cast<float>(defaults.fontSize.get()),
                  static_cast<float>(defaults.lineHeight.get()) / 100.0f,
                  static_cast<float>(defaults.margin.get()) / 100.0f);
}

/// Сколько картинок книги в этом состоянии.
size_t countState(const Book& book, PictureState state) {
    size_t counted = 0;
    for (uint32_t index = 0; index < book.pictureCount(); ++index)
        if (book.pictureState(index) == state) ++counted;
    return counted;
}

/// Первая картинка в этом состоянии; пусто — таких нет.
std::optional<uint32_t> firstInState(const Book& book, PictureState state) {
    for (uint32_t index = 0; index < book.pictureCount(); ++index)
        if (book.pictureState(index) == state) return index;
    return std::nullopt;
}

bool contains(const std::vector<uint32_t>& pictures, uint32_t picture) {
    return std::find(pictures.begin(), pictures.end(), picture) != pictures.end();
}

/// Раскодированы ли все эти картинки — готовы или отвергнуты по данным.
bool allSettled(const Book& book, const std::vector<uint32_t>& pictures) {
    for (const uint32_t picture : pictures) {
        const PictureState state = book.pictureState(picture);
        if (state != PictureState::Ready && state != PictureState::Refused) return false;
    }
    return true;
}

/// Сколько из этих картинок готово.
int countReady(const Book& book, const std::vector<uint32_t>& pictures) {
    int ready = 0;
    for (const uint32_t picture : pictures)
        if (book.pictureState(picture) == PictureState::Ready) ++ready;
    return ready;
}

/// Ходы петли, пока у неё что-нибудь в пути: раскодирования в пуле вернулись,
/// и очередь встала — на простое или кончилась. Под занятым рабочим потоком
/// не звать: ждать пришлось бы ворот.
void drain() {
    sta_loop::run_until([] { return sta_loop::outstanding() == 0; });
}

/// Рабочий поток занят, пока ворота не открыты: раскодирования, ушедшие ему
/// после, стоят не начатыми. Не открытые к концу — открываются на выходе.
class HeldWorker {
public:
    HeldWorker() : holdingTask_(sta_loop::async_call([this] { workerGate_.acquire(); })) {}

    HeldWorker(const HeldWorker&) = delete;
    HeldWorker& operator=(const HeldWorker&) = delete;

    ~HeldWorker() {
        if (holdingTask_) workerGate_.release();
    }

    /// Ворота открыты, и тело, державшее поток, вернулось.
    void open() {
        workerGate_.release();
        task<> holding = std::move(holdingTask_);
        sta_loop::run_until([&holding] { return holding.done(); });
        holding.result();
    }

private:
    std::binary_semaphore workerGate_{0};
    task<> holdingTask_;
};

/// Контекст Direct2D на своём программном устройстве (WARP) — без окна и без
/// видеокарты; у каждого вызова устройство своё. Пусто — не завёлся.
ComPtr<ID2D1DeviceContext> softwareContext() {
    ComPtr<ID3D11Device> d3dDevice;
    if (FAILED(::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                   nullptr, 0, D3D11_SDK_VERSION, &d3dDevice, nullptr, nullptr)))
        return nullptr;
    ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(d3dDevice.As(&dxgiDevice))) return nullptr;
    ComPtr<ID2D1Factory1> d2dFactory;
    if (FAILED(::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2dFactory.GetAddressOf())))
        return nullptr;
    ComPtr<ID2D1Device> d2dDevice;
    if (FAILED(d2dFactory->CreateDevice(dxgiDevice.Get(), &d2dDevice))) return nullptr;
    ComPtr<ID2D1DeviceContext> context;
    if (FAILED(d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &context))) return nullptr;
    return context;
}

/* ---------------- стенд очереди ---------------- */

/// Очередь на своём потоке страниц и поддельном простое; получатель — счётчик
/// стенда. Простой — раньше очереди (она держит ссылку на него) и умирает
/// последним; очередь — необязательная, чтобы проба могла её разрушить.
struct QueueBench {
    ParkingIdle idle;
    cancellation_source ownerSource;
    PageFlow flow;
    int shownReady = 0;
    std::optional<PictureQueue> queue{std::in_place, flow, idle, ownerSource.token(), [this] { ++shownReady; },
                                      [](std::exception_ptr) { check(false, "у очереди не должно было быть сбоя"); }};

    /// Книга на этой букве, вид и мера как у ведущего; открытие — как у
    /// ведущего: очередь перезаводится до смены книги, просьба — после
    /// вёрстки.
    void openAt(not_null<Book> book, uint32_t openLetter) {
        queue->restart();
        flow.open(book, openLetter);
        styleAsSettings(flow);
        flow.resize(kStripWidth, kHeight);
        flow.relayout();
        queue->request(WindowMove::Stays);
    }

    /// Переворот на разворот — как у ведущего: шаг книги, затем просьба.
    bool turn(bool forward) {
        const std::optional<PageFlow::Column> target = flow.turnTarget(forward);
        if (!target) return false;
        flow.show(*target);
        queue->request(forward ? WindowMove::TurnedForward : WindowMove::TurnedBackward);
        return true;
    }

    /// Прыжок к букве — как у ведущего.
    void jump(uint32_t letter) {
        flow.show(flow.columnOf(letter));
        queue->request(WindowMove::Jumped);
    }

    std::vector<uint32_t> shown() const { return flow.spreadPictures(); }
};

/// Разворот, найденный разведчиком: буква, с которой он открывается, его
/// картинки и картинки соседей; буква следующего разворота.
struct ScoutedSpread {
    uint32_t letter = 0;
    std::vector<uint32_t> shown;
    std::vector<uint32_t> forward;
    std::vector<uint32_t> backward;
    uint32_t nextLetter = 0;
};

/// Что разведчик ищет у разворота.
struct ScoutWants {
    bool shown = true;      ///< есть свои картинки
    bool forward = false;   ///< у соседа впереди — свои картинки
    size_t forwardCount = 1;  ///< сколько их самое меньшее
    bool backward = false;  ///< у соседа позади (в той же главе) — свои, не те, что впереди
};

/// Первый разворот полосы пробы, у которого есть то, что ищут. Ищет своим
/// потоком страниц на своей копии книги — та же мера и тот же вид, что у
/// стенда и ведущего, значит, те же развороты; сосед позади — в той же главе,
/// чтобы его знал и стенд, у которого предыдущая глава может быть не
/// досчитана. Пусто — не нашлось.
std::optional<ScoutedSpread> scoutSpread(ScoutWants wants) {
    const intrusive_ptr<Book> scoutBook = openPictureBook();
    if (!scoutBook) return std::nullopt;

    PageFlow scout;
    scout.open(as_not_null(scoutBook.get()), 0);
    styleAsSettings(scout);
    scout.resize(kStripWidth, kHeight);
    scout.relayout();

    for (int turn = 0; turn < kMaxScoutTurns; ++turn) {
        ScoutedSpread found;
        found.letter = scout.position();
        found.shown = scout.spreadPictures();
        found.forward = scout.neighborPictures(true);
        found.backward = scout.anchor().index > 0 ? scout.neighborPictures(false) : std::vector<uint32_t>{};
        bool disjoint = true;
        for (const uint32_t picture : found.backward) disjoint = disjoint && !contains(found.forward, picture);

        const std::optional<PageFlow::Column> next = scout.turnTarget(true);
        if (!next) return std::nullopt;   // конец книги
        scout.show(*next);
        found.nextLetter = scout.position();

        if (wants.shown && found.shown.empty()) continue;
        if (wants.forward && found.forward.size() < wants.forwardCount) continue;
        if (wants.backward && (found.backward.empty() || !disjoint)) continue;
        return found;
    }
    return std::nullopt;
}

/* ---------------- стенд ведущего ---------------- */

/// Исполнитель намерений, которому нечего делать; сбой машинерии полосы — провал
/// пробы.
class IdleActions final : public BookPresenter::Actions {
public:
    void togglePanel() override {}
    void reportFailure(std::exception_ptr) noexcept override { check(false, "у полосы не должно было быть сбоя"); }
};

/// Холст, который ничего не делает и не рисует. Встаёт под ведущего
/// последним своим членом и уходит, умирая, — как полоса Reader.
class SilentCanvas final : public PageCanvas {
public:
    explicit SilentCanvas(BookPresenter& presenter) : canvasAttachment_(presenter.attachCanvas(*this)) {}

    void turnStarting(bool) override {}
    void turnStarted(bool) override {}
    void finishTurns() override {}
    void redraw() noexcept override { ++redraws; }
    void restyle() override {}
    bool showNote(const PageFlow::NoteHit&) override { return false; }
    void hideNote() override {}
    void scrollNote(NoteScroll) override {}
    void activate(bool) override {}

    /// Все перерисовки — и досчёта главы, и очереди: считать их можно только
    /// там, где досчёт стоит.
    int redraws = 0;

private:
    BookPresenter::CanvasAttachment canvasAttachment_;
};

/// Всё, что ведущему даёт владелец, — в порядке жизни; холст —
/// необязательный, чтобы проба могла его снять.
struct PresenterBench {
    ParkingIdle idle;
    Settings settings;
    ThemeList themes{std::vector<Skin>{}};
    IdleActions actions;
    cancellation_source ownerSource;
    BookPresenter presenter{settings, themes, idle, actions, ownerSource.token()};
    std::optional<SilentCanvas> canvas{std::in_place, presenter};

    void openAt(not_null<Book> book, uint32_t openLetter) {
        presenter.resize(kStripWidth, kHeight, 1.0f);
        presenter.open(book, openLetter);
    }

    std::vector<uint32_t> shown() const { return presenter.flow().spreadPictures(); }
};

/* ---------------- книга ---------------- */

/// При открытии — только заголовки: не картинка (svg) — `Refused` без байтов,
/// прочие — `Unknown` со сжатыми байтами и без пикселей. Рисунок не
/// раскодирует: nullptr и то же состояние.
void testPicturesAtOpen() {
    std::printf("\n=== картинки книги: при открытии — только заголовки ===\n");
    const intrusive_ptr<Book> book = openPictureBook();
    if (!book) return;

    check(countState(*book, PictureState::Refused) > 0, "svg — не картинка для WIC, Refused уже при открытии");
    bool refusedHoldNothing = true;
    bool unknownHoldBytesOnly = true;
    for (uint32_t index = 0; index < book->pictureCount(); ++index) {
        const PictureHolding holding = book->pictureHolding(index);
        if (book->pictureState(index) == PictureState::Refused)
            refusedHoldNothing = refusedHoldNothing && !holding.bytes && !holding.pixels && !holding.bitmap;
        else
            unknownHoldBytesOnly = unknownHoldBytesOnly && book->pictureState(index) == PictureState::Unknown &&
                                   holding.bytes && !holding.pixels && !holding.bitmap;
    }
    check(refusedHoldNothing, "Refused не держит ничего");
    check(unknownHoldBytesOnly, "остальные — Unknown: сжатые байты, без пикселей и битмапа");

    const ComPtr<ID2D1DeviceContext> context = softwareContext();
    check(context != nullptr, "контекст Direct2D без окна (WARP) завёлся");
    if (!context) return;
    const std::optional<uint32_t> unknown = firstInState(*book, PictureState::Unknown);
    if (!unknown) return;
    check(book->bitmap(*unknown, context.Get()) == nullptr &&
              book->pictureState(*unknown) == PictureState::Unknown && !book->pictureHolding(*unknown).pixels,
          "рисунок не раскодирует: nullptr, картинка всё ещё Unknown");
}

/// Отказ данных: картинка — `Refused`, сжатые байты отпущены. Через очередь
/// не проверить: битого по данным файла с целым заголовком в тестовых книгах
/// нет, а обрезанные JPEG и PNG WIC не отвергает — отдаёт то, что успел
/// раскодировать. Поэтому ответ пула подаётся книге прямо.
void testRefusalReleasesBytes() {
    std::printf("\n=== картинки книги: отказ данных отпускает байты ===\n");
    const intrusive_ptr<Book> book = openPictureBook();
    if (!book) return;
    const std::optional<uint32_t> unknown = firstInState(*book, PictureState::Unknown);
    if (!unknown) return;

    const std::string sentBytes = book->beginDecoding(*unknown);
    check(!sentBytes.empty() && book->pictureHolding(*unknown).bytes, "в пул ушла копия, книга свои держит");
    book->acceptPicture(*unknown, DecodedPicture{PictureVerdict{PictureRefusal::NotAnImage, {}}, {}});
    check(book->pictureState(*unknown) == PictureState::Refused && !book->pictureHolding(*unknown).bytes,
          "не картинка по данным — Refused, байты отпущены");
}

/* ---------------- поток страниц: сосед назад ---------------- */

/// Сосед назад через границу главы: недосчитанную (здесь — не начатую)
/// предыдущую главу предвыборка не верстает и соседа не берёт; досчитанную —
/// берёт тот же разворот, что и переворот назад.
void testBackwardNeighborAcrossChapter() {
    std::printf("\n=== поток страниц: сосед назад через границу главы ===\n");
    const intrusive_ptr<Book> novel = openNovel();
    if (!novel || novel->chapterCount() <= kBoundaryChapter) return;

    PageFlow flow;
    flow.open(as_not_null(novel.get()), novel->chapterFirstChar(kBoundaryChapter));
    styleAsSettings(flow);
    flow.resize(kStripWidth, kHeight);
    flow.relayout();
    check(flow.anchor() == PageFlow::Column{kBoundaryChapter, 0}, "разворот — начало главы");

    check(!flow.neighborSpread(false).has_value(), "предыдущая не начата — сосед назад неизвестен");
    check(novel->chapterAt(kBoundaryChapter - 1).pageCount() == 0, "и она не свёрстана ради предвыборки");

    // Переворот назад верстает предыдущую главу целиком — дальше её конец
    // известен.
    const std::optional<PageFlow::Column> viaTurn = flow.turnTarget(false);
    const std::optional<PageFlow::Column> neighbor = flow.neighborSpread(false);
    check(viaTurn && neighbor && *neighbor == *viaTurn,
          "предыдущая досчитана — сосед назад тот же, что у переворота");
}

/* ---------------- очередь ---------------- */

/// Книга без картинок в потоке: очередь не заводит кадра — ни простоя, ни
/// пула.
void testBookWithoutPicturesStartsNothing() {
    std::printf("\n=== очередь картинок: книга без картинок ===\n");
    const intrusive_ptr<Book> novel = openNovel();
    if (!novel) return;
    check(!novel->picturesInFlow(), "у романа картинок в потоке нет");

    QueueBench bench;
    bench.openAt(as_not_null(novel.get()), 0);
    check(bench.idle.idleCalls() == 0 && sta_loop::outstanding() == 0, "кадр не заведён: ни простоя, ни пула");
}

/// Картинки показанного разворота раскодируются сразу, соседи — только в
/// простое; по каждой готовой показанной — ровно один зов получателя.
void testShownPicturesReadyOnce() {
    std::printf("\n=== очередь картинок: показанный разворот, по зову на картинку ===\n");
    const std::optional<ScoutedSpread> spread = scoutSpread({.shown = true, .forward = true});
    check(spread.has_value(), "в книге есть разворот с картинками и сосед с картинками");
    const intrusive_ptr<Book> book = openPictureBook();
    if (!spread || !book) return;

    QueueBench bench;
    bench.openAt(as_not_null(book.get()), spread->letter);
    check(bench.shown() == spread->shown, "стенд стоит на том же развороте, что разведчик");
    check(bench.shownReady == 0, "до ответа пула зовов нет");

    drain();
    const int readyShown = countReady(*book, spread->shown);
    check(allSettled(*book, spread->shown) && readyShown > 0, "картинки разворота раскодированы");
    check(bench.shownReady == readyShown, "по одному зову на готовую картинку");
    check(countReady(*book, spread->forward) == 0 && bench.idle.parked() == 1,
          "соседи ждут простоя: очередь стоит в нём, сосед не тронут");

    drain();
    check(bench.shownReady == readyShown, "повторных зовов нет");
}

/// В пуле — одно раскодирование за раз.
void testOneAtATime() {
    std::printf("\n=== очередь картинок: по одной ===\n");
    const std::optional<ScoutedSpread> spread = scoutSpread({});
    const intrusive_ptr<Book> book = openPictureBook();
    if (!spread || !book) return;

    QueueBench bench;
    HeldWorker held;
    bench.openAt(as_not_null(book.get()), spread->letter);
    check(countState(*book, PictureState::Decoding) == 1, "в пути ровно одна картинка");

    held.open();
    drain();
    check(countState(*book, PictureState::Decoding) == 0 && allSettled(*book, spread->shown),
          "разворот раскодирован, в пути ничего");
}

/// Перевёрстка идущее раскодирование не отменяет; переворот вперёд не
/// отменяет раскодирование показанной.
void testStayAndTurnKeepDecoding() {
    std::printf("\n=== очередь картинок: перевёрстка и переворот не отменяют показанную ===\n");
    const std::optional<ScoutedSpread> spread = scoutSpread({});
    const intrusive_ptr<Book> book = openPictureBook();
    if (!spread || !book) return;

    QueueBench bench;
    HeldWorker held;
    bench.openAt(as_not_null(book.get()), spread->letter);
    const std::optional<uint32_t> travelling = firstInState(*book, PictureState::Decoding);
    if (!travelling) return;

    bench.queue->request(WindowMove::Stays);
    check(bench.turn(true), "переворот вперёд");
    check(book->pictureState(*travelling) == PictureState::Decoding, "картинка всё ещё в пути");

    held.open();
    drain();
    check(book->pictureState(*travelling) == PictureState::Ready ||
              book->pictureState(*travelling) == PictureState::Refused,
          "её ответ отдан книге");
}

/// Соседи раскодируются в простое, с той стороны, куда идёт читатель,
/// первыми; переворот назад отменяет раскодирование соседа впереди — он
/// выпал из окна, и его ответ никому не уходит.
void testNeighborOrderAndDrop() {
    std::printf("\n=== очередь картинок: соседи по ходу листания, выпавший отменён ===\n");
    // Впереди — хотя бы две свои картинки: во второй части пробы одна из них
    // раскодируется до переворота назад, а другая должна остаться, чтобы
    // порядок соседей было чем различить.
    const std::optional<ScoutedSpread> spread =
        scoutSpread({.shown = false, .forward = true, .forwardCount = 2, .backward = true});
    check(spread.has_value(), "в книге есть разворот с картинками у соседей с обеих сторон");
    if (!spread) return;

    {
        const intrusive_ptr<Book> book = openPictureBook();
        if (!book) return;
        QueueBench bench;
        bench.openAt(as_not_null(book.get()), spread->letter);
        drain();

        HeldWorker held;
        wakeAll(bench.idle);
        const std::optional<uint32_t> first = firstInState(*book, PictureState::Decoding);
        check(first && contains(spread->forward, *first), "вперёд (по умолчанию) — сначала сосед впереди");
        if (!first) return;

        check(bench.turn(false), "переворот назад");
        sta_loop::run_until([&] { return book->pictureState(*first) != PictureState::Decoding; });
        check(book->pictureState(*first) == PictureState::Unknown, "сосед впереди выпал — раскодирование отменено");

        const int readyBefore = bench.shownReady;
        held.open();
        drain();
        check(book->pictureState(*first) != PictureState::Ready || contains(bench.shown(), *first),
              "ответ осиротевшего тела книге не отдан");
        check(bench.shownReady == readyBefore + countReady(*book, bench.shown()),
              "зовы — только по готовым картинкам нового разворота");
    }

    {
        const intrusive_ptr<Book> book = openPictureBook();
        if (!book) return;
        // С разворота после найденного — назад на него: до переворота пул
        // успевает взять не больше одной картинки того, что станет соседом
        // впереди, и у соседа впереди остаются нераскодированные.
        QueueBench bench;
        {
            HeldWorker held;
            bench.openAt(as_not_null(book.get()), spread->nextLetter);
            check(bench.turn(false), "назад на найденный разворот");
            check(bench.shown() == spread->shown, "стенд на нём");
            held.open();
        }
        drain();
        bool forwardPending = false;
        for (const uint32_t picture : spread->forward)
            forwardPending = forwardPending || book->pictureState(picture) == PictureState::Unknown;
        check(forwardPending, "у соседа впереди есть нераскодированные");

        HeldWorker held;
        wakeAll(bench.idle);
        const std::optional<uint32_t> first = firstInState(*book, PictureState::Decoding);
        check(first && contains(spread->backward, *first), "читатель идёт назад — сначала сосед позади");
        held.open();
        drain();
    }
}

/// Прыжок отменяет раскодирование соседа прежнего окна; ушедшие из окна
/// отпускают пиксели, сжатые байты целы.
void testJumpDropsAndEvicts() {
    std::printf("\n=== очередь картинок: прыжок — отмена соседа и вытеснение ===\n");
    const std::optional<ScoutedSpread> spread = scoutSpread({.shown = true, .forward = true});
    const intrusive_ptr<Book> book = openPictureBook();
    if (!spread || !book) return;

    QueueBench bench;
    bench.openAt(as_not_null(book.get()), spread->letter);
    drain();

    std::optional<uint32_t> dropped;
    {
        HeldWorker held;
        wakeAll(bench.idle);
        dropped = firstInState(*book, PictureState::Decoding);
        check(dropped && contains(spread->forward, *dropped), "сосед в пути");
        bench.jump(book->characterCount());
        // Ждать отмены — только если он выпал из окна: оказавшись в новом
        // развороте, он остаётся в пути до открытия ворот.
        if (dropped && !contains(bench.shown(), *dropped))
            sta_loop::run_until([&] { return book->pictureState(*dropped) != PictureState::Decoding; });
        held.open();
    }
    drain();
    wakeAll(bench.idle);
    drain();

    const std::vector<uint32_t> window = bench.shown();
    if (dropped && !contains(window, *dropped))
        check(book->pictureState(*dropped) == PictureState::Unknown, "его раскодирование отменено, ответа нет");

    bool evicted = true;
    bool bytesKept = true;
    for (const uint32_t picture : spread->shown) {
        if (contains(window, picture) || book->pictureState(picture) == PictureState::Refused) continue;
        const PictureHolding holding = book->pictureHolding(picture);
        evicted = evicted && book->pictureState(picture) != PictureState::Ready && !holding.pixels && !holding.bitmap;
        bytesKept = bytesKept && holding.bytes;
    }
    check(evicted, "картинки прежнего разворота отпустили пиксели");
    check(bytesKept, "сжатые байты у них целы");
}

/// Соседи раскодируются без зовов; переворот на соседа застаёт его картинки
/// готовыми — зова по ним нет.
void testPrefetchNeighbors() {
    std::printf("\n=== очередь картинок: соседи в простое ===\n");
    const std::optional<ScoutedSpread> spread = scoutSpread({.shown = true, .forward = true});
    const intrusive_ptr<Book> book = openPictureBook();
    if (!spread || !book) return;

    QueueBench bench;
    bench.openAt(as_not_null(book.get()), spread->letter);
    drain();
    const int readyShown = bench.shownReady;

    wakeAll(bench.idle);
    drain();
    check(bench.shownReady == readyShown, "соседи раскодируются без зовов");
    check(allSettled(*book, spread->forward), "сосед впереди раскодирован");

    check(bench.turn(true), "переворот вперёд");
    check(allSettled(*book, bench.shown()), "к перевороту картинки соседа готовы");
    drain();
    check(bench.shownReady == readyShown, "по готовым зова нет");
}

/// Готовая картинка становится битмапом при рисунке: пиксели отпускаются,
/// сжатые байты остаются; повторный рисунок — тот же битмап. Замена
/// устройства: битмап отпущен; очередь, стоящая в простое за соседями,
/// берёт картинку разворота сразу, не дожидаясь простоя (соседи после этого
/// всё ещё не узнаны); пиксели, ещё не ставшие битмапом, замена не трогает.
void testDeviceReplacementNotWaitingIdle() {
    std::printf("\n=== очередь картинок: замена устройства — без ожидания простоя ===\n");
    const std::optional<ScoutedSpread> spread = scoutSpread({.shown = true, .forward = true});
    const intrusive_ptr<Book> book = openPictureBook();
    const ComPtr<ID2D1DeviceContext> firstContext = softwareContext();
    const ComPtr<ID2D1DeviceContext> secondContext = softwareContext();
    check(firstContext && secondContext, "два контекста на своих программных устройствах");
    if (!spread || !book || !firstContext || !secondContext) return;

    QueueBench bench;
    bench.openAt(as_not_null(book.get()), spread->letter);
    drain();
    std::vector<uint32_t> readyShown;
    for (const uint32_t picture : spread->shown)
        if (book->pictureState(picture) == PictureState::Ready) readyShown.push_back(picture);
    if (readyShown.empty()) return;

    const uint32_t drawn = readyShown.front();
    ID2D1Bitmap1* const firstBitmap = book->bitmap(drawn, firstContext.Get());
    const PictureHolding afterDraw = book->pictureHolding(drawn);
    check(firstBitmap != nullptr && afterDraw.bitmap && !afterDraw.pixels && afterDraw.bytes,
          "рисунок: битмап есть, пиксели отпущены, сжатые байты целы");
    check(book->bitmap(drawn, firstContext.Get()) == firstBitmap, "повторный рисунок — тот же битмап");

    const std::optional<uint32_t> undrawn =
        readyShown.size() > 1 ? std::optional<uint32_t>{readyShown[1]} : std::nullopt;

    // Как у ведущего (renderingDeviceReplaced): книга отпускает битмапы,
    // очередь просят о том же окне.
    check(bench.idle.parked() == 1, "очередь стоит в простое за соседями");
    const int callsBefore = bench.shownReady;
    book->dropDeviceBitmaps();
    bench.queue->request(WindowMove::Stays);
    const PictureHolding afterDrop = book->pictureHolding(drawn);
    check(book->pictureState(drawn) == PictureState::Unknown && !afterDrop.bitmap && afterDrop.bytes,
          "замена: битмап отпущен, картинка снова Unknown, сжатые байты целы");
    if (undrawn)
        check(book->pictureState(*undrawn) == PictureState::Ready && book->pictureHolding(*undrawn).pixels,
              "пиксели, ещё не ставшие битмапом, замена не трогает");

    // Ход очереди потока (у ParkingIdle — wake): отменённое ожидание простоя
    // кончается только на нём, и кадр берёт картинку разворота, а не соседей.
    check(bench.idle.wake(), "ход очереди потока");
    drain();
    check(book->pictureState(drawn) == PictureState::Ready, "раскодирована заново из сжатых байтов");
    check(bench.shownReady == callsBefore + 1, "по ней — один зов");
    check(countReady(*book, spread->forward) == 0 && bench.idle.parked() == 1,
          "соседи не узнаны: картинка разворота не ждала простоя");
    check(book->bitmap(drawn, secondContext.Get()) != nullptr, "битмап нового устройства — без остановки");
}

/// Ответ пула, пришедший после замены устройства, отдаётся книге как обычно
/// — пиксели от устройства не зависят — и встаёт битмапом нового.
void testAnswerAfterDeviceReplacement() {
    std::printf("\n=== очередь картинок: ответ после замены устройства ===\n");
    const std::optional<ScoutedSpread> spread = scoutSpread({});
    const intrusive_ptr<Book> book = openPictureBook();
    const ComPtr<ID2D1DeviceContext> context = softwareContext();
    if (!spread || !book || !context) return;

    QueueBench bench;
    std::optional<uint32_t> travelling;
    {
        HeldWorker held;
        bench.openAt(as_not_null(book.get()), spread->letter);
        travelling = firstInState(*book, PictureState::Decoding);
        book->dropDeviceBitmaps();
        bench.queue->request(WindowMove::Stays);
        check(travelling && book->pictureState(*travelling) == PictureState::Decoding,
              "замена не трогает картинку в пути");
        held.open();
    }
    drain();
    if (!travelling) return;
    check(book->pictureState(*travelling) != PictureState::Decoding, "ответ отдан книге");
    if (book->pictureState(*travelling) == PictureState::Ready)
        check(book->bitmap(*travelling, context.Get()) != nullptr, "и встал битмапом нового устройства");
}

/// Разрушение очереди забывает ушедшее раскодирование: картинка снова
/// Unknown, ответ тела никому не уходит.
void testQueueDestructorForgets() {
    std::printf("\n=== очередь картинок: разрушение забывает ушедшее ===\n");
    const std::optional<ScoutedSpread> spread = scoutSpread({});
    const intrusive_ptr<Book> book = openPictureBook();
    if (!spread || !book) return;

    QueueBench bench;
    {
        HeldWorker held;
        bench.openAt(as_not_null(book.get()), spread->letter);
        check(countState(*book, PictureState::Decoding) == 1, "картинка в пути");
        bench.queue.reset();
        check(countState(*book, PictureState::Decoding) == 0, "очередь разрушена — ушедшее забыто");
        held.open();
    }
    drain();
    check(countState(*book, PictureState::Ready) == 0, "ответ тела никому не ушёл");
}

/// Перезавод очереди (restart, затем request) — и пока картинка в пути, и
/// пока очередь стоит в простое: новый кадр, присвоенный поверх,
/// останавливает прежний, и кадры не копятся — их столько же, сколько у одной
/// очереди. Разрушение очереди уносит и последний.
void testRestartKeepsOneFrame() {
    std::printf("\n=== очередь картинок: перезавод — один кадр ===\n");
    const std::optional<ScoutedSpread> spread = scoutSpread({.shown = true, .forward = true});
    const intrusive_ptr<Book> book = openPictureBook();
    if (!spread || !book) return;

    QueueBench bench;
    {
        HeldWorker held;
        const size_t before = wxl::async::live_frames();
        bench.openAt(as_not_null(book.get()), spread->letter);
        const size_t oneQueue = wxl::async::live_frames() - before;
        check(oneQueue > 0 && countState(*book, PictureState::Decoding) == 1, "кадр очереди стоит на раскодировании");

        bench.openAt(as_not_null(book.get()), spread->letter);
        check(wxl::async::live_frames() - before == oneQueue && countState(*book, PictureState::Decoding) == 1,
              "перезавод на раскодировании: прежний кадр остановлен, в пути одна картинка");
        held.open();
    }
    drain();
    check(allSettled(*book, spread->shown) && bench.idle.parked() == 1, "разворот раскодирован, очередь в простое");

    const size_t before = wxl::async::live_frames();
    bench.openAt(as_not_null(book.get()), spread->letter);
    drain();
    check(wxl::async::live_frames() == before && bench.idle.parked() == 1,
          "перезавод в простое: прежний кадр и его ожидание ушли, новый стоит на своём");

    bench.queue.reset();
    check(wxl::async::live_frames() < before && bench.idle.parked() == 0,
          "разрушение очереди уносит её кадр и ожидание простоя");
}

/* ---------------- проводка ведущего ---------------- */

/// Смена книги отменяет очередь: прежняя книга забывает ушедшее
/// раскодирование, его ответ никому не уходит, перерисовок от него нет.
/// Досчёт главы здесь стоит (простой не будят), так что счёт перерисовок —
/// только очереди и открытия.
void testPresenterBookChangeCancels() {
    std::printf("\n=== ведущий: смена книги отменяет очередь ===\n");
    const std::optional<ScoutedSpread> spread = scoutSpread({});
    const intrusive_ptr<Book> book = openPictureBook();
    const intrusive_ptr<Book> novel = openNovel();
    if (!spread || !book || !novel) return;

    PresenterBench bench;
    {
        HeldWorker held;
        bench.openAt(as_not_null(book.get()), spread->letter);
        check(countState(*book, PictureState::Decoding) == 1, "картинка прежней книги в пути");

        bench.presenter.open(as_not_null(novel.get()), 0);
        check(countState(*book, PictureState::Decoding) == 0, "прежняя книга забыла ушедшее раскодирование");
        held.open();
    }
    const int redrawsAfterOpen = bench.canvas->redraws;
    drain();
    check(bench.canvas->redraws == redrawsAfterOpen, "ответ осиротевшего тела перерисовки не дал");
    check(countState(*book, PictureState::Ready) == 0, "картинки прежней книги ответа не получили");
}

/// Уход холста отменяет очередь: ушедшее раскодирование книга забывает, а
/// ответ тела холста не зовёт (зови он — проба остановилась бы «просьбой к
/// холсту, которого нет»). Отмена владельцем: ответа нет, а ушедшее забывает
/// уход холста следом.
void testPresenterCanvasAndOwnerCancel() {
    std::printf("\n=== ведущий: уход холста и отмена владельцем ===\n");
    const std::optional<ScoutedSpread> spread = scoutSpread({});
    const intrusive_ptr<Book> book = openPictureBook();
    const intrusive_ptr<Book> secondBook = openPictureBook();
    if (!spread || !book || !secondBook) return;

    {
        PresenterBench bench;
        {
            HeldWorker held;
            bench.openAt(as_not_null(book.get()), spread->letter);
            check(countState(*book, PictureState::Decoding) == 1, "картинка в пути");
            bench.canvas.reset();
            check(countState(*book, PictureState::Decoding) == 0, "уход холста: ушедшее забыто");
            held.open();
        }
        drain();
        check(countState(*book, PictureState::Ready) == 0, "ответ тела никому не ушёл");
    }

    {
        PresenterBench bench;
        {
            HeldWorker held;
            bench.openAt(as_not_null(secondBook.get()), spread->letter);
            check(countState(*secondBook, PictureState::Decoding) == 1, "картинка в пути");
            bench.ownerSource.cancel();
            held.open();
        }
        const int redrawsBefore = bench.canvas->redraws;
        drain();
        check(bench.canvas->redraws == redrawsBefore && countState(*secondBook, PictureState::Ready) == 0,
              "отмена владельцем: ответа нет, перерисовки нет");
        bench.canvas.reset();
        check(countState(*secondBook, PictureState::Decoding) == 0, "уход холста следом забыл ушедшее");
    }
}

/// Замена устройства через ведущего: битмапы книги отпущены, очередь
/// раскодирует картинку заново, и она встаёт битмапом нового устройства.
void testPresenterDeviceReplacement() {
    std::printf("\n=== ведущий: замена устройства ===\n");
    const std::optional<ScoutedSpread> spread = scoutSpread({});
    const intrusive_ptr<Book> book = openPictureBook();
    const ComPtr<ID2D1DeviceContext> firstContext = softwareContext();
    const ComPtr<ID2D1DeviceContext> secondContext = softwareContext();
    if (!spread || !book || !firstContext || !secondContext) return;

    PresenterBench bench;
    bench.openAt(as_not_null(book.get()), spread->letter);
    drain();
    const std::optional<uint32_t> drawn = firstInState(*book, PictureState::Ready);
    if (!drawn) return;
    check(book->bitmap(*drawn, firstContext.Get()) != nullptr, "картинка — битмап первого устройства");

    bench.presenter.renderingDeviceReplaced();
    check(!book->pictureHolding(*drawn).bitmap && book->pictureState(*drawn) != PictureState::Ready,
          "битмап отпущен, картинка снова в очереди");

    wakeAll(bench.idle);
    drain();
    check(book->pictureState(*drawn) == PictureState::Ready, "раскодирована заново");
    check(book->bitmap(*drawn, secondContext.Get()) != nullptr, "битмап нового устройства — без остановки");
}

/// Нарушенный договор книги — остановка своей строкой.
void testPictureContractStops() {
    std::printf("\n=== картинки книги: нарушенный договор — остановка ===\n");
    checkStopProbe(kBeginTwice, "Book::beginDecoding: картинка не ждёт раскодирования");
    checkStopProbe(kAcceptUnasked, "Book::acceptPicture: ответ пула для картинки, которую не раскодировали");
    checkStopProbe(kAbandonUnasked,
                   "Book::abandonDecoding: отменено раскодирование картинки, которую не раскодировали");
    checkStopProbe(kReadyWithoutPixels, "Book::acceptPicture: вердикт «картинка», а пикселей нет");
    checkStopProbe(kForeignDevice, "Book: контекст другого устройства, а битмапы картинок — с прежнего");
}

}  // namespace

bool isPictureStopProbe(std::string_view probeName) {
    return probeName == kBeginTwice || probeName == kAcceptUnasked || probeName == kAbandonUnasked ||
           probeName == kReadyWithoutPixels || probeName == kForeignDevice;
}

/// Дочерний запуск пробы договора книги. Остановилась — сюда не вернулась;
/// не остановилась — код 0, и родитель засчитает провал.
int runPictureStopProbe(std::string_view probeName) {
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const intrusive_ptr<Book> book =
        make_refcounted<Book>(kPictureBook, fb3::Document{bytesOf(kPictureBook)}, dwriteFactory());
    const std::optional<uint32_t> unknown = firstInState(*book, PictureState::Unknown);
    if (!unknown) return 0;

    if (probeName == kBeginTwice) {
        static_cast<void>(book->beginDecoding(*unknown));
        static_cast<void>(book->beginDecoding(*unknown));
    } else if (probeName == kAcceptUnasked) {
        book->acceptPicture(*unknown, DecodedPicture{});
    } else if (probeName == kAbandonUnasked) {
        book->abandonDecoding(*unknown);
    } else if (probeName == kReadyWithoutPixels) {
        static_cast<void>(book->beginDecoding(*unknown));
        book->acceptPicture(*unknown, DecodedPicture{});
    } else if (probeName == kForeignDevice) {
        // Картинка — битмапом одного устройства, затем контекст другого без
        // замены: раскодирует проба сама, на месте, как раскодировал бы пул.
        const ComPtr<ID2D1DeviceContext> firstContext = softwareContext();
        const ComPtr<ID2D1DeviceContext> secondContext = softwareContext();
        if (!firstContext || !secondContext) return 0;
        ComPtr<IWICImagingFactory> wic;
        if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))))
            return 0;
        const std::string sentBytes = book->beginDecoding(*unknown);
        book->acceptPicture(*unknown, decodeImage(wic.Get(), sentBytes));
        if (book->pictureState(*unknown) != PictureState::Ready) return 0;
        static_cast<void>(book->bitmap(*unknown, firstContext.Get()));
        static_cast<void>(book->bitmap(*unknown, secondContext.Get()));
    }
    return 0;
}

void runPictureQueueTests() {
    testPicturesAtOpen();
    testRefusalReleasesBytes();
    testBackwardNeighborAcrossChapter();
    testBookWithoutPicturesStartsNothing();
    testShownPicturesReadyOnce();
    testOneAtATime();
    testStayAndTurnKeepDecoding();
    testNeighborOrderAndDrop();
    testJumpDropsAndEvicts();
    testPrefetchNeighbors();
    testDeviceReplacementNotWaitingIdle();
    testAnswerAfterDeviceReplacement();
    testQueueDestructorForgets();
    testRestartKeepsOneFrame();
    testPresenterBookChangeCancels();
    testPresenterCanvasAndOwnerCancel();
    testPresenterDeviceReplacement();
    testPictureContractStops();
}
