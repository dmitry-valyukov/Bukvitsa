// Тесты модели читалки (bukvitsa.lib): чистые функции над хранилищем книги и
// над кривыми обложек. Как у FB3 и вёрстки, без фреймворка.

#include <objbase.h>
#include <wincodec.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <optional>
#include <semaphore>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Заголовки читалки после всех стандартных: индекс книги ведёт к импорту
// модуля книги, и обложки — перед ним; рабочее место — последним, оно
// импортирует wxl.async.
#include "check.h"

#include "bukvitsa/reader/skins.h"

#include "bukvitsa/reader/book_index.h"
#include "bukvitsa/reader/book_places.h"
#include "bukvitsa/reader/imaging.h"
#include "bukvitsa/reader/shelf_text.h"
#include "imaging_probe.h"
#include "bukvitsa/reader/workspace.h"

import wxl.async;
import wxl.core;

using namespace bukvitsa;
using namespace bukvitsa::reader;
using bukvitsa::reader::tests::check;
using bukvitsa::reader::tests::failures;

namespace {

bool isLetterStart(std::wstring_view text, size_t at) {
    return at >= text.size() || unicode::floor_grapheme_boundary(text, at) == at;
}

/// Доли снимка: точнее тысячной доли высоты мастер всё равно не ставит.
bool aboutEqual(float a, float b) {
    return std::abs(a - b) < 1e-3f;
}

/// Блок с этим текстом — в конец хранилища, собранного тестом: один прогон
/// стиля на весь текст и один прогон позиций, то есть символы стоят в книге
/// подряд с `start`. Поток чтения отмеряет вызывающий (`flowCount`). Текст
/// собран тестом из кусков, поэтому проверяется, как всякий чужой.
void addBlock(typography::BookText& book, std::wstring_view text, uint32_t start = 0,
              typography::BlockKind kind = typography::BlockKind::Paragraph) {
    const u16_view checked = unicode::checked(text).value();
    const auto size = static_cast<uint32_t>(checked.size());

    typography::Block block;
    block.kind = kind;
    block.charOffset = start;
    block.paragraph.text = {static_cast<uint32_t>(book.text.size()), size};
    block.paragraph.spans = {static_cast<uint32_t>(book.spans.size()), 1};
    block.paragraph.notes = {static_cast<uint32_t>(book.notes.size()), 0};
    block.paragraph.offsets = {static_cast<uint32_t>(book.offsets.size()), 1};

    book.text += checked;
    book.spans.push_back({0, size, {}});
    book.offsets.push_back({0, start});
    book.blocks.push_back(block);
}

/// Книга из одного абзаца с этим текстом — весь поток чтения, позиции с нуля.
typography::BookText bookOf(std::wstring_view text) {
    typography::BookText book;
    addBlock(book, text);
    book.flowCount = 1;
    return book;
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

        const typography::BookText book = bookOf(text);
        const u16_text hint = reader::hintAt(book, 0);
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

        const typography::BookText book = bookOf(text);
        const sta_vector<reader::SearchHit> hits = reader::searchBook(book, u"q");

        check(hits.size() == 1, "находка одна");
        if (hits.size() != 1) continue;

        const std::wstring_view piece = withoutEllipses(hits[0].context.wchars());
        const size_t from = text.find(piece);
        check(from != std::wstring::npos && isLetterStart(text, from) &&
                  isLetterStart(text, from + piece.size()),
              "отрывок начинается и кончается между буквами");
    }
}

/// Оглавление не копирует заголовки: его строки живут, пока заполняется
/// вкладка, а текст хранилища — пока открыта книга.
void testContentsBorrowTitles() {
    std::printf("\n=== оглавление ===\n");

    typography::BookText book;
    addBlock(book, L"Глава первая", 0, typography::BlockKind::Title);
    addBlock(book, L"Текст главы.", 12);
    book.flowCount = 2;

    const sta_vector<reader::ContentsEntry> contents = reader::contentsOf(book);
    check(contents.size() == 1, "в оглавлении один заголовок");
    if (contents.size() != 1) return;

    check(contents[0].title == L"Глава первая", "строка оглавления — текст заголовка");
    check(contents[0].title.data() == book.textOf(book.blocks[0].paragraph).data(),
          "заголовок взят из хранилища, а не скопирован");
}

/// Оглавление, поиск и подсказка закладки идут по потоку чтения: тела сносок
/// лежат в хранилище за ним, и ни заголовок тела, ни его слова в них не
/// попадают.
void testIndexKeepsToFlow() {
    std::printf("\n=== оглавление и поиск — по потоку чтения ===\n");

    typography::BookText book;
    addBlock(book, L"Глава первая", 0, typography::BlockKind::Title);
    addBlock(book, L"Текст главы со сноской.", 12);
    book.flowCount = 2;

    // Тело сноски — за потоком, как его кладёт вёрстка: заголовок-номер и абзац.
    const auto body = static_cast<uint32_t>(book.blocks.size());
    addBlock(book, L"1", 100, typography::BlockKind::Title);
    addBlock(book, L"Пояснение к главе.", 101);
    book.noteBodies.push_back({body, 2});

    const sta_vector<reader::ContentsEntry> contents = reader::contentsOf(book);
    check(contents.size() == 1 && contents[0].title == L"Глава первая",
          "заголовок тела сноски в оглавление не входит");

    check(reader::searchBook(book, u"пояснение").empty(), "слова тела сноски поиск не находит");
    const sta_vector<reader::SearchHit> found = reader::searchBook(book, u"главы");
    check(found.size() == 1 && found[0].charOffset == 18, "слова потока — находит, на их месте");

    check(reader::hintAt(book, 150) == L"Текст главы со сноской.",
          "подсказка за концом потока — его последний абзац, а не тело сноски");
}

/// Позиция находки — по прогонам позиций абзаца: после свёрнутого пробела
/// позиция в книге убегает вперёд от индекса в тексте, и находка за ним
/// стоит там, где она в книге, а не в абзаце.
void testSearchPlacesHitsByOffsetRuns() {
    std::printf("\n=== поиск: место находки по прогонам позиций ===\n");

    // Свёртка оставила от четырёх пробелов между словами один: «два» в тексте
    // абзаца на 4, а в книге на 7 — второй прогон.
    typography::BookText book;
    addBlock(book, L"раз два", 0);
    book.offsets.push_back({4, 7});
    ++book.blocks.back().paragraph.offsets.count;
    book.flowCount = 1;

    const sta_vector<reader::SearchHit> before = reader::searchBook(book, u"раз");
    const sta_vector<reader::SearchHit> after = reader::searchBook(book, u"два");
    const sta_vector<reader::SearchHit> inside = reader::searchBook(book, u"ва");
    check(before.size() == 1 && before[0].charOffset == 0, "до скачка — по первому прогону");
    check(after.size() == 1 && after[0].charOffset == 7, "на скачке — начало своего прогона");
    check(inside.size() == 1 && inside[0].charOffset == 8, "внутри прогона — от его начала по индексу");
}

/// Что поиск считает совпадением: регистр не важен, «ё» равна «е»,
/// неразрывный пробел — обычному, а мягкий перенос внутри слова не мешает.
/// Место находки — в тексте абзаца как он есть.
void testSearchMatchesWhatTheReaderMeans() {
    std::printf("\n=== что поиск считает совпадением ===\n");

    // Текст — wchar_t, как его отдаёт вёрстка; запрос — u16_view, как его
    // приносит поле поиска: проверенный текст, литерал проверяется при сборке.
    struct Case {
        const char* what;
        std::wstring_view text;
        u16_view needle;
        uint32_t at;
    };

    const Case found[] = {
        {"регистр", L"Сказал Прометей.", u"прометей", 7},
        {"«ё» в тексте, «е» в запросе", L"Ну, ещё раз.", u"ЕЩЕ", 4},
        {"«е» в тексте, «ё» в запросе", L"еще раз", u"ещё", 0},
        {"мягкий перенос внутри слова", L"О Про\x00ADме\x00ADтее", u"прометее", 2},
        {"неразрывный пробел", L"за 10\x00A0лет", u"10 лет", 3},
    };

    for (const Case& test : found) {
        const typography::BookText book = bookOf(test.text);
        const sta_vector<reader::SearchHit> hits = reader::searchBook(book, test.needle);
        check(hits.size() == 1 && hits[0].charOffset == test.at,
              std::string("находка на своём месте: ") + test.what);
    }

    const typography::BookText hyphen = bookOf(L"Про-метей");
    check(reader::searchBook(hyphen, u"прометей").empty(), "обычный дефис — не мягкий перенос");
}

/// Кромка — два листа с общей точкой на корешке, посередине. Она проходит
/// через каждую точку, в корешке непрерывна с обеих сторон, точки другого
/// листа на лист не влияют, прямая остаётся прямой, а утянутая точка уводит
/// кромку за собой без большого размаха у соседок.
void testEdgeThroughPoints() {
    std::printf("\n=== кромка через точки ===\n");

    // Левый лист прямой, правый задран: излом в корешке, как у настоящего
    // сгиба.
    reader::EdgeCurve kink;
    kink.x = {0.05f, 0.15f, 0.25f, 0.375f, 0.5f, 0.55f, 0.7f, 0.85f, 0.95f};
    kink.y = {0.02f, 0.02f, 0.02f, 0.02f, 0.02f, 0.1f, 0.1f, 0.1f, 0.1f};

    constexpr size_t spine = static_cast<size_t>(reader::EdgeCurve::kSpine);
    constexpr size_t points = static_cast<size_t>(reader::EdgeCurve::kPoints);
    const reader::EdgeSpline bent{kink};
    const float atSpine = kink.x[spine];

    check(bent.at(atSpine) == kink.y[spine], "в корешке — точка корешка");
    check(aboutEqual(bent.at(atSpine - 0.0001f), kink.y[spine]) &&
              aboutEqual(bent.at(atSpine + 0.0001f), kink.y[spine]),
          "кромка подходит к корешку с обеих сторон");
    check(bent.at(0.3f) == 0.02f, "левый лист не знает о точках правого");
    check(bent.at(0.6f) > 0.05f, "правый лист идёт к своим точкам");
    check(bent.at(0.0f) == 0.02f && bent.at(1.0f) == 0.1f,
          "за крайними точками кромка держит их значение");

    // Низ «Брошюры» как он снят с фотографии: точки скачут вверх-вниз.
    reader::EdgeCurve booklet;
    booklet.x = {0.036752604f, 0.15469007f, 0.29072955f, 0.3609435f, 0.5f,
                 0.5485464f,   0.6544158f,  0.7778387f,  0.9478881f};
    booklet.y = {0.9531773f, 0.9319955f, 0.9632107f, 0.8361204f, 0.94147158f,
                 0.89966553f, 0.8573021f, 0.9509476f, 0.9587514f};
    const reader::EdgeSpline edge{booklet};

    bool through = true;
    for (size_t index = 0; index < points; ++index) {
        through = through && edge.at(booklet.x[index]) == booklet.y[index];
    }
    check(through, "кромка проходит через каждую точку");

    // Начальная прямая — прямая и есть, без единого отклонения.
    const reader::Skin fresh = reader::defaultSkin();
    const reader::EdgeSpline flat{fresh.top};
    bool straight = true;
    for (int step = 0; step <= 100; ++step) {
        straight = straight && flat.at(static_cast<float>(step) / 100.0f) == reader::kEdgeInset;
    }
    check(straight, "прямая остаётся прямой");

    // Одна точка «Томика» утянута с 0,025 до 0,11: сплайн перелетает её не
    // выше 0,111, а соседок уводит в противоход не ниже 0,014.
    reader::EdgeCurve pulled = fresh.top;
    pulled.y[3] = 0.11f;
    const reader::EdgeSpline tugged{pulled};
    float lowest = 1.0f;
    float highest = 0.0f;
    for (int step = 0; step <= 1000; ++step) {
        const float value = tugged.at(0.5f * static_cast<float>(step) / 1000.0f);
        lowest = std::min(lowest, value);
        highest = std::max(highest, value);
    }
    check(highest < 0.111f && lowest > 0.014f, "утянутая точка ведёт кромку без большого размаха");
}

/// Обложка, чья верхняя кромка ставлена по правилам мастера — доли снимка,
/// не ниже четверти высоты, зазор между точками не меньше 0,02, — но у
/// правого края сплайн между близкими точками перелетает их и уводит кромку
/// выше верха почти на высоту полосы. Нижняя кромка — начальная прямая.
reader::Skin foldedSkin() {
    reader::Skin folded = reader::defaultSkin();
    folded.top.x = {0.156f, 0.194f, 0.371f, 0.465f, 0.5f, 0.877f, 0.905f, 0.927f, 0.948f};
    folded.top.y = {0.0f, 0.0f, 0.0f, 0.0f, 0.039f, 0.0f, 0.25f, 0.0f, 0.0f};
    return folded;
}

/// Исправило ли чтение реестра обложек ровно одну обложку — эту и вот этим, в
/// этом порядке.
bool correctedOne(const std::optional<std::vector<reader::CorrectedSkin>>& read, std::u16string_view name,
                  std::initializer_list<reader::SkinCorrection> corrections) {
    return read && read->size() == 1 && (*read)[0].name == name &&
           std::ranges::equal((*read)[0].corrections, corrections);
}

/// Точки полосы на одном расстоянии с допуском в тысячную DIP — столько
/// float и даёт на высоте окна; глазом и мышью не видно и в сотни раз больше.
bool aboutSamePoint(reader::PageWarp::Point a, reader::PageWarp::Point b) {
    return std::abs(a.x - b.x) < 1e-3f && std::abs(a.y - b.y) < 1e-3f;
}

/// Изгиб страницы: под ровной обложкой перевод — тождество; кромка, поднятая
/// в столбце на Δ, поднимает там набор у верхнего края ровно на Δ высоты, а
/// опущенная нижняя — опускает у нижнего; знак сдвига тот же, что у карты
/// изгиба; экран и вёрстка переводятся друг в друга туда и обратно — и у
/// корешка с обеих сторон.
void testPageWarp() {
    std::printf("\n=== изгиб страницы ===\n");
    using reader::PageWarp;
    constexpr auto area = PageWarp::Size{1280.0f, 860.0f};

    // Сетка точек полосы: края, середина и корешок с обеих сторон — там у
    // каждого листа своя кривая.
    std::vector<PageWarp::Point> grid;
    for (const float u : {0.0f, 0.1f, 0.3f, 0.45f, 0.4999f, 0.5f, 0.5001f, 0.55f, 0.8f, 1.0f})
        for (const float v : {0.0f, 0.03f, 0.25f, 0.5f, 0.75f, 0.97f, 1.0f})
            grid.push_back(PageWarp::Point{u * area.width, v * area.height});

    // Ровная обложка: гнуть нечего, и обе стороны перевода отдают точку как
    // есть — до последнего разряда.
    auto const straight = PageWarp{reader::defaultSkin()};
    bool identity = straight.flat();
    for (const PageWarp::Point point : grid) {
        const PageWarp::Point screen = straight.toScreen(point, area);
        const PageWarp::Point layout = straight.toLayout(point, area);
        const float shiftHere = straight.shift(point.x / area.width, point.y / area.height);
        identity = identity && screen.x == point.x && screen.y == point.y && layout.x == point.x &&
                   layout.y == point.y && shiftHere == 0.0f;
    }
    check(identity, "ровная обложка — плоская, перевод точки — тождество");

    // Верхняя кромка поднята на Δ в точке левого листа, нижняя опущена на δ
    // в точке правого; остальные точки на прямых — и листы друг другу не
    // мешают.
    constexpr float kRaise = 0.03f;
    constexpr float kDrop = 0.02f;
    constexpr size_t kLeftKnot = 2;
    constexpr size_t kRightKnot = 6;
    reader::Skin bent = reader::defaultSkin();
    bent.top.y[kLeftKnot] = reader::kEdgeInset - kRaise;
    bent.bottom.y[kRightKnot] = 1.0f - reader::kEdgeInset + kDrop;
    auto const warp = PageWarp{bent};
    check(!warp.flat(), "тронутая кромка — изгиб есть");

    const float leftX = bent.top.x[kLeftKnot] * area.width;
    const float rightX = bent.bottom.x[kRightKnot] * area.width;

    // У поднятой верхней кромки экран показывает вёрстку на Δ·H ниже: набор
    // поднят. Сдвиг там отрицательный — как в карте, где у строк, уезжающих
    // вверх, канал выше середины: «взять снизу».
    check(warp.shift(bent.top.x[kLeftKnot], 0.0f) < 0.0f &&
              std::abs(warp.shift(bent.top.x[kLeftKnot], 0.0f) + kRaise) < 1e-6f,
          "сдвиг у поднятой кромки — минус её подъём");
    check(aboutSamePoint(warp.toLayout(PageWarp::Point{leftX, 0.0f}, area),
                         PageWarp::Point{leftX, kRaise * area.height}) &&
              aboutSamePoint(warp.toScreen(PageWarp::Point{leftX, kRaise * area.height}, area),
                             PageWarp::Point{leftX, 0.0f}),
          "поднятая на Δ верхняя кромка поднимает набор у верхнего края на Δ·H");
    check(aboutSamePoint(warp.toLayout(PageWarp::Point{leftX, area.height}, area),
                         PageWarp::Point{leftX, area.height}),
          "нижний край того же столбца, где нижняя кромка прямая, — на месте");

    // У опущенной нижней — вёрстка на δ·H выше: набор опущен.
    auto const lowered = PageWarp::Point{rightX, area.height - kDrop * area.height};
    check(warp.shift(bent.bottom.x[kRightKnot], 1.0f) > 0.0f &&
              aboutSamePoint(warp.toLayout(PageWarp::Point{rightX, area.height}, area), lowered) &&
              aboutSamePoint(warp.toScreen(lowered, area), PageWarp::Point{rightX, area.height}),
          "опущенная на δ нижняя кромка опускает набор у нижнего края на δ·H");
    check(aboutSamePoint(warp.toLayout(PageWarp::Point{rightX, 0.0f}, area),
                         PageWarp::Point{rightX, 0.0f}),
          "верхний край того же столбца, где верхняя кромка прямая, — на месте");

    // Туда и обратно — на сетке, с этой кромкой и с «Брошюрой», у которой
    // изгиб у корешка в десятки DIP. По горизонтали изгиб не двигает ничего.
    const auto roundTrips = [&](const PageWarp& pageWarp) {
        bool back = true;
        for (const PageWarp::Point point : grid) {
            const PageWarp::Point screen = pageWarp.toScreen(point, area);
            back = back && screen.x == point.x &&
                   aboutSamePoint(pageWarp.toLayout(screen, area), point) &&
                   aboutSamePoint(pageWarp.toScreen(pageWarp.toLayout(point, area), area), point);
        }
        return back;
    };
    check(roundTrips(warp), "вёрстка → экран → вёрстка и обратно — та же точка");

    const reader::Skin* booklet = nullptr;
    for (const reader::Skin& system : reader::systemSkins())
        if (system.name == u"Брошюра") booklet = &system;
    check(booklet != nullptr, "среди системных обложек есть «Брошюра»");
    if (!booklet) return;

    auto const bookletWarp = PageWarp{*booklet};
    check(!bookletWarp.flat() && roundTrips(bookletWarp),
          "«Брошюра»: туда и обратно — та же точка, у корешка с обеих сторон тоже");

    // Столбцы не выворачивает ни одна системная обложка и ни одна кромка выше.
    bool systemKeep = straight.keepsColumns() && warp.keepsColumns();
    for (const reader::Skin& system : reader::systemSkins())
        systemKeep = systemKeep && PageWarp{system}.keepsColumns();
    check(systemKeep, "системные обложки и тронутая кромка столбцов не выворачивают");

    // Точки — как ставит мастер: доли снимка, верхняя до четверти высоты, с
    // зазором не меньше 0,02. Но у правого края кромка между близкими точками
    // перелетает их выше верха почти на высоту полосы.
    check(!PageWarp{foldedSkin()}.keepsColumns(),
          "кромка, перелетевшая полосу между точками, выворачивает столбец");
}

/// Бюджет памяти из заголовка — в байтах, по формату кадра, — и уменьшение
/// до предела — на краях: квадрат 16384² в 32 бита ровно в бюджете, на
/// пиксель шире — нет; снимок на 200 Мп в 24 и 32 бита — в бюджете, в 48 —
/// нет; формат уже 32 бит считается за 32; 16384² в 64 и 128 бит, 65535² и
/// стороны в предел uint32_t — нет, без переполнения; нулевая сторона — нет.
/// Большая сторона уменьшенного — ровно предел, меньшая — целочисленно и не
/// меньше пикселя, и при сторонах, чьё произведение не влезает в uint32_t;
/// не больше предела — как есть, без увеличения.
void testPictureBudget() {
    std::printf("\n=== бюджет картинки ===\n");

    check(reader::withinPictureBudget(16384, 16384, 32), "16384×16384 в 32 бита — ровно в бюджете");
    check(!reader::withinPictureBudget(16385, 16384, 32) && !reader::withinPictureBudget(16384, 16385, 32),
          "на пиксель больше — за бюджетом");
    check(reader::withinPictureBudget(16320, 12240, 24) && reader::withinPictureBudget(16320, 12240, 32),
          "снимок на 200 Мп в 24 и 32 бита — в бюджете");
    check(!reader::withinPictureBudget(16320, 12240, 48), "снимок на 200 Мп в 48 бит — за бюджетом");
    check(reader::withinPictureBudget(16384, 16384, 1) && !reader::withinPictureBudget(16385, 16384, 1),
          "1 бит на пиксель считается за 32: декодер отдаёт PBGRA");
    check(!reader::withinPictureBudget(16384, 16384, 64) && !reader::withinPictureBudget(16384, 16384, 128) &&
              reader::withinPictureBudget(8192, 8192, 128),
          "16384² в 64 и 128 бит — за бюджетом, 8192² в 128 — ровно в нём");
    check(!reader::withinPictureBudget(65535, 65535, 1) &&
              !reader::withinPictureBudget(0xFFFFFFFFu, 0xFFFFFFFFu, 128),
          "65535×65535 и стороны в предел uint32_t — за бюджетом, без переполнения");
    check(!reader::withinPictureBudget(0, 100, 32) && !reader::withinPictureBudget(100, 0, 32),
          "нулевая сторона — не картинка");
    check(reader::withinPictureBudget(1, 1, 32) && reader::withinPictureBudget(268435456, 1, 32) &&
              !reader::withinPictureBudget(268435457, 1, 32),
          "полоса в пиксель в 32 бита — до 2²⁸ включительно");

    const auto same = [](reader::PictureSize size, uint32_t width, uint32_t height) {
        return size.width == width && size.height == height;
    };
    check(same(reader::fittedSize(16320, 12240, 4096), 4096, 3072), "200 Мп до 4096 — 4096×3072");
    check(same(reader::fittedSize(12240, 16320, 4096), 3072, 4096), "стоя — так же, большая сторона — предел");
    check(same(reader::fittedSize(4097, 4097, 4096), 4096, 4096), "квадрат на пиксель больше — ровно предел");
    check(same(reader::fittedSize(100000, 1, 4096), 4096, 1), "полоса — меньшая сторона не меньше пикселя");
    check(same(reader::fittedSize(0xFFFFFFFFu, 0xFFFFFFFEu, 1), 1, 1), "предел в пиксель и огромные стороны — 1×1");
    check(same(reader::fittedSize(0xFFFFFFFFu, 0x7FFFFFFFu, 4096), 4096, 2047),
          "стороны, чьё произведение не влезает в uint32_t, — целочисленно в uint64_t");
    check(same(reader::fittedSize(100, 50, 4096), 100, 50) && same(reader::fittedSize(4096, 4096, 4096), 4096, 4096) &&
              same(reader::fittedSize(4096, 1, 4096), 4096, 1),
          "не больше предела — как есть, без увеличения");
}

