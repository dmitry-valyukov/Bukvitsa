// Свой заголовок — единственный: он и несёт импорт.
#include "bukvitsa/reader/shelf_text.h"

import wxl.core;
import wxl.fmt;

namespace bukvitsa::reader {

u16_text progressLine(uint32_t charOffset, uint32_t characterCount) {
    if (characterCount == 0 || charOffset == 0) return u16_text{u"не открывалась"};

    const double share = static_cast<double>(charOffset) / characterCount;
    return format(u"прочитано {:.0f}%", share * 100.0);
}

u16_text bookmarksLine(size_t count) {
    if (count == 0) return {};

    return format(u"закладок: {}", count);
}

u16_text shelfLine(uint32_t charOffset, uint32_t characterCount, size_t bookmarks) {
    u16_text said = progressLine(charOffset, characterCount);

    // Закладки видно прямо на полке: их наличие — признак книги, к которой
    // возвращаются, и его стоит показать раньше, чем её откроют.
    if (characterCount == 0 || charOffset == 0 || bookmarks == 0) return said;

    said += u"    ";
    said += bookmarksLine(bookmarks);
    return said;
}

}  // namespace bukvitsa::reader
