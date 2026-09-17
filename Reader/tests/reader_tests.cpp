// Проверка чистых функций читалки над блоками книги. Как у FB3 и вёрстки, без
// фреймворка.

#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Заголовок читалки после всех стандартных: он ведёт к импорту модуля книги.
#include "book_index.h"

import wxl.core;

using namespace bukvitsa;

namespace {

int failures = 0;

void check(bool condition, std::string_view what) {
    std::printf("%s %.*s\n", condition ? "  ok  " : "FAILED", static_cast<int>(what.size()),
                what.data());
    if (!condition) ++failures;
}

bool isLetterStart(std::wstring_view text, std::size_t at) {
    return at >= text.size() || wxl::core::floor_grapheme_boundary(text, at) == at;
}

/// Текст собран тестом из кусков, поэтому проверяется, как всякий чужой.
typography::Block blockOf(std::wstring_view text) {
    typography::Block block;
    block.kind = typography::BlockKind::Paragraph;
    block.paragraph.text = wxl::core::u16_text(wxl::core::checked(text).value());
    block.paragraph.charOffsets.resize(block.paragraph.text.size());
    for (std::uint32_t i = 0; i < block.paragraph.charOffsets.size(); ++i)
        block.paragraph.charOffsets[i] = i;
    return block;
}

/// Отрывок без многоточий по краям — кусок текста абзаца с того места, где он
/// нашёлся.
std::wstring_view withoutEllipses(std::wstring_view piece) {
    if (piece.starts_with(L"\x2026")) piece.remove_prefix(1);
    if (piece.ends_with(L"\x2026")) piece.remove_suffix(1);
    return piece;
}

/// Буквы из нескольких кодовых точек, которые встают на край подсказки и
/// отрывка поиска.
constexpr std::wstring_view kLetters[] = {
    L"\x0438\x0306",                                   // и + краткая
    L"\x0435\x0308\x0301",                             // е + диерезис + акут
    L"\xD842\xDFB7",                                   // иероглиф вне BMP
    L"\xD83D\xDC4D\xD83C\xDFFD",                       // эмодзи с цветом кожи
    L"\xD83D\xDC68\x200D\xD83D\xDC69\x200D\xD83D\xDC67", // семья через ZWJ
};

/// Подсказка закладки — первые 60 единиц абзаца. Буква, которая на них
/// приходится, в подсказку либо входит целиком, либо не входит.
void testHintKeepsLetters() {
    std::printf("\n=== подсказка закладки ===\n");

    for (const std::wstring_view letter : kLetters) {
        std::wstring text(59, L'\x0436');
        text += letter;
        text.append(20, L'\x0436');

        const typography::Block blocks[] = {blockOf(text)};
        const wxl::core::u16_text hint = reader::hintAt(blocks, 0);
        const std::wstring_view cut = withoutEllipses(hint.wchars());

        check(text.starts_with(cut) && isLetterStart(text, cut.size()),
              "подсказка кончается между буквами");
    }
}

/// Отрывок вокруг находки — 30 единиц до и 70 после. Буквы на обоих краях.
void testSearchContextKeepsLetters() {
    std::printf("\n=== отрывок поиска ===\n");

    for (const std::wstring_view letter : kLetters) {
        // Находка «q» на 60-й единице: отрывок начинается с 30-й и кончается на
        // 131-й, и на обоих местах стоит вторая единица буквы.
        std::wstring text(29, L'\x0436');
        text += letter;
        text.resize(60, L'\x0436');
        text += L'q';
        text.resize(130, L'\x0436');
        text += letter;
        text.append(20, L'\x0436');

        const typography::Block blocks[] = {blockOf(text)};
        const wxl::core::sta_vector<reader::SearchHit> hits = reader::searchBook(blocks, L"q");

        check(hits.size() == 1, "находка одна");
        if (hits.size() != 1) continue;

        const std::wstring_view piece = withoutEllipses(hits[0].context.wchars());
        const std::size_t from = text.find(piece);
        check(from != std::wstring::npos && isLetterStart(text, from) &&
                  isLetterStart(text, from + piece.size()),
              "отрывок начинается и кончается между буквами");
    }
}

/// Оглавление не копирует заголовки: его строки живут, пока заполняется
/// вкладка, а текст блока — пока открыта книга.
void testContentsBorrowTitles() {
    std::printf("\n=== оглавление ===\n");

    typography::Block title = blockOf(L"Глава первая");
    title.kind = typography::BlockKind::Title;
    const typography::Block blocks[] = {title, blockOf(L"Текст главы.")};

    const wxl::core::sta_vector<reader::ContentsEntry> contents = reader::contentsOf(blocks);
    check(contents.size() == 1, "в оглавлении один заголовок");
    if (contents.size() != 1) return;

    check(contents[0].title == L"Глава первая", "строка оглавления — текст заголовка");
    check(contents[0].title.data() == blocks[0].paragraph.text.data(),
          "заголовок взят из блока, а не скопирован");
}

/// Что поиск считает совпадением: регистр не важен, «ё» равна «е»,
/// неразрывный пробел — обычному, а мягкий перенос внутри слова не мешает.
/// Место находки — в тексте абзаца как он есть.
void testSearchMatchesWhatTheReaderMeans() {
    std::printf("\n=== что поиск считает совпадением ===\n");

    struct Case {
        const char* what;
        std::wstring_view text;
        std::wstring_view needle;
        std::uint32_t at;
    };

    const Case found[] = {
        {"регистр", L"Сказал Прометей.", L"прометей", 7},
        {"«ё» в тексте, «е» в запросе", L"Ну, ещё раз.", L"ЕЩЕ", 4},
        {"«е» в тексте, «ё» в запросе", L"еще раз", L"ещё", 0},
        {"мягкий перенос внутри слова", L"О Про\x00ADме\x00ADтее", L"прометее", 2},
        {"неразрывный пробел", L"за 10\x00A0лет", L"10 лет", 3},
    };

    for (const Case& test : found) {
        const typography::Block blocks[] = {blockOf(test.text)};
        const wxl::core::sta_vector<reader::SearchHit> hits = reader::searchBook(blocks, test.needle);
        check(hits.size() == 1 && hits[0].charOffset == test.at,
              std::string("находка на своём месте: ") + test.what);
    }

    const typography::Block hyphen[] = {blockOf(L"Про-метей")};
    check(reader::searchBook(hyphen, L"прометей").empty(), "обычный дефис — не мягкий перенос");
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    testHintKeepsLetters();
    testSearchContextKeepsLetters();
    testContentsBorrowTitles();
    testSearchMatchesWhatTheReaderMeans();

    std::printf("\n%s\n", failures == 0 ? "OK" : "ЕСТЬ ОШИБКИ");
    return failures;
}
