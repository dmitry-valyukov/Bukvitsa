// Тесты модели читалки (bukvitsa.lib): чистые функции над блоками книги и над
// кривыми обложек. Как у FB3 и вёрстки, без фреймворка.

#include <objbase.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

// Заголовки читалки после всех стандартных: индекс книги ведёт к импорту
// модуля книги, и обложки — перед ним; рабочее место — последним, оно
// импортирует wxl.async.
#include "check.h"

#include "bukvitsa/reader/skins.h"

#include "bukvitsa/reader/book_index.h"
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
        const sta_vector<reader::SearchHit> hits = reader::searchBook(blocks, u"q");

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
        const typography::Block blocks[] = {blockOf(test.text)};
        const sta_vector<reader::SearchHit> hits = reader::searchBook(blocks, test.needle);
        check(hits.size() == 1 && hits[0].charOffset == test.at,
              std::string("находка на своём месте: ") + test.what);
    }

    const typography::Block hyphen[] = {blockOf(L"Про-метей")};
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


// ---- Рабочее место: файлы читалки без окна ----------------------------------
//
// Корутины рабочего места ждут операций wxl, а те живут на петле sta_loop:
// тест поднимает её один раз на процесс (как тесты самой wxl) и крутит до
// конца каждой корутины. Разбор идёт на этом же потоке, в STA-пуле.

using wxl::async::sta_loop;
using wxl::async::task;

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

/// Пустой каталог — первый запуск: умолчания, ни одного слова читателю.
void testWorkspaceStartsEmpty() {
    std::printf("\n=== рабочее место: первый запуск ===\n");

    Sandbox box("empty");
    Workspace ws(box.root);

    const Started started = run(ws.start());

    check(started.notices.empty(), "первый запуск: ни одного сообщения");
    check(started.lastBook.empty(), "первый запуск: продолжать нечего");
    check(ws.library.books().empty(), "первый запуск: реестр пуст");
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

        const Started started = run(ws.start());

        check(started.notices.size() == 3, "три битых файла — три сообщения");
        if (started.notices.size() == 3) {
            check(started.notices[0].headline == L"Настройки не прочитаны" &&
                      holds(started.notices[0].details, ws.settingsPath().wstring()),
                  "сообщение о настройках называет файл");
            check(started.notices[1].headline == L"Реестр обложек не прочитан", "сообщение об обложках");
            check(started.notices[2].headline == L"Реестр книг не прочитан", "сообщение о реестре книг");
        }
        check(ws.settings.fontSize.get() == kFontSizeDefault, "битые настройки — умолчания");
        check(ws.skins.list().empty() && ws.library.books().empty(), "битые реестры — пусто");

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

        run(ws.start());

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

    const Registered first = run(ws.registerBook(document, kBook, bytes.size()));

    check(first.isNew, "первая регистрация — новая книга");
    check(ws.library.books().size() == 1, "в реестре одна книга");
    check(!first.entry.guid.empty(), "у записи есть guid");
    check(first.entry.path == kBook, "путь записи — путь файла");

    const CoverBytes cover = coverOf(document, first.entry.guid);
    check(first.entry.cover == cover.name, "имя обложки в записи — имя файла в кэше");
    check(cover.name.empty() || std::filesystem::exists(ws.coverDirectory() / cover.name.wchars()),
          "обложка книги лежит в кэше");
    check(!std::filesystem::exists(ws.libraryPath()), "регистрация сама реестр не пишет");

    const Registered again = run(ws.registerBook(document, kBook, bytes.size()));

    check(!again.isNew, "повторная регистрация — не новая");
    check(again.entry.guid == first.entry.guid, "повторная регистрация — тот же guid");
    check(ws.library.books().size() == 1, "реестр не раздвоился");
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

    const Registered restored = run(ws.registerBook(document, kBook, bytes.size()));

    check(restored.isNew && restored.entry.guid == kept, "потерянная последняя книга — под прежним guid");

    const std::string otherBytes = onDisk(kOtherBook);
    const fb3::Document other{std::string(otherBytes)};

    ws.settings.lastBookPath = kOtherBook;   // guid занят первой книгой
    const Registered fresh = run(ws.registerBook(other, kOtherBook, otherBytes.size()));

    check(fresh.isNew && fresh.entry.guid != kept, "занятый guid не берётся — новый");
    check(ws.library.books().size() == 2, "две разные книги");
}