/// Реестр второй версии — листы порознь, по пять точек, — читается в кривые
/// из девяти: две точки у корешка становятся одной, на середине и высотой
/// посередине между ними. Записывается уже третья версия, и она читается
/// назад той же; корешок при чтении встаёт на середину, где бы ни стоял.
void testSkinsReadSeparateLeaves() {
    std::printf("\n=== реестр обложек ===\n");

    // Точки у корешка нарочно не симметричны середине: среднее их X — 0,495.
    const std::string old = R"(<?xml version="1.0" encoding="utf-8"?>
<skins version="2">
  <skin name="Старая" image="old.png">
    <topLeft>
      <point x="0.05" y="0.01"/>
      <point x="0.15" y="0.012"/>
      <point x="0.25" y="0.014"/>
      <point x="0.4" y="0.016"/>
      <point x="0.48" y="0.02"/>
    </topLeft>
    <topRight>
      <point x="0.51" y="0.04"/>
      <point x="0.6" y="0.03"/>
      <point x="0.75" y="0.02"/>
      <point x="0.85" y="0.01"/>
      <point x="0.95" y="0.005"/>
    </topRight>
  </skin>
</skins>
)";

    reader::Skins skins;
    const std::optional<std::vector<reader::CorrectedSkin>> oldRead = skins.loadFrom(old);
    check(skins.list().size() == 1, "обложка прочитана");
    check(oldRead && oldRead->empty(), "верный реестр второй версии — ни одной исправленной обложки");
    if (skins.list().size() != 1) return;

    const reader::Skin& skin = skins.list()[0];
    constexpr size_t spine = static_cast<size_t>(reader::EdgeCurve::kSpine);
    check(skin.top.x[spine] == reader::EdgeCurve::kSpineX && aboutEqual(skin.top.y[spine], 0.03f),
          "точка корешка — на середине, высотой посередине между прежними");
    check(aboutEqual(skin.top.x[spine - 1], 0.4f) && aboutEqual(skin.top.x[spine + 1], 0.6f),
          "соседки корешка — с обоих листов");
    check(aboutEqual(skin.top.x[0], 0.05f) && aboutEqual(skin.top.x[8], 0.95f) &&
              aboutEqual(skin.top.y[8], 0.005f),
          "крайние точки — кромки");

    const reader::Skin fresh = reader::defaultSkin();
    check(skin.bottom.x == fresh.bottom.x && skin.bottom.y == fresh.bottom.y,
          "чего в файле нет — начальная прямая");

    // У первой точки правого листа нет высоты: на её месте — точка начальной
    // прямой, а не точка корешка левого листа, так что корешок — посередине
    // между точкой левого и прямой.
    std::string rightWithoutHeight = old;
    constexpr std::string_view rightFirstPoint = "<point x=\"0.51\" y=\"0.04\"/>";
    const size_t rightFirstAt = rightWithoutHeight.find(rightFirstPoint);
    check(rightFirstAt != std::string::npos, "первая точка правого листа записана как была");
    if (rightFirstAt == std::string::npos) return;
    rightWithoutHeight.replace(rightFirstAt, rightFirstPoint.size(), "<point x=\"0.51\"/>");
    reader::Skins halvesRead;
    const std::optional<std::vector<reader::CorrectedSkin>> halvesCorrected = halvesRead.loadFrom(rightWithoutHeight);
    check(correctedOne(halvesCorrected, u"Старая",
                       {{reader::SkinFault::MissingCoordinate, reader::SkinEdge::Top, 1}}) &&
              aboutEqual(halvesRead.list()[0].top.y[spine], 0.5f * (0.02f + fresh.top.y[spine])),
          "правый лист второй версии без высоты у первой точки — точка начальной прямой, а не корешок левого");

    const std::string written = skins.toXml();
    check(written.find("<skins version=\"3\">") != std::string::npos &&
              written.find("<top>") != std::string::npos &&
              written.find("topLeft") == std::string::npos,
          "пишется третья версия");

    reader::Skins again;
    again.loadFrom(written);
    check(again.list().size() == 1, "записанное читается");
    if (again.list().size() != 1) return;

    bool same = true;
    for (size_t index = 0; index < static_cast<size_t>(reader::EdgeCurve::kPoints); ++index) {
        same = same && aboutEqual(again.list()[0].top.x[index], skin.top.x[index]) &&
               aboutEqual(again.list()[0].top.y[index], skin.top.y[index]);
    }
    check(same, "записанное читается тем же");

    // Файл поправили руками и увели корешок с середины.
    std::string bent = written;
    const size_t spineAt = bent.find("<point x=\"0.5\"");
    check(spineAt != std::string::npos, "корешок записан на середине");
    if (spineAt == std::string::npos) return;
    bent.replace(spineAt, 14, "<point x=\"0.47\"");

    reader::Skins repaired;
    const std::optional<std::vector<reader::CorrectedSkin>> bentRead = repaired.loadFrom(bent);
    check(repaired.list().size() == 1 &&
              repaired.list()[0].top.x[spine] == reader::EdgeCurve::kSpineX,
          "корешок из файла встаёт на середину");
    check(correctedOne(bentRead, u"Старая", {{reader::SkinFault::SpineMoved, reader::SkinEdge::Top}}),
          "и не молча: обложка названа, корешок верхней кромки поставлен на середину");

    // Точки в файле идут слева направо, но корешок сдвинут вправо, и четвёртая
    // точка стоит между серединой и им: с корешком на середине она за ним.
    // Названы оба исправления, и «не слева направо» — с корешком на середине.
    std::string shiftedOrder = written;
    const size_t shiftedSpineAt = shiftedOrder.find("<point x=\"0.5\"");
    const size_t shiftedFourthAt = shiftedOrder.find("<point x=\"0.4\"");
    check(shiftedSpineAt != std::string::npos && shiftedFourthAt != std::string::npos,
          "корешок и четвёртая точка записаны как были");
    if (shiftedSpineAt == std::string::npos || shiftedFourthAt == std::string::npos) return;
    shiftedOrder.replace(shiftedSpineAt, 14, "<point x=\"0.56\"");
    shiftedOrder.replace(shiftedFourthAt, 14, "<point x=\"0.52\"");
    reader::Skins reordered;
    check(correctedOne(reordered.loadFrom(shiftedOrder), u"Старая",
                       {{reader::SkinFault::SpineMoved, reader::SkinEdge::Top},
                        {reader::SkinFault::Unordered, reader::SkinEdge::Top}}),
          "корешок сдвинут, и с ним на середине точки не по порядку — оба исправления по порядку");

    // Точку увели за край страницы: на её месте — точка начальной прямой, и
    // чтение это называет.
    std::string offPage = written;
    constexpr std::string_view firstHeight = " y=\"0.01\"";
    const size_t firstHeightAt = offPage.find(firstHeight);
    check(firstHeightAt != std::string::npos, "первая точка записана как была");
    if (firstHeightAt == std::string::npos) return;
    offPage.replace(firstHeightAt, firstHeight.size(), " y=\"1.7\"");
    reader::Skins offPageRead;
    const std::optional<std::vector<reader::CorrectedSkin>> offPageCorrected = offPageRead.loadFrom(offPage);
    check(correctedOne(offPageCorrected, u"Старая", {{reader::SkinFault::OutsidePage, reader::SkinEdge::Top, 1}}) &&
              offPageRead.list()[0].top.x[0] == fresh.top.x[0] && offPageRead.list()[0].top.y[0] == fresh.top.y[0],
          "точка вне страницы — названа, на её месте точка начальной прямой целиком");

    // Высоты у точки нет вовсе: на её месте — тоже точка начальной прямой
    // целиком, а не x из файла с y из прямой, и чтение это называет.
    std::string heightless = written;
    heightless.replace(firstHeightAt, firstHeight.size(), "");
    reader::Skins heightlessRead;
    const std::optional<std::vector<reader::CorrectedSkin>> heightlessCorrected = heightlessRead.loadFrom(heightless);
    check(correctedOne(heightlessCorrected, u"Старая",
                       {{reader::SkinFault::MissingCoordinate, reader::SkinEdge::Top, 1}}) &&
              heightlessRead.list()[0].top.x[0] == fresh.top.x[0] &&
              heightlessRead.list()[0].top.y[0] == fresh.top.y[0],
          "точка без высоты — названа, на её месте точка начальной прямой целиком");

    // Десятая точка у кромки: лишняя отбрасывается, и это названо.
    std::string overfull = written;
    const size_t topEndAt = overfull.find("</top>");
    check(topEndAt != std::string::npos, "верхняя кромка записана");
    if (topEndAt == std::string::npos) return;
    overfull.insert(topEndAt, "  <point x=\"0.99\" y=\"0.5\"/>\n    ");
    reader::Skins overfullRead;
    check(correctedOne(overfullRead.loadFrom(overfull), u"Старая",
                       {{reader::SkinFault::ExtraPoints, reader::SkinEdge::Top, 1}}),
          "лишняя точка кромки — отброшена и названа");

    // Точки не на месте у обеих кромок: каждая названа своей причиной, верхняя
    // раньше нижней, и у каждой — число своих точек. Лишние за первой лишней
    // тоже сосчитаны.
    reader::Skin scattered = fresh;
    scattered.name = u16_text{u"Старая"};
    scattered.image = u16_text{u"old.png"};
    scattered.top.y[1] = 1.5f;
    scattered.top.y[2] = -0.5f;
    scattered.bottom.y[6] = 1.2f;
    reader::Skins scatteredRegistry;
    scatteredRegistry.put(scattered);
    std::string scatteredXml = scatteredRegistry.toXml();
    const size_t bottomEndAt = scatteredXml.find("</bottom>");
    check(bottomEndAt != std::string::npos, "нижняя кромка записана");
    if (bottomEndAt == std::string::npos) return;
    scatteredXml.insert(bottomEndAt, "  <point x=\"0.98\" y=\"0.9\"/>\n      <point x=\"0.99\" y=\"0.9\"/>\n    ");
    reader::Skins scatteredRead;
    check(correctedOne(scatteredRead.loadFrom(scatteredXml), u"Старая",
                       {{reader::SkinFault::OutsidePage, reader::SkinEdge::Top, 2},
                        {reader::SkinFault::OutsidePage, reader::SkinEdge::Bottom, 1},
                        {reader::SkinFault::ExtraPoints, reader::SkinEdge::Bottom, 2}}) &&
              scatteredRead.list()[0].top.y == fresh.top.y && scatteredRead.list()[0].bottom.y == fresh.bottom.y,
          "точки у обеих кромок — по кромке и с числом точек, на их местах точки начальной прямой");

    // Корешок сдвинут у обеих кромок — две причины, а не одна на обложку.
    reader::Skin bothSpines = fresh;
    bothSpines.name = u16_text{u"Старая"};
    bothSpines.image = u16_text{u"old.png"};
    bothSpines.top.x[spine] = 0.47f;
    bothSpines.bottom.x[spine] = 0.53f;
    reader::Skins bothSpinesRegistry;
    bothSpinesRegistry.put(bothSpines);
    reader::Skins bothSpinesRead;
    check(correctedOne(bothSpinesRead.loadFrom(bothSpinesRegistry.toXml()), u"Старая",
                       {{reader::SkinFault::SpineMoved, reader::SkinEdge::Top},
                        {reader::SkinFault::SpineMoved, reader::SkinEdge::Bottom}}),
          "корешок сдвинут у обеих кромок — названы обе");

    // Обложки без снимка и без имени не встают, и об этом сказано.
    const std::string incomplete = R"(<?xml version="1.0" encoding="utf-8"?>
<skins version="3">
  <skin name="Без снимка" image=""/>
  <skin image="noname.png"/>
</skins>
)";
    reader::Skins incompleteRead;
    const std::optional<std::vector<reader::CorrectedSkin>> incompleteCorrected = incompleteRead.loadFrom(incomplete);
    check(incompleteCorrected && incompleteCorrected->size() == 2 && incompleteRead.list().empty() &&
              (*incompleteCorrected)[0].name == std::u16string_view{u"Без снимка"} &&
              std::ranges::equal((*incompleteCorrected)[0].corrections,
                                 std::initializer_list<reader::SkinCorrection>{
                                     {reader::SkinFault::SkippedNoImage, std::nullopt}}) &&
              (*incompleteCorrected)[1].name.empty() &&
              std::ranges::equal((*incompleteCorrected)[1].corrections,
                                 std::initializer_list<reader::SkinCorrection>{
                                     {reader::SkinFault::SkippedNoName, std::nullopt}}),
          "обложки без снимка и без имени пропущены и названы");

    // Точку увели за соседку: такая кромка — начальная прямая.
    std::string crossed = written;
    const size_t fourthAt = crossed.find("<point x=\"0.4\"");
    check(fourthAt != std::string::npos, "четвёртая точка записана как была");
    if (fourthAt == std::string::npos) return;
    crossed.replace(fourthAt, 14, "<point x=\"0.6\"");

    reader::Skins straightened;
    const std::optional<std::vector<reader::CorrectedSkin>> crossedRead = straightened.loadFrom(crossed);
    check(straightened.list().size() == 1 && straightened.list()[0].top.x == fresh.top.x &&
              straightened.list()[0].top.y == fresh.top.y,
          "точки не по порядку — кромка становится начальной прямой");
    check(correctedOne(crossedRead, u"Старая", {{reader::SkinFault::Unordered, reader::SkinEdge::Top}}),
          "и не молча: обложка названа, причина — точки верхней кромки не слева направо, корешок на месте");

    // Не по порядку только нижняя: ровной становится она одна, и названа она.
    reader::Skin bottomCrossed = fresh;
    bottomCrossed.name = u16_text{u"Старая"};
    bottomCrossed.image = u16_text{u"old.png"};
    bottomCrossed.top.y[2] = 0.05f;
    bottomCrossed.bottom.x[3] = 0.6f;
    reader::Skins bottomCrossedRegistry;
    bottomCrossedRegistry.put(bottomCrossed);
    reader::Skins bottomStraightened;
    const std::optional<std::vector<reader::CorrectedSkin>> bottomCrossedRead =
        bottomStraightened.loadFrom(bottomCrossedRegistry.toXml());
    check(correctedOne(bottomCrossedRead, u"Старая", {{reader::SkinFault::Unordered, reader::SkinEdge::Bottom}}) &&
              aboutEqual(bottomStraightened.list()[0].top.y[2], 0.05f) &&
              bottomStraightened.list()[0].bottom.x == fresh.bottom.x,
          "не по порядку только нижняя — ровная и названа она одна, верхняя как была");

    // Кромки, которые между точками разошлись на высоту полосы и вывернули бы
    // столбец: ровной становится вся обложка.
    reader::Skin namedFold = foldedSkin();
    namedFold.name = u16_text{u"Вывернутая"};
    namedFold.image = u16_text{u"folded.png"};
    reader::Skins foldingRegistry;
    foldingRegistry.put(namedFold);
    reader::Skins unfolded;
    const std::optional<std::vector<reader::CorrectedSkin>> foldedRead = unfolded.loadFrom(foldingRegistry.toXml());
    check(unfolded.list().size() == 1 && unfolded.list()[0].top.y == fresh.top.y &&
              unfolded.list()[0].bottom.y == fresh.bottom.y &&
              reader::PageWarp{unfolded.list()[0]}.flat(),
          "обложка, выворачивающая столбец, читается ровной");
    check(correctedOne(foldedRead, u"Вывернутая", {{reader::SkinFault::FoldsColumn, std::nullopt}}),
          "и не молча: обложка названа, причина — кромки выворачивают столбец");

    // Файл поправили руками: первые две точки верхней кромки — на нулевом и на
    // субнормальном иксе. По возрастанию они идут, но шаг между ними сплайн
    // переводит в бесконечность и NaN, и карта изгиба взяла бы из него байт.
    // Файлу не верят: такая обложка тоже читается ровной.
    const std::string subnormal = R"(<?xml version="1.0" encoding="utf-8"?>
<skins version="3">
  <skin name="Сломанная" image="broken.png">
    <top>
      <point x="0" y="0.02"/>
      <point x="1e-45" y="0.03"/>
    </top>
  </skin>
</skins>
)";
    reader::Skins broken;
    const std::optional<std::vector<reader::CorrectedSkin>> subnormalRead = broken.loadFrom(subnormal);
    check(broken.list().size() == 1 && broken.list()[0].top.x == fresh.top.x &&
              broken.list()[0].top.y == fresh.top.y &&
              reader::PageWarp{broken.list()[0]}.flat(),
          "кромка на субнормальном шаге читается ровной");
    // Сплайн, ушедший в бесконечность, столбцы не держит (keepsColumns), и
    // причина названа та же.
    check(correctedOne(subnormalRead, u"Сломанная", {{reader::SkinFault::FoldsColumn, std::nullopt}}),
          "и не молча: обложка с субнормальным шагом названа");

    reader::Skins unparsed;
    check(!unparsed.loadFrom("<skins><skin name=\"Проба\" x=\"0.14"), "битый реестр — не разобран, а не пустой отчёт");
}


// ---- Рабочее место: файлы читалки без окна ----------------------------------
//
// Корутины рабочего места ждут операций wxl, а те живут на петле sta_loop:
// тест поднимает её один раз на процесс (как тесты самой wxl) и крутит до
// конца каждой корутины. Разбор идёт на этом же потоке, в STA-пуле.
//
// Операция до хода петли не кончается: вызов корутины возвращается, когда
// она встала на первом ожидании, и просьба кончиться, сделанная тут же, —
// просьба посреди этого ожидания.

using wxl::async::cancellation_source;
using wxl::async::cancellation_token;
using wxl::async::last_error;
using wxl::async::operation_canceled_exception;
using wxl::async::sta_loop;
using wxl::async::task;

/// Токен, которого никто не отменит, — для проб без отмены.
const auto never = cancellation_token{};

const std::filesystem::path testdata = BUKVITSA_TESTDATA_DIR;

/// Временный каталог на один тест: рабочее место живёт в нём, после теста он
/// стирается. Имя — с номером процесса: два прогона рядом не пересекутся.
struct Sandbox {
    std::filesystem::path root;

    explicit Sandbox(const char* name)
        : root(std::filesystem::temp_directory_path() /
               (L"bukvitsa-lib-tests-" + std::to_wstring(::GetCurrentProcessId())) / name) {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        std::filesystem::create_directories(root);
    }

    ~Sandbox() {
        std::error_code ignored;
        std::filesystem::remove_all(root.parent_path(), ignored);
    }
};

template <class T>
T run(task<T> work) {
    sta_loop::run_until([&] { return work.done(); });
    return work.result();
}

void run(task<> work) {
    sta_loop::run_until([&] { return work.done(); });
    work.result();
}

/// Кончилась ли работа отменой — не значением и не другим сбоем.
template <class T>
bool canceled(task<T> work) {
    sta_loop::run_until([&] { return work.done(); });
    try {
        static_cast<void>(work.result());
    } catch (const operation_canceled_exception&) {
        return true;
    } catch (...) {
    }
    return false;
}

// Рабочий поток спящей петли проб один и берёт операции по порядку
// (sta_loop::start: и async_call, и операции под токеном идут ему одним
// каналом), а его ответы лежат в обратном канале, пока петлю не повернут. На
// этом стоят два порядка, которых проба отмены иначе у свободного потока не
// выиграет: операция ещё не начата — или уже ответила, а ждущий не проснулся.

/// Рабочий поток занят, пока ворота не открыты: операции, ушедшие ему после,
/// стоят не начатыми. Не открытые к концу — открываются на выходе, чтобы
/// тело, державшее поток, кончилось.
class HeldWorker {
public:
    HeldWorker() : holdingTask_(sta_loop::async_call([this] { workerGate_.acquire(); })) {}

    HeldWorker(const HeldWorker&) = delete;
    HeldWorker& operator=(const HeldWorker&) = delete;

    ~HeldWorker() {
        if (holdingTask_) workerGate_.release();
    }

    /// Ворота открыты, и тело, державшее поток, вернулось.
    void open() {
        workerGate_.release();
        run(std::move(holdingTask_));
    }

private:
    std::binary_semaphore workerGate_{0};
    task<> holdingTask_;
};

/// Рабочий поток ответил на всё, что ему ушло раньше: знак, поставленный
/// следом, исполняется только после них, и конструктор ждёт его. Ответы лежат
/// в обратном канале, ни одна ждущая корутина ещё не проснулась; deliver()
/// поворачивает петлю, пока знак не вернётся, — и ответы, лежавшие раньше
/// него, доходят до ждущих.
class AnsweredWorker {
public:
    AnsweredWorker() : markTask_(sta_loop::async_call([this] { reachedSignal_.release(); })) {
        reachedSignal_.acquire();
    }

    AnsweredWorker(const AnsweredWorker&) = delete;
    AnsweredWorker& operator=(const AnsweredWorker&) = delete;

