#include <filesystem>
#include <memory>
#include <optional>
#include <utility>

// Свой заголовок — после стандартных: через рабочее место он несёт импорт.
#include "bukvitsa/reader/warm_slot.h"

namespace bukvitsa::reader {

void WarmSlot::expect(std::filesystem::path path) {
    wanted_ = std::move(path);
    warmed_ = Warmed{};
}

void WarmSlot::fill(const std::filesystem::path& path, std::shared_ptr<Book> book, uint64_t fileSize) {
    // Пока шло чтение, читатель мог открыть эту книгу и сам — тогда заказ
    // снят, и вторая её копия в памяти никому не нужна.
    if (wanted_.empty() || wanted_ != path) return;

    warmed_.book = std::move(book);
    warmed_.fileSize = fileSize;
}

bool WarmSlot::holds(const std::filesystem::path& path) const {
    return warmed_.book && wanted_ == path;
}

std::optional<Warmed> WarmSlot::take(const std::filesystem::path& path) {
    if (!holds(path)) return std::nullopt;

    std::optional<Warmed> taken{std::move(warmed_)};
    cancel();
    return taken;
}

void WarmSlot::cancel() {
    wanted_.clear();
    warmed_ = Warmed{};
}

}  // namespace bukvitsa::reader
