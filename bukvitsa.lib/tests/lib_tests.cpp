// Тесты модели читалки (bukvitsa.lib): чистые функции над хранилищем книги и
// над кривыми обложек. Как у FB3 и вёрстки, без фреймворка.

#include <objbase.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
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
#include "bukvitsa/reader/shelf_text.h"
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
    skins.loadFrom(old);
    check(skins.list().size() == 1, "обложка прочитана");
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
    repaired.loadFrom(bent);
    check(repaired.list().size() == 1 &&
              repaired.list()[0].top.x[spine] == reader::EdgeCurve::kSpineX,
          "корешок из файла встаёт на середину");

    // Точку увели за соседку: такая кромка — начальная прямая.
    std::string crossed = written;
    const size_t fourthAt = crossed.find("<point x=\"0.4\"");
    check(fourthAt != std::string::npos, "четвёртая точка записана как была");
    if (fourthAt == std::string::npos) return;
    crossed.replace(fourthAt, 14, "<point x=\"0.6\"");

    reader::Skins straightened;
    straightened.loadFrom(crossed);
    check(straightened.list().size() == 1 && straightened.list()[0].top.x == fresh.top.x &&
              straightened.list()[0].top.y == fresh.top.y,
          "точки не по порядку — кромка становится начальной прямой");

    // Кромки, которые между точками разошлись на высоту полосы и вывернули бы
    // столбец: ровной становится вся обложка.
    reader::Skin namedFold = foldedSkin();
    namedFold.name = u16_text{u"Вывернутая"};
    namedFold.image = u16_text{u"folded.png"};
    reader::Skins foldingRegistry;
    foldingRegistry.put(namedFold);
    reader::Skins unfolded;
    unfolded.loadFrom(foldingRegistry.toXml());
    check(unfolded.list().size() == 1 && unfolded.list()[0].top.y == fresh.top.y &&
              unfolded.list()[0].bottom.y == fresh.bottom.y &&
              reader::PageWarp{unfolded.list()[0]}.flat(),
          "обложка, выворачивающая столбец, читается ровной");

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
    broken.loadFrom(subnormal);
    check(broken.list().size() == 1 && broken.list()[0].top.x == fresh.top.x &&
              broken.list()[0].top.y == fresh.top.y &&
              reader::PageWarp{broken.list()[0]}.flat(),
          "кромка на субнормальном шаге читается ровной");
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

const std::filesystem::path kBook = testdata / L"Turgenev_I._Spisokshkolnoy._Otcyi_I_Deti.fb3";
const std::filesystem::path kOtherBook = testdata / L"nightmare_example.fb3";

/// Книга в реестр тем, что регистрация берёт из документа: описанием, числом
/// знаков и копией обложки. Описание читается, а не забирается: документ
/// теста живёт до конца пробы.
const BookEntry& addTo(Library& library, const fb3::Document& document, const std::filesystem::path& path,
                       uint64_t fileSize) {
    return library.add(document.description(), document.characterCount(), copyCover(document), path, fileSize);
}

/// Регистрация книги в рабочем месте — тем же, что снимает с документа
/// чтение книги. Обложка — в кадре этой функции: регистрация держит её
/// ссылкой, пока ждёт записи.
Registered registerFrom(Workspace& ws, const fb3::Document& document, const std::filesystem::path& path,
                        uint64_t fileSize) {
    const std::optional<BookCover> cover = copyCover(document);
    return run(ws.registerBook(document.description(), document.characterCount(), cover, path, fileSize));
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

/// Пустой каталог — первый запуск: умолчания, ни одного слова читателю.
void testWorkspaceStartsEmpty() {
    std::printf("\n=== рабочее место: первый запуск ===\n");

    Sandbox box("empty");
    Workspace ws(box.root);

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
        Workspace ws(box.root);
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
        Workspace ws(box.root);
        putOnDisk(ws.settingsPath(), brokenAgain);

        run(ws.start(never));

        check(onDisk(box.root / L"settings.xml.bad") == brokenAgain, "повторная порча — копия перезаписана");
        check(!std::filesystem::exists(box.root / L"settings.xml.bad.bad"), "копия одна, не множится");
    }
}

