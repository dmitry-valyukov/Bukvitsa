// Ожидание простоя окна — из wxl.ui, его заголовок первым: он тянет за собой
// стандартные заголовки и обёртку очереди и сам кончается импортом wxl.async.
#include "UiThread.h"

#include "ui_idle_scheduler.h"

namespace bukvitsa::reader {

wxl::async::task<> UiIdleScheduler::idle(wxl::async::cancellation_token token) {
    // Под отменой ожидание кончается operation_canceled_exception: стоящее —
    // на ближайшем ходу очереди, начатое под отменённым токеном — сразу, не
    // вставая. Уход окна кончает его так же.
    co_await wxl::UiThread::onIdle(std::move(token));
}

}  // namespace bukvitsa::reader
