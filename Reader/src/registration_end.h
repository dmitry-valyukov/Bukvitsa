#pragma once

// Ожидание конца шага регистрации книги под токеном сценария — для открытия,
// которому нужна запись, какой её отдал реестр.
//
// Механизм — тот же, что у всех ожиданий событий wxl.ui (`impl/event_waits.h`):
// ждущий стоит в общем списке ожиданий, пока приостановлен; отмена токена
// никого не будит внутри себя — ожидание переходит в список отменённых, и его
// возобновляет следующий ход очереди потока, а конец шага, пришедший раньше
// этого хода, возобновляет его сам, и оно кончается отменой; выход приложения
// кончает его вместе со всеми. Своё здесь одно — источник: событие конца шага
// модели (`QueuedRegistration::ended`), а не событие элемента, — и потому
// класс свой, а не `on_event`: та подписывает обработчик с отправителем-объектом
// WinRT, а у модели его нет.
//
// Подписка на событие живёт, пока корутина стоит здесь: снимается, когда она
// проснулась, — концом шага, ходом очереди после отмены или выходом, — и
// когда её кадр разрушен на ожидании. Регистрацию отмена ожидания не трогает:
// шаг идёт своим чередом, и книга встанет в реестр, только открытие её не
// дождётся.

#include "event_awaitable.h"

// Заголовок модели — последним: он несёт импорт.
#include "bukvitsa/reader/workspace.h"

namespace bukvitsa::reader {

class RegistrationEnd : wxl::impl::waiting_event {
public:
    explicit RegistrationEnd(not_null<intrusive_ptr<QueuedRegistration>> queued) noexcept
        : queued_(std::move(queued)) {}

    ~RegistrationEnd() {
        if (armed_) disarm();
        leave();
    }

    RegistrationEnd(const RegistrationEnd&) = delete;
    RegistrationEnd& operator=(const RegistrationEnd&) = delete;

    /// Шаг кончился раньше, чем открытие до него дошло, — итог уже в
    /// элементе, ждать нечего; отменено до начала или приложение уходит — тоже
    /// не приостанавливается.
    bool await_ready() const noexcept { return queued_->done() || canceled_ || wxl::impl::events_closing(); }

    /// Место co_await — параметром, и в каждой сборке: ожидание под токеном
    /// передаёт его всякому, кто его берёт, и сборка, которая проверяет
    /// корутины, называет строку co_await, а не этого файла. Пары объявлений с
    /// `requires`, как у ожиданий wxl, здесь не бывает: класс не шаблон.
    void await_suspend(std::coroutine_handle<> waiter,
                       wxl::async::detail::site where = wxl::async::detail::site::current()) {
        subscribe();
        arm(waiter, where);
        armed_ = true;
    }

    /// Запись, какой её отдал реестр; шаг не удался — его сбой как есть
    /// (`last_error` записи обложки — тем же путём, каким открытие говорит о
    /// сбое читателю), очередь кончилась отменой раньше шага — её отмена.
    /// Шаг не кончился, а ожидание кончилось, — приложение уходит.
    const Registered& await_resume() {
        armed_ = false;
        leave();

        if (!queued_->done()) throw_ended();

        const RegistrationOutcome& outcome = queued_->outcome();
        if (!outcome) std::rethrow_exception(outcome.error());
        return *outcome;
    }

    /// Отменено токеном (`async::detail::cancellable_awaiter`): стоящее
    /// возобновит следующий ход очереди или конец шага, если он раньше, —
    /// никогда не сама отмена; не начатое не приостанавливается.
    void cancel() noexcept {
        if (armed_) {
            waiting_event::cancel();
        } else {
            canceled_ = true;
        }
    }

private:
    /// Подписка — раньше, чем ожидание встанет в список: между ними шаг не
    /// кончится — он кончается на своём ходу потока, а ход сейчас у ждущего.
    void subscribe() {
        subscription_ = queued_->ended().add([this](const RegistrationOutcome&) noexcept {
            // Последнее, что делает подписчик: проснувшийся может кончиться, и
            // это ожидание — с его кадром.
            if (waiting()) deliver();
        });
    }

    /// Подписчик, снявший себя посреди своего вызова, договаривает его, и
    /// событие отпускает его после.
    void leave() noexcept {
        if (subscription_) queued_->ended().remove(*std::exchange(subscription_, std::nullopt));
    }

    not_null<intrusive_ptr<QueuedRegistration>> queued_;
    std::optional<cookie_t> subscription_;

    // Никогда не оба: стоит в ожидании — или отменено раньше, чем встало.
    bool armed_ = false;
    bool canceled_ = false;
};

/// Конец шага регистрации под токеном: отменён — ожидание кончается
/// `operation_canceled_exception`, а шаг идёт дальше.
[[nodiscard]] inline wxl::impl::event_wait_under<RegistrationEnd, wxl::async::cancellation_token> registrationEnd(
    not_null<intrusive_ptr<QueuedRegistration>> queued, wxl::async::cancellation_token stop) {
    return wxl::impl::event_wait_under<RegistrationEnd, wxl::async::cancellation_token>{std::move(stop),
                                                                                        std::move(queued)};
}

}  // namespace bukvitsa::reader