    void deliver() { run(std::move(markTask_)); }

private:
    std::binary_semaphore reachedSignal_{0};
    task<> markTask_;
};

/// Байты файла, прочитанные обычным способом: проверка того, что записало
/// рабочее место.
std::string onDisk(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void putOnDisk(const std::filesystem::path& file, std::string_view bytes) {
    std::filesystem::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

bool holds(const std::wstring& text, std::wstring_view piece) {
    return text.find(piece) != std::wstring::npos;
}

/// Сколько раз кусок встречается в тексте файла.
size_t occurrences(std::string_view text, std::string_view piece) {
    size_t count = 0;
    for (size_t at = text.find(piece); at != std::string_view::npos; at = text.find(piece, at + piece.size())) ++count;
    return count;
}

const std::filesystem::path kBook = testdata / L"Turgenev_I._Spisokshkolnoy._Otcyi_I_Deti.fb3";
const std::filesystem::path kOtherBook = testdata / L"nightmare_example.fb3";
/// Третья книга — для «новый файл в каталоге»: у неё свой UUID.
const std::filesystem::path kThirdBook = testdata / L"anathomy_tutorial_example.fb3";

/// Наблюдаемый каталог пробы — копии книг в `shelf` песочницы.
std::filesystem::path shelfOf(const Sandbox& box, std::initializer_list<std::filesystem::path> books) {
    const std::filesystem::path folder = box.root / L"shelf";
    std::filesystem::create_directories(folder);
    for (const std::filesystem::path& book : books) std::filesystem::copy_file(book, folder / book.filename());
    return folder;
}

/// Карточка книги, чей файл лежит по этому пути; нет такой — nullptr.
ShelfCard* cardAt(Library& library, const std::filesystem::path& path) {
    for (const intrusive_ptr<ShelfCard>& card : library.cards())
        if (samePath(card->entry.path, path)) return card.get();
    return nullptr;
}

/// Время правки файла сдвигается вперёд: для перепроверки это уже другой файл,
/// хотя байты те же.
std::filesystem::file_time_type touch(const std::filesystem::path& file) {
    const std::filesystem::file_time_type later = std::filesystem::last_write_time(file) + std::chrono::seconds(1);
    std::filesystem::last_write_time(file, later);
    return later;
}

/// Книга в реестр тем, что регистрация берёт из документа: описанием, числом
/// знаков и копией обложки. Описание читается, а не забирается: документ
/// теста живёт до конца пробы.
const BookEntry& addTo(Library& library, const fb3::Document& document, const std::filesystem::path& path,
                       uint64_t fileSize) {
    return library
        .add(document.description(), document.characterCount(), copyCover(document),
             BookFile{.path = path, .size = fileSize})
        .entry;
}

/// Регистрация книги тем, что снимает с документа чтение книги: описание,
/// число знаков, копия обложки. Задача стартует в вызове: свободный замок шаг
/// берёт сразу и встаёт на записи обложки до хода петли, а занятого — ждёт.
task<Registered> registrationOf(Workspace& ws, const fb3::Document& document, BookFile file,
                                cancellation_token token = never) {
    return ws.registerBook(document.description(), document.characterCount(), copyCover(document), std::move(file),
                           std::move(token));
}

/// Регистрация книги в рабочем месте — до конца её шага.
Registered registerFrom(Workspace& ws, const fb3::Document& document, const std::filesystem::path& path,
                        uint64_t fileSize) {
    return run(registrationOf(ws, document, BookFile{.path = path, .size = fileSize}));
}

/// Открытие тем же порядком, что у Reader: прочитанная книга — в регистрацию
/// под токеном открытия, затем открытие записи, какой её отдал реестр.
task<Opened> openIn(Workspace& ws, const LoadedBook& loaded, cancellation_token token) {
    token.throw_if_canceled();

    const Registered registered =
        co_await ws.registerBook(loaded.description, loaded.book->characterCount(), loaded.cover,
                                 BookFile{.path = loaded.book->path(), .size = loaded.fileSize}, token);

    Opened opened = co_await ws.openBook(registered.entry, std::move(token));
    co_return opened;
}

/// Кончилась ли регистрация сбоем записи обложки — не значением и не другим
/// сбоем.
bool coverRefused(task<Registered> registration) {
    sta_loop::run_until([&] { return registration.done(); });
    try {
        static_cast<void>(registration.result());
    } catch (const last_error&) {
        return true;
    } catch (...) {
    }
    return false;
}

/// Обход до конца — и его регистрации до последней: обход шагов не ждёт, и
/// что встало, видно в реестре только после них. События рабочего места за
/// это время — счётом рядом с отчётом обхода.
struct Scanned : ScanReport {
    uint32_t registered = 0;              ///< шагов, переменивших реестр (`bookRegistered`)
    uint32_t added = 0;                   ///< из них — новых книг
    std::vector<std::wstring> refused;    ///< отказы регистраций обхода (`registrationRefused`)

    /// Реестр переменился — пометками обхода или вставшими книгами.
    bool changed() const noexcept { return ScanReport::changed() || registered != 0; }
};

Scanned scanned(Workspace& ws, task<ScanReport> work) {
    Scanned result;
    event_token registeredWatch = ws.bookRegistered().on_change([&result](const Registered& registered) noexcept {
        ++result.registered;
        if (registered.isNew) ++result.added;
    });
    event_token refusedWatch = ws.registrationRefused().on_change(
        [&result](const std::wstring& line) noexcept { result.refused.push_back(line); });
    const auto unwatch = [&] {
        ws.bookRegistered().remove_change(registeredWatch);
        ws.registrationRefused().remove_change(refusedWatch);
    };

    try {
        static_cast<ScanReport&>(result) = run(std::move(work));
        sta_loop::run_until([&] { return !ws.registering(); });
    } catch (...) {
        unwatch();
        throw;
    }
    unwatch();
    return result;
}

/// Реестр — карточками полки: новая книга — одна карточка в конце, та же книга
/// снова — без перемен, переложенная — замена своей карточки на её месте,
/// прочитанный файл — один сброс; строка прогресса — поле карточки, и полка о
/// ней не слышит.
void testLibraryCards() {
    std::printf("\n=== реестр: карточки полки ===\n");

    const std::string bytes = onDisk(kBook);
    const fb3::Document document{std::string(bytes)};
    const std::string otherBytes = onDisk(kOtherBook);
    const fb3::Document other{std::string(otherBytes)};

    // Слышанное — раньше реестра: слушатель уходит вместе с ним.
    std::vector<list_change> heard;

    Library library;
    static_cast<void>(library.cards().on_change(
        [&heard](const list_change& change) noexcept { heard.push_back(change); }));

    const u16_text guid = addTo(library, document, kBook, bytes.size()).guid;
    check(heard.size() == 1 && heard[0] == list_change{list_change::inserted, 0, 1},
          "новая книга — одна карточка в конце");

    addTo(library, other, kOtherBook, otherBytes.size());
    check(heard.size() == 2 && heard[1] == list_change{list_change::inserted, 1, 1}, "вторая — за первой");

    heard.clear();
    ShelfCard* const card = library.cards()[0].get();
    addTo(library, document, kBook, bytes.size());
    check(heard.empty() && library.cards()[0].get() == card,
          "та же книга снова — та же карточка, полка не слышит");

    check(card->progress.get().empty(), "пока файл состояния не прочитан, строка прогресса пуста");

    BookState state;
    state.charOffset = card->entry.characterCount / 2;
    state.bookmarks.push_back(Bookmark{10, u16_text{u"закладка"}});
    card->showState(state);
    const u16_text line = shelfLine(state.charOffset, card->entry.characterCount, 1);
    check(card->progress.get() == line && heard.empty(), "прогресс — поле карточки: строка своя, полка не слышит");

    card->showState(BookState{});
    check(card->progress.get() == line, "книгу открывали, а места не записали — строка остаётся");

    // Тот же UUID книги под другим путём — её переложили: запись другая,
    // карточка новая, место на полке и guid прежние.
    const std::filesystem::path moved = kBook.parent_path() / L"moved" / kBook.filename();
    addTo(library, document, moved, bytes.size());
    check(heard.size() == 1 && heard[0] == list_change{list_change::replaced, 0, 1},
          "переложенная книга — замена своей карточки");
    check(library.cards()[0]->entry.guid == guid && library.cards()[0]->entry.path == moved,
          "guid прежний, путь новый");
    check(library.cards()[0]->progress.get() == line, "строка прогресса переходит на новую карточку");

    const std::string xml = library.toXml();
    heard.clear();
    check(library.loadFrom(xml), "свой же реестр читается");
    check(heard.size() == 1 && heard[0] == list_change{list_change::reset, 0, 2}, "прочитанный реестр — один сброс");
    check(library.find(guid) && library.find(guid)->path == moved, "записи — из файла");

    heard.clear();
    check(!library.loadFrom("<library><book authors=\"Фрэнк"), "битый реестр — не разобран");
    check(heard.size() == 1 && heard[0] == list_change{list_change::reset, 0, 0} && library.cards().empty(),
          "битый реестр — пустая полка одним сбросом");
}

/// Реестр второй версии: наблюдаемые каталоги без дублей; пометка «не
/// найдена» — поле карточки, которое слышит она, а не полка, и которое реестр
/// пишет и читает; прочитанный файл снимает пометку, а открытие файлом не
/// отрывает книгу от каталога и времени; убранная книга уходит своей
/// карточкой; первая версия читается без новых полей.
void testLibraryFolders() {
    std::printf("\n=== реестр: каталоги и пометка «не найдена» ===\n");

    const std::string bytes = onDisk(kBook);
    const fb3::Document document{std::string(bytes)};
    const std::string otherBytes = onDisk(kOtherBook);
    const fb3::Document other{std::string(otherBytes)};

    const std::filesystem::path folder = kBook.parent_path();
    const std::filesystem::file_time_type modified = std::filesystem::last_write_time(kBook);
    auto const file = BookFile{.path = kBook, .size = bytes.size(), .modified = modified, .folder = folder};

    // Слышанное — раньше реестра: слушатели уходят вместе с ним и с карточкой.
    std::vector<list_change> heard;
    std::vector<bool> shown;

    Library library;

    check(library.addFolder(folder) && library.folders().size() == 1, "каталог — наблюдаемый");
    std::wstring shouted = folder.wstring();
    for (wchar_t& unit : shouted) unit = static_cast<wchar_t>(std::towupper(unit));
    check(!library.addFolder(std::filesystem::path{shouted}) && library.folders().size() == 1,
          "тот же каталог в другом регистре — не второй");

    const u16_text guid =
        library.add(document.description(), document.characterCount(), copyCover(document), file).entry.guid;
    addTo(library, other, kOtherBook, otherBytes.size());   // файлом: без каталога и времени

    static_cast<void>(library.cards().on_change(
        [&heard](const list_change& change) noexcept { heard.push_back(change); }));
    ShelfCard* const card = library.cards()[0].get();
    static_cast<void>(card->missing().on_change([&shown](bool missing) noexcept { shown.push_back(missing); }));

    check(!card->missing().get(), "новая книга — на месте");
    check(library.setMissing(guid, true) && card->missing().get(), "пометка ставится");
    check(!library.setMissing(guid, true), "поставленная снова — не перемена");
    check(shown.size() == 1 && shown[0] && heard.empty(), "пометку слышит карточка, а не полка");
    check(!library.setMissing(newGuid(), true), "чужой guid — ничего");

    const std::string xml = library.toXml();
    check(occurrences(xml, "<library version=\"2\">") == 1, "реестр — второй версии");
    check(occurrences(xml, "<folders>") == 1 && occurrences(xml, "<folder path=") == 1, "каталоги — разделом");
    check(occurrences(xml, " missing=\"true\"") == 1, "пометка — у одной книги");
    check(occurrences(xml, " folder=") == 1 && occurrences(xml, " modified=") == 1,
          "каталог и время — у книги из каталога; у добавленной файлом их нет");

    Library reread;
    check(reread.loadFrom(xml), "вторая версия читается");
    const BookEntry* back = reread.find(guid);
    check(reread.folders().size() == 1 && reread.folders()[0] == folder, "каталог — из файла");
    check(back && back->folder == folder && back->modified == modified && back->fileSize == bytes.size(),
          "каталог, время и размер книги — из файла");
    check(reread.cards().size() == 2 && reread.cards()[0]->missing().get() && !reread.cards()[1]->missing().get(),
          "пометка — из файла, у своей книги");
    check(reread.toXml() == xml, "прочитанный и записанный снова — тот же текст");

    // Пометка — только «true»: вписанное руками «false» книгу не помечает.
    std::string unmarked = xml;
    if (const size_t at = unmarked.find(" missing=\"true\""); at != std::string::npos)
        unmarked.replace(at, 15, " missing=\"false\"");
    Library handEdited;
    check(occurrences(unmarked, " missing=\"false\"") == 1 && handEdited.loadFrom(unmarked) &&
              handEdited.cards().size() == 2 && !handEdited.cards()[0]->missing().get(),
          "missing=\"false\" — не пометка");

    // Файл прочитан — значит, он на месте: та же запись, та же карточка, и
    // снята пометка.
    const AddedBook again =
        library.add(document.description(), document.characterCount(), copyCover(document), file);
    check(!again.isNew && !again.isChanged && again.restored, "тот же файл прочитан — запись та же, пометка снята");
    check(library.cards()[0].get() == card && !card->missing().get() && heard.empty(),
          "карточка та же, полка не слышит");

    // Открытие файлом не знает ни каталога, ни времени: у того же файла они
    // остаются от записи, и запись не меняется.
    const AddedBook opened = library.add(document.description(), document.characterCount(), copyCover(document),
                                         BookFile{.path = kBook, .size = bytes.size()});
    check(!opened.isChanged && opened.entry.folder == folder && opened.entry.modified == modified,
          "открытие файлом не отрывает книгу ни от каталога, ни от времени");

    check(library.erase(guid) && library.cards().size() == 1 && heard.size() == 1 &&
              heard[0] == list_change{list_change::erased, 0, 1},
          "убранная книга — её карточка ушла с полки");
    check(!library.erase(guid) && !library.find(guid), "убранной в реестре нет");

    // Файл первой версии — как его писали до каталогов.
    const std::string v1 =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<library version=\"1\">\n"
        "  <book guid=\"{3F2504E0-4F89-11D3-9A0C-0305E82C3301}\" title=\"Рудин\" authors=\"Иван Тургенев\" "
        "bookId=\"\" cover=\"\" path=\"C:\\Книги\\Рудин.fb3\" size=\"10\" characters=\"5\"/>\n"
        "</library>\n";
    Library first;
    check(first.loadFrom(v1) && first.cards().size() == 1 && first.folders().empty(), "первая версия читается");
    if (first.cards().size() == 1) {
        const BookEntry& old = first.cards()[0]->entry;
        check(old.modified == kUnknownTime && old.folder.empty() && old.fileSize == 10 &&
                  !first.cards()[0]->missing().get(),
              "у записи первой версии время неизвестно, каталога и пометки нет");
    }
}

/// Путь сводит запись с книгой, только если UUID этого не опровергает: запись
/// без UUID по своему пути узнаёт книгу; другая книга с другим UUID по тому же
/// пути — новая запись с новым guid, а прежняя уходит с полки. И guid, который
/// настройки помнят за этим путём, ей не достаётся: он прежней книги.
void testLibraryPathTakenByOtherBook() {
    std::printf("\n=== реестр: путь и UUID ===\n");

    const std::string bytes = onDisk(kBook);
    const fb3::Document document{std::string(bytes)};
    const std::string otherBytes = onDisk(kOtherBook);
    const fb3::Document other{std::string(otherBytes)};

    std::vector<list_change> heard;

    // Запись без UUID — по этому пути: файл правили руками или книга его не
    // знает. Тот же путь узнаёт её.
    Library unnamed;
    const std::string xml = "<library version=\"2\"><book guid=\"{3F2504E0-4F89-11D3-9A0C-0305E82C3301}\" "
                            "title=\"x\" authors=\"\" bookId=\"\" cover=\"\" path=\"" +
                            xmlValue(kBook.native()) + "\" size=\"1\" characters=\"1\"/></library>";
    check(unnamed.loadFrom(xml) && unnamed.cards().size() == 1, "запись без UUID прочитана");
    const AddedBook named = unnamed.add(document.description(), document.characterCount(), copyCover(document),
                                        BookFile{.path = kBook, .size = bytes.size()});
    check(!named.isNew && named.entry.guid == u16_text{u"{3F2504E0-4F89-11D3-9A0C-0305E82C3301}"} &&
              unnamed.cards().size() == 1,
          "запись без UUID по своему пути — та же книга, guid прежний");

    Library library;
    static_cast<void>(library.cards().on_change(
        [&heard](const list_change& change) noexcept { heard.push_back(change); }));

    const u16_text former = addTo(library, document, kBook, bytes.size()).guid;
    heard.clear();

    const AddedBook replaced = library.add(other.description(), other.characterCount(), copyCover(other),
                                           BookFile{.path = kBook, .size = otherBytes.size()}, former);
    check(replaced.isNew && replaced.isChanged && replaced.entry.guid != former,
          "другой UUID по тому же пути — новая книга под новым guid, и guid из настроек ей не достаётся");
    check(!library.find(former) && library.cards().size() == 1 && heard.size() == 2 &&
              heard[0] == list_change{list_change::erased, 0, 1} &&
              heard[1] == list_change{list_change::inserted, 0, 1},
          "прежняя запись ушла с полки, новая встала");

    // Книгу с UUID переложили поверх файла книги без UUID: реестр узнаёт её по
    // UUID, а запись той, чей файл перезаписан, уходит — у пути одна запись.
    std::vector<list_change> moved;
    const std::filesystem::path taken = kBook.parent_path() / L"без-UUID.fb3";
    Library overwritten;
    const std::string unnamedThere =
        "<library version=\"2\"><book guid=\"{3F2504E0-4F89-11D3-9A0C-0305E82C3301}\" title=\"x\" "
        "authors=\"\" bookId=\"\" cover=\"\" path=\"" +
        xmlValue(taken.native()) + "\" size=\"1\" characters=\"1\"/></library>";
    check(overwritten.loadFrom(unnamedThere) && overwritten.cards().size() == 1, "запись без UUID по своему пути");

    const u16_text movedGuid = addTo(overwritten, document, kBook, bytes.size()).guid;
    static_cast<void>(overwritten.cards().on_change(
        [&moved](const list_change& change) noexcept { moved.push_back(change); }));

    const AddedBook landed = overwritten.add(document.description(), document.characterCount(), copyCover(document),
                                             BookFile{.path = taken, .size = bytes.size()});
    check(!landed.isNew && landed.entry.guid == movedGuid && landed.entry.path == taken,
          "переложенная книга — узнана по UUID, guid прежний, путь новый");
    check(overwritten.cards().size() == 1 && overwritten.findFile(taken) &&
              overwritten.findFile(taken)->guid == movedGuid &&
              !overwritten.find(u16_text{u"{3F2504E0-4F89-11D3-9A0C-0305E82C3301}"}),
          "у пути одна запись — переложенной книги");
    check(moved.size() == 2 && moved[0] == list_change{list_change::erased, 0, 1} &&
              moved[1] == list_change{list_change::replaced, 0, 1},
          "запись перезаписанной ушла с полки, переложенная — заменой своей карточки");
}

/// Пустой каталог — первый запуск: умолчания, ни одного слова читателю.
void testWorkspaceStartsEmpty() {
    std::printf("\n=== рабочее место: первый запуск ===\n");

    Sandbox box("empty");
    Workspace ws(box.root, never);

    const Started started = run(ws.start(never));

    check(started.notices.empty(), "первый запуск: ни одного сообщения");
    check(started.lastBook.empty(), "первый запуск: продолжать нечего");
    check(ws.library.cards().empty(), "первый запуск: реестр пуст");
    check(ws.skins.list().empty(), "первый запуск: обложек нет");
    check(ws.settings.fontSize.get() == kFontSizeDefault, "первый запуск: кегль по умолчанию");
    check(!std::filesystem::exists(ws.settingsPath()), "первый запуск ничего не пишет");
}

/// Три испорченных файла: слово о каждом, копия .bad байт в байт, умолчания;
/// первая запись заменяет файл, копия остаётся одна и перезаписывается.
void testWorkspaceKeepsBrokenFiles() {
    std::printf("\n=== рабочее место: битые файлы ===\n");

    Sandbox box("broken");

    const std::string brokenSettings = "<settings version=\"4\"><view fontSize=\"2";
    const std::string brokenSkins = "<skins><skin name=\"Проба\" x=\"0.14";
    const std::string brokenLibrary = "<library><book authors=\"Фрэнк";

    {
        Workspace ws(box.root, never);
        putOnDisk(ws.settingsPath(), brokenSettings);
        putOnDisk(ws.skinsPath(), brokenSkins);
        putOnDisk(ws.libraryPath(), brokenLibrary);

        const Started started = run(ws.start(never));

        check(started.notices.size() == 3, "три битых файла — три сообщения");
        if (started.notices.size() == 3) {
            check(started.notices[0].headline == L"Настройки не прочитаны" &&
                      holds(started.notices[0].details, ws.settingsPath().wstring()),
                  "сообщение о настройках называет файл");
            check(started.notices[1].headline == L"Реестр обложек не прочитан", "сообщение об обложках");
            check(started.notices[2].headline == L"Реестр книг не прочитан", "сообщение о реестре книг");
        }
        check(ws.settings.fontSize.get() == kFontSizeDefault, "битые настройки — умолчания");
        check(ws.skins.list().empty() && ws.library.cards().empty(), "битые реестры — пусто");

        check(onDisk(box.root / L"settings.xml.bad") == brokenSettings, "копия settings.xml.bad байт в байт");
        check(onDisk(box.root / L"skins.xml.bad") == brokenSkins, "копия skins.xml.bad байт в байт");
        check(onDisk(box.root / L"library.xml.bad") == brokenLibrary, "копия library.xml.bad байт в байт");

        run(ws.saveSettings());

        Settings fresh;
        check(readSettings(onDisk(ws.settingsPath()), fresh), "первая запись заменила файл целым");
        check(onDisk(box.root / L"settings.xml.bad") == brokenSettings, "копия после замены на месте");
    }

    const std::string brokenAgain = "<settings version=\"4\"><view margin=\"0.0";
    {
        Workspace ws(box.root, never);
        putOnDisk(ws.settingsPath(), brokenAgain);

        run(ws.start(never));

        check(onDisk(box.root / L"settings.xml.bad") == brokenAgain, "повторная порча — копия перезаписана");
        check(!std::filesystem::exists(box.root / L"settings.xml.bad.bad"), "копия одна, не множится");
    }
}

/// Правит записанный реестр так, как его поправил бы читатель руками: у
/// точек `heightless` (номера по возрастанию) кромки `curve` (её элемент —
/// «<top>» или «<bottom>») стирает высоту, а в конец кромки дописывает
/// `extra` лишних точек. false — в записи нет такой кромки или таких точек.
bool handEditCurve(std::string& xml, std::string_view curve, std::initializer_list<size_t> heightless, size_t extra) {
    const size_t curveAt = xml.find(curve);
    if (curveAt == std::string::npos) return false;
    const size_t endAt = xml.find("</", curveAt);
    if (endAt == std::string::npos) return false;
    for (size_t added = 0; added < extra; ++added) xml.insert(endAt, "  <point x=\"0.99\" y=\"0.5\"/>\n    ");

    // С конца: стёртая высота не сдвигает точки, которые ещё впереди.
    std::vector<size_t> points;
    for (size_t at = xml.find("<point", curveAt); at != std::string::npos && at < xml.find("</", curveAt);
         at = xml.find("<point", at + 1))
        points.push_back(at);
    for (size_t order = heightless.size(); order-- > 0;) {
        const size_t index = heightless.begin()[order];
        if (index >= points.size()) return false;
        const size_t heightAt = xml.find(" y=\"", points[index]);
        const size_t heightEnd = heightAt == std::string::npos ? heightAt : xml.find('"', heightAt + 4);
        if (heightEnd == std::string::npos) return false;
        xml.erase(heightAt, heightEnd + 1 - heightAt);
    }
    return true;
}

/// Обложки, чьи кромки в skins.xml неверны, запуск показывает исправленными,
/// кладёт рядом копию файла и называет их одним сообщением: строкой на
/// обложку с тем, что исправлено, путями файла и копии и словом о том, что
/// первое сохранение или удаление перепишет файл. Сам файл не переписывается.
/// Одна обложка — заголовок в единственном; верный реестр — ни сообщения, ни
/// копии.
void testWorkspaceNamesCorrectedSkins() {
    std::printf("\n=== рабочее место: исправленные обложки ===\n");

    Skin good = defaultSkin();
    good.name = u16_text{u"Верная"};
    good.image = u16_text{u"good.png"};

    // Корешок нижней кромки — правее середины, остальные точки по порядку
    // (верхнюю со сдвинутым корешком проверяет проба реестра обложек).
    Skin spineMoved = defaultSkin();
    spineMoved.name = u16_text{u"Сдвинутая"};
    spineMoved.image = u16_text{u"spine.png"};
    spineMoved.bottom.x[static_cast<size_t>(reader::EdgeCurve::kSpine)] = 0.55f;

    // Четвёртая точка верхней кромки — за корешком: точки не слева направо.
    Skin unordered = defaultSkin();
    unordered.name = u16_text{u"Беспорядочная"};
    unordered.image = u16_text{u"unordered.png"};
    unordered.top.x[3] = 0.6f;

    Skin folded = foldedSkin();
    folded.name = u16_text{u"Вывернутая"};
    folded.image = u16_text{u"folded.png"};

    // Третья точка верхней кромки ниже страницы — одна точка.
    Skin offPage = defaultSkin();
    offPage.name = u16_text{u"Вне страницы"};
    offPage.image = u16_text{u"offpage.png"};
    offPage.top.y[2] = 1.5f;

    // Две точки верхней кромки и одна нижней — вне страницы: две кромки,
    // у каждой — в числе своих точек.
    Skin scattered = defaultSkin();
    scattered.name = u16_text{u"Разбросанная"};
    scattered.image = u16_text{u"scattered.png"};
    scattered.top.y[1] = 1.5f;
    scattered.top.y[2] = -0.5f;
    scattered.bottom.y[6] = 1.2f;

    // Снимка нет, имени нет — такие обложки не встанут.
    Skin imageless = defaultSkin();
    imageless.name = u16_text{u"Без снимка"};
    Skin nameless = defaultSkin();
    nameless.image = u16_text{u"nameless.png"};

    Skins handEdited;
    handEdited.put(good);
    handEdited.put(spineMoved);
    handEdited.put(unordered);
    handEdited.put(folded);
    handEdited.put(offPage);
    handEdited.put(scattered);
    handEdited.put(imageless);
    handEdited.put(nameless);
    const std::string xml = handEdited.toXml();

    {
        Sandbox box("corrected");
        Workspace ws(box.root, never);
        putOnDisk(ws.skinsPath(), xml);

        const Started started = run(ws.start(never));

        const std::filesystem::path registryCopy = box.root / L"skins.xml.bad";
        check(std::filesystem::exists(registryCopy) && onDisk(registryCopy) == xml, "копия skins.xml.bad байт в байт");
        check(onDisk(ws.skinsPath()) == xml, "файл читателя не переписан");

        check(started.notices.size() == 1, "одно сообщение на все исправленные обложки");
        if (started.notices.size() == 1) {
            const Notice& notice = started.notices[0];
            check(notice.headline == L"Обложки из skins.xml исправлены при чтении", "заголовок — во множественном");
            check(holds(notice.details,
                        L"Обложка «Сдвинутая» в skins.xml: корешок нижней кромки не на середине — поставлен на "
                        L"середину.\n"),
                  "строка обложки со сдвинутым корешком");
            check(holds(notice.details, L"Обложка «Беспорядочная» в skins.xml: с корешком на середине точки "
                                        L"верхней кромки идут не слева направо — кромка показана ровной.\n"),
                  "строка обложки с точками не по порядку");
            check(holds(notice.details, L"Обложка «Вывернутая» в skins.xml: кромки выворачивают столбец — обложка "
                                        L"показана ровной.\n"),
                  "строка обложки, выворачивающей столбец");
            check(holds(notice.details, L"Обложка «Вне страницы» в skins.xml: у точки верхней кромки координата не "
                                        L"в долях страницы — на её месте точка начальной прямой.\n"),
                  "строка обложки с одной точкой вне страницы — в единственном");
            check(holds(notice.details, L"Обложка «Разбросанная» в skins.xml: у точек верхней кромки координаты не "
                                        L"в долях страницы — на их местах точки начальной прямой; у точки нижней "
                                        L"кромки координата не в долях страницы — на её месте точка начальной "
                                        L"прямой.\n"),
                  "строка обложки с точками вне страницы у двух кромок — по кромке, в числе её точек");
            check(holds(notice.details, L"Обложка «Без снимка» в skins.xml: пропущена — у неё нет снимка.\n"),
                  "строка пропущенной обложки");
            check(holds(notice.details, L"Обложка без имени в skins.xml: пропущена — у неё нет имени.\n"),
                  "строка обложки без имени");
            check(!holds(notice.details, L"Верная"), "верная обложка не названа");
            check(holds(notice.details, L"Файл: " + ws.skinsPath().wstring()) &&
                      holds(notice.details, L"Копия: " + registryCopy.wstring()),
                  "сообщение называет файл и копию");
            check(holds(notice.details, L"Первое же сохранение обложки в мастере или удаление обложки перепишет "
                                        L"skins.xml исправленными кромками, а пропущенные обложки из него исчезнут; "
                                        L"правка рук сохранена в копии."),
                  "сообщение говорит, что станет с правкой и с пропущенными");
        }

        const Skin* const shownSpine = ws.skins.find(u"Сдвинутая");
        const Skin* const shownUnordered = ws.skins.find(u"Беспорядочная");
        const Skin* const shownFolded = ws.skins.find(u"Вывернутая");
        check(ws.skins.list().size() == 6 && shownSpine && shownUnordered && shownFolded &&
                  shownSpine->bottom.x[static_cast<size_t>(reader::EdgeCurve::kSpine)] == reader::EdgeCurve::kSpineX &&
                  PageWarp{*shownUnordered}.flat() && PageWarp{*shownFolded}.flat(),
              "все шесть обложек в реестре, корешок на середине, неверные кромки — ровные");
    }

    {
        // Точки без высоты и лишние у обеих кромок: у верхней по одной, у
        // нижней по две, — слова в числе по каждой кромке.
        Sandbox box("corrected-points");
        Skin incomplete = defaultSkin();
        incomplete.name = u16_text{u"Неполная"};
        incomplete.image = u16_text{u"incomplete.png"};
        Skins incompleteRegistry;
        incompleteRegistry.put(incomplete);
        std::string incompleteXml = incompleteRegistry.toXml();
        check(handEditCurve(incompleteXml, "<top>", {1}, 1) && handEditCurve(incompleteXml, "<bottom>", {2, 3}, 2),
              "реестр поправлен руками: высоты стёрты, лишние точки дописаны");
        Workspace ws(box.root, never);
        putOnDisk(ws.skinsPath(), incompleteXml);

        const Started started = run(ws.start(never));

        check(started.notices.size() == 1 &&
                  holds(started.notices[0].details,
                        L"Обложка «Неполная» в skins.xml: у точки верхней кромки нет координаты — на её месте точка "
                        L"начальной прямой; у точек нижней кромки нет координат — на их местах точки начальной "
                        L"прямой; у верхней кромки лишняя точка — отброшена; у нижней кромки лишние точки — "
                        L"отброшены.\n"),
              "точки без координаты и лишние — по кромке, в единственном у одной точки, во множественном у двух");
        check(ws.skins.list().size() == 1 && ws.skins.list()[0].top.y == defaultSkin().top.y &&
                  ws.skins.list()[0].bottom.y == defaultSkin().bottom.y,
              "на местах точек без высоты — точки начальной прямой, лишних нет");
    }

    {
        Sandbox box("corrected-one");
        Skins oneEdited;
        oneEdited.put(good);
        oneEdited.put(folded);
        Workspace ws(box.root, never);
        putOnDisk(ws.skinsPath(), oneEdited.toXml());

        const Started started = run(ws.start(never));

        check(started.notices.size() == 1 &&
                  started.notices[0].headline == L"Обложка из skins.xml исправлена при чтении",
              "одна исправленная обложка — заголовок в единственном");
        check(started.notices.size() == 1 &&
                  holds(started.notices[0].details, L"перепишет skins.xml исправленными кромками; правка рук "
                                                    L"сохранена в копии.") &&
                  !holds(started.notices[0].details, L"пропущенные"),
              "без пропущенных — о них ни слова");
    }

    {
        Sandbox box("corrected-skipped");
        Skins onlySkipped;
        onlySkipped.put(good);
        onlySkipped.put(imageless);
        Workspace ws(box.root, never);
        putOnDisk(ws.skinsPath(), onlySkipped.toXml());

        const Started started = run(ws.start(never));

        check(started.notices.size() == 1 &&
                  holds(started.notices[0].details, L"перепишет skins.xml, и пропущенные обложки из него исчезнут; "
                                                    L"правка рук сохранена в копии.") &&
                  !holds(started.notices[0].details, L"исправленными кромками"),
              "только пропущенные — о кромках ни слова, о пропущенных — что исчезнут");
        check(started.notices.size() == 1 &&
                  started.notices[0].headline == L"Обложка из skins.xml пропущена при чтении",
              "только пропущенные — заголовок о пропуске, а не об исправлении");
    }

    {
        Sandbox box("corrected-none");
        Skins clean;
        clean.put(good);
        Workspace ws(box.root, never);
        putOnDisk(ws.skinsPath(), clean.toXml());

        const Started started = run(ws.start(never));

        check(started.notices.empty() && ws.skins.list().size() == 1, "верный реестр — ни одного сообщения");
        check(!std::filesystem::exists(box.root / L"skins.xml.bad"), "верный реестр — и копии нет");
    }
}

/// Регистрация: запись в реестре, обложка в кэше, повтор — та же запись.
void testWorkspaceRegistersBook() {
    std::printf("\n=== рабочее место: регистрация книги ===\n");

    Sandbox box("register");
    Workspace ws(box.root, never);

    const std::string bytes = onDisk(kBook);
    const fb3::Document document{std::string(bytes)};

    const Registered first = registerFrom(ws, document, kBook, bytes.size());

    check(first.isNew, "первая регистрация — новая книга");
    check(ws.library.cards().size() == 1, "в реестре одна книга");
    check(!first.entry.guid.empty(), "у записи есть guid");
    check(first.entry.path == kBook, "путь записи — путь файла");

    const std::optional<BookCover> bookCover = copyCover(document);
    const CoverBytes cover = coverOf(bookCover, first.entry.guid);
    check(first.entry.cover == cover.name, "имя обложки в записи — имя файла в кэше");
    check(cover.name.empty() || std::filesystem::exists(ws.coverDirectory() / cover.name.wchars()),
          "обложка книги лежит в кэше");
    check(!std::filesystem::exists(ws.libraryPath()), "регистрация сама реестр не пишет");

    const Registered again = registerFrom(ws, document, kBook, bytes.size());

    check(!again.isNew, "повторная регистрация — не новая");
    check(again.entry.guid == first.entry.guid, "повторная регистрация — тот же guid");
    check(ws.library.cards().size() == 1, "реестр не раздвоился");
}

/// Реестр потерян, а настройки помнят guid последней книги: запись заводится
/// под ним, и место чтения находится; занятый guid не берётся.
void testWorkspaceKeepsLastBookGuid() {
    std::printf("\n=== рабочее место: guid последней книги ===\n");

    Sandbox box("remembered");
    Workspace ws(box.root, never);

    const std::string bytes = onDisk(kBook);
    const fb3::Document document{std::string(bytes)};

    const u16_text kept = newGuid();
    ws.settings.lastBookPath = kBook;
    ws.settings.lastBookGuid = kept;

    const Registered restored = registerFrom(ws, document, kBook, bytes.size());

    check(restored.isNew && restored.entry.guid == kept, "потерянная последняя книга — под прежним guid");

    const std::string otherBytes = onDisk(kOtherBook);
    const fb3::Document other{std::string(otherBytes)};

    ws.settings.lastBookPath = kOtherBook;   // guid занят первой книгой
    const Registered fresh = registerFrom(ws, other, kOtherBook, otherBytes.size());

    check(fresh.isNew && fresh.entry.guid != kept, "занятый guid не берётся — новый");
    check(ws.library.cards().size() == 2, "две разные книги");
}

/// Открытие: реестр и настройки в памяти указывают на книгу, а файлов их
/// открытие не пишет — пишет вызывающий; записанные, они ведут к книге;
/// состояние читается и пишется, последняя книга находится.
void testWorkspaceOpensBook() {
    std::printf("\n=== рабочее место: открытие книги ===\n");

    Sandbox box("open");
    Workspace ws(box.root, never);

    const LoadedBook loaded = run(ws.readBook(kBook, never));

    check(loaded.description.title.chars() == "Отцы и дети", "описание книги — при прочитанной, с названием");
    check(!loaded.description.coverImageIndex || (loaded.cover && !loaded.cover->bytes.empty()),
          "обложка, которую описание называет, — при прочитанной, с байтами");
    check(loaded.fileSize == std::filesystem::file_size(kBook), "размер файла — настоящий");

    // Тела сносок Тургенева открываются заголовком-номером: в хранилище они
    // лежат за потоком чтения, и в оглавление не попадают.
    const typography::BookText& text = loaded.book->text();
    check(text.flowCount > 0 && text.blocks.size() > text.flowCount && !text.noteBodies.empty(),
          "книга развёрнута: поток чтения, за ним тела сносок");
    size_t flowTitles = 0;
    for (const typography::Block& block : text.flow())
        if (block.kind == typography::BlockKind::Title && block.paragraph.text.count != 0) ++flowTitles;
    check(reader::contentsOf(text).size() == flowTitles, "оглавление — заголовки потока чтения, и только они");

    const Opened opened = run(openIn(ws, loaded, never));

    check(opened.state.charOffset == 0 && opened.state.bookmarks.empty(), "новая книга — с чистого листа");
    check(ws.settings.lastBookGuid == opened.entry.guid && ws.settings.lastBookPath == kBook,
          "настройки указывают на книгу");
    check(ws.library.cards().size() == 1 && ws.library.cards()[0]->entry.guid == opened.entry.guid,
          "книга в реестре");
    check(!std::filesystem::exists(ws.libraryPath()) && !std::filesystem::exists(ws.settingsPath()),
          "реестр и настройки открытие меняет в памяти — файлов не пишет");

    run(ws.saveLibrary());
    run(ws.saveSettings());

    Library onDiskLibrary;
    check(onDiskLibrary.loadFrom(onDisk(ws.libraryPath())) && onDiskLibrary.cards().size() == 1 &&
              onDiskLibrary.cards()[0]->entry.guid == opened.entry.guid,
          "записанный реестр — с книгой");
    Settings onDiskSettings;
    check(readSettings(onDisk(ws.settingsPath()), onDiskSettings) &&
              onDiskSettings.lastBookGuid == opened.entry.guid,
          "записанные настройки указывают на книгу");

    BookState state;
    state.charOffset = 12345;
    run(ws.saveState(opened.entry.guid, state));

    const Opened reopened = run(openIn(ws, loaded, never));

    check(reopened.entry.guid == opened.entry.guid, "повторное открытие — та же запись");
    check(reopened.state.charOffset == 12345, "повторное открытие — то же место");
    check(run(ws.lastBookPath(never)) == kBook, "последняя книга — она");

    Workspace later(box.root, never);
    const Started started = run(later.start(never));
    check(started.notices.empty() && started.lastBook == kBook, "следующий запуск продолжит её");
}

/// Обход каталога: книги регистрируются по одной, о каждой новой говорят
/// сразу, мусор пропускается; реестр меняется в памяти, а файл его пишет
/// вызывающий; повтор ничего не добавляет.
void testWorkspaceAddsFolder() {
    std::printf("\n=== рабочее место: обход каталога ===\n");

    Sandbox box("folder");
    Workspace ws(box.root, never);

    const std::filesystem::path folder = box.root / L"shelf";
    std::filesystem::create_directories(folder);
    std::filesystem::copy_file(kBook, folder / kBook.filename());
    std::filesystem::copy_file(kOtherBook, folder / kOtherBook.filename());
    putOnDisk(folder / L"not-a-book.fb3", "this is not a book");
    putOnDisk(folder / L"note.txt", "and this is not even fb3");

    // Полка слышит обход сама: карточка за карточкой, по мере разбора.
    std::vector<list_change> heard;
    event_token watch = ws.library.cards().on_change(
        [&heard](const list_change& change) noexcept { heard.push_back(change); });

    const Scanned first = scanned(ws, ws.addFolder(folder, never));

    check(first.listed == 3 && first.queued == 2 && first.added == 2 && first.changed(),
          "каталог с книгами: три файла *.fb3, две книги прочитаны и добавлены");
    check(first.unread.empty(), "все файлы прочитались");
    check(ws.library.cards().size() == 2, "две книги, мусор пропущен");
    check(ws.library.folders().size() == 1 && ws.library.folders()[0] == folder, "каталог — наблюдаемый");
    check(heard.size() == 2 && heard[0] == list_change{list_change::inserted, 0, 1} &&
              heard[1] == list_change{list_change::inserted, 1, 1},
          "полка растёт по одной карточке в конец");

    check(!std::filesystem::exists(ws.libraryPath()), "файла реестра обход не пишет");

    run(ws.saveLibrary());

    Library onDiskLibrary;
    check(onDiskLibrary.loadFrom(onDisk(ws.libraryPath())) && onDiskLibrary.cards().size() == 2,
          "записанный реестр — две книги");

    const Scanned second = scanned(ws, ws.addFolder(folder, never));

    check(second.unread.empty() && second.queued == 0 && ws.library.cards().size() == 2,
          "повторный обход не множит записи и неизменного не читает");
    check(heard.size() == 2, "повторный обход — ни одной новой карточки, полка не тронута");
    check(ws.library.folders().size() == 1, "каталог, добавленный снова, — тот же");

    ws.library.cards().remove_change(watch);

    bool refused = false;
    try {
        run(ws.addFolder(box.root / L"nowhere", never));
    } catch (const last_error&) {
        refused = true;
    }
    check(refused && ws.library.folders().size() == 1, "каталог, которого нет, — сбой, и реестр его не запоминает");
}

/// Книга, у которой испорчены сжатые байты части: первый байт потока deflate
/// — блок запрещённого типа 3, и разжать часть нельзя. Часть ищется по имени
/// в локальных заголовках архива; не нашлась или не сжата — пусто.
std::string withBrokenPart(std::string bytes, std::string_view name) {
    const auto word = [&bytes](size_t at) {
        return uint32_t(uint8_t(bytes[at])) | uint32_t(uint8_t(bytes[at + 1])) << 8;
    };
    const std::string_view signature("PK\x03\x04", 4);
    for (size_t at = bytes.find(signature); at != std::string::npos && at + 30 <= bytes.size();
         at = bytes.find(signature, at + signature.size())) {
        const size_t nameAt = at + 30;
        const size_t data = nameAt + word(at + 26) + word(at + 28);
        if (data >= bytes.size()) break;
        if (word(at + 8) == 8 && std::string_view(bytes).substr(nameAt, word(at + 26)) == name) {
            bytes[data] = '\xFF';
            return bytes;
        }
    }
    return {};
}

/// Книга FB3, которая не разобралась или чья обложка не прочиталась из пакета,
/// — в непрочитанных обхода с путём и причиной, а обход идёт дальше: соседняя
/// книга встаёт на полку. Чужой файл с расширением .fb3 — не zip —
/// пропускается молча. Обложку разбор принимает — байтов картинок он не читает,
/// — сжатые связи тела читает на шаге картинок, связи пакета — на шаге связи
/// Book, а обрезанной книге, у которой нет конца каталога zip, отказывает
/// раньше Packaging API.
void testWorkspaceScanNamesBrokenBooks() {
    std::printf("\n=== рабочее место: битые книги в обходе ===\n");

    Sandbox box("broken-books");
    Workspace ws(box.root, never);

    // Обложка романа — его миниатюра fb3/img/cover.jpg, она же img1 тела.
    const std::string brokenCover = withBrokenPart(onDisk(kBook), "fb3/img/cover.jpg");
    const std::string brokenRelationships = withBrokenPart(onDisk(kBook), "fb3/_rels/body.xml.rels");
    const std::string brokenPackageRelationships = withBrokenPart(onDisk(kBook), "_rels/.rels");
    check(!brokenCover.empty() && !brokenRelationships.empty() && !brokenPackageRelationships.empty(),
          "сжатые обложка, связи тела и связи пакета романа найдены и испорчены");
    const std::string novel = onDisk(kBook);

    const std::filesystem::path folder = shelfOf(box, {kOtherBook});
    putOnDisk(folder / L"broken-cover.fb3", brokenCover);
    putOnDisk(folder / L"broken-rels.fb3", brokenRelationships);
    putOnDisk(folder / L"broken-package-rels.fb3", brokenPackageRelationships);
    putOnDisk(folder / L"truncated.fb3", novel.substr(0, novel.size() / 2));
    putOnDisk(folder / L"text.fb3", "Это текст, а не книга, хоть и назван .fb3: ни zip, ни пакета OPC в нём нет.");

    const Scanned report = scanned(ws, ws.addFolder(folder, never));
    check(report.listed == 6 && report.queued == 1 && report.refused.empty() && ws.library.cards().size() == 1,
          "обход идёт дальше: соседняя книга на полке, битые и чужой файл регистрации не отданы");

    const auto lineAbout = [&report](std::wstring_view file, std::wstring_view reason) {
        return std::ranges::count_if(report.unread, [file, reason](const std::wstring& line) {
            return holds(line, std::wstring(file) + L" — ") && holds(line, reason);
        });
    };
    check(report.unread.size() == 4 && lineAbout(L"broken-cover.fb3", L"HRESULT 0x") == 1,
          "книга с битой обложкой — в непрочитанных, с путём и кодом отказа");
    check(lineAbout(L"broken-rels.fb3", L"HRESULT 0x") == 1,
          "книга с битыми связями тела — в непрочитанных, с путём и кодом");
    check(lineAbout(L"broken-package-rels.fb3", L"HRESULT 0x") == 1,
          "книга с битыми связями пакета — в непрочитанных, с путём и кодом, а не молча");
    check(lineAbout(L"truncated.fb3", L"нет конца каталога zip") == 1,
          "обрезанная книга — в непрочитанных: нет конца каталога zip");
    check(lineAbout(L"text.fb3", L"") == 0, "чужой файл .fb3 — молча");
}

/// Наблюдаемый каталог: у книг добавленного — каталог и время правки из
/// листинга, и они переживают запись реестра; перепроверка неизменного не
/// читает и обложек не пишет, как и открытие той же книги; файл с другим
/// временем перечитывается и обновляет запись и обложку; пропавший
/// помечается, вернувшийся — снимает пометку не читая, новый встаёт в конец;
/// пропавший каталог помечает все свои книги.
void testWorkspaceRescansFolders() {
    std::printf("\n=== рабочее место: перепроверка каталога ===\n");

    Sandbox box("watched");
    const std::filesystem::path folder = shelfOf(box, {kBook, kOtherBook});
    const std::filesystem::path book = folder / kBook.filename();
    const std::filesystem::path otherBook = folder / kOtherBook.filename();

    {
        Workspace ws(box.root, never);

        const Scanned added = scanned(ws, ws.addFolder(folder, never));
        check(added.listed == 2 && added.queued == 2 && added.added == 2 && added.marked == 0 && added.unread.empty(),
              "добавленный каталог: обе книги прочитаны и добавлены");

        const BookEntry* entry = ws.library.findFile(book);
        check(entry && entry->folder == folder && entry->modified == std::filesystem::last_write_time(book),
              "у книги — её каталог и время правки из листинга");

        run(ws.saveLibrary());
        const std::string xml = onDisk(ws.libraryPath());
        check(occurrences(xml, "<folder path=") == 1 && occurrences(xml, " folder=") == 2 &&
                  occurrences(xml, " modified=") == 2,
              "в файле реестра — каталог, а у книг их каталог и время");
    }

    // Новый запуск — по файлу реестра: каталог, время и место книг пережили
    // запись.
    Workspace ws(box.root, never);
    const Started started = run(ws.start(never));
    check(started.notices.empty() && ws.library.folders().size() == 1 && ws.library.cards().size() == 2,
          "запуск: каталог и книги — из файла");

    const u16_text guid = ws.library.findFile(book)->guid;

    // Обложка состарена: её перезапись видна наверняка, без надежды на часы.
    const std::filesystem::path cover = ws.coverDirectory() / ws.library.findFile(book)->cover.wchars();
    const std::filesystem::file_time_type aged = std::filesystem::last_write_time(cover) - std::chrono::hours(24);
    std::filesystem::last_write_time(cover, aged);

    std::vector<list_change> heard;
    event_token watch = ws.library.cards().on_change(
        [&heard](const list_change& change) noexcept { heard.push_back(change); });

    const Scanned same = scanned(ws, ws.rescanFolders(never));
    check(same.listed == 2 && same.queued == 0 && same.added == 0 && same.marked == 0 && same.restored == 0 &&
              !same.changed(),
          "без перемен: обе книги в листинге, ни одна не прочитана");
    check(heard.empty() && std::filesystem::last_write_time(cover) == aged,
          "без перемен: полка не тронута, обложка не переписана");

    // Та же книга, открытая с полки, — та же запись: каталог и время остаются,
    // обложку открытие не переписывает.
    {
        const LoadedBook loaded = run(ws.readBook(book, never));
        const Opened opened = run(openIn(ws, loaded, never));
        check(opened.entry.guid == guid && opened.entry.folder == folder &&
                  opened.entry.modified == std::filesystem::last_write_time(book),
              "открытая книга — та же запись, при своём каталоге и времени");
        check(heard.empty() && std::filesystem::last_write_time(cover) == aged,
              "открытие той же книги: полка не тронута, обложка не переписана");
    }

    // Другое время правки при тех же байтах — для перепроверки другой файл.
    const std::filesystem::file_time_type touched = touch(book);
    const Scanned retouched = scanned(ws, ws.rescanFolders(never));
    check(retouched.queued == 1 && retouched.added == 0 && retouched.changed(),
          "время правки сменилось — книга перечитана");
    check(ws.library.findFile(book)->guid == guid && ws.library.findFile(book)->modified == touched &&
              heard.size() == 1 && heard[0].kind == list_change::replaced,
          "запись — с новым временем, под прежним guid: замена своей карточки");
    check(std::filesystem::last_write_time(cover) != aged, "у изменённой записи обложка записана заново");

    // Файла нет — «не найдена»: и в карточке, и в файле реестра.
    const std::filesystem::path away = box.root / L"away.fb3";
    std::filesystem::rename(otherBook, away);
    heard.clear();

    const Scanned gone = scanned(ws, ws.rescanFolders(never));
    ShelfCard* const otherCard = cardAt(ws.library, otherBook);
    check(gone.listed == 1 && gone.marked == 1 && gone.queued == 0 && gone.changed(),
          "файла нет — запись помечена, ничего не прочитано");
    check(otherCard && otherCard->missing().get() && heard.empty(), "помеченная карточка остаётся на полке");

    run(ws.saveLibrary());
    Library onDiskLibrary;
    check(onDiskLibrary.loadFrom(onDisk(ws.libraryPath())) && cardAt(onDiskLibrary, otherBook) &&
              cardAt(onDiskLibrary, otherBook)->missing().get() && !cardAt(onDiskLibrary, book)->missing().get(),
          "пометка пережила запись реестра — у своей книги");

    // Вернулся тот же файл (переименование время правки хранит) — пометка
    // снята, читать нечего.
    std::filesystem::rename(away, otherBook);

    const Scanned back = scanned(ws, ws.rescanFolders(never));
    check(back.restored == 1 && back.marked == 0 && back.queued == 0 && back.changed() && otherCard &&
              !otherCard->missing().get() && heard.empty(),
          "файл вернулся — пометка снята, книга не прочитана");

    // Новый файл — новая книга в конце полки.
    std::filesystem::copy_file(kThirdBook, folder / kThirdBook.filename());

    const Scanned fresh = scanned(ws, ws.rescanFolders(never));
    check(fresh.listed == 3 && fresh.queued == 1 && fresh.added == 1 && heard.size() == 1 &&
              heard[0] == list_change{list_change::inserted, 2, 1},
          "новый файл — новая карточка в конце");

    ws.library.cards().remove_change(watch);

    // Каталога нет вовсе — нет и его книг: все помечены, а вернулся — пометки
    // сняты, и ничего не прочитано.
    const std::filesystem::path hidden = box.root / L"hidden";
    std::filesystem::rename(folder, hidden);

    const Scanned vanished = scanned(ws, ws.rescanFolders(never));
    check(vanished.listed == 0 && vanished.marked == 3 && vanished.unread.empty(),
          "каталога нет — все три книги помечены, сбоя нет");

    std::filesystem::rename(hidden, folder);

    const Scanned returned = scanned(ws, ws.rescanFolders(never));
    check(returned.listed == 3 && returned.restored == 3 && returned.queued == 0,
          "каталог вернулся — пометки сняты, ничего не прочитано");
}

/// Обложка — раньше карточки: в миг, когда полка слышит новую или
/// обновлённую книгу, файл её обложки уже в кэше. Картинка карточки берёт его
/// сразу и сама его не ждёт.
void testWorkspaceCoverBeforeCard() {
    std::printf("\n=== рабочее место: обложка раньше карточки ===\n");

    Sandbox box("cover-first");
    const std::filesystem::path folder = shelfOf(box, {kBook, kOtherBook});
    Workspace ws(box.root, never);

    uint32_t heard = 0;
    uint32_t ready = 0;
    event_token watch = ws.library.cards().on_change([&](const list_change& change) noexcept {
        if (change.kind != list_change::inserted && change.kind != list_change::replaced) return;

        const BookEntry& entry = ws.library.cards()[change.at]->entry;
        ++heard;

        std::error_code ignored;
        if (!entry.cover.empty() && std::filesystem::exists(ws.coverDirectory() / entry.cover.wchars(), ignored))
            ++ready;
    });

    scanned(ws, ws.addFolder(folder, never));
    check(heard == 2 && ready == 2, "новые книги: обложка в кэше раньше карточки");

    // Изменённая — так же. Прежний файл обложки убран: иначе он остался бы от
    // первой регистрации и доказывал бы не то.
    const std::filesystem::path book = folder / kBook.filename();
    std::filesystem::remove(ws.coverDirectory() / ws.library.findFile(book)->cover.wchars());
    touch(book);
    heard = 0;
    ready = 0;

    scanned(ws, ws.rescanFolders(never));
    check(heard == 1 && ready == 1, "изменённая книга: обложка в кэше раньше замены карточки");

    ws.library.cards().remove_change(watch);
}

/// Реестр первой версии — без каталогов и времени правки: читается как есть, а
/// первый обход каталога его книг перечитывает их — под прежними guid, то
/// есть с прежним местом чтения, — и узнаёт время; дальше они не читаются.
void testWorkspaceReadsLibraryV1() {
    std::printf("\n=== рабочее место: реестр первой версии ===\n");

    Sandbox box("library-v1");
    const std::filesystem::path folder = shelfOf(box, {kBook, kOtherBook});
    const std::filesystem::path book = folder / kBook.filename();
    const std::filesystem::path otherBook = folder / kOtherBook.filename();

    // Файл первой версии — тот, что писали до каталогов: книги добавлены по
    // одной, без каталога и времени, а заголовок называет первую версию.
    std::string v1;
    u16_text guid;
    {
        Workspace ws(box.root, never);
        const std::string bytes = onDisk(book);
        const fb3::Document document{std::string(bytes)};
        guid = registerFrom(ws, document, book, bytes.size()).entry.guid;
        const std::string otherBytes = onDisk(otherBook);
        const fb3::Document other{std::string(otherBytes)};
        registerFrom(ws, other, otherBook, otherBytes.size());

        v1 = ws.library.toXml();
        const size_t version = v1.find("version=\"2\"");
        if (version != std::string::npos) v1.replace(version, 11, "version=\"1\"");
    }
    check(occurrences(v1, "<library version=\"1\">") == 1 && occurrences(v1, " modified=") == 0 &&
              occurrences(v1, " folder=") == 0 && occurrences(v1, "<folders>") == 0,
          "файл первой версии: ни каталогов, ни времени");

    Workspace ws(box.root, never);
    putOnDisk(ws.libraryPath(), v1);

    const Started started = run(ws.start(never));
    check(started.notices.empty() && ws.library.cards().size() == 2 && ws.library.folders().empty(),
          "первая версия прочитана без слова читателю");
    check(ws.library.find(guid) && ws.library.find(guid)->modified == kUnknownTime &&
              ws.library.find(guid)->folder.empty(),
          "у записи первой версии время неизвестно и каталога нет");

    const Scanned first = scanned(ws, ws.addFolder(folder, never));
    check(first.listed == 2 && first.queued == 2 && first.added == 0,
          "первый обход перечитывает книги первой версии и новых не заводит");
    check(ws.library.cards().size() == 2 && ws.library.find(guid) &&
              ws.library.find(guid)->modified == std::filesystem::last_write_time(book) &&
              ws.library.find(guid)->folder == folder,
          "под прежним guid — уже со временем правки и каталогом");

    const Scanned second = scanned(ws, ws.rescanFolders(never));
    check(second.listed == 2 && second.queued == 0, "дальше — по времени, без чтения");
}

/// Байты книги с комментарием архива в конце: тот же контейнер и та же книга,
/// а файл длиннее. Комментарий — законный хвост ZIP: его длина — последнее
/// поле записи конца каталога архива, два байта, младший первым. Нет такой
/// записи — пусто, и проба скажет об этом сама.
std::string withArchiveComment(std::string bytes, std::string_view comment) {
    const size_t record = bytes.rfind(std::string_view("PK\x05\x06", 4));
    if (record == std::string::npos || record + 22 > bytes.size()) return {};

    const size_t length = static_cast<unsigned char>(bytes[record + 20]) |
                          static_cast<size_t>(static_cast<unsigned char>(bytes[record + 21])) << 8;
    const size_t grown = length + comment.size();
    bytes[record + 20] = static_cast<char>(grown & 0xFF);
    bytes[record + 21] = static_cast<char>(grown >> 8);
    bytes += comment;
    return bytes;
}

/// Перемену выдаёт и один размер: та же книга (тот же UUID) стала на байт
/// длиннее, а время правки ей возвращено прежнее, — перепроверка её
/// перечитывает, запись остаётся под прежним guid, соседняя книга не
/// читается.
void testWorkspaceRescanSeesSize() {
    std::printf("\n=== рабочее место: другой размер при том же времени ===\n");

    Sandbox box("size-only");
    const std::filesystem::path folder = shelfOf(box, {kBook, kOtherBook});
    const std::filesystem::path book = folder / kBook.filename();

    Workspace ws(box.root, never);
    scanned(ws, ws.addFolder(folder, never));

    const u16_text guid = ws.library.findFile(book)->guid;
    const u16_text bookId = ws.library.findFile(book)->bookId;

    // Байт — комментарием архива, а не просто в конец: хвост за концом архива
    // читает не всякий разбор контейнера, а комментарий — часть формата.
    const std::string original = onDisk(book);
    const std::string grown = withArchiveComment(original, " ");
    check(grown.size() == original.size() + 1, "копия с комментарием архива — на байт длиннее");

    const std::filesystem::file_time_type kept = std::filesystem::last_write_time(book);
    putOnDisk(book, grown);
    std::filesystem::last_write_time(book, kept);
    check(std::filesystem::last_write_time(book) == ws.library.findFile(book)->modified,
          "время правки — прежнее, другой только размер");

    bool sameBook = false;
    try {
        sameBook = run(ws.readBook(book, never)).description.id.to_utf16() == bookId;
    } catch (const std::exception&) {
    }
    check(sameBook, "копия с комментарием архива читается той же книгой — с тем же UUID");

    const Scanned report = scanned(ws, ws.rescanFolders(never));
    check(report.listed == 2 && report.queued == 1 && report.added == 0,
          "другой размер — книга перечитана, соседняя — нет");
    check(ws.library.cards().size() == 2 && ws.library.findFile(book)->guid == guid &&
              ws.library.findFile(book)->fileSize == grown.size(),
          "запись — с новым размером, под прежним guid");
}

/// Другая книга легла на место прежней: тот же путь, другой UUID. Перепроверка
/// её читает и заводит новой — с новым guid и с чистого листа, — а запись
/// прежней убирает вместе с местом чтения: её файла больше нет.
void testWorkspaceRescanTellsReplacedBook() {
    std::printf("\n=== рабочее место: на месте книги — другая ===\n");

    Sandbox box("replaced");
    const std::filesystem::path folder = shelfOf(box, {kBook, kOtherBook});
    const std::filesystem::path book = folder / kBook.filename();

    Workspace ws(box.root, never);
    scanned(ws, ws.addFolder(folder, never));

    // У прежней книги — место чтения и строка прогресса на карточке.
    const u16_text formerGuid = ws.library.findFile(book)->guid;
    BookState read;
    read.charOffset = ws.library.findFile(book)->characterCount / 2;
    run(ws.saveState(formerGuid, read));
    cardAt(ws.library, book)->showState(read);

    std::vector<list_change> heard;
    event_token watch = ws.library.cards().on_change(
        [&heard](const list_change& change) noexcept { heard.push_back(change); });

    std::filesystem::copy_file(kThirdBook, book, std::filesystem::copy_options::overwrite_existing);

    const Scanned report = scanned(ws, ws.rescanFolders(never));
    check(report.listed == 2 && report.queued == 1 && report.added == 1 && report.marked == 0,
          "другая книга по тому же пути — прочитана и добавлена новой");
    check(!ws.library.find(formerGuid) && ws.library.cards().size() == 2, "записи прежней книги нет, книг на полке две");
    check(heard.size() == 2 && heard[0].kind == list_change::erased && heard[1].kind == list_change::inserted,
          "полка: карточка прежней ушла, новая встала в конец");

    const ShelfCard* const card = cardAt(ws.library, book);
    check(card && card->entry.guid != formerGuid && card->entry.fileSize == std::filesystem::file_size(book) &&
              card->entry.folder == folder,
          "новая запись — под новым guid, со своим размером и в своём каталоге");
    check(card && card->progress.get().empty() && run(ws.readState(card->entry.guid, never)).charOffset == 0,
          "новая книга — с чистого листа: ни строки прогресса, ни места чтения");

    ws.library.cards().remove_change(watch);
}

/// Запись с чужим каталогом при тех же размере и времени — не запись этого
/// каталога: перепроверка перечитывает её файл, и запись переходит к своему
/// каталогу под прежним guid; остальные книги не читаются.
void testWorkspaceRescanAdoptsStrayEntry() {
    std::printf("\n=== рабочее место: запись чужого каталога ===\n");

    Sandbox box("stray-entry");
    const std::filesystem::path folder = shelfOf(box, {kBook, kOtherBook});

    // Реестр поправлен руками: у первой книги каталог чужой, а размер и время
    // — настоящие.
    std::string xml;
    {
        Workspace ws(box.root, never);
        scanned(ws, ws.addFolder(folder, never));
        xml = ws.library.toXml();
    }
    const std::string own = " folder=\"" + xmlValue(folder.native()) + "\"";
    const std::string stray = " folder=\"" + xmlValue((box.root / L"elsewhere").native()) + "\"";
    if (const size_t at = xml.find(own); at != std::string::npos) xml.replace(at, own.size(), stray);
    check(occurrences(xml, own) == 1 && occurrences(xml, stray) == 1, "у одной книги каталог чужой");

    Workspace ws(box.root, never);
    putOnDisk(ws.libraryPath(), xml);
    run(ws.start(never));

    const u16_text guid = ws.library.cards()[0]->entry.guid;

    const Scanned report = scanned(ws, ws.rescanFolders(never));
    check(report.listed == 2 && report.queued == 1 && report.added == 0 && report.marked == 0,
          "запись чужого каталога перечитана, своя — нет");
    check(ws.library.cards().size() == 2 && ws.library.cards()[0]->entry.guid == guid &&
              ws.library.cards()[0]->entry.folder == folder,
          "запись перешла к своему каталогу под прежним guid");
}

/// Последняя книга: путь из настроек протух — по guid из реестра; нет и там —
/// пусто.
void testWorkspaceFindsLastBook() {
    std::printf("\n=== рабочее место: последняя книга ===\n");

    Sandbox box("last");
    Workspace ws(box.root, never);

    const std::string bytes = onDisk(kBook);
    const fb3::Document document{std::string(bytes)};
    const Registered registered = registerFrom(ws, document, kBook, bytes.size());

    ws.settings.lastBookPath = box.root / L"moved-away.fb3";
    ws.settings.lastBookGuid = registered.entry.guid;

    check(run(ws.lastBookPath(never)) == kBook, "путь протух — книга найдена по guid");

    ws.settings.lastBookGuid = newGuid();

    check(run(ws.lastBookPath(never)).empty(), "guid неизвестен — продолжать нечего");
}

/// PNG в 68 байт: верный заголовок 65535×65535 в 1 бит на пиксель и начало
/// данных. Бюджет считает его не меньше чем по 4 байта на пиксель — 17 ГБ, —
/// так что ответ «слишком велик», а не «не картинка».
constexpr auto kHugePng = std::string_view{
    "\x89\x50\x4e\x47\x0d\x0a\x1a\x0a\x00\x00\x00\x0d\x49\x48\x44\x52"
    "\x00\x00\xff\xff\x00\x00\xff\xff\x01\x00\x00\x00\x00\x9e\x7e\xe4"
    "\xfd\x00\x00\x00\x0b\x49\x44\x41\x54\x78\x9c\x63\x60\x40\x05\x00"
    "\x00\x10\x00\x01\x39\xbd\x8f\x65\x00\x00\x00\x00\x49\x45\x4e\x44"
    "\xae\x42\x60\x82",
    68};

/// Снимок пробы: JPEG 800×507.
const std::filesystem::path kPhoto = testdata / L"bg_paper2.jpg";

/// Раскодирование на месте — формой книги, своей фабрикой пробы.
DecodedPicture decodedHere(std::string_view bytes) {
    Microsoft::WRL::ComPtr<IWICImagingFactory> wic;
    if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))))
        wxl::core::abort("decodedHere: фабрика WIC");
    return decodeImage(wic.Get(), bytes);
}

