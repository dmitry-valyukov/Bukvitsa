#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Заголовки модели после всех стандартных: они ведут к импорту модулей книги,
// wxl.async и wxl.core, а стандартный заголовок после импорта MSVC уже не
// принимает. Поток страниц первым: он тянет за собой книгу, вёрстку и
// раскодирование с их стандартными заголовками.
#include "bukvitsa/reader/page_flow.h"
#include "bukvitsa/reader/picture_queue.h"
#include "bukvitsa/reader/imaging.h"

namespace bukvitsa::reader {

namespace {

bool contains(const std::vector<uint32_t>& pictures, uint32_t picture) {
    return std::find(pictures.begin(), pictures.end(), picture) != pictures.end();
}

}  // namespace

PictureQueue::PictureQueue(PageFlow& flow, IdleScheduler& scheduler,
                           wxl::async::cancellation_token ownerToken, function<void()> shownReceiver,
                           function<void(std::exception_ptr)> failureReceiver)
    : flow_(flow),
      scheduler_(scheduler),
      shownReceiver_(std::move(shownReceiver)),
      failureReceiver_(std::move(failureReceiver)),
      ownerToken_(std::move(ownerToken)),
      stop_(ownerToken_) {}

PictureQueue::~PictureQueue() {
    stop_.cancel();

    // Книга живёт дольше очереди (её держат и поток страниц, и прочитанная
    // книга у владельца): раскодирование, которого больше никто не ждёт, она
    // должна забыть, иначе картинка навсегда осталась бы «в пути».
    if (Book* const book = flow_.book()) book->abandonDecoding();
}

void PictureQueue::request(WindowMove move) {
    if (stop_.is_canceled()) return;   // владелец ушёл

    Book* const book = flow_.book();
    if (!book || !book->picturesInFlow()) return;

    // Выпала ли из окна картинка, которую раскодируют, — по прежнему окну и
    // новым показанным, пока прежнее окно ещё здесь.
    std::vector<uint32_t> newShown = flow_.spreadPictures();
    const bool dropDecoding = decodingPicture_ && !staysInWindow(*decodingPicture_, move, newShown);

    shownPictures_ = std::move(newShown);
    forwardPictures_.clear();
    backwardPictures_.clear();
    neighborsKnown_ = false;
    if (move == WindowMove::TurnedForward) travelForward_ = true;
    if (move == WindowMove::TurnedBackward) travelForward_ = false;

    // Отмены — когда окно уже новое: ожидание, отменённое здесь, кончается на
    // ходу очереди, и кадр, проснувшись, читает окно заново.
    if (dropDecoding) decodeWait_.cancel();

    if (running_) {
        // Показанные не ждут простоя: кадр, стоящий за соседями, будится
        // отменой своего ожидания и берёт их.
        if (awaitingIdle_ && nextPicture(*book)) idleWait_.cancel();
        return;
    }

    // Знак — до вызова, как у досчёта главы: кадр может кончиться ещё внутри
    // него (сбой на первом ожидании) и сам опустить знак; поднятый после
    // вызова, знак затёр бы это и остался бы поднятым навсегда.
    running_ = true;
    loop_ = loop();
}

void PictureQueue::restart() {
    // Отменить до замены обязательно: замена источника старого не отменяет, и
    // прежний кадр отдал бы книге ответ, которого она уже не ждёт. Источники
    // ожидания и раскодирования — дочерние, отменяются с ним.
    stop_.cancel();
    running_ = false;
    awaitingIdle_ = false;
    decodingPicture_.reset();
    stop_ = wxl::async::cancellation_source{ownerToken_};

    if (Book* const book = flow_.book()) book->abandonDecoding();
    shownPictures_.clear();
    forwardPictures_.clear();
    backwardPictures_.clear();
    neighborsKnown_ = false;
}

std::optional<uint32_t> PictureQueue::nextPicture(const Book& book) const {
    const std::vector<uint32_t>& aheadPictures = travelForward_ ? forwardPictures_ : backwardPictures_;
    const std::vector<uint32_t>& behindPictures = travelForward_ ? backwardPictures_ : forwardPictures_;
    for (const std::vector<uint32_t>* pictures : {&shownPictures_, &aheadPictures, &behindPictures})
        for (const uint32_t picture : *pictures)
            if (book.pictureState(picture) == PictureState::Unknown) return picture;
    return std::nullopt;
}

std::vector<uint32_t> PictureQueue::window() const {
    std::vector<uint32_t> pictures = shownPictures_;
    pictures.insert(pictures.end(), forwardPictures_.begin(), forwardPictures_.end());
    pictures.insert(pictures.end(), backwardPictures_.begin(), backwardPictures_.end());
    return pictures;
}

bool PictureQueue::staysInWindow(uint32_t picture, WindowMove move, const std::vector<uint32_t>& newShown) const {
    if (contains(newShown, picture)) return true;
    switch (move) {
        case WindowMove::Stays:
            return true;
        // Переворот на разворот вперёд: прежний показанный — теперь сосед
        // позади, прежний сосед впереди — показанный; выпадает только прежний
        // сосед позади. Назад — зеркально. Соседи ещё не узнаны — раскодируют
        // показанную, а она в окне.
        case WindowMove::TurnedForward:
            return contains(shownPictures_, picture) || contains(forwardPictures_, picture);
        case WindowMove::TurnedBackward:
            return contains(shownPictures_, picture) || contains(backwardPictures_, picture);
        case WindowMove::Jumped:
            return false;
    }
    wxl::core::abort("PictureQueue: неизвестный сдвиг окна");
}

void PictureQueue::settle(Book& book, uint32_t picture) {
    // Пока соседи нового окна не узнаны, вне окна картинку не считают: она
    // может оказаться соседом, и отпустить её значило бы раскодировать её
    // снова. Узнав соседей, кадр вытесняет всё лишнее разом (keepPictures).
    if (neighborsKnown_) {
        const std::vector<uint32_t> keptPictures = window();
        book.keepPictures(keptPictures);
    }

    if (book.pictureState(picture) == PictureState::Ready && contains(shownPictures_, picture))
        shownReceiver_();
}

wxl::async::task<> PictureQueue::loop() {
    // Правило кадра: отменённый вместе с очередью, он не трогает ни
    // держателя, ни книгу — к этому ходу очереди у держателя может идти уже
    // другой кадр (смена книги, уход холста). Отмену очереди кадр узнаёт по
    // копии её токена, взятой здесь раз и навсегда: источник очереди держатель заменяет при restart(), и
    // токен нового источника о прежнем кадре ничего бы не сказал. Отменено
    // только своё ожидание (простоя или раскодирования) при живом токене
    // очереди — это сделал request(), держатель жив, и кадр идёт дальше. Книга
    // та же, что при заводе кадра: её смену держатель встречает отменой.
    const wxl::async::cancellation_token frameToken = stop_.token();
    try {
        for (;;) {
            Book* const book = flow_.book();
            if (!book) break;

            const std::optional<uint32_t> picture = nextPicture(*book);
            if (!picture) {
                if (neighborsKnown_) break;   // окно раскодировано

                // Соседи — в простое: ввод и показанный разворот потока важнее
                // предвыборки. Узнать соседей — это вёрстка ленты, малая
                // (страница вперёд, главу назад — только досчитанную), но не
                // даровая: при растяжке окна она повторяется после каждой
                // перевёрстки, как только поток свободен. Узнав соседей —
                // отпустить всё вне окна.
                idleWait_ = wxl::async::cancellation_source{frameToken};
                const wxl::async::cancellation_token idleToken = idleWait_.token();
                awaitingIdle_ = true;
                bool idleCame = true;
                try {
                    co_await scheduler_.idle(idleToken);
                } catch (const wxl::async::operation_canceled_exception&) {
                    // Отменена очередь — уйти, ничего не тронув; ушла очередь
                    // потока при живых токенах (выход приложения) — тоже.
                    if (frameToken.is_canceled() || !idleToken.is_canceled()) throw;
                    idleCame = false;   // request(): у показанных есть работа
                }
                awaitingIdle_ = false;
                if (!idleCame) continue;

                forwardPictures_ = flow_.neighborPictures(true);
                backwardPictures_ = flow_.neighborPictures(false);
                neighborsKnown_ = true;
                const std::vector<uint32_t> keptPictures = window();
                book->keepPictures(keptPictures);
                continue;
            }

            // Копия байтов — телу в пуле: оно может пережить и ожидание, и
            // книгу. Предел — общий; слабому устройству битмап уменьшит
            // deviceBitmapOf при рисунке.
            decodeWait_ = wxl::async::cancellation_source{frameToken};
            const wxl::async::cancellation_token decodeToken = decodeWait_.token();
            decodingPicture_ = *picture;
            DecodedPicture decodedPicture;
            bool droppedFromWindow = false;
            try {
                decodedPicture =
                    co_await decodeImageAsync(book->beginDecoding(*picture), kMaxPictureSide, decodeToken);
            } catch (const wxl::async::operation_canceled_exception&) {
                // Отменена очередь — уйти, ничего не тронув. Отмена при живом
                // токене раскодирования — не наша и не «выпадение»: её нельзя
                // выдать за пропуск картинки, и кадр кончается на ней, как
                // отменённый.
                if (frameToken.is_canceled() || !decodeToken.is_canceled()) throw;
                // request(): картинка выпала из окна. Тело в пуле осиротело и
                // доработает до ближайшей полосы, его ответ никому не уйдёт.
                droppedFromWindow = true;
            }
            decodingPicture_.reset();

            if (droppedFromWindow) {
                book->abandonDecoding(*picture);
                continue;
            }
            book->acceptPicture(*picture, std::move(decodedPicture));
            settle(*book, *picture);
        }
        running_ = false;
    } catch (const wxl::async::operation_canceled_exception&) {
        // Отменённый кадр уходит, ничего не тронув. Отмена, которую кадр не
        // отличил от своей (ушла очередь потока, раскодирование отменено не
        // им), — тоже конец без знаков: её не бывает при живом держателе,
        // кроме выхода приложения, когда токен владельца уже отменён.
    } catch (...) {
        // Сбой — не отмена: сам сбой — получателю сбоев, и знаки опускаются
        // только после него, иначе всякий следующий request() молча выходил
        // бы, и картинки не приходили бы до смены книги. Не раньше: пока
        // поднят знак, request() из получателя задачу под бегущим кадром не
        // меняет. Трогать `this` здесь можно: не-отмену бросает только
        // неотменённый кадр, а неотменённый кадр — кадр живого держателя.
        failureReceiver_(std::current_exception());
        running_ = false;
        awaitingIdle_ = false;
        decodingPicture_.reset();
    }
}

}  // namespace bukvitsa::reader
