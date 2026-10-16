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
#include <cassert>
#include <cstdint>
#include <span>
#include <string_view>

// Свой заголовок после стандартных: он ведёт к block.h, а тот к импорту
// модуля книги, после которого MSVC стандартный заголовок уже не примет.
#include "bukvitsa/typography/hyphenation.h"

import wxl.core;

namespace bukvitsa::typography {
namespace {

#include "hyphen_patterns.inc"

/// Номер символа в алфавите образцов; ноль — «такой буквы в образцах нет».
///
/// Регистр складывается самой таблицей: прописной букве в ней стоит номер
/// строчной. Номер есть только у латиницы, кириллицы и у точки с дефисом —
/// знаков, которыми образцы пишут край слова и его составные части.
constexpr uint8_t symbolOf(const char16_t unit) noexcept {
    const auto code_point = static_cast<uint32_t>(unit);
    return code_point < hyphenTableEnd ? hyphenSymbol[code_point] : 0;
}

/// Признаки кодовой точки из той же таблицы: заглавная, гласная, буква, знак
/// и язык буквы алфавита. За границей таблицы — ни одного.
constexpr uint8_t traitsOf(const char16_t unit) noexcept {
    const auto code_point = static_cast<uint32_t>(unit);
    return code_point < hyphenTableEnd ? hyphenTraits[code_point] : 0;
}

/// Слово, переведённое в номера символов и окружённое точками, — то, с чем
/// работает и обход бора, и поиск среди исключений.
struct Symbols {
    uint8_t at[kMaxHyphenatedWord + 2] = {};
    size_t size = 0;                             ///< вместе с точками по краям
    bool complete = false;                       ///< все буквы нашлись в алфавите
};

Symbols symbolsOf(const std::u16string_view word) noexcept {
    Symbols symbols;
    if (word.empty() || word.size() > kMaxHyphenatedWord)
        return symbols;

    symbols.at[0] = hyphenBoundarySymbol;
    for (size_t i = 0; i < word.size(); ++i) {
        const uint8_t symbol = symbolOf(word[i]);
        if (symbol == 0)
            return symbols;
        symbols.at[i + 1] = symbol;
    }

    symbols.size = word.size() + 2;
    symbols.at[symbols.size - 1] = hyphenBoundarySymbol;
    symbols.complete = true;
    return symbols;
}

/// Язык первой буквы решает, чьими образцами слово размечать: книги почти
/// всегда смешанные, а язык книги знает про них только то, какого он сам.
constexpr size_t kLanguageNone = static_cast<size_t>(-1);

/// Язык буквы алфавита образцов — номер в hyphenLanguages; у точки, дефиса
/// и всех прочих кодовых точек — kLanguageNone.
size_t languageOf(const char16_t letter) noexcept {
    const auto language = static_cast<size_t>(traitsOf(letter) >> hyphenLanguageShift);
    return language == 0 ? kLanguageNone : language - 1;
}

/* ---------------- таблица ---------------- */

/// Переход по символу; ноль — такого нет.
///
/// Перебором, а не двоичным поиском: переходов у узла единицы, лежат они
/// подряд в кэше, и замер показал перебор быстрее почти на четверть — предсказатель
/// переходов на коротком цикле не ошибается, а на дереве поиска ошибается всегда.
/// Широкий корень из этого счёта вынут: его переходы разложены таблицей ниже.
uint32_t childAt(const uint32_t node, const uint8_t symbol) noexcept {
    const uint32_t edges = hyphenTrie[node];
    for (uint32_t edge = 0; edge < edges; ++edge) {
        const uint32_t at = node + 2 + edge * 4;
        if (hyphenTrie[at] != symbol)
            continue;

        return static_cast<uint32_t>(hyphenTrie[at + 1]) |
               (static_cast<uint32_t>(hyphenTrie[at + 2]) << 8) |
               (static_cast<uint32_t>(hyphenTrie[at + 3]) << 16);
    }
    return 0;
}

/// Уровни узла — это уровни образца, кончающегося здесь; место каждого
/// отсчитывается от начала образца, а значит от `from` в слове.
void applyLevels(const uint32_t node, const size_t from,
                 uint8_t* const levels) noexcept {
    const uint32_t edges = hyphenTrie[node];
    const uint32_t count = hyphenTrie[node + 1];
    const uint32_t values = node + 2 + edges * 4;

    for (uint32_t value = 0; value < count; ++value) {
        const size_t at = from + hyphenTrie[values + value * 2];
        const uint8_t level = hyphenTrie[values + value * 2 + 1];
        levels[at] = std::max(levels[at], level);
    }
}

/// Слово, целиком стоящее в словаре исключений: там перечислены все его
/// переносы и только они. Исключения отсортированы по номерам символов, и
/// сравнение идёт по ним же — то же слово в другом регистре уже переведено.
bool exceptionFor(const Symbols& symbols, const size_t language,
                  HyphenPoints& points) noexcept {
    const uint8_t* const letters = symbols.at + 1;
    const auto length = static_cast<uint8_t>(symbols.size - 2);

    uint32_t low = hyphenExceptionBounds[language];
    uint32_t high = hyphenExceptionBounds[language + 1];

    while (low < high) {
        const uint32_t middle = low + (high - low) / 2;
        const uint32_t at = hyphenExceptionAt[middle];
        const uint8_t size = hyphenExceptions[at];
        const uint8_t* const word = hyphenExceptions + at + 1;

        const auto order = std::lexicographical_compare_three_way(word, word + size, letters,
                                                                  letters + length);
        if (order < 0) {
            low = middle + 1;
        } else if (order > 0) {
            high = middle;
        } else {
            uint32_t mask = 0;
            for (int byte = 0; byte < 4; ++byte)
                mask |= static_cast<uint32_t>(word[size + byte]) << (byte * 8);
            points = mask;
            return true;
        }
    }
    return false;
}

/// Ответ образцов на слово как оно есть. hyphenate() отдаёт его без перемен,
/// а hyphenPointsOf спрашивает о части слова, сложенной из основ её букв, и
/// судит найденное правилами набора.
HyphenPoints patternPoints(const std::u16string_view word) noexcept {
    if (word.empty())
        return 0;

    const size_t language = languageOf(word.front());
    if (language == kLanguageNone)
        return 0;

    const Symbols symbols = symbolsOf(word);
    if (!symbols.complete)
        return 0;

    const uint8_t left = hyphenLanguages[language].left;
    const uint8_t right = hyphenLanguages[language].right;
    if (word.size() < static_cast<size_t>(left) + right)
        return 0;

    HyphenPoints points = 0;
    if (!exceptionFor(symbols, language, points)) {
        // Уровень стыка перед i-м символом слова лежит в levels[i + 1]: первая
        // точка слева сдвигает слово на единицу, как и в образцах.
        uint8_t levels[kMaxHyphenatedWord + 3] = {};

        for (size_t from = 0; from + 1 < symbols.size; ++from) {
            // Первый шаг — по таблице корня: обход начинается с каждого места
            // слова, и корень, у которого переход есть на всякую букву обоих
            // алфавитов, иначе перебирался бы чаще всех прочих узлов вместе.
            uint32_t node = hyphenRootEdges[language][symbols.at[from]];

            for (size_t at = from + 1; node != 0; ++at) {
                applyLevels(node, from, levels);
                if (at == symbols.size)
                    break;
                node = childAt(node, symbols.at[at]);
            }
        }

        for (size_t i = 1; i < word.size(); ++i)
            if (levels[i + 1] % 2 != 0)
                points |= HyphenPoints{1} << i;
    }

    // Одну букву не оставляют и не переносят: минимумы объявлены в самих
    // образцах и держатся здесь, чтобы ни один их читатель не забыл о них.
    const HyphenPoints allowed = ((HyphenPoints{1} << (word.size() - right + 1)) - 1) &
                                 ~((HyphenPoints{1} << left) - 1);
    return points & allowed;
}

/* ---------------- правила набора ---------------- */

/// Чем буква служит слову, когда абзац делится на слова.
enum class WordRole : uint8_t {
    Boundary,      ///< пробел, пунктуация, символ: слова здесь нет
    Letter,        ///< буква алфавита образцов
    Foreign,       ///< буква, которой образцы не знают: слово с ней не переносится
    Digit,
    Period,
    Joiner,        ///< дефис и апостроф: делят слово на части
    SoftHyphen,    ///< мягкий перенос: место, назначенное автором
    Transparent,   ///< знак без основы: слову не принадлежит и его не кончает
};

/// Буква, которую образцы знают, — у неё есть язык. У точки и дефиса номер в
/// образцах есть, а языка нет: ими образцы пишут край слова и его составные
/// части, а не буквы.
bool isPatternLetter(const char16_t unit) noexcept {
    return (traitsOf(unit) >> hyphenLanguageShift) != 0;
}

/// Знак ударения — единственный, которого образцы не замечают: буква под ним
/// та же. Всякий другой знак делает букву другой — «и» с краткой это «й», «е»
/// с диерезисом — «ё», — и сводить такую пару к одной кодовой точке —
/// нормализация Юникода (NFC), которой в wxl::core нет. Без неё буква с таким
/// знаком для образцов чужая: перенос в её слове теряется, но не ставится на
/// неверное место, как стал бы, увидь образцы вместо «й» гласную «и». С
/// нормализацией такие буквы вернутся.
constexpr bool isStressMark(const char16_t unit) noexcept {
    return unit == u'\x0301' || unit == u'\x0300';
}

/// Роль буквы — от `at` до `next`, границы следующей. Решает основа, первая
/// единица; знаки за ней меняют роль только буквы алфавита: с ударением она
/// та же, с другим знаком — чужая. Про основу за границей таблицы — и тем
/// более вне BMP — таблица не знает, буква ли это, и она остаётся границей
/// слова, как пунктуация.
WordRole roleOf(const std::u16string_view units, const size_t at, const size_t next) noexcept {
    const char16_t base = units[at];
    if (base >= u'0' && base <= u'9')
        return WordRole::Digit;

    switch (base) {
    case u'.':
        return WordRole::Period;
    case u'-':        // дефис-минус
    case u'\x2010':   // дефис
    case u'\x2011':   // неразрывный дефис
    case u'\'':
    case u'\x2019':   // апостроф, он же закрывающая кавычка
    case u'\x02BC':   // апостроф-буква
        return WordRole::Joiner;
    case u'\x00AD':
        return WordRole::SoftHyphen;
    default:
        break;
    }

    if (isPatternLetter(base)) {
        for (size_t i = at + 1; i < next; ++i)
            if (!isStressMark(units[i]))
                return WordRole::Foreign;
        return WordRole::Letter;
    }

    const uint8_t traits = traitsOf(base);
    if ((traits & hyphenLetter) != 0)
        return WordRole::Foreign;
    // Буква начинается со знака, только если перед ним не было основы: после
    // мягкого переноса, в начале абзаца. Делить слово ему не с чего.
    if ((traits & hyphenMark) != 0)
        return WordRole::Transparent;
    return WordRole::Boundary;
}

/// Край знака сноски в разметке абзаца, пока она считается: значение, которого
/// у места переноса не бывает, — часть уносит не больше kMaxHyphenatedWord букв.
constexpr uint8_t kNoteEdge = 0xFF;
static_assert(kMaxHyphenatedWord < kNoteEdge);

/// Часть слова между дефисами — основы её букв и то, где каждая начинается в
/// абзаце.
struct WordPart {
    char16_t letters[kMaxHyphenatedWord] = {};
    size_t at[kMaxHyphenatedWord] = {};
    size_t size = 0;
    bool tooLong = false;
    bool softHyphen = false;      ///< мягкий перенос между её буквами
    bool softPending = false;     ///< мягкий перенос после буквы, следующей ещё не было

