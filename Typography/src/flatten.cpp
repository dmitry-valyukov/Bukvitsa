// Дерево книги -> хранилище блоков.
//
// Два дела делаются здесь и больше нигде: свёртка пробелов (потому что это
// первое место, где известно, что значат элементы) и перевод текста в UTF-16
// (потому что дальше всё делает DirectWrite). Заодно строятся прогоны позиций
// «символ строки -> позиция в книге»: свёртка рвёт прямое соответствие, а
// закладка обязана указывать на книгу, а не на результат вёрстки.
//
// Абзац собирается прямо в хвосте массивов хранилища, без своего буфера: всё,
// что сборке приходится откатывать, — хвостовые пробелы, знак нулевой длины,
// пустой блок целиком — лежит в хвосте собираемого абзаца, и откат — это
// укоротить массивы. Оттого же открытая сборка всегда одна, и тела сносок
// разворачиваются не по месту знака, а отдельной фазой после потока чтения.

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

// ensure — макрос: с импортом он не приходит.
#include "abi.h"

// Свой заголовок после всех стандартных: он ведёт к импорту модуля книги, а
// стандартный заголовок после импорта MSVC уже не принимает.
#include "bukvitsa/typography/block.h"

import bukvitsa.fb3;
import wxl.core;

namespace bukvitsa::typography {
namespace fb3 = bukvitsa::fb3;

namespace {

constexpr bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

/// Знак сноски, который ставит вёрстка, когда его не поставил автор.
///
/// В FB3 `<note>` сплошь и рядом пуст: в нём только ссылка на тело, а чем её
/// обозначить — дело того, кто набирает. Официальный пример `nightmare_example`
/// именно такой, и без этого знака его сноски были бы недостижимы — щёлкать
/// не по чему.
std::wstring noteMarker(fb3::NoteNumbering numbering, uint32_t number) {
    switch (numbering) {
    case fb3::NoteNumbering::Asterisk:
        // Звёздочки растут числом, а не значением: *, **, ***. Дальше третьей
        // это нечитаемо, поэтому дальше — цифры.
        if (number <= 3)
            return std::wstring(number, L'*');
        break;

    case fb3::NoteNumbering::Alpha:
        if (number >= 1 && number <= 26)
            return std::wstring(1, static_cast<wchar_t>(L'a' + number - 1));
        break;

    case fb3::NoteNumbering::Roman: {
        static constexpr std::pair<uint32_t, const wchar_t*> kRoman[] = {
            {1000, L"m"}, {900, L"cm"}, {500, L"d"}, {400, L"cd"}, {100, L"c"}, {90, L"xc"},
            {50, L"l"},   {40, L"xl"},  {10, L"x"},  {9, L"ix"},   {5, L"v"},   {4, L"iv"},
            {1, L"i"},
        };

        std::wstring result;
        uint32_t rest = number;
        for (const auto& [value, sign] : kRoman)
            while (rest >= value) {
                result += sign;
                rest -= value;
            }
        return result.empty() ? std::to_wstring(number) : result;
    }

    default:
        break;
    }

    // Арабские — и они же запасной вариант для всего, что не уложилось: сноска
    // без знака недостижима, а это хуже неточного знака.
    return std::to_wstring(number);
}

/// Есть ли среди прямых детей блочный узел. Определяет, разворачивать ли узел
/// как контейнер или как абзац: схема FB3 не даёт блок внутри `<p>`, но `<li>`
/// и ячейка таблицы бывают и с текстом, и с абзацами внутри.
bool hasBlockChild(const fb3::Node& node) {
    for (const fb3::Node& child : node.children())
        if (!fb3::isInline(child.kind()))
            return true;
    return false;
}

/// Размер массива хранилища как индекс в нём: диапазоны и позиции — uint32_t.
template <typename Array>
uint32_t sizeOf(const Array& array) {
    return static_cast<uint32_t>(array.size());
}

class Flattener {
public:
    BookText run(const fb3::Document& document) {
        reserve(document);
        noteBodyCount_ = document.noteBodies().size();

        walk(document.body(), Context{});
        finishBlock();
        book_.flowCount = sizeOf(book_.blocks);

        // Тела сносок — каждое своими блоками, в порядке документа, следом за
        // потоком. Глав они не открывают: метка секции, оставшаяся от конца
        // потока, им не достаётся, а секции внутри тела меток не ставят.
        pendingSection_ = 0;
        inFlow_ = false;

        for (const fb3::Node* body : document.noteBodies()) {
            // Список тел заполняет разбор FB3 адресами узлов, которые сам же
            // завёл, — нуль там ошибка разбора, не книги. Ничего в памяти эта
            // проверка не стережёт: разыменование нуля остановит и без неё.
            assert(body != nullptr && "тело сноски — узел документа");

            const uint32_t first = sizeOf(book_.blocks);
            walkChildren(*body, Context{});
            finishBlock();
            book_.noteBodies.push_back(Range{first, sizeOf(book_.blocks) - first});
        }

        // Резерв взят по оценке: где она вышла с запасом, массив отдаёт
        // излишек, и дальше хранилище не растёт.
        book_.text.shrink_to_fit();
        book_.spans.shrink_to_fit();
        book_.notes.shrink_to_fit();
        book_.offsets.shrink_to_fit();
        book_.blocks.shrink_to_fit();
        book_.noteBodies.shrink_to_fit();

        return std::move(book_);
    }

private:
    /// Что наследуется вниз по дереву: во что превращать абзац, какого уровня
    /// заголовок, на какой глубине список.
    struct Context {
        BlockKind paragraphKind = BlockKind::Paragraph;
        uint8_t level = 0;
        uint8_t listDepth = 0;
    };

