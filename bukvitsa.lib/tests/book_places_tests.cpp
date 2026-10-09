// Тесты мест открытой книги: оглавление и закладки как списки, за которыми
// следит панель, — что в них лежит и что слышит тот, кто к ним привязан.
// Книга — блоки, собранные тестом: оглавлению нужны только заголовки и их
// места, вёрстка ему не нужна.

#include <cstdint>
#include <cstdio>
#include <vector>

#include "check.h"

// Модель — последней: через блоки вёрстки она ведёт к импорту модулей.
#include "bukvitsa/reader/book_places.h"

import wxl.core;

using namespace bukvitsa;
using namespace bukvitsa::reader;
using bukvitsa::reader::tests::check;

namespace {

/// Блок книги с этим текстом с позиции `start`; заголовок — с уровнем.
typography::Block blockAt(u16_view text, uint32_t start, typography::BlockKind kind, uint8_t level = 0) {
    typography::Block block;
    block.kind = kind;
    block.level = level;
    block.charOffset = start;
    block.paragraph.text = u16_text{text};
    block.paragraph.charOffsets.resize(text.size());
    for (uint32_t i = 0; i < block.paragraph.charOffsets.size(); ++i)
        block.paragraph.charOffsets[i] = start + i;
    return block;
}

std::vector<uint32_t> offsetsOf(observable_list<Bookmark const>& marks) {
    std::vector<uint32_t> offsets;
    for (const Bookmark& mark : marks) offsets.push_back(mark.charOffset);
    return offsets;
}

bool isChange(const std::vector<list_change>& heard, list_change::kind_t kind, uint32_t at, uint32_t count) {
    return heard.size() == 1 && heard[0] == list_change{kind, at, count};
}

/// Новая книга — новое оглавление одним сбросом; строки смотрят в блоки той
/// книги, что открыта сейчас, даже когда заголовки у двух книг одинаковы.
void testContentsFollowBook() {
    std::printf("\n=== места книги: оглавление ===\n");

    const typography::Block first[] = {
        blockAt(u"Часть первая", 0, typography::BlockKind::Title, 0),
        blockAt(u"Глава первая", 20, typography::BlockKind::Title, 1),
        blockAt(u"Текст главы.", 40, typography::BlockKind::Paragraph),
    };
    const typography::Block second[] = {
        blockAt(u"Часть первая", 0, typography::BlockKind::Title, 0),
        blockAt(u"Глава первая", 20, typography::BlockKind::Title, 1),
    };
    const typography::Block untitled[] = {blockAt(u"Только текст.", 0, typography::BlockKind::Paragraph)};

    // Слышанное — раньше модели: слушатели уходят вместе с ней и не должны
    // пережить то, во что пишут.
    std::vector<list_change> heard;
    uint32_t countWhenHeard = 0;

    BookPlaces places;
    static_cast<void>(places.contents().on_change([&](const list_change& change) noexcept {
        heard.push_back(change);
        countWhenHeard = places.contents().count().get();
    }));

    places.open(first, {});
    observable_list<ContentsEntry const>& contents = places.contents();
    check(isChange(heard, list_change::reset, 0, 2), "открыли книгу — один сброс на два заголовка");
    check(countWhenHeard == 2, "число строк — уже новое, когда перемену слышат");
    check(contents.size() == 2 && contents[1].level == 1 && contents[1].charOffset == 20,
          "уровень и место заголовка — его блока");
    check(contents.size() == 2 && contents[0].title.data() == first[0].paragraph.text.data(),
          "заголовок — вид в блок книги, не копия");

    heard.clear();
    places.open(second, {});
    check(isChange(heard, list_change::reset, 0, 2), "та же картина заголовков у другой книги — всё равно сброс");
    check(contents.size() == 2 && contents[0].title.data() == second[0].paragraph.text.data(),
          "строки смотрят в блоки новой книги, а не прежней");

    heard.clear();
    places.open(untitled, {});
    check(isChange(heard, list_change::reset, 0, 0) && contents.empty(),
          "книга без заголовков — оглавление пусто");
}

/// Закладка: одна кнопка ставит и снимает; список всегда по порядку книги,
/// куда бы ни встала новая, и слышно это одной вставкой или одним стиранием
/// на своём месте, а не пересборкой списка.
void testBookmarkToggles() {
    std::printf("\n=== места книги: закладка поставить и снять ===\n");

    std::vector<list_change> heard;

    BookPlaces places;
    static_cast<void>(places.bookmarks().on_change(
        [&heard](const list_change& change) noexcept { heard.push_back(change); }));
    observable_list<Bookmark const>& marks = places.bookmarks();

    check(places.toggleBookmark(300, u16_text{u"триста"}), "на пустом месте — поставлена");
    check(isChange(heard, list_change::inserted, 0, 1) && marks.size() == 1, "одна вставка в начало");

    places.toggleBookmark(100, u16_text{u"сто"});
    heard.clear();
    check(places.toggleBookmark(200, u16_text{u"двести"}), "между двумя — поставлена");
    check(isChange(heard, list_change::inserted, 1, 1), "вставка на своё место, в середину");
    check(offsetsOf(marks) == std::vector<uint32_t>{100, 200, 300}, "вставка в середину держит порядок книги");
    check(marks[1].hint == L"двести", "подсказка — у своей закладки");

    heard.clear();
    check(!places.toggleBookmark(200, u16_text{u"другие слова"}), "на том же месте — снята");
    check(isChange(heard, list_change::erased, 1, 1), "одно стирание на её месте");
    check(offsetsOf(marks) == std::vector<uint32_t>{100, 300}, "снята только она");

    heard.clear();
    check(places.toggleBookmark(0, u16_text{}), "в самом начале книги — тоже ставится");
    check(isChange(heard, list_change::inserted, 0, 1), "в начало — вставка в начало");
    check(offsetsOf(marks) == std::vector<uint32_t>{0, 100, 300}, "начало книги — первой");

    heard.clear();
    check(places.toggleBookmark(400, u16_text{u"в конце"}), "за последней — поставлена");
    check(isChange(heard, list_change::inserted, 3, 1), "за последней — вставка в конец");

    check(places.toggleBookmark(200, u16_text{u"снова"}), "снятая ставится снова");
    check(offsetsOf(marks) == std::vector<uint32_t>{0, 100, 200, 300, 400} && marks[2].hint == L"снова",
          "на своё место и с новой подсказкой");
    check(marks.count().get() == 5, "число закладок — полем, к нему привязана подпись «пока нет»");
}

/// Закладки новой книги — из её состояния, одним сбросом и по порядку книги;
/// состояние для записи несёт место чтения и те же закладки.
void testOpenAndState() {
    std::printf("\n=== места книги: закладки новой книги и запись ===\n");

    const typography::Block book[] = {blockAt(u"Глава", 0, typography::BlockKind::Title)};

    std::vector<list_change> heard;

    BookPlaces places;
    static_cast<void>(places.bookmarks().on_change(
        [&heard](const list_change& change) noexcept { heard.push_back(change); }));

    places.toggleBookmark(5, u16_text{u"прежняя книга"});
    heard.clear();

    // Файл поправлен руками: закладки не по порядку.
    places.open(book, {Bookmark{300, u16_text{u"триста"}}, Bookmark{100, u16_text{u"сто"}}});
    check(isChange(heard, list_change::reset, 0, 2), "новая книга — один сброс закладок");
    check(offsetsOf(places.bookmarks()) == std::vector<uint32_t>{100, 300}, "закладки по порядку книги");

    places.toggleBookmark(200, u16_text{u"двести"});
    check(offsetsOf(places.bookmarks()) == std::vector<uint32_t>{100, 200, 300},
          "после упорядочения новая встаёт на своё место");

    const BookState state = places.stateAt(4242);
    check(state.charOffset == 4242, "место чтения — то, что дали");
    check(state.bookmarks.size() == 3 && state.bookmarks[0].charOffset == 100 &&
              state.bookmarks[1].hint == L"двести" && state.bookmarks[2].charOffset == 300,
          "закладки для записи — те, что в списке");

    heard.clear();
    places.open(book, {});
    check(isChange(heard, list_change::reset, 0, 0) && places.bookmarks().empty(),
          "книга без закладок — список пуст");
}

}  // namespace

void runBookPlacesTests() {
    testContentsFollowBook();
    testBookmarkToggles();
    testOpenAndState();
}