    void clear() noexcept {
        size = 0;
        tooLong = false;
        softHyphen = false;
        softPending = false;
    }
};

/// Места переноса одной части — по образцам и правилам набора.
void markPart(const WordPart& part, sta_vector<uint8_t>& hyphens) noexcept {
    if (part.size == 0 || part.tooLong || part.softHyphen)
        return;

    // Заглавная не первой буквой и буквы двух алфавитов вперемешку отменяют
    // часть: образцы одного языка к чужим буквам не применяются. Заодно —
    // где первая и последняя гласные.
    const size_t language = languageOf(part.letters[0]);
    size_t firstVowel = part.size;
    size_t lastVowel = 0;
    for (size_t i = 0; i < part.size; ++i) {
        const uint8_t traits = traitsOf(part.letters[i]);
        if ((i > 0 && (traits & hyphenCapital) != 0) || languageOf(part.letters[i]) != language)
            return;
        if ((traits & hyphenVowel) == 0)
            continue;
        firstVowel = std::min(firstVowel, i);
        lastVowel = i;
    }

    const HyphenPoints points = patternPoints(std::u16string_view(part.letters, part.size));

    // Слева от места гласная есть, если первая гласная части стоит раньше
    // него; справа — если последняя стоит на нём или позже.
    for (size_t i = 1; i < part.size; ++i) {
        if (((points >> i) & 1) == 0 || firstVowel >= i || lastVowel < i)
            continue;
        hyphens[part.at[i]] = static_cast<uint8_t>(part.size - i);
    }
}

/// Слово абзаца, пока оно собирается.
///
/// Часть размечается, как только кончилась, не дожидаясь конца слова, — а
/// цифра, чужая буква или точка между буквами, найденные позже, стирают уже
/// размеченное («Москвич-412»). Так каждая буква проходится один раз: границы
/// букв считаются по таблице свойств Юникода, и второй проход по слову стоил
/// бы столько же, сколько первый.
struct Word {
    size_t begin = 0;
    bool letterSeen = false;
    bool periodPending = false;   ///< точка после буквы: буква за ней отменит слово
    bool rejected = false;        ///< слово не переносится вовсе
    WordPart part;

