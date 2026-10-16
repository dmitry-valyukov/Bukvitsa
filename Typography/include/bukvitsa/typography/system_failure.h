#pragma once
// Отказ системного вызова, на который у программы нет ответа, — остановка на
// месте отказа.
//
// Так отвечают Direct2D и DirectWrite при рисовании, системные фабрики
// (Direct2D, DirectWrite, WIC), поток памяти WIC, COM-диалог выбора файла,
// выдача guid и каталога данных: отказать они могут лишь от нехватки памяти,
// потери устройства или ошибки в коде — продолжать нечем, а страница или
// выбор, молча оставшиеся пустыми, были бы сбоем без имени. Остановка —
// `core::abort` с текстом «что не вышло, HRESULT 0x…» и строкой вызова:
// отладчик встаёт там, текст и место уходят в stderr и в отладчик, и путь
// один, кто бы ни звал. Потеря устройства (D2DERR_RECREATE_TARGET) тоже
// останавливает: восстанавливаться после неё полоса пока не умеет.
//
// Отказы вёрстки на DirectWrite (engine.cpp) этим путём не идут — они
// бросают, как бросали. Чужие байты — картинка, которую не раскодировать, —
// тоже не сюда: это ответ о файле, а не отказ системы.
//
// Живёт в вёрстке, а не выше: она — нижний слой, который зовёт Direct2D
// (formula.cpp рисует), и её заголовки берут и модель, и читалка. Импорта
// здесь нет — текст остановки собирается в system_failure.cpp, — так что
// порядок включения ему безразличен; HRESULT приходит с windows.h общего
// pch.h.

#include <source_location>
#include <string_view>

namespace bukvitsa::typography {

namespace detail {

/// Остановка с текстом «what, HRESULT 0x…» на месте `where`. Отдельно и не
/// встроенной: проверка на каждом вызове — одно сравнение, а сборка текста —
/// только на пути отказа.
[[noreturn]] void stopOnFailure(HRESULT result, std::string_view what, std::source_location where) noexcept;

}  // namespace detail

/// Не удался вызов — остановка (`core::abort`) со словами и строкой вызова.
/// @param what что не вышло и у кого: «BookView: не создалась кисть текста».
inline void stopIfFailed(HRESULT result, std::string_view what,
                         std::source_location where = std::source_location::current()) noexcept {
    if (FAILED(result)) [[unlikely]]
        detail::stopOnFailure(result, what, where);
}

}  // namespace bukvitsa::typography
