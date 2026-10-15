// Тесты модели поиска по книге: что говорит подпись под полем, что лежит в
// находках и что слышит тот, кто к ним привязан. Книга — хранилище из
// абзацев, собранное тестом: поиск смотрит только на текст блоков и позиции
// символов, вёрстка ему не нужна.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string_view>

#include "check.h"

// Модель поиска — последней: через индекс книги она ведёт к импорту модулей.
#include "bukvitsa/reader/book_search.h"

import wxl.core;

using namespace bukvitsa;
using namespace bukvitsa::reader;
using bukvitsa::reader::tests::check;

namespace {

constexpr std::u16string_view kPrompt = u"Введите слово и нажмите Enter.";

/// Абзац книги, чьи символы стоят подряд с позиции `start`.
struct Passage {
    u16_view text;
    uint32_t start = 0;
};

/// Книга из этих абзацев — весь поток чтения: у каждого один прогон стиля на
/// весь текст и один прогон позиций.
typography::BookText paragraphsAt(std::initializer_list<Passage> passages) {
    typography::BookText book;

    for (const Passage& paragraph : passages) {
        const auto size = static_cast<uint32_t>(paragraph.text.size());

        typography::Block block;
        block.kind = typography::BlockKind::Paragraph;
        block.charOffset = paragraph.start;
        block.paragraph.text = {static_cast<uint32_t>(book.text.size()), size};
        block.paragraph.spans = {static_cast<uint32_t>(book.spans.size()), 1};
        block.paragraph.notes = {static_cast<uint32_t>(book.notes.size()), 0};
        block.paragraph.offsets = {static_cast<uint32_t>(book.offsets.size()), 1};

        book.text += paragraph.text;
        book.spans.push_back({0, size, {}});
        book.offsets.push_back({0, paragraph.start});
        book.blocks.push_back(block);
    }

    book.flowCount = static_cast<uint32_t>(book.blocks.size());
    return book;
}

/// Подпись сейчас — приглашение набрать слово, и находок нет.
bool waitsForWord(BookSearch& search) {
    return search.status().get().plain() == kPrompt && search.hits().empty();
}

/// До поиска и при запросе без слова — приглашение, а не поиск.
void testPrompt() {
    std::printf("\n=== поиск: подпись, пока искать нечего ===\n");

    const typography::BookText book = paragraphsAt({{u"Мама мыла раму.", 0}});

    BookSearch search;
    check(waitsForWord(search), "до поиска — приглашение, находок нет");

    search.run(book);
    check(waitsForWord(search), "пустой запрос — приглашение");

    search.query.set(u16_text{u"   "});
    search.run(book);
    check(waitsForWord(search), "из пробелов — приглашение");

    search.query.set(u16_text{u" \t "});
    search.run(book);
    check(waitsForWord(search), "из пробелов и табуляций — приглашение");
}

/// Нашлось — число и находки на своих местах; не нашлось — запрос в подписи
/// и прежние находки ушли; пустой запрос после поиска находки тоже убирает.
void testFound() {
    std::printf("\n=== поиск: находки и подпись ===\n");

    // Второй абзац — не с нуля, как в настоящей книге: позиция находки —
    // позиция в книге, а не в абзаце.
    const typography::BookText book = paragraphsAt({
        {u"Мама мыла раму.", 0},
        {u"Рама у окна.", 100},
    });

    BookSearch search;
    search.query.set(u16_text{u"рам"});
    search.run(book);

    observable_list<SearchHit const>& found = search.hits();
    check(search.status().get().plain() == u"Нашлось: 2", "нашлось — «Нашлось: N»");
    check(found.size() == 2 && found[0].charOffset == 10 && found[1].charOffset == 100,
          "находки на своих местах в книге, в порядке чтения");
    check(found.size() == 2 && found[0].context.plain() == u"Мама мыла раму." &&
              found[1].context.plain() == u"Рама у окна.",
          "у находки — отрывок её абзаца");
    check(search.query.get().plain() == u"рам", "запрос остаётся в поле");

    search.query.set(u16_text{u"кот"});
    search.run(book);
    check(search.status().get().plain() == u"«кот» в книге не нашлось.",
          "не нашлось — запрос в подписи");
    check(search.hits().empty(), "прежние находки не остаются под новой подписью");

    search.query.set(u16_text{u"рам"});
    search.run(book);
    search.query.set({});
    search.run(book);
    check(waitsForWord(search), "пустой запрос после поиска — приглашение, список пуст");
}

/// Новая книга: всё к началу, прежние находки в неё не указывают.
void testClear() {
    std::printf("\n=== поиск: новая книга ===\n");

    const typography::BookText book = paragraphsAt({{u"Мама мыла раму.", 0}});

    BookSearch search;
    search.query.set(u16_text{u"мыла"});
    search.run(book);
    check(search.hits().size() == 1, "перед новой книгой — одна находка");

    search.clear();
    check(search.query.get().empty(), "запрос пуст");
    check(waitsForWord(search), "подпись — приглашение, находок нет");
}

/// Подпись — наблюдаемое поле, находки — список: к ним привязаны подпись и
/// список панели, и перемены должны до них доходить. Новый поиск — один
/// сброс списка, а не находка за находкой; находки приходят раньше подписи, а
/// тот же поиск ещё раз списка не трогает.
void testWatchers() {
    std::printf("\n=== поиск: перемены слышны привязанным ===\n");

    const typography::BookText book = paragraphsAt({
        {u"Мама мыла раму.", 0},
        {u"Рама у окна.", 100},
    });

    // Счётчики — раньше модели: слушатели уходят вместе с ней и не должны
    // пережить то, во что пишут.
    size_t statusChanges = 0;
    size_t hitsChanges = 0;
    size_t heardHits = 0;
    size_t hitsWhenStatus = 0;
    u16_text heardStatus;
    list_change lastChange{};

    BookSearch search;
    static_cast<void>(search.status().on_change([&](const u16_text& said) noexcept {
        ++statusChanges;
        heardStatus = said;
        hitsWhenStatus = search.hits().size();
    }));
    static_cast<void>(search.hits().on_change([&](const list_change& change) noexcept {
        ++hitsChanges;
        lastChange = change;
        heardHits = search.hits().size();
    }));

    search.query.set(u16_text{u"рам"});
    search.run(book);
    check(statusChanges == 1 && heardStatus.plain() == u"Нашлось: 2", "подпись слышна");
    check(hitsChanges == 1 && heardHits == 2, "находки слышны");
    check(lastChange == list_change{list_change::reset, 0, 2}, "новый поиск — один сброс на все находки");
    check(hitsWhenStatus == 2, "к «Нашлось: N» находки уже на месте");

    search.run(book);
    check(statusChanges == 1 && hitsChanges == 1,
          "тот же поиск ещё раз — без перемен: список не перестраивается");

    search.clear();
    check(statusChanges == 2 && heardStatus.plain() == kPrompt, "новая книга — приглашение слышно");
    check(hitsChanges == 2 && heardHits == 0 && lastChange == list_change{list_change::reset, 0, 0},
          "новая книга — пустой список слышен одним сбросом");

    search.clear();
    check(hitsChanges == 2, "пустой список ещё раз не очищается — без перемен");
}

}  // namespace

void runBookSearchTests() {
    testPrompt();
    testFound();
    testClear();
    testWatchers();
}
