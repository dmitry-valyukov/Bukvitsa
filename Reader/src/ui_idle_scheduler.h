#pragma once
// UiIdleScheduler — простой потока окна для корутин модели: ожидание
// `UiThread::onIdle` под токеном, за интерфейсом библиотеки (`IdleScheduler`),
// которая очереди окна не видит.

// Последним: интерфейс простоя импортирует wxl.async.
#include "bukvitsa/reader/idle_scheduler.h"

namespace bukvitsa::reader {

/// Простой потока окна. Своего состояния нет: очередь — того потока, на
/// котором ждут, а снимает ожидание отмена токена или разрушение кадра, что в
/// нём стоит. Владеет приложение; досчёт главы у ведущего полосы держит
/// ссылку и живёт не дольше.
class UiIdleScheduler final : public IdleScheduler {
public:
    /// Звать на потоке окна: ожидание встаёт в его очередь низким приоритетом,
    /// и ввод, разметка и отрисовка берутся раньше.
    wxl::async::task<> idle(wxl::async::cancellation_token stop) override;
};

}  // namespace bukvitsa::reader
