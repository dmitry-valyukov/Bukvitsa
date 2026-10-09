#include <optional>
#include <utility>

// Свой заголовок после всех стандартных: он ведёт к импорту модулей книги, а
// стандартный заголовок после импорта MSVC уже не принимает.
#include "bukvitsa/reader/book_search.h"

import wxl.core;
import wxl.fmt;

namespace bukvitsa::reader {
namespace {

/// Подпись, пока искать нечего. Поиск идёт по Enter, а не по каждой букве:
/// искать по одной букве в романе — это тысячи находок, из которых читателю
/// не нужна ни одна.
u16_text prompt() {
    return u16_text{u"Введите слово и нажмите Enter."};
}

}  // namespace

BookSearch::BookSearch() : status_{prompt()} {}

void BookSearch::run(std::span<const typography::Block> blocks) {
    // Текст поля уже проверен привязкой; `searchQuery` здесь решает только,
    // есть ли в нём слово, — правило пустого запроса одно на всю читалку.
    const std::optional<u16_view> needle = searchQuery(query.get().plain());
    if (!needle) {
        hits_.set({});
        status_.set(prompt());
        return;
    }

    sta_vector<SearchHit> found = searchBook(blocks, *needle);

    // Подпись собрана раньше, чем поля заговорят: `needle` — вид в запрос, а
    // слушатель находок волен запрос поменять.
    u16_text said = found.empty() ? format(u"«{}» в книге не нашлось.", *needle)
                                  : format(u"Нашлось: {}", found.size());

    // Находки — раньше подписи: кто слышит «Нашлось: N», видит их уже на месте.
    hits_.set(std::move(found));
    status_.set(std::move(said));
}

void BookSearch::clear() {
    query.set({});
    hits_.set({});
    status_.set(prompt());
}

}  // namespace bukvitsa::reader
