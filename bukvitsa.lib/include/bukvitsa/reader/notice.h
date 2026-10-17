#pragma once

// Слова читателю о том, что не удалось: заголовок и подробности. Это модель —
// окон здесь нет; показывает слова приложение.

#include <exception>
#include <string>

// Последним: `last_error` живёт в wxl.async.
import wxl.async;

namespace bukvitsa::reader {

/// Слова для читателя: заголовок и подробности. Модель окон не показывает.
struct Notice {
    std::wstring headline;
    std::wstring details;
};

/// Причина сбоя операции словами системы на языке читателя
/// (`last_error::explanation()`), для сообщения ему. Когда у системы слов для
/// кода нет, остаётся диагностика `what()`: что делали и код.
std::wstring reasonOf(const wxl::async::last_error& failure);

/// Причина всякого другого сбоя — текст исключения, если он UTF-8: чей он и в
/// какой кодировке, неизвестно, и он проверяется, а не принимается на веру.
std::wstring reasonOf(const std::exception& failure);

/// Сбой сценария словами для читателя: то, что дошло до обработчика сбоев,
/// потому что сам сценарий ответа на это не знал. Операция с файлом —
/// причиной словами системы (`reasonOf`); всякое другое исключение — его
/// текстом, если он UTF-8; что-то ещё — просто фактом.
Notice noticeOf(std::exception_ptr error);

/// Сбой операции с файлом, на который у сценария есть ответ читателю: что
/// не удалось, с чем (путь; может быть пусто) и почему — причина словами
/// системы через пустую строку после подробностей.
Notice noticeOf(std::wstring headline, std::wstring details, const wxl::async::last_error& failure);

}  // namespace bukvitsa::reader