/// Пиксели снимка такого размера — со своими байтами и шагом строки w·4.
bool sized(const DecodedPixels& pixels, uint32_t width, uint32_t height) {
    return !pixels.empty() && pixels.width == width && pixels.height == height && pixels.stride == width * 4;
}

/// Картинка или нет — по байтам, а не по имени; картинка больше бюджета
/// памяти — свой отказ, с размером из заголовка, до всякого раскодирования.
/// Вердикт и снимок приходят вместе: снимок — только у картинки.
void testWorkspaceTellsImages() {
    std::printf("\n=== рабочее место: снимок ===\n");

    Sandbox box("image");
    Workspace ws(box.root, never);

    const DecodedPicture photo = run(ws.probeImageAsync(kPhoto, never));
    check(photo.verdict.refusal == PictureRefusal::None && sized(photo.pixels, 800, 507),
          "jpeg — картинка, и снимок при вердикте — 800×507");

    const DecodedPicture book = run(ws.probeImageAsync(kBook, never));
    check(book.verdict.refusal == PictureRefusal::NotAnImage && book.pixels.empty(), "книга — не картинка, снимка нет");

    putOnDisk(box.root / L"huge.png", kHugePng);
    const DecodedPicture huge = run(ws.probeImageAsync(box.root / L"huge.png", never));
    check(huge.verdict.refusal == PictureRefusal::TooLarge && huge.verdict.declaredSize.width == 65535 &&
              huge.verdict.declaredSize.height == 65535 && huge.pixels.empty(),
          "PNG 65535×65535 — слишком велик, с размером из заголовка, снимка нет");
}

