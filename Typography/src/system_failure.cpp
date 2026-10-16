#include <format>
#include <iterator>
#include <source_location>
#include <string_view>

#include "bukvitsa/typography/system_failure.h"

// Последним: остановка — wxl.core.
import wxl.core;

namespace bukvitsa::typography::detail {

void stopOnFailure(HRESULT result, std::string_view what, std::source_location where) noexcept {
    // Текст — в буфер на стеке, а не в строку: среди причин остановки —
    // нехватка памяти, и выделить её на пути отказа может не выйти.
    char text[512];
    const auto code = static_cast<unsigned>(result);
    auto written = std::format_to_n(text, std::size(text), "{}, HRESULT 0x{:08X}", what, code);

    // Обрезать слова нельзя — посреди буквы вышел бы не UTF-8, — а слова
    // вызова много короче буфера. Не уместились — это ошибка в коде, и
    // остановка называет её саму, с тем же HRESULT и тем же местом.
    if (written.size > static_cast<std::ptrdiff_t>(std::size(text)))
        written = std::format_to_n(text, std::size(text), "stopIfFailed: слова отказа длиннее {} байт, HRESULT 0x{:08X}",
                                   std::size(text), code);

    wxl::core::abort(std::string_view(text, static_cast<size_t>(written.out - text)), where);
}

}  // namespace bukvitsa::typography::detail