/// Регистрация: запись в реестре, обложка в кэше, повтор — та же запись.
void testWorkspaceRegistersBook() {
    std::printf("\n=== рабочее место: регистрация книги ===\n");

    Sandbox box("register");
    Workspace ws(box.root);

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
    Workspace ws(box.root);

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
    Workspace ws(box.root);

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

    const Opened opened = run(ws.openBook(loaded, never));

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

    const Opened reopened = run(ws.openBook(loaded, never));

    check(reopened.entry.guid == opened.entry.guid, "повторное открытие — та же запись");
    check(reopened.state.charOffset == 12345, "повторное открытие — то же место");
    check(run(ws.lastBookPath(never)) == kBook, "последняя книга — она");

    Workspace later(box.root);
    const Started started = run(later.start(never));
    check(started.notices.empty() && started.lastBook == kBook, "следующий запуск продолжит её");
}

/// Обход каталога: книги регистрируются по одной, о каждой новой говорят
/// сразу, мусор пропускается; реестр меняется в памяти, а файл его пишет
/// вызывающий; повтор ничего не добавляет.
void testWorkspaceAddsFolder() {
    std::printf("\n=== рабочее место: обход каталога ===\n");

    Sandbox box("folder");
    Workspace ws(box.root);

    const std::filesystem::path folder = box.root / L"shelf";
    std::filesystem::create_directories(folder);
    std::filesystem::copy_file(kBook, folder / kBook.filename());
    std::filesystem::copy_file(kOtherBook, folder / kOtherBook.filename());
    putOnDisk(folder / L"not-a-book.fb3", "this is not a book");
    putOnDisk(folder / L"note.txt", "and this is not even fb3");

    // Полка слышит обход сама: карточка за карточкой, по мере разбора.
    std::vector<list_change> heard;
    const cookie_t watch = ws.library.cards().on_change(
        [&heard](const list_change& change) noexcept { heard.push_back(change); });

    const FolderAdded first = run(ws.addFolder(folder, never));

    check(first.added, "каталог с книгами — реестр изменён");
    check(first.unread.empty(), "все файлы прочитались");
    check(ws.library.cards().size() == 2, "две книги, мусор пропущен");
    check(heard.size() == 2 && heard[0] == list_change{list_change::inserted, 0, 1} &&
              heard[1] == list_change{list_change::inserted, 1, 1},
          "полка растёт по одной карточке в конец");

    check(!std::filesystem::exists(ws.libraryPath()), "файла реестра обход не пишет");

    run(ws.saveLibrary());

    Library onDiskLibrary;
    check(onDiskLibrary.loadFrom(onDisk(ws.libraryPath())) && onDiskLibrary.cards().size() == 2,
          "записанный реестр — две книги");

    const FolderAdded second = run(ws.addFolder(folder, never));

    check(second.unread.empty() && ws.library.cards().size() == 2, "повторный обход не множит записи");
    check(heard.size() == 2, "повторный обход — ни одной новой карточки, полка не тронута");

    ws.library.cards().remove_change(watch);
}

/// Последняя книга: путь из настроек протух — по guid из реестра; нет и там —
/// пусто.
void testWorkspaceFindsLastBook() {
    std::printf("\n=== рабочее место: последняя книга ===\n");

    Sandbox box("last");
    Workspace ws(box.root);

    const std::string bytes = onDisk(kBook);
    const fb3::Document document{std::string(bytes)};
    const Registered registered = registerFrom(ws, document, kBook, bytes.size());

    ws.settings.lastBookPath = box.root / L"moved-away.fb3";
    ws.settings.lastBookGuid = registered.entry.guid;

    check(run(ws.lastBookPath(never)) == kBook, "путь протух — книга найдена по guid");

    ws.settings.lastBookGuid = newGuid();

    check(run(ws.lastBookPath(never)).empty(), "guid неизвестен — продолжать нечего");
}

/// Картинка или нет — по байтам, а не по имени.
void testWorkspaceTellsImages() {
    std::printf("\n=== рабочее место: снимок ===\n");

    Sandbox box("image");
    Workspace ws(box.root);

    check(run(ws.isImage(testdata / L"bg_paper2.jpg", never)), "jpeg — картинка");
    check(!run(ws.isImage(kBook, never)), "книга — не картинка");
    check(run(ws.readBytes(kBook, never)) == onDisk(kBook), "байты файла целиком");
}