    BookText book_;

    /// Блок, который собирается: его свойства и начала диапазонов. Текст и
    /// прогоны лежат уже в хвосте массивов книги, а число элементов каждого
    /// диапазона ставит finishBlock.
    Block block_;

    bool open_ = false;
    bool preformatted_ = false;
    bool pendingSpace_ = false;      ///< свёрнутый пробел, ещё не записанный
    uint32_t pendingSpaceAt_ = 0;
    std::vector<FontStyle> styles_{FontStyle{}};

    /// Номер текущего блока. Нужен тем, кто уходит вглубь дерева и хочет
    /// вернуться к блоку, с которого начал: блок мог смениться под ними.
    uint64_t blockSerial_ = 0;

    /// Сквозной номер сноски по книге. Не по секции: секция в FB3 бывает
    /// вложена во что угодно, и «сноска 3» в двух местах книги смутила бы
    /// сильнее, чем «сноска 37».
    uint32_t noteNumber_ = 0;

    /// Глубина секции, открытой с прошлого положенного блока и ещё не
    /// отмеченной. Её понесёт первый же следующий блок (`markSectionStart`):
    /// вход в секцию сам блока не создаёт, а метка нужна на первом блоке
    /// секции, каким бы путём тот ни попал в список.
    uint8_t pendingSection_ = 0;

    /// Идёт ли поток чтения. За ним — тела сносок, а их секции глав не
    /// открывают: метка там значила бы новую страницу посреди сноски.
    bool inFlow_ = true;

    /// Сколько тел сносок у документа: номер тела в ссылке — индекс в их списке.
    size_t noteBodyCount_ = 0;

    /// Где в тексте блока начинаются буквы. Прогон стиля и знак сноски,
    /// поставленный вёрсткой, начинаются только с буквы: разметка внутри
    /// буквы (`и<em>◌̆</em>`) иначе отдала бы знак шейперу отдельным прогоном,
    /// без основы.
    unicode::grapheme_breaker letters_;

    /// Знак сноски, который ставит вёрстка, ждёт начала следующей буквы:
    /// `<note>` мог встать между буквой и её знаком, и знак сноски там
    /// разорвал бы букву.
    struct PendingMarker {
        /// Индекс якоря в BookText::notes. Пусто у битой ссылки: знак она
        /// получает, а якоря у неё нет.
        std::optional<size_t> anchor;
        std::wstring text;
        FontStyle style;
        uint32_t charOffset = 0;
    };
    std::vector<PendingMarker> pendingMarkers_;

