#include <algorithm>
#include <iterator>
#include <span>
#include <utility>

// Свой заголовок после всех стандартных: он ведёт к импорту модулей книги, а
// стандартный заголовок после импорта MSVC уже не принимает.
#include "bukvitsa/reader/book_places.h"

import wxl.core;

namespace bukvitsa::reader {

void BookPlaces::open(u16_text guid, std::span<const typography::Block> blocks, std::vector<Bookmark> marks) {
    guid_ = std::move(guid);

    // Оглавление — всегда сброс: строки не сравниваются (`ContentsEntry`), и
    // одинаковые заголовки двух книг не оставят в списке видов в прежнюю.
    contents_.assign(contentsOf(blocks));

    // По порядку книги, как бы их ни записали: место закладки ищется двоичным
    // поиском. Файл пишем мы и по порядку, но его могли поправить руками.
    std::ranges::stable_sort(marks, {}, &Bookmark::charOffset);
    bookmarks_.assign(
        sta_vector<Bookmark>(std::make_move_iterator(marks.begin()), std::make_move_iterator(marks.end())));
}

bool BookPlaces::toggleBookmark(uint32_t offset, u16_text hint) {
    const std::span<const Bookmark> marks = bookmarks_.get();
    const auto found = std::ranges::lower_bound(marks, offset, {}, &Bookmark::charOffset);
    const auto at = static_cast<uint32_t>(found - marks.begin());

    if (found != marks.end() && found->charOffset == offset) {
        bookmarks_.erase(at);
        return false;
    }

    bookmarks_.insert(at, Bookmark{offset, std::move(hint)});
    return true;
}

BookState BookPlaces::stateAt(uint32_t charOffset) const {
    BookState state;
    state.charOffset = charOffset;
    state.bookmarks.assign(bookmarks_.begin(), bookmarks_.end());
    return state;
}

}  // namespace bukvitsa::reader
