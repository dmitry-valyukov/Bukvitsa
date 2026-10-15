// Свой заголовок — до импортов: стандартный заголовок после импорта MSVC не
// принимает.
#include "bukvitsa/reader/notice.h"

import wxl.async;
import wxl.core;

using wxl::async::last_error;

namespace bukvitsa::reader {

std::wstring reasonOf(const last_error& failure) {
    std::wstring words = failure.explanation();
    if (!words.empty()) return words;

    // Системе нечего сказать о коде: показывается диагностика — что делали, код и
    // «unknown error»; текст наш и ASCII, но проверяется, как любой чужой.
    const std::optional<u8_view> said = unicode::checked(std::string_view(failure.what()));
    return said ? std::wstring(said->to_utf16().wchars()) : std::wstring(L"(сообщение не в UTF-8)");
}

Notice noticeOf(std::exception_ptr error) {
    try {
        std::rethrow_exception(std::move(error));
    } catch (const last_error& failure) {
        return {L"Не удалось выполнить операцию с файлом", reasonOf(failure)};
    } catch (const std::exception& failure) {
        // Чужой текст: чей он и в какой кодировке, здесь неизвестно, потому
        // проверяется, а не принимается на веру.
        const std::optional<u8_view> said = unicode::checked(std::string_view(failure.what()));
        return {L"Ошибка", said ? std::wstring(said->to_utf16().wchars()) : std::wstring(L"(сообщение не в UTF-8)")};
    } catch (...) {
        return {L"Неизвестная ошибка", L"Сценарий прерван."};
    }
}

Notice noticeOf(std::wstring headline, std::wstring details, const last_error& failure) {
    if (!details.empty()) details += L"\n\n";
    details += reasonOf(failure);
    return {std::move(headline), std::move(details)};
}

}  // namespace bukvitsa::reader