    /// Якорь знака сноски, чей текст начнётся с первой же буквы. С неё
    /// заводится новый прогон стиля, даже если начертание то же, что у
    /// прежней, и с неё же якорь отсчитывает своё начало. Место знака на
    /// строке — объединение его прогонов, и чужой текст в них — `<sup>`
    /// вплотную или соседний знак — растянул бы это место на себя: щелчок по
    /// чужому открывал бы эту сноску. А начало — по букве, а не по входу в
    /// `<note>`: знак, поставленный вёрсткой для пустого `<note/>` перед ним,
    /// встаёт перед этой же буквой, и якорь, отсчитанный от входа, накрыл бы
    /// и его; так же и знак внутри знака, стоящий до его первой буквы. Съедает
    /// якорь первая же буква; блок, закрывшийся раньше неё, уносит его с собой
    /// (openBlock).
    std::optional<size_t> openingAnchor_;

    /* ---------------- хранилище ---------------- */

    /// Резерв по мерам разбора. Это оценка, а не граница: символ вне BMP —
    /// две единицы, знак сноски, поставленный вёрсткой, добавляет свои цифры,
    /// а абзацем узел становится не всегда. Где её не хватит, массив растёт.
    void reserve(const fb3::Document& document) {
        const fb3::Measures measures = document.measures();

        // Картинка дважды: блоком сама и разрезом абзаца, в котором стоит, —
        // его продолжение за ней ещё один блок.
        const size_t blocks = size_t{measures.paragraphs} + measures.separators +
                              2 * size_t{measures.images};

        book_.text.reserve(measures.codePoints);
        book_.blocks.reserve(blocks);

        // Прогон стиля на абзац, и стилевой элемент добавляет два: свой и
        // продолжение прежнего начертания за ним.
        book_.spans.reserve(size_t{measures.paragraphs} + 2 * size_t{measures.inlineStyles});
        book_.notes.reserve(measures.noteRefs);

        // Прогон позиций на блок, и знак сноски добавляет до трёх: цифры знака
        // стоят на одной позиции, а текст за ним возвращается к своей.
        book_.offsets.reserve(blocks + 3 * size_t{measures.noteRefs});
        book_.noteBodies.reserve(document.noteBodies().size());
    }

    /// Длина текста собираемого абзаца: его текст — хвост общего.
    uint32_t length() const { return sizeOf(book_.text) - block_.paragraph.text.first; }

    /// Пустой абзац на конце массивов — то, с чего начинается всякий блок.
    /// У картинки и разделителя он таким и остаётся: начала их диапазонов
    /// стоят там же, где у соседей, и блоки подряд смотрят в массивы подряд.
    Paragraph tail() const {
        return Paragraph{Range{sizeOf(book_.text), 0}, Range{sizeOf(book_.spans), 0},
                         Range{sizeOf(book_.notes), 0}, Range{sizeOf(book_.offsets), 0}};
    }

    /* ---------------- блоки ---------------- */

    void beginBlock(BlockKind kind, const fb3::Node& source, const Context& context) {
        finishBlock();
        openBlock(kind, source.charOffset(), context.level, context.listDepth);
    }

    /// Открывает сборку блока в хвосте массивов. Отдельно от beginBlock, потому
    /// что продолжение абзаца за картинкой открывается тем же путём, но
    /// закрывать перед ним уже нечего.
    void openBlock(BlockKind kind, uint32_t charOffset, uint8_t level, uint8_t listDepth) {
        block_ = Block{};
        block_.kind = kind;
        block_.level = level;
        block_.listDepth = listDepth;
        block_.charOffset = charOffset;
        block_.paragraph = tail();

        open_ = true;
        preformatted_ = kind == BlockKind::Preformatted;
        pendingSpace_ = false;
        letters_ = {};
        ++blockSerial_;

        // Якорь, ждавший буквы, остался в закрытом блоке: тот выбросил его
        // вместе с нулевой длиной (cutTail), и номер больше ни на что не
        // указывает.
        openingAnchor_.reset();
    }

    void finishBlock() {
        if (!open_) return;

        // Буква, после которой ждали знаки сносок, кончилась вместе с блоком.
        flushMarkers();

        open_ = false;
        pendingSpace_ = false;

        // Хвостовой пробел абзаца не значит ничего: он появился от того, как
        // файл разложен по строкам. В преформате значимо всё.
        Paragraph& paragraph = block_.paragraph;
        uint32_t kept = length();
        if (!preformatted_) {
            const std::u16string_view text =
                u16_view(book_.text).substr(paragraph.text.first).plain();
            kept = static_cast<uint32_t>(text.find_last_not_of(u' ') + 1);
        }

        // Блок без текста не кладётся: всё, что он успел записать, снимается,
        // и его метку секции понесёт следующий.
        if (kept == 0) {
            rewind();
            return;
        }

        cutTail(kept);

        paragraph.text.count = kept;
        paragraph.spans.count = sizeOf(book_.spans) - paragraph.spans.first;
        paragraph.notes.count = sizeOf(book_.notes) - paragraph.notes.first;
        paragraph.offsets.count = sizeOf(book_.offsets) - paragraph.offsets.first;

        markSectionStart(block_);
        book_.blocks.push_back(block_);
    }