/// Раскодирование вне потока вызова (у проб — на рабочем потоке спящей
/// петли; путь системного пула — imaging_pool_tests) отвечает то же, что на
/// месте: картинка — снимком
/// того же размера, уменьшенным до предела вызывающего; книга — «не
/// картинка»; PNG больше бюджета — «слишком велик» с размером из заголовка.
void testDecodeAsyncTellsImages() {
    std::printf("\n=== раскодирование на рабочем потоке петли: вердикты ===\n");

    const std::string photo = onDisk(kPhoto);
    const std::string book = onDisk(kBook);

    const DecodedPicture here = decodedHere(photo);
    const DecodedPicture pooled = run(decodeImageAsync(photo, kMaxPictureSide, never));
    check(pooled.verdict.refusal == PictureRefusal::None && here.verdict.refusal == PictureRefusal::None &&
              sized(pooled.pixels, 800, 507) && sized(here.pixels, 800, 507),
          "jpeg — картинка 800×507, как на месте");

    const DecodedPicture shrunk = run(decodeImageAsync(photo, 400, never));
    check(shrunk.verdict.refusal == PictureRefusal::None && sized(shrunk.pixels, 400, 253),
          "предел вызывающего доходит до тела: 400 по большей стороне — 400×253");

    const DecodedPicture notImage = run(decodeImageAsync(book, kMaxPictureSide, never));
    check(notImage.verdict.refusal == PictureRefusal::NotAnImage && notImage.pixels.empty() &&
              decodedHere(book).verdict.refusal == PictureRefusal::NotAnImage,
          "книга — не картинка, как на месте");

    const DecodedPicture huge = run(decodeImageAsync(std::string{kHugePng}, kMaxPictureSide, never));
    const DecodedPicture hugeHere = decodedHere(kHugePng);
    check(huge.verdict.refusal == PictureRefusal::TooLarge && huge.verdict.declaredSize.width == 65535 &&
              huge.verdict.declaredSize.height == 65535 && huge.pixels.empty() &&
              hugeHere.verdict.refusal == PictureRefusal::TooLarge,
          "PNG 65535×65535 — слишком велик, с размером из заголовка, как на месте");

    const DecodedPicture empty = run(decodeImageAsync(std::string{}, kMaxPictureSide, never));
    check(empty.verdict.refusal == PictureRefusal::NotAnImage && empty.pixels.empty(), "пустые байты — не картинка");
}

