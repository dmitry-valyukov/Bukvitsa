#include <algorithm>
#include <array>
#include <optional>
#include <string_view>
#include <vector>

// Свой заголовок после всех стандартных: он ведёт к импорту модуля книги, а
// стандартный заголовок после импорта MSVC уже не принимает.
#include "bukvitsa/reader/book_index.h"

import wxl.core;

namespace bukvitsa::reader {
namespace {

/// Чем единица текста считается при поиске: строчной буквой, «ё» — «е»,
/// неразрывный пробел — обычным, а мягкий перенос — нулём, то есть «пропустить».
///
/// Строчные — средствами Windows, а не `std::towlower`: тот смотрит в локаль C,
/// а она по умолчанию «C», где кириллицы нет вовсе. `CharLowerBuffW` знает
/// Unicode целиком и от локали процесса не зависит.
///
/// Поиск сравнивает текст абзаца как он есть, по единице, и звать Windows на
/// каждую не может, поэтому ответы собраны в таблицу один раз. Хранится сдвиг
/// от самой единицы, страницами по 256: у почти всех страниц Unicode регистра
/// нет, их сдвиги нулевые, и такая страница одна на всех.
class SearchFolding {
public:
    SearchFolding() {
        storage_.assign(256, 0);
        std::array<size_t, 256> starts{};

        std::array<wchar_t, 256> units{};
        std::array<uint16_t, 256> shifts{};

        for (unsigned high = 0; high < 256; ++high) {
            for (unsigned low = 0; low < 256; ++low)
                units[low] = static_cast<wchar_t>(high << 8 | low);

            // Половины суррогатных пар не буквы, и их страницы остаются как есть.
            if (high < 0xD8 || high > 0xDF)
                ::CharLowerBuffW(units.data(), static_cast<DWORD>(units.size()));

            bool same = true;
            for (unsigned low = 0; low < 256; ++low) {
                const auto unit = static_cast<wchar_t>(high << 8 | low);
                shifts[low] = static_cast<uint16_t>(forSearch(units[low]) - unit);
                same = same && shifts[low] == 0;
            }

            if (!same) {
                starts[high] = storage_.size();
                storage_.insert(storage_.end(), shifts.begin(), shifts.end());
            }
        }

        for (unsigned high = 0; high < 256; ++high)
            pages_[high] = storage_.data() + starts[high];
    }

    wchar_t operator()(wchar_t unit) const noexcept {
        return static_cast<wchar_t>(unit + pages_[unit >> 8][unit & 0xFF]);
    }

private:
    /// Что сверх строчных: то, что читатель пишет иначе, чем набрано в книге.
    static wchar_t forSearch(wchar_t lower) noexcept {
        switch (lower) {
        case L'\x0451': return L'\x0435';   // ё — е
        case L'\x00A0':                      // неразрывный пробел
        case L'\x202F': return L' ';         // узкий неразрывный пробел
        case L'\x00AD': return 0;            // мягкий перенос
        default: return lower;
        }
    }

    std::vector<uint16_t> storage_;
    std::array<const uint16_t*, 256> pages_{};
};

const SearchFolding& searchFolding() {
    static const SearchFolding folding;
    return folding;
}

/// Сколько единиц абзаца с `at` занимает ключ, или ноль, если ключа там нет.
/// Мягкий перенос внутри совпадения пропускается и в длину входит.
size_t matchLength(const SearchFolding& fold, std::wstring_view text, size_t at,
                   std::wstring_view key) noexcept {
    size_t end = at;
    for (const wchar_t wanted : key) {
        wchar_t folded = 0;
        while (end < text.size() && (folded = fold(text[end])) == 0)
            ++end;
        if (end == text.size() || folded != wanted) return 0;
        ++end;
    }
    return end - at;
}

/// Кусок текста вокруг находки: немного до и побольше после.
///
/// Края считаются в единицах UTF-16 и потому сдвигаются к началу буквы: знак
/// краткой без своей «и» или половина пары на краю отрывка — порченая буква.
u16_text contextAround(u16_view text, size_t at, size_t length) {
    constexpr size_t kBefore = 30;
    constexpr size_t kAfter = 70;

    const size_t from = unicode::floor_grapheme_boundary(text, at > kBefore ? at - kBefore : 0);
    const size_t to =
        unicode::floor_grapheme_boundary(text, std::min(text.size(), at + length + kAfter));

    u16_text out;
    out.reserve(to - from + 2);
    if (from > 0) out += u"…";
    out += text.substr(from, to - from);
    if (to < text.size()) out += u"…";
    return out;
}

}  // namespace

sta_vector<ContentsEntry> contentsOf(const typography::BookText& book) {
    sta_vector<ContentsEntry> contents;

    for (const typography::Block& block : book.flow()) {
        if (block.kind != typography::BlockKind::Title) continue;
        if (block.paragraph.text.count == 0) continue;

        contents.push_back({book.textOf(block.paragraph), block.level, block.charOffset});
    }

    return contents;
}

sta_vector<SearchHit> searchBook(const typography::BookText& book, u16_view needle, size_t limit) {
    sta_vector<SearchHit> hits;
    if (needle.empty() || limit == 0) return hits;

    const SearchFolding& fold = searchFolding();

    sta_wstring key;
    key.reserve(needle.size());
    for (const wchar_t unit : needle.wchars())
        if (const wchar_t folded = fold(unit)) key.push_back(folded);
    if (key.empty()) return hits;

    // Абзац не копируется: сравнение идёт по его тексту как есть, так что место
    // находки — сразу место в абзаце, а пропущенный мягкий перенос его не сдвигает.
    // Позицию в книге даёт хранилище по прогонам позиций абзаца; у блока без
    // текста находок нет, и спрашивать её не о чем.
    for (const typography::Block& block : book.flow()) {
        const u16_view paragraph = book.textOf(block.paragraph);
        const std::wstring_view text = paragraph.wchars();

        for (size_t at = 0; at < text.size(); ++at) {
            if (fold(text[at]) != key.front()) continue;

            const size_t length = matchLength(fold, text, at, key);
            if (length == 0) continue;

            hits.push_back({contextAround(paragraph, at, length),
                            book.charOffsetAt(block.paragraph, static_cast<uint32_t>(at))});
            if (hits.size() >= limit) return hits;

            at += length - 1;
        }
    }

    return hits;
}

std::optional<u16_view> searchQuery(std::u16string_view typed) {
    const std::optional<u16_view> needle = unicode::checked(typed);

    // Пусто ли введённое — пробелы и табуляции не в счёт.
    if (!needle || trim(needle->plain(), u" \t").empty()) return std::nullopt;

    return needle;
}

u16_text hintAt(const typography::BookText& book, uint32_t charOffset) {
    constexpr size_t kWords = 60;

    // Последний блок потока, начинающийся не позже искомой позиции: блоки идут
    // по возрастанию, и это проверено тестом вёрстки.
    const typography::Block* found = nullptr;
    for (const typography::Block& block : book.flow()) {
        if (block.charOffset > charOffset) break;
        if (block.paragraph.text.count != 0) found = &block;
    }
    if (!found) return {};

    // Граница в kWords единиц может прийтись на середину буквы — тогда
    // подсказка на эту букву короче, зато без её обрубка.
    const u16_view text = book.textOf(found->paragraph);
    const size_t cut = unicode::floor_grapheme_boundary(text, kWords);

    u16_text hint(text.substr(0, cut));
    if (cut < text.size()) hint += u"…";
    return hint;
}

}  // namespace bukvitsa::reader