    /// Возвращает все массивы туда, где начался собираемый блок.
    void rewind() {
        const Paragraph& paragraph = block_.paragraph;
        book_.text.erase(paragraph.text.first);
        book_.spans.resize(paragraph.spans.first);
        book_.notes.resize(paragraph.notes.first);
        book_.offsets.resize(paragraph.offsets.first);
    }

    /// Укорачивает собираемый абзац до kept единиц: то, что начиналось за
    /// ними, уходит, то, что через них переходит, укорачивается. Всё это —
    /// хвост массивов: прогоны и якоря идут по возрастанию начала.
    void cutTail(uint32_t kept) {
        const Paragraph& paragraph = block_.paragraph;
        book_.text.erase(paragraph.text.first + kept);

        while (sizeOf(book_.spans) > paragraph.spans.first && book_.spans.back().start >= kept)
            book_.spans.pop_back();

        // Прогоны стиля — разбиение текста, поэтому через обрезку переходит
        // разве что последний.
        if (sizeOf(book_.spans) > paragraph.spans.first) {
            StyleSpan& last = book_.spans.back();
            last.length = std::min(last.length, kept - last.start);
        }

        while (sizeOf(book_.offsets) > paragraph.offsets.first &&
               book_.offsets.back().start >= kept)
            book_.offsets.pop_back();

        // Знак нулевой длины ничего не занимает на строке, значит по нему
        // нельзя щёлкнуть, значит его нет. Такой остаётся от знака, чей блок
        // сменился, пока знак набирался, и стоит он не обязательно последним.
        const auto first = book_.notes.begin() + paragraph.notes.first;
        for (auto note = first; note != book_.notes.end(); ++note)
            if (note->position < kept)
                note->length = std::min(note->length, kept - note->position);

        book_.notes.erase(std::remove_if(first, book_.notes.end(),
                                         [&](const NoteAnchor& note) {
                                             return note.position >= kept || note.length == 0;
                                         }),
                          book_.notes.end());
    }

    /// Помечает блок началом секции, если с прошлого положенного блока в
    /// дерево вошла секция. Метку несёт первый же блок после входа, каким бы
    /// путём тот ни пришёл; пустой блок, выброшенный finishBlock, метку не
    /// тратит — до push_back дело у него не доходит.
    void markSectionStart(Block& block) {
        block.startsSection = pendingSection_;
        pendingSection_ = 0;
    }

    /// Блок без текста — картинка или разделитель — пишется сразу.
    void emitStandalone(Block block) {
        finishBlock();
        block.paragraph = tail();
        markSectionStart(block);
        book_.blocks.push_back(block);
    }

    /* ---------------- текст ---------------- */

    /// Прогоны стиля образуют разбиение текста, а не вложенные интервалы:
    /// новый прогон начинается там, где сменилось начертание, и там, где
    /// начинается знак сноски с якорем (`openingAnchor_`). Так шейперу
    /// не приходится разбирать перекрытия, а `<strong><em>` — это просто
    /// прогон, у которого стоят оба флага.
    ///
    /// Но только с начала буквы: продолжение буквы, размеченное иначе, берёт
    /// начертание её начала — буква набирается одним прогоном.
    ///
    /// Всё, что попадает в текст блока, идёт сюда: автомат букв должен
    /// увидеть каждый символ, иначе его границы разойдутся с текстом.
    void appendCodePoint(char32_t code, uint32_t charOffset) {
        // Знаки сносок встают перед буквой, которая начинается здесь. Проба —
        // на копии автомата: сам он должен увидеть сперва знаки, потом символ.
        if (!pendingMarkers_.empty()) {
            unicode::grapheme_breaker probe = letters_;
            if (probe.breaks_before(code))
                flushMarkers();
        }

        const bool startsLetter = letters_.breaks_before(code);
        const uint32_t at = length();

        if (sizeOf(book_.spans) == block_.paragraph.spans.first ||
            (startsLetter && (openingAnchor_ || !(book_.spans.back().style == styles_.back()))))
            book_.spans.push_back(StyleSpan{at, 0, styles_.back()});
        if (startsLetter && openingAnchor_) {
            book_.notes[*openingAnchor_].position = at;
            openingAnchor_.reset();
        }

        book_.text.push_back(code);
        const uint32_t units = length() - at;
        book_.spans.back().length += units;

        // Символ вне BMP — пара единиц, и позицию в книге несут обе: вторая
        // половина заводит свой прогон, потому что арифметика от первой дала
        // бы ей следующую позицию.
        for (uint32_t unit = 0; unit < units; ++unit)
            placeOffset(at + unit, charOffset);
    }