    void start(const size_t at) noexcept {
        begin = at;
        letterSeen = false;
        periodPending = false;
        rejected = false;
        part.clear();
    }

    /// Очередная буква слова: `role` — её роль, `base` — основа, `at` — где
    /// она начинается.
    void take(const WordRole role, const char16_t base, const size_t at,
              sta_vector<uint8_t>& hyphens) noexcept {
        if (rejected)
            return;

        switch (role) {
        case WordRole::Letter:
            if (periodPending) {
                reject(at, hyphens);
                return;
            }
            letterSeen = true;

            // Основой, без знака ударения над ней: образцы знаков не знают,
            // и «досто́примечательность» для них — «достопримечательность».
            // Длиннее предела часть не размечается вовсе, а не по первым
            // буквам: места усечённого слова — не места этого.
            if (part.size == kMaxHyphenatedWord) {
                part.tooLong = true;
                return;
            }
            part.softHyphen = part.softHyphen || part.softPending;
            part.letters[part.size] = base;
            part.at[part.size] = at;
            ++part.size;
            return;

        case WordRole::Foreign:
        case WordRole::Digit:
            reject(at, hyphens);
            return;

        case WordRole::Period:
            // Точку между буквами выдаст буква, которая придёт за ней; до
            // первой буквы и после последней точка — пунктуация, не слово.
            periodPending = letterSeen;
            markPart(part, hyphens);
            part.clear();
            return;

        case WordRole::Joiner:
            markPart(part, hyphens);
            part.clear();
            return;

        case WordRole::SoftHyphen:
            part.softPending = part.size != 0;
            return;

        case WordRole::Transparent:
            return;

        case WordRole::Boundary:
            assert(false && "граница кончает слово, а не входит в него");
            return;
        }
    }