/// Место чтения пишется под guid мест книги, а не последней книги из
/// настроек: открытие другой книги ставит её в настройки раньше, чем полоса и
/// места её возьмут, и всё это время место на полосе — прежней книги.
void testStateGoesUnderPlacesGuid() {
    std::printf("\n=== место чтения — под guid мест книги ===\n");

    Sandbox box("places");
    Workspace ws(box.root);

    const LoadedBook first = run(ws.readBook(kBook, never));
    Opened firstOpened = run(ws.openBook(first, never));

    BookPlaces places;
    places.open(firstOpened.entry.guid, first.book->text(), std::move(firstOpened.state.bookmarks));
    places.toggleBookmark(500, u16_text{u"пятьсот"});

    const LoadedBook second = run(ws.readBook(kOtherBook, never));
    const Opened secondOpened = run(ws.openBook(second, never));

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
    Workspace ws(box.root);

    cancellation_source source;
    source.cancel();
    const cancellation_token stop = source.token();

    const std::filesystem::path missing = box.root / L"missing.fb3";

    check(canceled(ws.readBook(missing, stop)), "книга: не начинается — отмена, а не «файла нет»");
    check(canceled(ws.readBytes(missing, stop)), "байты: не начинается");
    check(canceled(ws.isImage(missing, stop)), "снимок: не начинается");
    check(canceled(ws.addFolder(box.root / L"missing", stop)), "каталог: не начинается — отмена, а не «нет каталога»");

    const u16_text guid = newGuid();
    check(canceled(ws.readState(guid, stop)), "состояние: не начинается — отмена, а не чистый лист");

    ws.settings.lastBookPath = missing;
    check(canceled(ws.lastBookPath(stop)), "последняя книга: не начинается — отмена, а не «продолжать нечего»");

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
        Workspace ws(box.root);
        ws.settings.fontSize.set(31.0);
        ws.settings.lastBookGuid = newGuid();
        ws.settings.lastBookPath = kBook;
        run(ws.saveSettings());
    }

    Workspace ws(box.root);

    cancellation_source source;
    source.cancel();

    check(canceled(ws.start(source.token())), "запуск кончился отменой — на поиске книги");
    check(ws.settings.fontSize.get() == 31.0 && ws.settings.lastBookPath == kBook,
          "настройки прочитаны целиком: запись при закрытии окна не ляжет умолчаниями");
}

/// Открытие под отменённым токеном не трогает ничего — ни модели, ни диска;
/// отменённое на записи обложки меняет в памяти и реестр, и настройки и
/// кончается отменой на чтении состояния. Файлов реестра и настроек открытие
/// не пишет ни так, ни так.
void testWorkspaceOpenStopsOnStateRead() {
    std::printf("\n=== отмена: открытие книги ===\n");

    Sandbox box("stop-open");
    Workspace ws(box.root);

    const LoadedBook loaded = run(ws.readBook(kBook, never));

    cancellation_source before;
    before.cancel();

    check(canceled(ws.openBook(loaded, before.token())), "под отменённым токеном — отмена");
    check(ws.library.cards().empty() && ws.settings.lastBookGuid.empty(),
          "под отменённым токеном модель не тронута");
    check(!std::filesystem::exists(ws.libraryPath()) && !std::filesystem::exists(ws.settingsPath()) &&
              !std::filesystem::exists(ws.coverDirectory()),
          "под отменённым токеном на диске ничего");

    cancellation_source during;
    task<Opened> opening = ws.openBook(loaded, during.token());
    check(!opening.done(), "открытие стоит на записи обложки");
    during.cancel();

    check(canceled(std::move(opening)),
          "попросили на записи обложки — открытие кончается отменой на чтении состояния");
    check(ws.library.cards().size() == 1 && ws.settings.lastBookGuid == ws.library.cards()[0]->entry.guid,
          "модель: книга в реестре, настройки на неё указывают");
    check(!std::filesystem::exists(ws.libraryPath()) && !std::filesystem::exists(ws.settingsPath()),
          "файлов реестра и настроек открытие не пишет — их пишет вызывающий");
}