/// Раскодирование идёт не на потоке вызова: пока рабочий поток занят, ответа
/// нет, а поток вызова свободен — сам раскодирует снимок на месте; ворота
/// открыты — снимок приходит.
void testDecodeAsyncLeavesCaller() {
    std::printf("\n=== раскодирование на рабочем потоке петли: поток вызова свободен ===\n");

    const std::string photo = onDisk(kPhoto);

    HeldWorker heldWorker;
    task<DecodedPicture> decoding = decodeImageAsync(photo, kMaxPictureSide, never);
    check(!decoding.done(), "рабочий поток занят — раскодирование не кончилось");
    check(sized(decodedHere(photo).pixels, 800, 507) && !decoding.done(),
          "поток вызова тем временем свободен: раскодирует сам, а ответа пула всё нет");

    heldWorker.open();
    const DecodedPicture decoded = run(std::move(decoding));
    check(decoded.verdict.refusal == PictureRefusal::None && sized(decoded.pixels, 800, 507),
          "ворота открыты — снимок пришёл");
}

/// Под отменённым токеном раскодирование не начинается: ответ — отмена сразу,
/// и рабочему потоку ничего не ушло. Отменённое, пока рабочий поток занят,
/// кончает ожидание отменой за один проход петли, не дожидаясь ворот, — а
/// тело сиротеет и
/// остаётся на счету петли, пока поток до него не дойдёт: открыли ворота —
/// сирота вернулась, и на счету ничего.
void testDecodeAsyncStopsOnRequest() {
    std::printf("\n=== раскодирование на рабочем потоке петли: отмена ===\n");

    const std::string photo = onDisk(kPhoto);
    const size_t before = sta_loop::outstanding();

    cancellation_source already;
    already.cancel();
    task<DecodedPicture> refused = decodeImageAsync(photo, kMaxPictureSide, already.token());
    check(refused.done() && sta_loop::outstanding() == before, "под отменённым токеном рабочему потоку ничего не ушло");
    check(canceled(std::move(refused)), "под отменённым токеном — отмена, а не снимок");

    cancellation_source later;
    HeldWorker heldWorker;
    const size_t held = sta_loop::outstanding();
    task<DecodedPicture> decoding = decodeImageAsync(photo, kMaxPictureSide, later.token());
    check(!decoding.done() && sta_loop::outstanding() == held + 1, "раскодирование стоит за занятым потоком");

    // Отмена кладёт ожидающего в ход петли: один проход, который ничего не
    // ждёт, кончает ожидание, ворота ещё закрыты.
    later.cancel();
    sta_loop::run_pending();
    const bool endedAtOnce = decoding.done();
    check(endedAtOnce && canceled(std::move(decoding)),
          "отменено под закрытыми воротами — ожидание кончилось отменой за один проход петли, ворота ещё закрыты");
    check(sta_loop::outstanding() == held + 1, "сирота на счету петли, пока поток до неё не дошёл");

    heldWorker.open();
    sta_loop::run_until([before] { return sta_loop::outstanding() == before; });
    check(sta_loop::outstanding() == before, "ворота открыты — сирота вернулась, на счету петли ничего");
}

/// PNG, у которого цвет говорит номер строки: B — младший байт номера, G —
/// старший, R — номер, сведённый к 0…255 (y·255/(h−1)), альфа 255. По B и G
/// строка узнаётся точно, R меняется плавно и не убывает — его не ломает
/// уменьшение. Сжимается в килобайты, а раскодируется целиком. Кодирует WIC
/// пробы, в памяти; отказ — ошибка пробы, остановка.
std::string rowsPng(uint32_t width, uint32_t height) {
    using Microsoft::WRL::ComPtr;
    const auto must = [](HRESULT result, const char* what) {
        if (FAILED(result)) wxl::core::abort(what);
    };

    ComPtr<IWICImagingFactory> wic;
    must(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)),
         "rowsPng: фабрика");
    ComPtr<IStream> stream;
    must(::CreateStreamOnHGlobal(nullptr, TRUE, &stream), "rowsPng: поток");
    ComPtr<IWICBitmapEncoder> encoder;
    must(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder), "rowsPng: кодировщик");
    must(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "rowsPng: кодировщик на поток");
    ComPtr<IWICBitmapFrameEncode> frame;
    must(encoder->CreateNewFrame(&frame, nullptr), "rowsPng: кадр");
    must(frame->Initialize(nullptr), "rowsPng: кадр без свойств");
    must(frame->SetSize(width, height), "rowsPng: размер");
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    must(frame->SetPixelFormat(&format), "rowsPng: формат");
    if (format != GUID_WICPixelFormat32bppBGRA) wxl::core::abort("rowsPng: кодировщик сменил формат");

    std::vector<BYTE> row(size_t{width} * 4);
    for (uint32_t y = 0; y < height; ++y) {
        for (size_t at = 0; at < row.size(); at += 4) {
            row[at] = static_cast<BYTE>(y & 0xFF);
            row[at + 1] = static_cast<BYTE>(y >> 8);
            row[at + 2] = static_cast<BYTE>(uint64_t{y} * 255 / (height - 1));
            row[at + 3] = 255;
        }
        must(frame->WritePixels(1, width * 4, width * 4, row.data()), "rowsPng: строка");
    }
    must(frame->Commit(), "rowsPng: кадр записан");
    must(encoder->Commit(), "rowsPng: файл записан");

    STATSTG stat{};
    must(stream->Stat(&stat, STATFLAG_NONAME), "rowsPng: длина");
    must(stream->Seek(LARGE_INTEGER{}, STREAM_SEEK_SET, nullptr), "rowsPng: к началу");
    std::string bytes(static_cast<size_t>(stat.cbSize.QuadPart), '\0');
    ULONG read = 0;
    must(stream->Read(bytes.data(), static_cast<ULONG>(bytes.size()), &read), "rowsPng: чтение");
    if (read != bytes.size()) wxl::core::abort("rowsPng: прочитано меньше записанного");
    return bytes;
}

/// Байты пикселя снимка: B, G, R, A — PBGRA, у непрозрачного как есть.
struct PixelBytes {
    uint8_t b, g, r, a;
};

PixelBytes pixelAt(const DecodedPixels& pixels, uint32_t x, uint32_t y) {
    PixelBytes pixel{};
    std::memcpy(&pixel, pixels.bytes.get() + size_t{y} * pixels.stride + size_t{x} * 4, sizeof pixel);
    return pixel;
}

/// Первая и последняя строка каждой полосы снимка такой высоты — по 256 строк.
std::vector<uint32_t> bandEdges(uint32_t height) {
    std::vector<uint32_t> rows;
    for (uint32_t top = 0; top < height; top += 256) {
        rows.push_back(top);
        rows.push_back(std::min(top + 256, height) - 1);
    }
    return rows;
}

/// Кадр копируется полосами, каждая — на своё место и до конца: у снимка
/// 4000×3000 — двенадцать полос (одиннадцать по 256 строк и хвост в 184); в
/// первой и последней строке каждой полосы, у обоих краёв, по B и G узнаётся
/// ровно эта строка. Уменьшенный до 1000 — 1000×750, три полосы через
/// уменьшение: точного равенства Fant не обещает, поэтому R по строкам не
/// убывает, а на краях полос — в двух единицах от среднего четырёх строк
/// исходника. На рабочем потоке петли — то же, что на месте.
void testDecodeCopiesAllBands() {
    std::printf("\n=== раскодирование полосами: все строки ===\n");

    const std::string png = rowsPng(4000, 3000);

    const auto rowsInPlace = [](const DecodedPixels& picture, uint32_t width, uint32_t height) {
        if (!sized(picture, width, height)) return false;
        for (const uint32_t y : bandEdges(height)) {
            for (const uint32_t x : {0u, width - 1}) {
                const PixelBytes pixel = pixelAt(picture, x, y);
                if (pixel.b != (y & 0xFF) || pixel.g != (y >> 8) || pixel.a != 255) return false;
            }
        }
        return true;
    };

    const DecodedPicture here = decodedHere(png);
    check(here.verdict.refusal == PictureRefusal::None && rowsInPlace(here.pixels, 4000, 3000),
          "4000×3000 на месте — края всех двенадцати полос на своих строках");

    const DecodedPicture pooled = run(decodeImageAsync(png, kMaxPictureSide, never));
    check(pooled.verdict.refusal == PictureRefusal::None && rowsInPlace(pooled.pixels, 4000, 3000),
          "4000×3000 на рабочем потоке петли — так же");

    const DecodedPicture shrunk = run(decodeImageAsync(png, 1000, never));
    bool rising = sized(shrunk.pixels, 1000, 750);
    for (uint32_t y = 1; rising && y < 750; ++y) {
        for (const uint32_t x : {0u, 999u})
            rising = rising && pixelAt(shrunk.pixels, x, y).r >= pixelAt(shrunk.pixels, x, y - 1).r;
    }
    bool nearSource = sized(shrunk.pixels, 1000, 750);
    for (const uint32_t y : bandEdges(750)) {
        // Строка уменьшенного — четыре строки исходника, 4y…4y+3: R их
        // середины, 4y+1,5, сведённый к 0…255.
        const double expected = (4.0 * y + 1.5) * 255.0 / 2999.0;
        for (const uint32_t x : {0u, 999u})
            nearSource = nearSource && std::abs(pixelAt(shrunk.pixels, x, y).r - expected) <= 2.0;
    }
    check(shrunk.verdict.refusal == PictureRefusal::None && rising && nearSource,
          "уменьшенный до 1000 — 1000×750 полосами через уменьшение: R не убывает, края полос — у своих строк");
}

/// Тело копирует полосы по порядку и между ними спрашивает, не осиротело ли.
/// Ступень проб после полосы (detail::decodeImageAsync) видит все полосы
/// снимка 4000×3000 — с 0-й по 11-ю. Отмена ровно между полосами — без
/// опроса: тело, скопировав первую полосу, встаёт на ступени и будит пробу;
/// проба отменяет токен (на своём потоке: токен — состояние потока STA) —
/// ожидание кончается отменой за один проход петли, который ничего не ждёт,
/// хотя тело ещё стоит, — и отпускает тело:
/// осиротевшее, оно кончается на следующей полосе, второй полосы ступень не
/// видит, и сирота возвращается петле. Ступень ссылается на семафоры и полосы
/// пробы, а проба ждёт возврата сироты, так что они живы, пока тело их
/// трогает.
void testDecodeStopsBetweenBands() {
    std::printf("\n=== раскодирование полосами: отмена между полосами ===\n");

    const std::string png = rowsPng(4000, 3000);
    const size_t before = sta_loop::outstanding();

    // Полосы записывает рабочий поток, читает проба — после возврата тела,
    // который приходит каналом петли.
    std::vector<uint32_t> bands;
    const DecodedPicture counted = run(bukvitsa::reader::detail::decodeImageAsync(
        png, kMaxPictureSide, never, [&bands](uint32_t band) { bands.push_back(band); }));
    bool inOrder = bands.size() == 12;
    for (size_t at = 0; inOrder && at < bands.size(); ++at) inOrder = bands[at] == at;
    check(inOrder && sized(counted.pixels, 4000, 3000), "3000 строк — двенадцать полос по порядку, снимок целиком");

    std::vector<uint32_t> orphanBands;
    std::binary_semaphore firstBandCopied{0};
    std::binary_semaphore mayGoOn{0};
    cancellation_source source;
    task<DecodedPicture> decoding = bukvitsa::reader::detail::decodeImageAsync(
        png, kMaxPictureSide, source.token(), [&orphanBands, &firstBandCopied, &mayGoOn](uint32_t band) {
            orphanBands.push_back(band);
            if (band != 0) return;
            firstBandCopied.release();
            mayGoOn.acquire();
        });

    firstBandCopied.acquire();
    source.cancel();
    sta_loop::run_pending();
    const bool endedAtOnce = decoding.done();
    check(endedAtOnce && canceled(std::move(decoding)),
          "отменено после первой полосы — ожидание кончилось отменой за один проход петли, тело ещё стоит");
    check(sta_loop::outstanding() == before + 1, "сирота на счету петли, пока тело стоит");

    mayGoOn.release();
    sta_loop::run_until([before] { return sta_loop::outstanding() == before; });
    check(orphanBands.size() == 1 && orphanBands[0] == 0,
          "осиротевшее тело кончилось на следующей полосе: ступень видела одну первую");
    check(sta_loop::outstanding() == before, "сирота вернулась — на счету петли ничего");
}

/// Проба снимка — одно чтение и одно раскодирование, и оба — операциями
/// петли: после чтения уходит ровно одна операция, раскодирование, и после
/// неё проба кончается снимком с вердиктом — третьей операции нет.
void testProbeDecodesOnce() {
    std::printf("\n=== проба снимка: один проход раскодирования ===\n");

    Sandbox box("probe-once");
    Workspace ws(box.root, never);

    const size_t before = sta_loop::outstanding();
    task<DecodedPicture> probing = ws.probeImageAsync(kPhoto, never);
    check(!probing.done() && sta_loop::outstanding() == before + 1, "проба ждёт чтения — одна операция");

    AnsweredWorker{}.deliver();
    check(!probing.done() && sta_loop::outstanding() == before + 1,
          "байты пришли — ушла одна операция, раскодирование, и проба ждёт её");

    AnsweredWorker{}.deliver();
    check(probing.done() && sta_loop::outstanding() == before,
          "раскодирование ответило — проба кончилась, других операций не было");

    const DecodedPicture probed = run(std::move(probing));
    check(probed.verdict.refusal == PictureRefusal::None && sized(probed.pixels, 800, 507),
          "вердикт и снимок пришли вместе");
}

/// Место чтения пишется под guid мест книги, а не последней книги из
/// настроек: открытие другой книги ставит её в настройки раньше, чем полоса и
/// места её возьмут, и всё это время место на полосе — прежней книги.
void testStateGoesUnderPlacesGuid() {
    std::printf("\n=== место чтения — под guid мест книги ===\n");

    Sandbox box("places");
    Workspace ws(box.root, never);

    const LoadedBook first = run(ws.readBook(kBook, never));
    Opened firstOpened = run(openIn(ws, first, never));

    BookPlaces places;
    places.open(firstOpened.entry.guid, first.book->text(), std::move(firstOpened.state.bookmarks));
    places.toggleBookmark(500, u16_text{u"пятьсот"});

    const LoadedBook second = run(ws.readBook(kOtherBook, never));
    const Opened secondOpened = run(openIn(ws, second, never));

    check(ws.settings.lastBookGuid == secondOpened.entry.guid, "настройки уже указывают на новую книгу");
    check(places.guid() == firstOpened.entry.guid, "места — ещё прежней: открытие их не трогает");

    run(ws.saveState(places.guid(), places.stateAt(777)));

    const BookState kept = run(ws.readState(firstOpened.entry.guid, never));
    check(kept.charOffset == 777 && kept.bookmarks.size() == 1 && kept.bookmarks[0].charOffset == 500,
          "место и закладки прежней — в её файле");
    check(!std::filesystem::exists(ws.statePath(secondOpened.entry.guid)), "файл новой книги не тронут");
}

// ---- Отмена: чтения под токеном, записи до конца ----------------------------

/// Чтение под отменённым токеном не начинается: файла, которого нет, оно и
/// не ищет — кончается отменой, а не «файла нет»; начатое кончается отменой,
/// хотя байты и пришли.
void testWorkspaceReadsStopOnRequest() {
    std::printf("\n=== отмена: чтения ===\n");

    Sandbox box("stop-reads");
    Workspace ws(box.root, never);

    cancellation_source source;
    source.cancel();
    const cancellation_token token = source.token();

    const std::filesystem::path missing = box.root / L"missing.fb3";

    check(canceled(ws.readBook(missing, token)), "книга: не начинается — отмена, а не «файла нет»");
    check(canceled(ws.probeImageAsync(missing, token)), "снимок: не начинается");
    check(canceled(ws.addFolder(box.root / L"missing", token)), "каталог: не начинается — отмена, а не «нет каталога»");
    check(canceled(ws.scanFolder(box.root / L"missing", token)),
          "перепроверка каталога: не начинается — отмена, а не «каталога нет»");
    check(canceled(ws.rescanFolders(token)), "перепроверка всех каталогов: не начинается — и когда их нет");

    const u16_text guid = newGuid();
    check(canceled(ws.readState(guid, token)), "состояние: не начинается — отмена, а не чистый лист");

    ws.settings.lastBookPath = missing;
    check(canceled(ws.lastBookPath(token)), "последняя книга: не начинается — отмена, а не «продолжать нечего»");

    cancellation_source later;
    task<LoadedBook> reading = ws.readBook(kBook, later.token());
    check(!reading.done(), "чтение книги ушло к диску");
    later.cancel();
    check(canceled(std::move(reading)), "попросили посреди чтения — отмена, а не книга");
}

/// Запуск: настройки, обложки и реестр читаются и под отменённым токеном —
/// из них потом пишется всё, — а поиск книги для продолжения уже нет.
void testWorkspaceStartReadsModelsWhole() {
    std::printf("\n=== отмена: запуск ===\n");

    Sandbox box("stop-start");

    {
        Workspace ws(box.root, never);
        ws.settings.fontSize.set(31.0);
        ws.settings.lastBookGuid = newGuid();
        ws.settings.lastBookPath = kBook;
        run(ws.saveSettings());
    }

    Workspace ws(box.root, never);

    cancellation_source source;
    source.cancel();

    check(canceled(ws.start(source.token())), "запуск кончился отменой — на поиске книги");
    check(ws.settings.fontSize.get() == 31.0 && ws.settings.lastBookPath == kBook,
          "настройки прочитаны целиком: запись при закрытии окна не ляжет умолчаниями");
}

