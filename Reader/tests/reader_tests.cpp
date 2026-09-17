// Проверка чистых функций читалки — над блоками книги и над кривыми обложек.
// Как у FB3 и вёрстки, без фреймворка.

#include <cmath>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Заголовки читалки после всех стандартных: индекс книги ведёт к импорту
// модуля книги, и обложки — перед ним.
#include "skins.h"

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

bool isLetterStart(std::wstring_view text, size_t at) {
    return at >= text.size() || unicode::floor_grapheme_boundary(text, at) == at;
}

/// Доли снимка: точнее тысячной доли высоты мастер всё равно не ставит.
bool aboutEqual(float a, float b) {
    return std::abs(a - b) < 1e-3f;
}

/// Текст собран тестом из кусков, поэтому проверяется, как всякий чужой.
typography::Block blockOf(std::wstring_view text) {
    typography::Block block;
    block.kind = typography::BlockKind::Paragraph;
    block.paragraph.text = u16_text(unicode::checked(text).value());
    block.paragraph.charOffsets.resize(block.paragraph.text.size());
    for (uint32_t i = 0; i < block.paragraph.charOffsets.size(); ++i)
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
        const u16_text hint = reader::hintAt(blocks, 0);
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
        const sta_vector<reader::SearchHit> hits = reader::searchBook(blocks, L"q");

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
/// вкладка, а текст блока — пока открыта книга.
void testContentsBorrowTitles() {
    std::printf("\n=== оглавление ===\n");

    typography::Block title = blockOf(L"Глава первая");
    title.kind = typography::BlockKind::Title;
    const typography::Block blocks[] = {title, blockOf(L"Текст главы.")};

    const sta_vector<reader::ContentsEntry> contents = reader::contentsOf(blocks);
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
        uint32_t at;
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
        const sta_vector<reader::SearchHit> hits = reader::searchBook(blocks, test.needle);
        check(hits.size() == 1 && hits[0].charOffset == test.at,
              std::string("находка на своём месте: ") + test.what);
    }

    const typography::Block hyphen[] = {blockOf(L"Про-метей")};
    check(reader::searchBook(hyphen, L"прометей").empty(), "обычный дефис — не мягкий перенос");
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
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    testHintKeepsLetters();
    testSearchContextKeepsLetters();
    testContentsBorrowTitles();
    testSearchMatchesWhatTheReaderMeans();
    testEdgeThroughPoints();
    testSkinsReadSeparateLeaves();

    std::printf("\n%s\n", failures == 0 ? "OK" : "ЕСТЬ ОШИБКИ");
    return failures;
}
