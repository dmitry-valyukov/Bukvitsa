// Обход таблицы образцов: алгоритм Лянга, как он описан у Кнута в приложении H
// «The TeXbook» и в диссертации Лянга 1983 года.
//
// Слово окружается точками — «.слово.», — и от каждого места в нём идёт обход
// бора. Каждый образец, узнавший себя, расставляет по стыкам букв уровни;
// побеждает наибольший, нечётный разрешает разрыв, чётный запрещает. Отсюда
// вся сила способа: запрет пишется таким же образцом, что и разрешение, и
// частный случай, стоящий в словаре, отменяет общее правило сам, без списка
// исключений.

#include <algorithm>
#include <cstdint>

// Свой заголовок после стандартных: он ведёт к block.h, а тот к импорту
// модуля книги, после которого MSVC стандартный заголовок уже не примет.
#include "bukvitsa/typography/hyphenation.h"

namespace bukvitsa::typography {
namespace {

#include "hyphen_patterns.inc"

/// Номер символа в алфавите образцов; ноль — «такой буквы в образцах нет».
///
/// Регистр складывается самой таблицей: прописной букве в ней стоит номер
/// строчной. Ничего, кроме латиницы, кириллицы и точки с дефисом, номера не
/// имеет — см. hyphenate() о том, почему это к лучшему.
constexpr std::uint8_t symbolOf(const wchar_t character) noexcept {
    const auto code_point = static_cast<std::uint32_t>(character);

    if (code_point >= 0x0020 && code_point < 0x007F)
        return hyphenSymbolAscii[code_point - 0x0020];
    if (code_point >= 0x0400 && code_point < 0x0460)
        return hyphenSymbolCyrillic[code_point - 0x0400];

    return 0;
}

/// Слово, переведённое в номера символов и окружённое точками, — то, с чем
/// работает и обход бора, и поиск среди исключений.
struct Symbols {
    std::uint8_t at[kMaxHyphenatedWord + 2] = {};
    std::size_t size = 0;                        ///< вместе с точками по краям
    bool complete = false;                       ///< все буквы нашлись в алфавите
};

Symbols symbolsOf(const std::wstring_view word) noexcept {
    Symbols symbols;
    if (word.empty() || word.size() > kMaxHyphenatedWord)
        return symbols;

    symbols.at[0] = hyphenBoundarySymbol;
    for (std::size_t i = 0; i < word.size(); ++i) {
        const std::uint8_t symbol = symbolOf(word[i]);
        if (symbol == 0)
            return symbols;
        symbols.at[i + 1] = symbol;
    }

    symbols.size = word.size() + 2;
    symbols.at[symbols.size - 1] = hyphenBoundarySymbol;
    symbols.complete = true;
    return symbols;
}

/// Письменность первой буквы решает, чьими образцами слово размечать: книги
/// почти всегда смешанные, а язык книги знает про них только то, какого он
/// сам.
constexpr std::size_t kLanguageNone = static_cast<std::size_t>(-1);

std::size_t languageOf(const std::wstring_view word) noexcept {
    const auto first = static_cast<std::uint32_t>(word.front());

    if (first >= 0x0400 && first < 0x0460) return 0;  // Russian
    if (first < 0x007F) return 1;                     // English

    return kLanguageNone;
}

/* ---------------- таблица ---------------- */

/// Переход по символу; ноль — такого нет.
///
/// Перебором, а не двоичным поиском: переходов у узла единицы, лежат они
/// подряд в кэше, и замер показал перебор быстрее почти на четверть — предсказатель
/// переходов на коротком цикле не ошибается, а на дереве поиска ошибается всегда.
/// Широкий корень из этого счёта вынут: его переходы разложены таблицей ниже.
std::uint32_t childAt(const std::uint32_t node, const std::uint8_t symbol) noexcept {
    const std::uint32_t edges = hyphenTrie[node];
    for (std::uint32_t edge = 0; edge < edges; ++edge) {
        const std::uint32_t at = node + 2 + edge * 4;
        if (hyphenTrie[at] != symbol)
            continue;

        return static_cast<std::uint32_t>(hyphenTrie[at + 1]) |
               (static_cast<std::uint32_t>(hyphenTrie[at + 2]) << 8) |
               (static_cast<std::uint32_t>(hyphenTrie[at + 3]) << 16);
    }
    return 0;
}

/// Уровни узла — это уровни образца, кончающегося здесь; место каждого
/// отсчитывается от начала образца, а значит от `from` в слове.
void applyLevels(const std::uint32_t node, const std::size_t from,
                 std::uint8_t* const levels) noexcept {
    const std::uint32_t edges = hyphenTrie[node];
    const std::uint32_t count = hyphenTrie[node + 1];
    const std::uint32_t values = node + 2 + edges * 4;

    for (std::uint32_t value = 0; value < count; ++value) {
        const std::size_t at = from + hyphenTrie[values + value * 2];
        const std::uint8_t level = hyphenTrie[values + value * 2 + 1];
        levels[at] = std::max(levels[at], level);
    }
}

/// Слово, целиком стоящее в словаре исключений: там перечислены все его
/// переносы и только они. Исключения отсортированы по номерам символов, и
/// сравнение идёт по ним же — то же слово в другом регистре уже переведено.
bool exceptionFor(const Symbols& symbols, const std::size_t language,
                  HyphenPoints& points) noexcept {
    const std::uint8_t* const letters = symbols.at + 1;
    const auto length = static_cast<std::uint8_t>(symbols.size - 2);

    std::uint32_t low = hyphenExceptionBounds[language];
    std::uint32_t high = hyphenExceptionBounds[language + 1];

    while (low < high) {
        const std::uint32_t middle = low + (high - low) / 2;
        const std::uint32_t at = hyphenExceptionAt[middle];
        const std::uint8_t size = hyphenExceptions[at];
        const std::uint8_t* const word = hyphenExceptions + at + 1;

        const auto order = std::lexicographical_compare_three_way(word, word + size, letters,
                                                                  letters + length);
        if (order < 0) {
            low = middle + 1;
        } else if (order > 0) {
            high = middle;
        } else {
            std::uint32_t mask = 0;
            for (int byte = 0; byte < 4; ++byte)
                mask |= static_cast<std::uint32_t>(word[size + byte]) << (byte * 8);
            points = mask;
            return true;
        }
    }
    return false;
}

}  // namespace

bool isHyphenLetter(const wchar_t character) noexcept {
    const std::uint8_t symbol = symbolOf(character);
    return symbol != 0 && symbol != hyphenBoundarySymbol && character != L'-';
}

HyphenPoints hyphenate(const std::wstring_view word) noexcept {
    if (word.empty())
        return 0;

    const std::size_t language = languageOf(word);
    if (language == kLanguageNone)
        return 0;

    const Symbols symbols = symbolsOf(word);
    if (!symbols.complete)
        return 0;

    const std::uint8_t left = hyphenLanguages[language].left;
    const std::uint8_t right = hyphenLanguages[language].right;
    if (word.size() < static_cast<std::size_t>(left) + right)
        return 0;

    HyphenPoints points = 0;
    if (!exceptionFor(symbols, language, points)) {
        // Уровень стыка перед i-м символом слова лежит в levels[i + 1]: первая
        // точка слева сдвигает слово на единицу, как и в образцах.
        std::uint8_t levels[kMaxHyphenatedWord + 3] = {};

        for (std::size_t from = 0; from + 1 < symbols.size; ++from) {
            // Первый шаг — по таблице корня: обход начинается с каждого места
            // слова, и корень, у которого переход есть на всякую букву обоих
            // алфавитов, иначе перебирался бы чаще всех прочих узлов вместе.
            std::uint32_t node = hyphenRootEdges[language][symbols.at[from]];

            for (std::size_t at = from + 1; node != 0; ++at) {
                applyLevels(node, from, levels);
                if (at == symbols.size)
                    break;
                node = childAt(node, symbols.at[at]);
            }
        }

        for (std::size_t i = 1; i < word.size(); ++i)
            if (levels[i + 1] % 2 != 0)
                points |= HyphenPoints{1} << i;
    }

    // Одну букву не оставляют и не переносят: минимумы объявлены в самих
    // образцах и держатся здесь, чтобы ни один их читатель не забыл о них.
    const HyphenPoints allowed = ((HyphenPoints{1} << (word.size() - right + 1)) - 1) &
                                 ~((HyphenPoints{1} << left) - 1);
    return points & allowed;
}

}  // namespace bukvitsa::typography