/// Сохранение обложки: под отменённым токеном не начинается, отмена на
/// чтении снимка ничего не пишет, а с первой записи — копии снимка — оно
/// доходит до строки реестра.
void testWorkspaceSaveSkinStopsBeforeWrites() {
    std::printf("\n=== отмена: сохранение обложки ===\n");

    Sandbox box("stop-skin");
    Workspace ws(box.root);

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
    Workspace ws(box.root);

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

/// Дерево источников: отмена родителя доходит до чтения, которое ждёт под
/// токеном дочернего, — так закрытие окна кончает сценарии; отмена дочернего
/// родителя не трогает — так владелец отменяет одну задачу.
void testWorkspaceReadsStopWithParent() {
    std::printf("\n=== отмена: дерево источников ===\n");

    Sandbox box("stop-tree");
    Workspace ws(box.root);

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
    Workspace ws(box.root);

    const std::filesystem::path folder = box.root / L"shelf";
    std::filesystem::create_directories(folder);
    std::filesystem::copy_file(kBook, folder / kBook.filename());
    std::filesystem::copy_file(kOtherBook, folder / kOtherBook.filename());

    // Просьба — в тот миг, когда полка слышит первую книгу: обход стоит между
    // её регистрацией и чтением следующей.
    cancellation_source source;
    const cookie_t watch =
        ws.library.cards().on_change([&source](const list_change&) noexcept { source.cancel(); });

    check(canceled(ws.addFolder(folder, source.token())), "обход кончился отменой");

    ws.library.cards().remove_change(watch);

    check(ws.library.cards().size() == 1, "следующую книгу не читали");
    check(!std::filesystem::exists(ws.libraryPath()), "файла реестра обход не пишет и под отменой");

    run(ws.saveLibrary());

    Library onDiskLibrary;
    check(onDiskLibrary.loadFrom(onDisk(ws.libraryPath())) && onDiskLibrary.cards().size() == 1 &&
              onDiskLibrary.cards()[0]->entry.guid == ws.library.cards()[0]->entry.guid,
          "записанный реестр — с той книгой, что полка показала");
}

}  // namespace

// Тесты других слоёв модели — каждый в своём файле.
void runSkinEditorTests();
// Мелкие куски модели, вынесенные из экранов (pieces_tests.cpp).
void runPiecesTests();
// Слои модели своими файлами: карта клавиш, список тем, поиск по книге, места
// открытой книги, файл настроек.
void runKeyMapTests();
void runThemeListTests();
void runBookSearchTests();
void runBookPlacesTests();
void runSettingsTests();
// Пробы слоёв модели — каждая в своём файле.
void runPageFlowTests();
void runChapterTailTests();
void runBookPresenterTests();
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
    testSkinsReadSeparateLeaves();
    runSkinEditorTests();
    runKeyMapTests();
    runThemeListTests();
    runBookSearchTests();
    runBookPlacesTests();
    runSettingsTests();

    // Петля операций wxl — одна на процесс, как у тестов самой wxl: её каналы
    // живут столько же, сколько процесс, и второй раз не стартуют. COM — для
    // декодера картинок (WIC).
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    sta_loop::start("bukvitsa.lib tests: I/O");

    testLibraryCards();
    testWorkspaceStartsEmpty();
    testWorkspaceKeepsBrokenFiles();
    testWorkspaceRegistersBook();
    testWorkspaceKeepsLastBookGuid();
    testWorkspaceOpensBook();
    testWorkspaceAddsFolder();
    testWorkspaceFindsLastBook();
    testWorkspaceTellsImages();
    testStateGoesUnderPlacesGuid();
    testWorkspaceReadsStopOnRequest();
    testWorkspaceStartReadsModelsWhole();
    testWorkspaceOpenStopsOnStateRead();
    testWorkspaceSaveSkinStopsBeforeWrites();
    testWorkspaceDeleteSkinStopsBeforeChange();
    testWorkspaceReadsStopWithParent();
    testWorkspaceFolderKeepsWhatShelfShowed();

    runPiecesTests();
    // Поток страниц верстает настоящую книгу: DirectWrite и WIC — после COM.
    runPageFlowTests();
    // Досчёт главы — на том же романе, с поддельным простоем вместо окна.
    runChapterTailTests();
    // Ведущий полосы — на том же романе, с поддельными холстом и простоем.
    runBookPresenterTests();

    sta_loop::stop();
    ::CoUninitialize();

    std::printf("\n%s\n", failures == 0 ? "OK" : "ЕСТЬ ОШИБКИ");
    return failures;
}