    /// Позиция единицы at в книге. Новый прогон — только там, где арифметика
    /// от последнего прогона абзаца её не даёт: после свёрнутых пробелов, на
    /// цифрах знака сноски, на второй половине пары.
    void placeOffset(uint32_t at, uint32_t charOffset) {
        if (sizeOf(book_.offsets) > block_.paragraph.offsets.first) {
            const OffsetSpan& last = book_.offsets.back();
            if (last.charOffset + (at - last.start) == charOffset)
                return;
        }
        book_.offsets.push_back(OffsetSpan{at, charOffset});
    }

    void flushMarkers() {
        std::vector<PendingMarker> markers = std::move(pendingMarkers_);
        pendingMarkers_.clear();

        // Знаки встают перед буквой, которую набирает зовущий; якорь, который
        // ждёт её, — его, и после знаков он должен остаться как был: начало
        // ему отсчитает та буква, уже за знаками.
        const std::optional<size_t> callerAnchor = openingAnchor_;

        for (const PendingMarker& marker : markers) {
            const uint32_t position = length();
            if (marker.anchor) book_.notes[*marker.anchor].position = position;

            styles_.push_back(marker.style);
            openingAnchor_ = marker.anchor;
            for (const wchar_t sign : marker.text)
                appendCodePoint(sign, marker.charOffset);
            openingAnchor_.reset();
            styles_.pop_back();

            if (marker.anchor) {
                NoteAnchor& note = book_.notes[*marker.anchor];
                note.length = length() - note.position;
                markNote(note.position);
            }
        }

        openingAnchor_ = callerAnchor;
    }

    /// Роль знака — прогонам знака, чей якорь жив: его длина записана в том же
    /// блоке, где он начат. Роль ставится здесь, а не стилем на входе в
    /// `<note>`: якорь, чей блок сменился, пока знак набирался (картинка внутри
    /// знака), выбрасывается, и роль его тексту тогда не достаётся вовсе — по
    /// роли знак красят, а щёлкнуть по нему было бы нечем. Прогоны знака —
    /// хвост прогонов блока: с начала знака там разрез (`openingAnchor_`), а
    /// за знаком ещё ничего не набрано.
    void markNote(uint32_t position) {
        for (size_t at = book_.spans.size(); at > block_.paragraph.spans.first; --at) {
            StyleSpan& span = book_.spans[at - 1];
            if (span.start < position) break;
            span.style.role = TextRole::NoteMark;
        }
    }

    /// Прикреплён ли символ к пробелу перед ним. Знак ударения после пробела —
    /// буква из двух символов, и пробел в ней основа: выбросить его значит
    /// оставить знак без буквы.
    static bool extendsSpace(char32_t code) {
        unicode::grapheme_breaker letters;
        letters.breaks_before(U' ');
        return !letters.breaks_before(code);
    }

    /// Свёрнутый пробел, за которым идёт не символ текста, а элемент.
    void flushPendingSpace() { appendPendingSpace(false); }

    /// Свёрнутый пробел перед символом `next`.
    void flushPendingSpace(char32_t next) {
        if (pendingSpace_) appendPendingSpace(extendsSpace(next));
    }