    /// Слово кончилось: остаётся его последняя часть.
    void finish(sta_vector<uint8_t>& hyphens) noexcept {
        if (!rejected)
            markPart(part, hyphens);
    }

private:
    void reject(const size_t at, sta_vector<uint8_t>& hyphens) noexcept {
        rejected = true;
        std::fill_n(hyphens.data() + begin, at - begin, uint8_t{0});
    }
};

}  // namespace

HyphenPoints hyphenate(const u16_view checked) noexcept {
    // Дальше — по единицам: переводить их в номера символов проверенность
    // текста не нужна, а слово с чем-либо вне алфавита отпадает целиком.
    return patternPoints(checked.plain());
}

sta_vector<uint8_t> hyphenPointsOf(const u16_view paragraph,
                                   const std::span<const NoteAnchor> notes) {
    const std::u16string_view units = paragraph.plain();
    sta_vector<uint8_t> hyphens(units.size(), uint8_t{0});

    // Знак сноски — слово сам по себе: цифра, приставленная вплотную
    // («маркизу5»), слово не продолжает, а текст знака размечается как всякий
    // другой. Края знаков помечаются в самом ответе — он ещё пуст, и своя
    // память ради них не нужна; пометку снимает последний шаг. Места переноса
    // на краю не встанет: край начинает слово, а перед первой буквой слова
    // места не бывает.
    for (const NoteAnchor& note : notes) {
        assert(note.position + note.length <= units.size() && "знак сноски лежит в тексте абзаца");
        for (const size_t edge : {size_t{note.position}, size_t{note.position} + note.length})
            if (edge < units.size())
                hyphens[edge] = kNoteEdge;
    }

    // Слово — от первой буквы, которая не граница, до первой, которая граница,
    // или до края знака сноски. Идём по буквам, а не по единицам: знак над
    // буквой разделить её не может, и роль решает основа.
    Word word;
    bool inWord = false;
    for (size_t at = 0, next = 0; at < units.size(); at = next) {
        next = unicode::next_grapheme_boundary(units, at);
        const WordRole role = roleOf(units, at, next);
        const bool edge = hyphens[at] == kNoteEdge;

        if (inWord && (role == WordRole::Boundary || edge)) {
            word.finish(hyphens);
            inWord = false;
        }
        if (role == WordRole::Boundary)
            continue;

        if (!inWord) {
            word.start(at);
            inWord = true;
        }
        word.take(role, units[at], at, hyphens);
    }
    if (inWord)
        word.finish(hyphens);

    std::replace(hyphens.begin(), hyphens.end(), kNoteEdge, uint8_t{0});
    return hyphens;
}

}  // namespace bukvitsa::typography
