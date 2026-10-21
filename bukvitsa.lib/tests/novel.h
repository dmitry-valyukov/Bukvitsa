#pragma once
// Общее проб модели чтения на настоящей книге: роман и ответы о нём на
// полосе, вид и мера полосы по умолчанию, поддельный простой. Пробы потока
// страниц, досчёта главы и ведущего полосы стоят на одном романе и спрашивают
// у него одно и то же — какая глава самая длинная, в левой ли колонке буква,
// где на развороте знак сноски, — а ведущий досчитывает главу тем же
// досчётом, что и пробы досчёта, и ждёт того же простоя. Ответы в копиях
// однажды разошлись бы, и пробы одного слоя мерили бы не то же, что пробы
// другого. Своё у каждого файла — как он ставит поток страниц на роман и меры
// собственных проб.
//
// Всё — inline: заголовок включают несколько единиц трансляции одного
// исполняемого файла проб.

#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>

#include "check.h"

// Последними: поток страниц ведёт к импорту модулей книги и wxl.core,
// интерфейс простоя — к wxl.async.
#include "bukvitsa/reader/page_flow.h"
#include "bukvitsa/reader/idle_scheduler.h"

import wxl.async;
import wxl.core;

namespace bukvitsa::reader::tests {

inline const std::filesystem::path kNovel = std::filesystem::path(BUKVITSA_TESTDATA_DIR) /
                                            L"Turgenev_I._Spisokshkolnoy._Otcyi_I_Deti.fb3";

/// Высота полосы во всех пробах — окно ноутбука; ширина у каждой своя.
inline constexpr float kHeight = 900.0f;

/// Вид по умолчанию — тот, что у настроек.
inline constexpr float kFontSize = 20.0f;
inline constexpr float kLineHeight = 1.45f;
inline constexpr float kMargin = 0.075f;

/// Сколько ходов очереди проба делает самое большее: глава романа досчитывается
/// за единицы порций, а зависшая проба хуже упавшей.
inline constexpr int kMaxTurns = 10000;

/// Чем падает простой на ходу со сбоем — по тексту проба узнаёт, что до
/// обработчика дошёл именно этот сбой.
inline constexpr char kIdleFailure[] = "простой упал";

inline std::string bytesOf(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

/// Роман, открытый заново для каждой пробы: кэш глав и текущая глава у книги
/// свои, и пробы не должны видеть друг друга через них. Документ разбора
/// живёт только на время открытия, как у рабочего места. Пусто — роман не
/// открылся, и проба кончается, не начавшись.
inline intrusive_ptr<Book> openNovel() {
    intrusive_ptr<Book> book;
    try {
        book = make_refcounted<Book>(kNovel, fb3::Document{bytesOf(kNovel)}, dwriteFactory());
    } catch (const std::exception& failure) {
        // Почему роман не открылся — в отчёт пробы: без этого «не открыт» не
        // говорит, где искать.
        std::printf("  роман не открылся: %s\n", failure.what());
        book = nullptr;
    }
    check(book && book->chapterCount() > 3, "роман открыт, в нём главы");
    if (book && book->chapterCount() <= 3) book = nullptr;
    return book;
}

/// Длина главы в символах книги.
inline uint32_t chapterLength(const Book& book, size_t chapterIndex) {
    const uint32_t chapterEnd = chapterIndex + 1 < book.chapterCount()
                                    ? book.chapterFirstChar(chapterIndex + 1)
                                    : book.characterCount();
    return chapterEnd - book.chapterFirstChar(chapterIndex);
}

/// Самая длинная глава, кроме этой (номер вне книги — без исключений): хвост
/// у неё заведомо есть, и в её середине развороту есть куда шагнуть в обе
/// стороны, не выходя из неё.
inline size_t longestChapter(const Book& book, size_t excludedChapter) {
    size_t longestIndex = excludedChapter == 0 ? 1 : 0;
    for (size_t chapterIndex = 0; chapterIndex < book.chapterCount(); ++chapterIndex)
        if (chapterIndex != excludedChapter &&
            chapterLength(book, chapterIndex) > chapterLength(book, longestIndex))
            longestIndex = chapterIndex;
    return longestIndex;
}

/// Самая длинная глава книги — без исключений.
inline size_t longestChapter(const Book& book) {
    return longestChapter(book, book.chapterCount());
}

/// Буква на трёх пятых самой длинной главы: от её разворота есть куда шагнуть
/// в обе стороны, не выходя из главы, а хвост у главы заведомо есть.
inline uint32_t middleLetter(const Book& book) {
    const size_t longestIndex = longestChapter(book);
    return book.chapterFirstChar(longestIndex) + chapterLength(book, longestIndex) * 3 / 5;
}

/// Буква лежит в левой колонке разворота: колонка начинается не позже неё, а
/// следующая колонка той же главы — позже.
inline bool letterInLeftColumn(const PageFlow& flow, uint32_t letter) {
    const typography::Chapter& currentChapter = flow.book()->paginator();
    const size_t leftColumn = flow.page();
    if (currentChapter.pageCount() == 0 ||
        currentChapter.page(leftColumn).firstCharOffset > letter)
        return false;
    return leftColumn + 1 >= currentChapter.pageCount() ||
           currentChapter.page(leftColumn + 1).firstCharOffset > letter;
}

/// Место чтения — начало левой колонки разворота: туда его ставят листание и
/// прыжок.
inline bool placeAtLeftColumn(const PageFlow& flow) {
    return !flow.spread().pages.empty() &&
           flow.position() == flow.spread().pages[0]->firstCharOffset;
}

/// Первый знак сноски в потоке чтения: номер тела и буква знака.
struct NoteMark {
    uint32_t noteBody = 0;
    uint32_t markLetter = 0;
};

/// Знак — текст абзаца, как всё остальное, и его букву в книге дают прогоны
/// позиций. Пусто — сносок в книге нет.
inline std::optional<NoteMark> firstNoteMark(const Book& book) {
    const typography::BookText& bookText = book.text();
    for (const typography::Block& flowBlock : bookText.flow()) {
        const std::span<const typography::NoteAnchor> blockNotes =
            bookText.notesOf(flowBlock.paragraph);
        if (blockNotes.empty()) continue;
        return NoteMark{blockNotes.front().body,
                        bookText.charOffsetAt(flowBlock.paragraph, blockNotes.front().position)};
    }
    return std::nullopt;
}

/// Знак сноски на показанном развороте: куда по нему щёлкают — середина знака
/// на высоте его строки — и где под ним встаёт всплывашка — та же середина на
/// нижнем краю строки.
struct MarkPlace {
    PageFlow::Point tapPoint;
    PageFlow::Point popupAnchor;
};

/// Первый знак с этим телом на показанном развороте; пусто — знака на
/// развороте нет.
inline std::optional<MarkPlace> markPlaceOf(const PageFlow& flow, uint32_t noteBody) {
    const PageFlow::Spread& shownSpread = flow.spread();
    for (size_t column = 0; column < shownSpread.pages.size(); ++column) {
        for (const typography::PlacedLine& placedLine : shownSpread.pages[column]->lines) {
            const typography::Line& line = *placedLine.line;
            const float baseline = flow.verticalMargin() + placedLine.baseline;
            const float lineLeft = flow.columnLeft(column) + placedLine.x;
            for (const typography::PlacedNote& placedNote : line.notes) {
                if (placedNote.body != noteBody) continue;
                const float markMiddle = lineLeft + placedNote.x + placedNote.width * 0.5f;
                return MarkPlace{PageFlow::Point{markMiddle, baseline - line.ascent * 0.5f},
                                 PageFlow::Point{markMiddle, baseline + line.descent}};
            }
        }
    }
    return std::nullopt;
}

/// Середина знака сноски с этим телом на показанном развороте, на высоте его
/// строки, — туда и щёлкают; пусто — знака на развороте нет.
inline std::optional<PageFlow::Point> markPointOf(const PageFlow& flow, uint32_t noteBody) {
    return markPlaceOf(flow, noteBody).transform([](const MarkPlace& place) {
        return place.tapPoint;
    });
}

/// Точка в верхнем поле полосы на этой доле её ширины: мимо всякой строки, а
/// значит, мимо всякого знака сноски, — щелчок решает одна карта.
inline PageFlow::Point marginPoint(const PageFlow& flow, float widthShare) {
    return PageFlow::Point{flow.width() * widthShare, 1.0f};
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

    wxl::async::task<> idle(wxl::async::cancellation_token stop) override {
        // Счёт — до всякой проверки: по нему видно, спрашивал ли держатель
        // простоя вообще.
        ++idleCalls_;
        stop.throw_if_canceled();
        auto const woken = co_await Parking{*this};
        if (woken == Woken::closing) throw wxl::async::operation_canceled_exception{};
        stop.throw_if_canceled();
        if (woken == Woken::failure) throw std::runtime_error(kIdleFailure);
    }

    /// Ход очереди: будит первое стоящее ожидание; false — будить некого.
    /// Ожидание уходит из очереди раньше, чем кадр возобновлён: кадр может
    /// кончиться внутри этого вызова вместе с ожиданием.
    bool wake(Woken how = Woken::turn) {
        if (parkedWaits_.empty()) return false;
        Parking* const firstParked = parkedWaits_.front();
        parkedWaits_.pop_front();
        firstParked->woken = how;
        auto const parkedWaiter = firstParked->waiter;
        parkedWaiter.resume();
        return true;
    }

    /// Сколько ожиданий стоит.
    size_t parked() const { return parkedWaits_.size(); }

    /// Сколько раз вставали в ожидание.
    int parkings() const { return parkings_; }

    /// Сколько раз спрашивали простоя — и под отменённым токеном тоже.
    int idleCalls() const { return idleCalls_; }

private:
    /// Ожидание в кадре корутины. Кадр может уйти и стоящим — его задачу
    /// бросил держатель, — и тогда ожидание снимает себя с очереди само: ход,
    /// пришедший потом, разрушенного кадра не будит.
    struct Parking {
        ParkingIdle& owner;
        std::coroutine_handle<> waiter;
        Woken woken = Woken::turn;

        explicit Parking(ParkingIdle& scheduler) noexcept : owner(scheduler) {}
        Parking(const Parking&) = delete;
        Parking& operator=(const Parking&) = delete;

        ~Parking() { std::erase(owner.parkedWaits_, this); }

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
inline void wakeAll(ParkingIdle& idle) {
    for (int turns = 0; turns < kMaxTurns && idle.wake(); ++turns) {
    }
}

}  // namespace bukvitsa::reader::tests