    void appendPendingSpace(bool isLetterBase) {
        if (!pendingSpace_) return;
        pendingSpace_ = false;

        if (length() == 0) {
            // В начале абзаца свёрнутый пробел исчезает вовсе — если он не
            // основа буквы.
            if (!isLetterBase)
                return;
        } else {
            // И не удваивается: между двумя текстовыми узлами бывает
            // инлайновый элемент, который сам ничего не написал (пустая
            // ссылка, знак сноски без текста), — и тогда пробелы по обе
            // стороны от него схлопываются здесь, а не остаются двумя. После
            // перевода строки пробел, который служит основой буквы, остаётся.
            const char16_t last = book_.text.plain().back();
            if (last == u' ' || (last == u'\n' && !isLetterBase))
                return;
        }

        appendCodePoint(U' ', pendingSpaceAt_);
    }

    void appendText(u8_view utf8, uint32_t firstCharOffset) {
        uint32_t offset = firstCharOffset;

        // Текст пришёл проверенным: wxl.xml проверяет документ целиком, прежде
        // чем его разбирать, и её дерево — а за ним и модель книги — отдаёт
        // u8_view. Обход кодовых точек берёт этот довод готовым и не
        // спрашивает заново на каждом байте.
        for (const char32_t code : unicode::code_points(utf8)) {
            if (!preformatted_ && code < 0x80u && isSpace(static_cast<char>(code))) {
                if (!pendingSpace_) {
                    pendingSpace_ = true;
                    pendingSpaceAt_ = offset;
                }
            } else {
                flushPendingSpace(code);
                appendCodePoint(code, offset);
            }

            ++offset;
        }
    }

    /* ---------------- стили ---------------- */

    void pushStyle(void (*apply)(FontStyle&)) {
        FontStyle style = styles_.back();
        apply(style);
        styles_.push_back(style);
    }

    void popStyle() { styles_.pop_back(); }

    /* ---------------- обход ---------------- */

    void walkChildren(const fb3::Node& node, Context context) {
        for (const fb3::Node& child : node.children())
            walk(child, context);
    }

    void inlineChildren(const fb3::Node& node) {
        Context inlineContext;
        for (const fb3::Node& child : node.children())
            walkInline(child, inlineContext);
    }