/// Открытие: реестр и настройки на диске указывают на книгу, состояние
/// читается и пишется, последняя книга находится.
void testWorkspaceOpensBook() {
    std::printf("\n=== рабочее место: открытие книги ===\n");

    Sandbox box("open");
    Workspace ws(box.root);

    const Warmed warmed = run(ws.readBook(kBook));

    check(warmed.book != nullptr, "книга прочитана и разобрана");
    check(warmed.fileSize == std::filesystem::file_size(kBook), "размер файла — настоящий");

    const Opened opened = run(ws.openBook(*warmed.book, warmed.fileSize));

    check(opened.state.charOffset == 0 && opened.state.bookmarks.empty(), "новая книга — с чистого листа");
    check(ws.settings.lastBookGuid == opened.entry.guid && ws.settings.lastBookPath == kBook,
          "настройки указывают на книгу");

    Library onDiskLibrary;
    check(onDiskLibrary.loadFrom(onDisk(ws.libraryPath())) && onDiskLibrary.books().size() == 1,
          "реестр записан");
    Settings onDiskSettings;
    check(readSettings(onDisk(ws.settingsPath()), onDiskSettings) &&
              onDiskSettings.lastBookGuid == opened.entry.guid,
          "настройки записаны");

    BookState state;
    state.charOffset = 12345;
    run(ws.saveState(opened.entry.guid, state));

    const Opened reopened = run(ws.openBook(*warmed.book, warmed.fileSize));

    check(reopened.entry.guid == opened.entry.guid, "повторное открытие — та же запись");
    check(reopened.state.charOffset == 12345, "повторное открытие — то же место");
    check(run(ws.lastBookPath()) == kBook, "последняя книга — она");

    Workspace later(box.root);
    const Started started = run(later.start());
    check(started.notices.empty() && started.lastBook == kBook, "следующий запуск продолжит её");
}

/// Обход каталога: книги регистрируются по одной, о каждой новой говорят
/// сразу, мусор пропускается, реестр пишется один раз; повтор ничего не
/// добавляет.
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

    std::vector<BookEntry> appeared;
    const auto onNew = [&appeared](const BookEntry& entry) { appeared.push_back(entry); };

    const FolderAdded first = run(ws.addFolder(folder, onNew));

    check(first.added, "каталог с книгами — реестр записан");
    check(first.unread.empty(), "все файлы прочитались");
    check(appeared.size() == 2 && ws.library.books().size() == 2, "две книги, мусор пропущен");

    Library onDiskLibrary;
    check(onDiskLibrary.loadFrom(onDisk(ws.libraryPath())) && onDiskLibrary.books().size() == 2,
          "реестр на диске — две книги");

    const FolderAdded second = run(ws.addFolder(folder, onNew));

    check(second.unread.empty() && appeared.size() == 2, "повторный обход — ни одной новой");
    check(ws.library.books().size() == 2, "повторный обход не множит записи");
}

/// Последняя книга: путь из настроек протух — по guid из реестра; нет и там —
/// пусто.
void testWorkspaceFindsLastBook() {
    std::printf("\n=== рабочее место: последняя книга ===\n");

    Sandbox box("last");
    Workspace ws(box.root);

    const std::string bytes = onDisk(kBook);
    const fb3::Document document{std::string(bytes)};
    const Registered registered = run(ws.registerBook(document, kBook, bytes.size()));

    ws.settings.lastBookPath = box.root / L"moved-away.fb3";
    ws.settings.lastBookGuid = registered.entry.guid;

    check(run(ws.lastBookPath()) == kBook, "путь протух — книга найдена по guid");

    ws.settings.lastBookGuid = newGuid();

    check(run(ws.lastBookPath()).empty(), "guid неизвестен — продолжать нечего");
}

/// Картинка или нет — по байтам, а не по имени.
void testWorkspaceTellsImages() {
    std::printf("\n=== рабочее место: снимок ===\n");

    Sandbox box("image");
    Workspace ws(box.root);

    check(run(ws.isImage(testdata / L"bg_paper2.jpg")), "jpeg — картинка");
    check(!run(ws.isImage(kBook)), "книга — не картинка");
    check(run(ws.readBytes(kBook)) == onDisk(kBook), "байты файла целиком");
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

    // Петля операций wxl — одна на процесс, как у тестов самой wxl: её каналы
    // живут столько же, сколько процесс, и второй раз не стартуют. COM — для
    // декодера картинок (WIC).
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    sta_loop::start("bukvitsa.lib tests: I/O");

    testWorkspaceStartsEmpty();
    testWorkspaceKeepsBrokenFiles();
    testWorkspaceRegistersBook();
    testWorkspaceKeepsLastBookGuid();
    testWorkspaceOpensBook();
    testWorkspaceAddsFolder();
    testWorkspaceFindsLastBook();
    testWorkspaceTellsImages();

    sta_loop::stop();
    ::CoUninitialize();

    std::printf("\n%s\n", failures == 0 ? "OK" : "ЕСТЬ ОШИБКИ");
    return failures;
}
