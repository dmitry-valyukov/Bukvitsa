#pragma once

// Слова читателю о том, что не удалось: заголовок и подробности. Это модель —
// окон здесь нет; показывает слова приложение.

#include <exception>
#include <string>

// Последним: `system_exception` живёт в wxl.async.
import wxl.async;

namespace bukvitsa::reader {

/// Слова для читателя: заголовок и подробности. Модель окон не показывает.
struct Notice {
    std::wstring headline;
    std::wstring details;
};

/// Причина сбоя операции словами системы, для сообщения читателю. Текст
/// приходит в кодировке потока (`FormatMessageA`), потому переводится через
/// CP_ACP.
std::wstring reasonOf(const wxl::async::system_exception& failure);

/// Сбой сценария словами для читателя: то, что дошло до обработчика сбоев,
/// потому что сам сценарий ответа на это не знал. Операция с файлом —
/// причиной словами системы (`reasonOf`); всякое другое исключение — его
/// текстом, если он UTF-8; что-то ещё — просто фактом.
Notice noticeOf(std::exception_ptr error);

}  // namespace bukvitsa::reader