    void walkInline(const fb3::Node& node, const Context& context) {
        using fb3::NodeKind;

        switch (node.kind()) {
        case NodeKind::Text:
            appendText(node.text(), node.charOffset());
            return;

        case NodeKind::Strong:  pushStyle([](FontStyle& s) { s.bold = true; }); break;
        case NodeKind::Emphasis: pushStyle([](FontStyle& s) { s.italic = true; }); break;
        case NodeKind::Strikethrough: pushStyle([](FontStyle& s) { s.strikethrough = true; }); break;
        case NodeKind::Underline: pushStyle([](FontStyle& s) { s.underline = true; }); break;
        case NodeKind::Superscript: pushStyle([](FontStyle& s) { s.script = 1; }); break;
        case NodeKind::Subscript: pushStyle([](FontStyle& s) { s.script = -1; }); break;
        case NodeKind::Code: pushStyle([](FontStyle& s) { s.monospace = true; }); break;
        case NodeKind::SmallCaps: pushStyle([](FontStyle& s) { s.smallCaps = true; }); break;
        case NodeKind::Spacing: pushStyle([](FontStyle& s) { s.spaced = true; }); break;

        case NodeKind::NoteRef: {
            // Знак сноски набирается надстрочным, а место запоминается: по нему
            // читалка потом ловит щелчок и показывает тело сноски. Якорь — только
            // у ссылки, за которой есть тело: по битой показать нечего. Знак она
            // всё равно получает — где автор сослался, видно и так, — но без
            // роли знака: щелчка он не держит.
            flushPendingSpace();

            const fb3::NoteRefData* data = node.noteRef();
            const uint64_t serial = blockSerial_;

            std::optional<size_t> anchor;
            if (data && data->body) {
                // Из файла у ссылки только id; номер по нему ставит разбор FB3,
                // а ссылку на id, которого нет, оставляет без номера — она и
                // идёт битой, знаком без якоря. Номер за списком — ошибка
                // разбора, но читатель якоря берёт по нему noteBodies[body] без
                // проверки, поэтому граница стережётся во всех сборках: раз на
                // ссылку, даром.
                ensure(*data->body < noteBodyCount_);
                // Начало якорю ставит первая буква знака (openingAnchor_), а
                // пустому — знак вёрстки (flushMarkers); вход остаётся началом
                // только у знака без единого начала буквы.
                anchor = book_.notes.size();
                book_.notes.push_back(NoteAnchor{length(), 0, *data->body});
            }

            pushStyle([](FontStyle& s) { s.script = 1; });

            // Знак с якорем начинает свой прогон стиля, каким бы ни было
            // начертание перед ним, и с его первой буквы якорь отсчитывает
            // начало (openingAnchor_); роль знака его прогоны получат, когда
            // станет известна длина (markNote). Якорь, не взятый буквой, —
            // знак без своего текста — снимается: знак, поставленный
            // вёрсткой, возьмёт его сам (flushMarkers).
            //
            // Якорь внешнего знака, ещё ждущий своей первой буквы, — не наш:
            // этот знак стоит внутри него до его первой буквы, и буква этого
            // знака внешнему не своя. После этого знака якорь внешнего
            // возвращается, как и после знаков вёрстки, — если блок тот же:
            // сменившийся блок унёс его с собой (openBlock).
            const std::optional<size_t> outerAnchor = openingAnchor_;
            openingAnchor_ = anchor;
            const uint32_t before = length();
            for (const fb3::Node& child : node.children())
                walkInline(child, context);
            openingAnchor_.reset();
            if (serial == blockSerial_) openingAnchor_ = outerAnchor;

            // Автор знака не поставил — ставим сами и нумеруем по порядку. Но
            // не сразу, а перед следующей буквой: `<note>` мог встать между
            // буквой и её знаком. Место и длину знака запишет flushMarkers.
            if (serial == blockSerial_ && length() == before) {
                pendingMarkers_.push_back(PendingMarker{
                    anchor,
                    noteMarker(data ? data->numbering : fb3::NoteNumbering::Arabic, ++noteNumber_),
                    styles_.back(), node.charOffset()});
            }

            popStyle();

            // Длина знака известна только теперь: её набрали дети. Но пока они
            // набирались, блок мог смениться — картинка внутри знака сноски
            // закрывает абзац и начинает новый, — и тогда записывать длину
            // некуда: тот абзац уже лёг в хранилище, а якорь без длины
            // finishBlock выбросил вместе с ним.
            if (anchor && serial == blockSerial_) {
                NoteAnchor& note = book_.notes[*anchor];
                const uint32_t end = length();
                note.length = end > note.position ? end - note.position : 0;
                markNote(note.position);
            }
            return;
        }

        case NodeKind::Image:
            // Картинка внутри абзаца разрывает его: v1 не умеет обтекания,
            // и честнее показать её отдельной строкой, чем потерять.
            emitImage(node);
            return;

        case NodeKind::Break:
            flushPendingSpace();
            appendCodePoint(U'\n', node.charOffset());
            return;

        default:
            // Ссылка, span и всё, чего вёрстка v1 не различает: содержимое
            // остаётся, оформление не меняется. Роли у ссылки нет: перехода по
            // ней читалка не делает, и цвет ссылки обещал бы то, чего нет.
            for (const fb3::Node& child : node.children())
                walkInline(child, context);
            return;
        }

        for (const fb3::Node& child : node.children())
            walkInline(child, context);
        popStyle();
    }

    void emitImage(const fb3::Node& node) {
        Block image;
        image.kind = BlockKind::Image;
        image.charOffset = node.charOffset();
        if (const fb3::ImageData* data = node.image())
            image.imageIndex = data->partIndex;

        // finishBlock() выбрасывает пустой абзац, поэтому картинка между двумя
        // кусками текста не оставляет за собой пустой строки. Продолжению
        // достаются только свойства блока: текст до картинки уже лёг в
        // хранилище своим блоком.
        const BlockKind kind = block_.kind;
        const uint8_t level = block_.level;
        const uint8_t listDepth = block_.listDepth;
        const bool wasOpen = open_;

        emitStandalone(image);

        if (wasOpen)
            openBlock(kind, node.charOffset(), level, listDepth);
    }

    /// Узел, у которого нет блочных детей, целиком становится одним блоком.
    void emitParagraph(const fb3::Node& node, BlockKind kind, const Context& context) {
        beginBlock(kind, node, context);
        styles_.assign(1, FontStyle{});
        inlineChildren(node);
        finishBlock();
    }