/// Открытие под отменённым токеном не трогает ничего — ни модели, ни диска;
/// отмена, пришедшая на записи обложки, книгу в реестр ставит — шаг, взявший
/// замок, доходит до реестра, — а открытие её записи не начинается и настроек
/// не трогает; отмена на чтении состояния
/// кончает открытие, когда настройки уже указывают на книгу. Файлов реестра и
/// настроек открытие не пишет ни так, ни так.
void testWorkspaceOpenStopsOnStateRead() {
    std::printf("\n=== отмена: открытие книги ===\n");

    Sandbox box("stop-open");
    Workspace ws(box.root, never);

    const LoadedBook loaded = run(ws.readBook(kBook, never));

    cancellation_source before;
    before.cancel();

    check(canceled(openIn(ws, loaded, before.token())), "под отменённым токеном — отмена");
    check(ws.library.cards().empty() && ws.settings.lastBookGuid.empty(),
          "под отменённым токеном модель не тронута, книга не регистрировалась");
    check(!std::filesystem::exists(ws.libraryPath()) && !std::filesystem::exists(ws.settingsPath()) &&
              !std::filesystem::exists(ws.coverDirectory()),
          "под отменённым токеном на диске ничего");

    // Шаг регистрации, взявший замок, отмена открытия не делит: попросили,
    // пока пишется обложка, — книга встаёт в реестр, а открытие записи под
    // отменённым токеном не начинается и настроек не трогает.
    cancellation_source during;
    task<Opened> opening = openIn(ws, loaded, during.token());
    check(!opening.done() && ws.library.cards().empty(), "шаг регистрации открытия стоит на записи обложки");
    during.cancel();

    check(canceled(std::move(opening)), "попросили на записи обложки — открытие кончается отменой");
    check(ws.library.cards().size() == 1 && ws.settings.lastBookGuid.empty(),
          "модель: книга в реестре, настройки на неё не указывают");
    check(!std::filesystem::exists(ws.libraryPath()) && !std::filesystem::exists(ws.settingsPath()),
          "файлов реестра и настроек открытие не пишет — их пишет вызывающий");

    // Состояние: попросили на его чтении — открытие кончается отменой, и
    // настройки на книгу не указывают: их ставит конец открытия, после
    // последнего ожидания. Операция чтения
    // уходит рабочему потоку в самом вызове, и свободный поток кончает её раньше
    // отмены: файла состояния нет, и отказ CreateFileW приходит сразу. Оба
    // порядка — порознь и наверняка: чтение, которое держат не начатым, и
    // чтение, которое уже ответило.
    //
    // Не начатое: поток занят, чтение стоит за ним, отмена приходит раньше, и
    // поток, дойдя до чтения, его не начинает — отменённое до начала не
    // исполняется и отвечает отменой. Поток занимают, когда шаг регистрации
    // уже кончился: шаг с записью обложки ждал бы её за занятым потоком, и
    // конец шага не пришёл бы.
    cancellation_source reading;
    {
        const Registered reregistered =
            run(ws.registerBook(loaded.description, loaded.book->characterCount(), loaded.cover,
                                BookFile{.path = loaded.book->path(), .size = loaded.fileSize}, never));
        check(!reregistered.isNew && !reregistered.isChanged, "та же запись — шаг без записи обложки");

        HeldWorker heldWorker;
        task<Opened> reopening = ws.openBook(reregistered.entry, reading.token());
        check(!reopening.done(), "открытие стоит на чтении состояния");
        check(ws.settings.lastBookGuid.empty() && ws.settings.lastBookPath.empty(),
              "пока открытие стоит на чтении состояния, настройки ещё не указывают на книгу");
        reading.cancel();
        heldWorker.open();

        check(canceled(std::move(reopening)), "отмена раньше чтения состояния — открытие кончается отменой");
        check(ws.settings.lastBookGuid.empty() && ws.settings.lastBookPath.empty(),
              "отмена раньше чтения состояния: настройки на книгу не указывают — их ставит конец открытия");
    }

    // Ответившее: чтение кончилось — «файла нет», чистый лист в обратном
    // канале, — и только потом попросили. Ответ, успевший раньше отмены, —
    // ответ, но открытие после отмены дальше не идёт, и до настроек оно не
    // доходит.
    cancellation_source lateStop;
    task<Opened> answeredOpening = openIn(ws, loaded, lateStop.token());
    check(!answeredOpening.done(), "открытие стоит на чтении состояния");
    AnsweredWorker answeredState;
    lateStop.cancel();
    answeredState.deliver();

    check(canceled(std::move(answeredOpening)), "чтение ответило раньше отмены — открытие всё равно кончается отменой");
    check(ws.settings.lastBookGuid.empty() && ws.settings.lastBookPath.empty(),
          "чтение ответило раньше отмены: настройки на книгу не указывают — открытие до них не дошло");
}

/// Листинг каталога ответил раньше отмены — и только потом попросили: обход и
/// добавление кончаются отменой, не сведя листинг с реестром. Перепроверка не
/// ставит пометок «не найдена», добавление не запоминает каталог.
void testWorkspaceScanStopsAfterListing() {
    std::printf("\n=== отмена: листинг каталога уже ответил ===\n");

    Sandbox box("stop-listing");
    const std::filesystem::path folder = shelfOf(box, {kBook});
    Workspace ws(box.root, never);
    scanned(ws, ws.addFolder(folder, never));

    // Файл ушёл — перепроверка пометила бы его запись «не найдена».
    std::filesystem::remove(folder / kBook.filename());

    cancellation_source rescanStop;
    task<ScanReport> pendingRescan = ws.scanFolder(folder, rescanStop.token());
    check(!pendingRescan.done(), "перепроверка стоит на листинге");
    {
        AnsweredWorker answeredListing;
        rescanStop.cancel();
        answeredListing.deliver();
    }
    check(canceled(std::move(pendingRescan)), "листинг ответил раньше отмены — перепроверка кончается отменой");
    check(ws.library.cards().size() == 1 && !ws.library.cards()[0]->missing().get(),
          "пометки «не найдена» нет: листинг с записями не сведён");

    const std::filesystem::path anotherFolder = box.root / L"another";
    std::filesystem::create_directories(anotherFolder);
    std::filesystem::copy_file(kOtherBook, anotherFolder / kOtherBook.filename());

    cancellation_source addingStop;
    task<ScanReport> pendingAddition = ws.addFolder(anotherFolder, addingStop.token());
    check(!pendingAddition.done(), "добавление стоит на листинге");
    {
        AnsweredWorker answeredListing;
        addingStop.cancel();
        answeredListing.deliver();
    }
    check(canceled(std::move(pendingAddition)), "листинг ответил раньше отмены — добавление кончается отменой");
    check(ws.library.folders().size() == 1 && !ws.registering() && ws.library.cards().size() == 1,
          "каталог не запомнен, его книги регистрации не отданы");
}

/// Сохранение обложки: под отменённым токеном не начинается, отмена на
/// чтении снимка ничего не пишет, а с первой записи — копии снимка — оно
/// доходит до строки реестра.
void testWorkspaceSaveSkinStopsBeforeWrites() {
    std::printf("\n=== отмена: сохранение обложки ===\n");

    Sandbox box("stop-skin");
    Workspace ws(box.root, never);

    const std::filesystem::path photo = testdata / L"bg_paper2.jpg";

    Skin skin = defaultSkin();
    skin.name = u16_text{u"Бумага"};

    const auto nothingWritten = [&ws] {
        return ws.skins.list().empty() && !std::filesystem::exists(ws.skinsPath()) &&
               !std::filesystem::exists(ws.skinDirectory());
    };

    cancellation_source before;
    before.cancel();
    check(canceled(ws.saveSkin(skin, photo, before.token())), "под отменённым токеном — отмена");
    check(nothingWritten(), "под отменённым токеном ничего не записано");

    cancellation_source reading;
    task<> onRead = ws.saveSkin(skin, photo, reading.token());
    reading.cancel();
    check(canceled(std::move(onRead)), "попросили на чтении снимка — отмена");
    check(nothingWritten(), "на чтении снимка ничего не записано");

    // Один ход петли — пришло чтение снимка, и сохранение встало на записи
    // его копии. Других операций в полёте нет: каждая проба выше дождалась
    // своих.
    cancellation_source writing;
    task<> onWrite = ws.saveSkin(skin, photo, writing.token());
    while (!sta_loop::run_one()) {
    }
    check(!onWrite.done(), "снимок прочитан, копия пишется");
    writing.cancel();

    check(!canceled(std::move(onWrite)), "попросили на записи — сохранение дошло до конца");
    check(ws.skins.list().size() == 1 && !ws.skins.list()[0].image.empty(), "обложка в реестре и с копией снимка");

    Skins onDiskSkins;
    check(onDiskSkins.loadFrom(onDisk(ws.skinsPath())) && onDiskSkins.list().size() == 1 &&
              onDisk(ws.skinDirectory() / ws.skins.list()[0].image.wchars()) == onDisk(photo),
          "на диске и строка реестра, и копия, на которую она ссылается");
}

/// Удаление обложки под отменённым токеном не начинается: реестр не тронут
/// ни в памяти, ни на диске; без отмены — обложка убрана и там, и там.
void testWorkspaceDeleteSkinStopsBeforeChange() {
    std::printf("\n=== отмена: удаление обложки ===\n");

    Sandbox box("stop-delete-skin");
    Workspace ws(box.root, never);

    Skin skin = defaultSkin();
    skin.name = u16_text{u"Бумага"};
    ws.skins.put(std::move(skin));

    // Реестр на диске — с этой обложкой: начатое удаление переписало бы его.
    const std::string written = ws.skins.toXml();
    putOnDisk(ws.skinsPath(), written);

    cancellation_source before;
    before.cancel();
    check(canceled(ws.deleteSkin(u"Бумага", before.token())), "под отменённым токеном — отмена");
    check(ws.skins.find(u"Бумага") != nullptr && onDisk(ws.skinsPath()) == written,
          "под отменённым токеном реестр обложек не тронут — ни в памяти, ни на диске");

    run(ws.deleteSkin(u"Бумага", never));

    Skins onDiskSkins;
    check(ws.skins.find(u"Бумага") == nullptr && onDiskSkins.loadFrom(onDisk(ws.skinsPath())) &&
              onDiskSkins.list().empty(),
          "без отмены — обложка убрана и в памяти, и на диске");
}

/// Отказ записи реестра обложек: удаление и сохранение кончаются сбоем
/// `last_error`, а реестр в памяти — тот же, что до вызова, с обложками на
/// прежних местах: номера тем считаются по порядку реестра; то, что под
/// тем же именем, пока шла запись, положил другой сценарий, откат не
/// трогает. Отказ — как у читателя: skins.xml только для чтения, и запись,
/// меняющая файл переименованием, получает «отказано в доступе». Потом файл снова можно
/// писать, и удачное сохранение пишет реестр из памяти — с обложкой, чьё
/// удаление не записалось: прежде она к этой записи из памяти уже ушла и
/// пропадала с диска вслед за ней.
void testWorkspaceSkinsWriteRefused() {
    std::printf("\n=== отказ записи реестра обложек ===\n");

    Sandbox box("refused-skins");
    Workspace ws(box.root, never);

    for (const u16_view name : {u16_view{u"Alpha"}, u16_view{u"Beta"}, u16_view{u"Gamma"}, u16_view{u"Delta"}}) {
        Skin skin = defaultSkin();
        skin.name = u16_text{name};
        skin.image = u16_text{u"снимок.jpg"};
        ws.skins.put(std::move(skin));
    }

    const std::string written = ws.skins.toXml();
    putOnDisk(ws.skinsPath(), written);

    // Все три бита записи разом: «только для чтения» у Windows — один
    // атрибут, и MS STL ставит его, лишь когда сняты все три, а снимает,
    // когда добавлен хоть один.
    const auto writable = [&ws](bool yes) {
        constexpr auto anyWrite = std::filesystem::perms::owner_write | std::filesystem::perms::group_write |
                                  std::filesystem::perms::others_write;
        std::filesystem::permissions(ws.skinsPath(), anyWrite,
                                     yes ? std::filesystem::perm_options::add
                                         : std::filesystem::perm_options::remove);
    };

    const auto names = [](const Skins& skins) {
        std::vector<u16_text> list;
        for (const Skin& skin : skins.list()) list.push_back(skin.name);
        return list;
    };
    const std::vector<u16_text> before = names(ws.skins);

    const auto refused = [](task<> work) {
        try {
            run(std::move(work));
        } catch (const last_error&) {
            return true;
        }
        return false;
    };

    writable(false);

    check(refused(ws.deleteSkin(u"Alpha", never)), "удаление: отказ записи — сбой");
    check(names(ws.skins) == before, "удаление не записалось — обложка снова в реестре и на своём месте");
    check(onDisk(ws.skinsPath()) == written, "файл цел");

    // Правка и новая — со снимком, который уже лежит: читать нечего, и
    // отказывает одна запись реестра.
    Skin edited = *ws.skins.find(u"Gamma");
    edited.image = u16_text{u"другой.jpg"};
    check(refused(ws.saveSkin(std::move(edited), {}, never)), "правка: отказ записи — сбой");
    check(names(ws.skins) == before && ws.skins.find(u"Gamma")->image == u"снимок.jpg",
          "правка не записалась — в реестре прежняя обложка");

    Skin refusedNew = defaultSkin();
    refusedNew.name = u16_text{u"Отказ"};
    refusedNew.image = u16_text{u"отказ.jpg"};
    check(refused(ws.saveSkin(std::move(refusedNew), {}, never)), "новая: отказ записи — сбой");
    check(names(ws.skins) == before, "новая не записалась — её нет в реестре");

    // Пока отказывающая запись в пути, под тем же именем ложится более
    // новое — так кладёт его другой сценарий. Откат его не трогает.
    Skin firstEdit = *ws.skins.find(u"Beta");
    firstEdit.image = u16_text{u"первая.jpg"};
    task<> refusedEdit = ws.saveSkin(std::move(firstEdit), {}, never);
    check(!refusedEdit.done(), "правка встала на записи реестра");
    Skin newerEdit = *ws.skins.find(u"Beta");
    newerEdit.image = u16_text{u"новее.jpg"};
    ws.skins.put(std::move(newerEdit));
    check(refused(std::move(refusedEdit)) && ws.skins.find(u"Beta")->image == u"новее.jpg",
          "откат правки не затирает более новую обложку под тем же именем");

    task<> refusedDelete = ws.deleteSkin(u"Delta", never);
    check(!refusedDelete.done(), "удаление встало на записи реестра");
    Skin newerDelta = defaultSkin();
    newerDelta.name = u16_text{u"Delta"};
    newerDelta.image = u16_text{u"новая-delta.jpg"};
    ws.skins.put(std::move(newerDelta));
    check(refused(std::move(refusedDelete)) && ws.skins.find(u"Delta")->image == u"новая-delta.jpg",
          "откат удаления не затирает обложку, заведённую под тем же именем заново");

    check(names(ws.skins) == before, "имена и порядок — прежние");

    writable(true);

    Skin probe = defaultSkin();
    probe.name = u16_text{u"Probe"};
    probe.image = u16_text{u"probe.jpg"};
    run(ws.saveSkin(std::move(probe), {}, never));

    std::vector<u16_text> after = before;
    after.push_back(u16_text{u"Probe"});

    Skins onDiskSkins;
    check(names(ws.skins) == after, "после удачного сохранения в реестре все прежние и новая");
    check(onDiskSkins.loadFrom(onDisk(ws.skinsPath())) && names(onDiskSkins) == after,
          "и на диске: обложка, чьё удаление не записалось, никуда не ушла");
}

/// Дерево источников: отмена родителя доходит до чтения, которое ждёт под
/// токеном дочернего, — так закрытие окна кончает сценарии; отмена дочернего
/// родителя не трогает — так владелец отменяет одну задачу.
void testWorkspaceReadsStopWithParent() {
    std::printf("\n=== отмена: дерево источников ===\n");

    Sandbox box("stop-tree");
    Workspace ws(box.root, never);

    cancellation_source parent;
    auto const child = cancellation_source{parent.token()};

    task<LoadedBook> reading = ws.readBook(kBook, child.token());
    check(!reading.done(), "чтение книги под дочерним токеном ушло к диску");
    parent.cancel();
    check(child.is_canceled(), "отмена родителя отменяет дочерний источник");
    check(canceled(std::move(reading)), "отмена родителя кончает чтение под дочерним токеном");

    check(cancellation_source{parent.token()}.is_canceled(), "под отменённым родителем источник отменён с рождения");

    cancellation_source owner;
    auto piece = cancellation_source{owner.token()};
    piece.cancel();
    check(piece.is_canceled() && !owner.is_canceled(), "отмена дочернего не отменяет родителя");
}

/// Обход каталога, которому сказали кончиться на первой книге: дальше он не
/// читает и кончается отменой, а реестр в памяти с ней остаётся — полка её
/// уже показала, — и записанный вызывающим, он её знает.
void testWorkspaceFolderKeepsWhatShelfShowed() {
    std::printf("\n=== отмена: обход каталога ===\n");

    Sandbox box("stop-folder");
    Workspace ws(box.root, never);

    const std::filesystem::path folder = box.root / L"shelf";
    std::filesystem::create_directories(folder);
    std::filesystem::copy_file(kBook, folder / kBook.filename());
    std::filesystem::copy_file(kOtherBook, folder / kOtherBook.filename());

    // Просьба — в тот миг, когда полка слышит первую книгу. Шага обход не
    // ждёт: запись обложки первой и чтение второй уходят одно за другим. В
    // петле проб рабочий поток один, и его каналы — очереди по порядку: запись
    // обложки кончается раньше чтения, карточка встаёт, отмена приходит, и
    // прочитанное после неё регистрации не отдаётся. Под очередью окна Reader
    // операции идут в пул системы, и там порядок может быть иным — отданное
    // до отмены встаёт: регистрации отданных книг отмена обхода не трогает.
    cancellation_source source;
    uint32_t registered = 0;
    event_token watch =
        ws.library.cards().on_change([&source](const list_change&) noexcept { source.cancel(); });
    event_token registeredWatch =
        ws.bookRegistered().on_change([&registered](const Registered&) noexcept { ++registered; });

    check(canceled(ws.addFolder(folder, source.token())), "обход кончился отменой");
    sta_loop::run_until([&] { return !ws.registering(); });

    ws.library.cards().remove_change(watch);
    ws.bookRegistered().remove_change(registeredWatch);

    check(ws.library.cards().size() == 1 && registered == 1, "следующую книгу не поставили — в реестре одна");
    check(!std::filesystem::exists(ws.libraryPath()), "файла реестра обход не пишет и под отменой");

    run(ws.saveLibrary());

    Library onDiskLibrary;
    check(onDiskLibrary.loadFrom(onDisk(ws.libraryPath())) &&
              onDiskLibrary.cards().size() == ws.library.cards().size() &&
              onDiskLibrary.cards()[0]->entry.guid == ws.library.cards()[0]->entry.guid,
          "записанный реестр — с теми книгами, что полка показала");
}

/// Перепроверка, которой сказали кончиться на первой перечитанной книге:
/// дальше она не ставит и кончается отменой, а реестр в памяти цел — первая
/// книга обновлена, вторая прежняя, обе на полке, — и записанный вызывающим,
/// он читается с обеими и с каталогом.
void testWorkspaceRescanKeepsWhatShelfShowed() {
    std::printf("\n=== отмена: перепроверка каталогов ===\n");

    Sandbox box("stop-rescan");
    const std::filesystem::path folder = shelfOf(box, {kBook, kOtherBook});
    Workspace ws(box.root, never);
    scanned(ws, ws.addFolder(folder, never));

    // Обе изменились — обе пошли бы на чтение.
    touch(folder / kBook.filename());
    touch(folder / kOtherBook.filename());

    // Просьба — в тот миг, когда полка слышит первую обновлённую книгу. Шага
    // перепроверка не ждёт, но в петле проб рабочий поток один, и запись
    // обложки первой кончается раньше чтения второй (как в пробе обхода
    // выше): вторая регистрации не отдаётся. Под пулом Reader порядок может
    // быть иным.
    cancellation_source source;
    uint32_t registered = 0;
    event_token watch =
        ws.library.cards().on_change([&source](const list_change&) noexcept { source.cancel(); });
    event_token registeredWatch =
        ws.bookRegistered().on_change([&registered](const Registered&) noexcept { ++registered; });

    check(canceled(ws.rescanFolders(source.token())), "перепроверка кончилась отменой");
    sta_loop::run_until([&] { return !ws.registering(); });

    ws.library.cards().remove_change(watch);
    ws.bookRegistered().remove_change(registeredWatch);

    uint32_t updated = 0;
    for (const intrusive_ptr<ShelfCard>& card : ws.library.cards())
        if (card->entry.modified == std::filesystem::last_write_time(card->entry.path)) ++updated;
    check(ws.library.cards().size() == 2 && updated == 1 && registered == 1,
          "одна книга обновлена, вторую не поставили, обе на полке");

    run(ws.saveLibrary());

    Library onDiskLibrary;
    check(onDiskLibrary.loadFrom(onDisk(ws.libraryPath())) && onDiskLibrary.cards().size() == 2 &&
              onDiskLibrary.folders().size() == 1,
          "записанный реестр цел: обе книги и каталог");
}

// ---- Замок регистраций -------------------------------------------------------
//
// Регистрации идут по одной, под замком рабочего места, по порядку прихода.
// Шаг первой регистрации пробы стоит на записи обложки, пока петля не
// повернётся, — так «разом» здесь и получается: всё, что начато до хода
// петли, начато, пока шаг держит замок.

/// Сколько файлов лежит в каталоге; нет каталога — ноль.
size_t filesIn(const std::filesystem::path& folder) {
    std::error_code ignored;
    size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(folder, ignored)) {
        static_cast<void>(entry);
        ++count;
    }
    return count;
}

/// Новая книга дважды разом — перепроверка и «Продолжить чтение»: вторая
/// регистрация ждёт замка и узнаёт книгу, которую поставила первая. Guid
/// один, запись одна, обложка в кэше одна, полка слышит одну новую карточку и
/// больше ничего.
void testLockSameBookAtOnce() {
    std::printf("\n=== замок регистраций: одна книга дважды разом ===\n");

    Sandbox box("lock-same");
    Workspace ws(box.root, never);

    const std::string bytes = onDisk(kBook);
    const fb3::Document document{std::string(bytes)};

    std::vector<list_change> heard;
    event_token watch = ws.library.cards().on_change(
        [&heard](const list_change& change) noexcept { heard.push_back(change); });

    task<Registered> rescanning = registrationOf(ws, document, BookFile{.path = kBook, .size = bytes.size()});
    task<Registered> opening = registrationOf(ws, document, BookFile{.path = kBook, .size = bytes.size()});
    check(!rescanning.done() && !opening.done(), "первая стоит на записи обложки, вторая ждёт замка");

    const Registered first = run(std::move(rescanning));
    const Registered second = run(std::move(opening));
    ws.library.cards().remove_change(watch);

    check(first.isNew && !second.isNew && !second.isChanged && second.entry.guid == first.entry.guid,
          "вторая узнала книгу первой: тот же guid, запись не переменилась");
    check(ws.library.cards().size() == 1 && heard.size() == 1 && heard[0].kind == list_change::inserted,
          "запись одна, полка услышала одну новую карточку и больше ничего");
    check(!first.entry.cover.empty() && filesIn(ws.coverDirectory()) == 1 &&
              std::filesystem::exists(ws.coverDirectory() / first.entry.cover.wchars()),
          "в кэше одна обложка — под guid записи");
}

