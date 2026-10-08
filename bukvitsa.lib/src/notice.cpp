// Свой заголовок — до импортов: стандартный заголовок после импорта MSVC не
// принимает.
#include "bukvitsa/reader/notice.h"

import wxl.async;
import wxl.core;

using wxl::async::system_exception;

namespace bukvitsa::reader {

std::wstring reasonOf(const system_exception& failure) {
    const std::string_view narrow = failure.what();
    const int size = static_cast<int>(narrow.size());

    std::wstring wide(static_cast<size_t>(::MultiByteToWideChar(CP_ACP, 0, narrow.data(), size, nullptr, 0)),
                      L'\0');
    ::MultiByteToWideChar(CP_ACP, 0, narrow.data(), size, wide.data(), static_cast<int>(wide.size()));

    return wide;
}

Notice noticeOf(std::exception_ptr error) {
    try {
        std::rethrow_exception(std::move(error));
    } catch (const system_exception& failure) {
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

}  // namespace bukvitsa::reader