    void walk(const fb3::Node& node, Context context) {
        using fb3::NodeKind;

        switch (node.kind()) {
        case NodeKind::Body:
            walkChildren(node, context);
            return;

        case NodeKind::Section: {
            const auto depth = static_cast<uint8_t>(std::min(context.level + 1, 6));
            context.level = depth;
            // Первый блок секции понесёт метку её начала. Открылись секции
            // подряд (вложенная сразу за родителем, без блока между) — метку
            // несёт самая мелкая: глава верхнего уровня важнее своей подсекции.
            // Секция внутри тела сноски главы не открывает.
            if (inFlow_)
                pendingSection_ = pendingSection_ ? std::min(pendingSection_, depth) : depth;
            walkChildren(node, context);
            return;
        }

        case NodeKind::Title:
            context.paragraphKind = BlockKind::Title;
            walkContainerOrParagraph(node, BlockKind::Title, context);
            return;

        case NodeKind::Epigraph:
            context.paragraphKind = BlockKind::Epigraph;
            walkChildren(node, context);
            return;

        case NodeKind::Annotation:
            context.paragraphKind = BlockKind::Annotation;
            walkChildren(node, context);
            return;

        case NodeKind::Subscription:
            context.paragraphKind = BlockKind::Subscription;
            walkContainerOrParagraph(node, BlockKind::Subscription, context);
            return;

        case NodeKind::Blockquote:
            context.paragraphKind = BlockKind::Quote;
            walkChildren(node, context);
            return;

        case NodeKind::Poem:
        case NodeKind::Stanza:
            context.paragraphKind = BlockKind::Verse;
            walkChildren(node, context);
            return;

        case NodeKind::List:
            context.listDepth = static_cast<uint8_t>(std::min(context.listDepth + 1, 6));
            context.paragraphKind = BlockKind::ListItem;
            walkChildren(node, context);
            return;

        case NodeKind::ListItem:
            walkContainerOrParagraph(node, BlockKind::ListItem, context);
            return;

        case NodeKind::Table:
        case NodeKind::TableRow:
            // v1 не верстает таблицы: ячейки идут подряд, как абзацы. Лучше
            // прочесть содержимое не в колонках, чем не прочесть вовсе.
            walkChildren(node, context);
            return;

        case NodeKind::TableCell:
            walkContainerOrParagraph(node, BlockKind::Paragraph, context);
            return;

        case NodeKind::Notes:
            // Блок сносок в поток чтения не идёт. Каждая сноска и так достижима
            // — по своему знаку, который стоит там, где на неё сослались, — а
            // вываленная в текст она читается как случайный кусок из другого
            // места. Тела сносок разворачиваются после потока (run), и якорь
            // знака несёт номер тела.
            return;

        case NodeKind::NoteBody:
        case NodeKind::Div:
            walkChildren(node, context);
            return;

        case NodeKind::Paragraph:
            emitParagraph(node, context.paragraphKind, context);
            return;

        case NodeKind::Subtitle:
            emitParagraph(node, BlockKind::Subtitle, context);
            return;

        case NodeKind::Preformatted:
            emitParagraph(node, BlockKind::Preformatted, context);
            return;

        case NodeKind::Image:
            emitImage(node);
            return;

        case NodeKind::Break:
        case NodeKind::PaperPageBreak: {
            Block separator;
            separator.kind = BlockKind::Separator;
            separator.charOffset = node.charOffset();
            emitStandalone(separator);
            return;
        }

        case NodeKind::Marker:
            return;

        default:
            // Инлайновый узел там, где ждали блочный: бывает у книг, которые
            // писал не редактор, а скрипт. Заворачиваем в абзац.
            if (fb3::isInline(node.kind())) {
                emitParagraph(node, context.paragraphKind, context);
                return;
            }
            walkChildren(node, context);
            return;
        }
    }

    /// Узел вроде `<li>` или ячейки: бывает и с текстом, и с абзацами внутри.
    void walkContainerOrParagraph(const fb3::Node& node, BlockKind kind, Context context) {
        if (hasBlockChild(node)) {
            context.paragraphKind = kind;
            walkChildren(node, context);
        } else {
            emitParagraph(node, kind, context);
        }
    }
};

}  // namespace

BookText flatten(const fb3::Document& document) {
    return Flattener{}.run(document);
}

}  // namespace bukvitsa::typography