/// Новая книга с двух путей разом — файл и его копия с тем же UUID: вторая
/// регистрация ждёт замка и переводит запись первой на свой файл. Guid у
/// книги один, запись — с файла поздней регистрации, обложка одна, сироты в
/// кэше нет.
void testLockNewBookFromTwoPaths() {
    std::printf("\n=== замок регистраций: новая книга с двух путей разом ===\n");

    Sandbox box("lock-two-paths");
    Workspace ws(box.root, never);

    const std::string bytes = onDisk(kBook);
    const fb3::Document document{std::string(bytes)};
    const std::filesystem::path copy = box.root / L"copy.fb3";
    std::filesystem::copy_file(kBook, copy);

    task<Registered> original = registrationOf(ws, document, BookFile{.path = kBook, .size = bytes.size()});
    task<Registered> copied = registrationOf(ws, document, BookFile{.path = copy, .size = bytes.size()});
    check(!original.done() && !copied.done(), "первая стоит на записи обложки, копия ждёт замка");

    const Registered first = run(std::move(original));
    const Registered later = run(std::move(copied));

    check(first.isNew && !later.isNew && later.isChanged && later.entry.guid == first.entry.guid,
          "книга встала первой регистрацией, вторая — та же книга под тем же guid");
    check(later.entry.path == copy && ws.library.cards().size() == 1 && ws.library.cards()[0]->entry.path == copy,
          "запись одна — с файлом поздней регистрации");
    check(!later.entry.cover.empty() && filesIn(ws.coverDirectory()) == 1 &&
              std::filesystem::exists(ws.coverDirectory() / later.entry.cover.wchars()),
          "в кэше одна обложка — под guid записи, сироты нет");
}

/// Обход и открытие того же файла разом: открытие файлом не знает ни
/// каталога, ни времени листинга, а реестр у того же файла держит их от
/// записи — после обеих регистраций запись с каталогом и временем обхода, и
/// открытие получает тот же guid.
void testLockScanThenOpen() {
    std::printf("\n=== замок регистраций: обход, затем открытие того же файла ===\n");

    Sandbox box("lock-scan-open");
    Workspace ws(box.root, never);

    const std::string bytes = onDisk(kBook);
    const fb3::Document document{std::string(bytes)};
    const std::filesystem::path folder = box.root / L"shelf";
    const std::filesystem::path book = folder / L"book.fb3";
    const auto listed = std::chrono::file_clock::now();

    task<Registered> fromScan = registrationOf(
        ws, document, BookFile{.path = book, .size = bytes.size(), .modified = listed, .folder = folder});
    task<Registered> fromOpening = registrationOf(ws, document, BookFile{.path = book, .size = bytes.size()});
    check(!fromScan.done() && !fromOpening.done(), "регистрация обхода стоит на записи обложки, открытие ждёт замка");

    const Registered scannedBook = run(std::move(fromScan));
    const Registered openedBook = run(std::move(fromOpening));

    check(openedBook.entry.guid == scannedBook.entry.guid && !openedBook.isChanged,
          "открытие — та же запись под тем же guid");
    check(openedBook.entry.folder == folder && openedBook.entry.modified == listed &&
              ws.library.cards().size() == 1,
          "каталог и время листинга не затёрты неизвестными");
}

/// Три разные книги разом: регистрации идут по порядку прихода — перемены
/// реестра и карточки встают в том же порядке.
void testLockKeepsOrder() {
    std::printf("\n=== замок регистраций: по порядку прихода ===\n");

    Sandbox box("lock-order");
    Workspace ws(box.root, never);

    const std::string bytes = onDisk(kBook);
    const fb3::Document document{std::string(bytes)};
    const std::string otherBytes = onDisk(kOtherBook);
    const fb3::Document other{std::string(otherBytes)};
    const std::string thirdBytes = onDisk(kThirdBook);
    const fb3::Document third{std::string(thirdBytes)};

    std::vector<std::filesystem::path> heard;
    event_token watch = ws.bookRegistered().on_change(
        [&heard](const Registered& registered) noexcept { heard.push_back(registered.entry.path); });

    task<Registered> first = registrationOf(ws, document, BookFile{.path = kBook, .size = bytes.size()});
    task<Registered> second = registrationOf(ws, other, BookFile{.path = kOtherBook, .size = otherBytes.size()});
    task<Registered> last = registrationOf(ws, third, BookFile{.path = kThirdBook, .size = thirdBytes.size()});
    check(!first.done() && !second.done() && !last.done(), "первая стоит на записи обложки, две ждут замка");

    sta_loop::run_until([&] { return first.done() && second.done() && last.done(); });
    ws.bookRegistered().remove_change(watch);

    check(first.has_value() && second.has_value() && last.has_value(), "все три встали");
    check(heard.size() == 3 && heard[0] == kBook && heard[1] == kOtherBook && heard[2] == kThirdBook,
          "реестр переменился трижды — по порядку прихода");
    check(ws.library.cards().size() == 3 && ws.library.cards()[0]->entry.path == kBook &&
              ws.library.cards()[1]->entry.path == kOtherBook && ws.library.cards()[2]->entry.path == kThirdBook,
          "карточки встали в том же порядке");
}

/// Отмена, пока регистрация ждёт замка: ожидание кончается отменой, книга не
/// встаёт, шаг впереди доходит до реестра, а замок после них свободен —
/// следующая регистрация встаёт.
void testLockWaitEndsWithToken() {
    std::printf("\n=== замок регистраций: отмена на ожидании замка ===\n");

    Sandbox box("lock-wait-stop");
    Workspace ws(box.root, never);

    const std::string bytes = onDisk(kBook);
    const fb3::Document document{std::string(bytes)};
    const std::string otherBytes = onDisk(kOtherBook);
    const fb3::Document other{std::string(otherBytes)};

    cancellation_source waiting;
    task<Registered> going = registrationOf(ws, document, BookFile{.path = kBook, .size = bytes.size()});
    task<Registered> behind =
        registrationOf(ws, other, BookFile{.path = kOtherBook, .size = otherBytes.size()}, waiting.token());
    check(!going.done() && !behind.done(), "первая стоит на записи обложки, вторая ждёт замка");

    waiting.cancel();
    check(canceled(std::move(behind)), "отменённое ожидание замка кончается отменой");

    const Registered registered = run(std::move(going));
    check(registered.isNew && ws.library.cards().size() == 1 && !ws.library.findFile(kOtherBook),
          "встала только книга, чей шаг шёл: отменённая в реестр не попала");

    const Registered after = registerFrom(ws, other, kOtherBook, otherBytes.size());
    check(after.isNew && ws.library.cards().size() == 2, "замок свободен: следующая регистрация встаёт");
}

/// Отмена, когда шаг уже взял замок, — на записи обложки, которую держит
/// занятый рабочий поток: шаг доходит до реестра, обложка на диске. Токен —
/// для ожидания замка, а шаг его не слушает.
void testLockHeldStepIgnoresToken() {
    std::printf("\n=== замок регистраций: отмена после того, как шаг взял замок ===\n");

    Sandbox box("lock-held");
    Workspace ws(box.root, never);

    const std::string bytes = onDisk(kBook);
    const fb3::Document document{std::string(bytes)};

    cancellation_source source;
    HeldWorker heldWorker;
    task<Registered> registration =
        registrationOf(ws, document, BookFile{.path = kBook, .size = bytes.size()}, source.token());
    check(!registration.done(), "шаг взял замок, а запись обложки стоит за занятым потоком");

    source.cancel();
    heldWorker.open();

    const Registered registered = run(std::move(registration));
    check(registered.isNew && ws.library.cards().size() == 1, "шаг дошёл до реестра под отменённым токеном");
    check(!registered.entry.cover.empty() &&
              std::filesystem::exists(ws.coverDirectory() / registered.entry.cover.wchars()),
          "обложка записана");
}

/// Обложка не записалась — на месте каталога обложек файл: прямая регистрация
/// кончается `last_error`, реестр книги не узнаёт, отказом рабочее место не
/// говорит — исход у ждущего, — а замок после неё свободен.
void testLockCoverRefused() {
    std::printf("\n=== замок регистраций: обложка не записалась ===\n");

    Sandbox box("lock-refused");
    Workspace ws(box.root, never);

    const std::string bytes = onDisk(kBook);
    const fb3::Document document{std::string(bytes)};

    // Файл на месте каталога: обложка туда не ляжет, и каталог на его месте
    // запись не заведёт.
    putOnDisk(ws.coverDirectory(), "");

    std::vector<std::wstring> refusals;
    event_token refusedWatch = ws.registrationRefused().on_change(
        [&refusals](const std::wstring& line) noexcept { refusals.push_back(line); });

    task<Registered> failing = registrationOf(ws, document, BookFile{.path = kBook, .size = bytes.size()});
    check(!failing.done(), "шаг стоит на записи обложки");
    check(coverRefused(std::move(failing)), "обложка не записалась — регистрация кончается last_error");
    check(ws.library.cards().empty() && refusals.empty(),
          "реестр книги не узнал, события отказа нет — отказ у того, кто ждал");

    std::filesystem::remove(ws.coverDirectory());
    const Registered registered = registerFrom(ws, document, kBook, bytes.size());
    ws.registrationRefused().remove_change(refusedWatch);

    check(registered.isNew && ws.library.cards().size() == 1 && refusals.empty() &&
              filesIn(ws.coverDirectory()) == 1,
          "замок свободен: следующая регистрация встаёт со своей обложкой");
}

/// Регистраций своих книг обход не ждёт, и их исходы говорят события
/// рабочего места: обход трёх новых книг — три перемены реестра; перепроверка,
/// у одной книги которой обложка не записалась, — один отказ строкой с путём,
/// а запись этой книги прежняя.
void testLockScanTellsOutcomes() {
    std::printf("\n=== замок регистраций: исходы регистраций обхода ===\n");

    Sandbox box("lock-scan");
    const std::filesystem::path folder = shelfOf(box, {kBook, kOtherBook, kThirdBook});
    Workspace ws(box.root, never);

    const Scanned added = scanned(ws, ws.addFolder(folder, never));
    check(added.queued == 3 && added.registered == 3 && added.added == 3 && added.refused.empty() &&
              ws.library.cards().size() == 3,
          "три новые книги — три перемены реестра, отказов нет");

    // Изменённая книга пишет обложку заново под тем же именем — а на его
    // месте каталог: файл туда не ляжет.
    const std::filesystem::path book = folder / kBook.filename();
    const BookEntry known = *ws.library.findFile(book);
    const std::filesystem::path cover = ws.coverDirectory() / known.cover.wchars();
    std::filesystem::remove(cover);
    std::filesystem::create_directories(cover);
    touch(book);

    const Scanned rescanned = scanned(ws, ws.rescanFolders(never));
    check(rescanned.queued == 1 && rescanned.registered == 0 && rescanned.refused.size() == 1 &&
              holds(rescanned.refused[0], book.wstring()),
          "обложка одной книги не записалась — один отказ, строкой с путём");
    check(*ws.library.findFile(book) == known, "запись книги, чья регистрация не удалась, — прежняя");

    std::filesystem::remove(cover);
}

/// Регистрации книг обхода ждут замка под общим токеном приложения, а не под
/// токеном обхода: источник обхода живёт в его сценарии и уходит вместе с
/// ним, а отданные книги должны встать. Отмена источника обхода, пока его
/// регистрация ждёт замка за чужим шагом, её не трогает — книга встаёт.
/// Выход приложения кончает ожидание отменой — книга не встаёт, и рабочее
/// место ничего не говорит, — а шаг, взявший замок, доходит до реестра, и
/// `registering()` опускается, только когда он кончится.
void testLockScanStops() {
    std::printf("\n=== замок регистраций: отмена обхода и выход ===\n");

    Sandbox box("lock-scan-stop");
    const std::string bytes = onDisk(kBook);
    const fb3::Document document{std::string(bytes)};

    // Каталог в реестре — знак, что листинг сведён и чтение книги ушло
    // рабочему потоку: добавление делает и то и другое одним ходом. Чужой шаг
    // берёт замок следом, и его запись обложки уходит потоку после чтения:
    // регистрация книги обхода застаёт замок занятым.
    const auto scanBehindStep = [&](Workspace& ws, const std::filesystem::path& folder,
                                    cancellation_token scanToken) {
        task<ScanReport> scan = ws.addFolder(folder, std::move(scanToken));
        sta_loop::run_until(
            [&] { return !ws.library.folders().empty() && samePath(ws.library.folders().back(), folder); });
        task<Registered> going = registrationOf(ws, document, BookFile{.path = kBook, .size = bytes.size()});
        check(!going.done(), "чужой шаг взял замок и стоит на записи обложки");

        sta_loop::run_until([&] { return scan.done(); });
        check(run(std::move(scan)).queued == 1 && ws.registering() && !going.done(),
              "обход кончился, регистрация его книги ждёт замка");
        return going;
    };

    {
        // Отмена источника обхода — то же, что его уход со сценарием.
        std::filesystem::create_directories(box.root / L"scan-source");
        cancellation_source application;
        Workspace ws(box.root / L"scan-source", application.token());
        const std::filesystem::path folder = box.root / L"shelf-one";
        std::filesystem::create_directories(folder);
        std::filesystem::copy_file(kOtherBook, folder / kOtherBook.filename());

        std::vector<std::wstring> refusals;
        uint32_t registered = 0;
        event_token refusedWatch = ws.registrationRefused().on_change(
            [&refusals](const std::wstring& line) noexcept { refusals.push_back(line); });
        event_token registeredWatch =
            ws.bookRegistered().on_change([&registered](const Registered&) noexcept { ++registered; });

        cancellation_source scanStop;
        task<Registered> going = scanBehindStep(ws, folder, scanStop.token());
        scanStop.cancel();
        check(ws.registering(), "отмена обхода регистрацию его книги, ждущую замка, не кончает");

        const Registered held = run(std::move(going));
        sta_loop::run_until([&] { return !ws.registering(); });
        check(held.isNew && ws.library.findFile(folder / kOtherBook.filename()) && registered == 2 &&
                  refusals.empty(),
              "дождавшись замка, книга обхода встала");

        // Шаг, взявший замок: копия книги с полки другого каталога — та же
        // книга, запись другая, и шаг пишет обложку; обход кончился раньше
        // записи, и тогда выходит приложение.
        const std::filesystem::path anotherFolder = box.root / L"shelf-two";
        std::filesystem::create_directories(anotherFolder);
        std::filesystem::copy_file(kBook, anotherFolder / kBook.filename());

        task<ScanReport> second = ws.addFolder(anotherFolder, never);
        sta_loop::run_until([&] { return second.done(); });
        check(ws.registering(), "обход кончился, шаг его книги стоит на записи обложки");

        application.cancel();
        check(ws.registering(), "выход шаг, взявший замок, не кончает");
        sta_loop::run_until([&] { return !ws.registering(); });

        ws.bookRegistered().remove_change(registeredWatch);
        ws.registrationRefused().remove_change(refusedWatch);

        const BookEntry* moved = ws.library.findFile(anotherFolder / kBook.filename());
        check(run(std::move(second)).queued == 1 && moved && moved->guid == held.entry.guid && registered == 3 &&
                  refusals.empty(),
              "шаг дошёл до реестра: книга перешла к файлу второго каталога под прежним guid");
        check(std::filesystem::exists(ws.coverDirectory() / moved->cover.wchars()), "обложка записана");
    }
    {
        // Выход, пока регистрация книги обхода ждёт замка.
        std::filesystem::create_directories(box.root / L"exit");
        cancellation_source application;
        Workspace ws(box.root / L"exit", application.token());
        const std::filesystem::path folder = box.root / L"shelf-three";
        std::filesystem::create_directories(folder);
        std::filesystem::copy_file(kOtherBook, folder / kOtherBook.filename());

        std::vector<std::wstring> refusals;
        uint32_t registered = 0;
        event_token refusedWatch = ws.registrationRefused().on_change(
            [&refusals](const std::wstring& line) noexcept { refusals.push_back(line); });
        event_token registeredWatch =
            ws.bookRegistered().on_change([&registered](const Registered&) noexcept { ++registered; });

        task<Registered> going = scanBehindStep(ws, folder, never);
        application.cancel();
        sta_loop::run_until([&] { return !ws.registering(); });
        check(!going.done(), "отменённое выходом ожидание кончилось, не дожидаясь шага впереди");

        const Registered held = run(std::move(going));
        ws.bookRegistered().remove_change(registeredWatch);
        ws.registrationRefused().remove_change(refusedWatch);

        check(held.isNew && registered == 1 && refusals.empty(), "отмена ожидания — не отказ: рабочее место молчит");
        check(ws.library.cards().size() == 1 && !ws.library.findFile(folder / kOtherBook.filename()),
              "книга обхода не встала");
    }
}

}  // namespace

// Тесты других слоёв модели — каждый в своём файле.
void runSkinEditorTests();
// Мелкие куски модели, вынесенные из экранов (pieces_tests.cpp).
void runPiecesTests();
// Слои модели своими файлами: карта клавиш, список тем, поиск по книге, места
// открытой книги, файл настроек, полосы длинной сноски, мера всплывашки и её
// прокрутка с клавиатуры.
void runKeyMapTests();
void runThemeListTests();
void runBookSearchTests();
void runBookPlacesTests();
void runSettingsTests();
void runNoteBandsTests();
void runNoteFrameTests();
void runNoteScrollTests();
// Пробы слоёв модели — каждая в своём файле.
void runPageFlowTests();
void runChapterTailTests();
void runBookPresenterTests();
void runPictureQueueTests();
// Проба остановки ведущего на нарушенной проводке — в дочернем запуске этого
// же исполняемого файла (book_presenter_tests.cpp, флаг `--stop-probe=<имя>`).
// Пусто — запуск обычный; иначе — код выхода дочернего.
std::optional<int> stopProbeExitCode(int argc, char* argv[]);

int main(int argc, char* argv[]) {
    // Дочерний запуск — одна проба остановки, и ничего больше: остальные пробы
    // идут у родителя, а он смотрит на код выхода и отчёт об остановке.
    if (const std::optional<int> probeExitCode = stopProbeExitCode(argc, argv))
        return *probeExitCode;

    std::setvbuf(stdout, nullptr, _IONBF, 0);

    testHintKeepsLetters();
    testSearchContextKeepsLetters();
    testContentsBorrowTitles();
    testIndexKeepsToFlow();
    testSearchPlacesHitsByOffsetRuns();
    testSearchMatchesWhatTheReaderMeans();
    testEdgeThroughPoints();
    testPageWarp();
    testPictureBudget();
    testSkinsReadSeparateLeaves();
    runSkinEditorTests();
    runKeyMapTests();
    runThemeListTests();
    runBookSearchTests();
    runBookPlacesTests();
    runSettingsTests();
    runNoteBandsTests();
    runNoteFrameTests();
    runNoteScrollTests();

    // Петля операций wxl — одна на процесс, как у тестов самой wxl: её каналы
    // живут столько же, сколько процесс, и второй раз не стартуют. COM — для
    // декодера картинок (WIC).
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    sta_loop::start("bukvitsa.lib tests: I/O");

    testLibraryCards();
    testLibraryFolders();
    testLibraryPathTakenByOtherBook();
    testWorkspaceStartsEmpty();
    testWorkspaceKeepsBrokenFiles();
    testWorkspaceNamesCorrectedSkins();
    testWorkspaceRegistersBook();
    testWorkspaceKeepsLastBookGuid();
    testWorkspaceOpensBook();
    testWorkspaceAddsFolder();
    testWorkspaceScanNamesBrokenBooks();
    testWorkspaceRescansFolders();
    testWorkspaceCoverBeforeCard();
    testWorkspaceReadsLibraryV1();
    testWorkspaceRescanSeesSize();
    testWorkspaceRescanTellsReplacedBook();
    testWorkspaceRescanAdoptsStrayEntry();
    testWorkspaceFindsLastBook();
    testWorkspaceTellsImages();
    testDecodeAsyncTellsImages();
    testDecodeAsyncLeavesCaller();
    testDecodeAsyncStopsOnRequest();
    testDecodeCopiesAllBands();
    testDecodeStopsBetweenBands();
    testProbeDecodesOnce();
    testStateGoesUnderPlacesGuid();
    testWorkspaceReadsStopOnRequest();
    testWorkspaceStartReadsModelsWhole();
    testWorkspaceOpenStopsOnStateRead();
    testWorkspaceScanStopsAfterListing();
    testWorkspaceSaveSkinStopsBeforeWrites();
    testWorkspaceDeleteSkinStopsBeforeChange();
    testWorkspaceSkinsWriteRefused();
    testWorkspaceReadsStopWithParent();
    testWorkspaceFolderKeepsWhatShelfShowed();
    testWorkspaceRescanKeepsWhatShelfShowed();
    testLockSameBookAtOnce();
    testLockNewBookFromTwoPaths();
    testLockScanThenOpen();
    testLockKeepsOrder();
    testLockWaitEndsWithToken();
    testLockHeldStepIgnoresToken();
    testLockCoverRefused();
    testLockScanTellsOutcomes();
    testLockScanStops();

    runPiecesTests();
    // Поток страниц верстает настоящую книгу: DirectWrite и WIC — после COM.
    runPageFlowTests();
    // Досчёт главы — на том же романе, с поддельным простоем вместо окна.
    runChapterTailTests();
    // Ведущий полосы — на том же романе, с поддельными холстом и простоем.
    runBookPresenterTests();
    // Очередь картинок — через ведущего, на книге с картинками: раскодирует
    // рабочий поток той же петли.
    runPictureQueueTests();

    sta_loop::stop();
    ::CoUninitialize();

    std::printf("\n%s\n", failures == 0 ? "OK" : "ЕСТЬ ОШИБКИ");
    return failures;
}
