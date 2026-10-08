// Тесты мелких кусков модели читалки:
// закладка туда-обратно, слова о сбое сценария, слот прогретой книги, строка
// карточки полки, запрос поиска. Каждый — чистая функция или маленький
// объект, и проверяется без окна.

#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "check.h"

// Заголовки читалки после всех стандартных: они несут импорт. Порядок тот же,
// что в lib_tests.cpp: индекс книги раньше, рабочее место (его включает слот
// прогретой книги) — последним.
#include "bukvitsa/reader/book_index.h"
#include "bukvitsa/reader/shelf_text.h"
#include "bukvitsa/reader/warm_slot.h"

import wxl.async;
import wxl.core;

using namespace bukvitsa::reader;
using bukvitsa::reader::tests::check;
using wxl::async::system_exception;

namespace {

const std::filesystem::path testdata = BUKVITSA_TESTDATA_DIR;

std::string bytesOf(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::vector<uint32_t> offsetsOf(const BookState& state) {
    std::vector<uint32_t> offsets;
    for (const Bookmark& mark : state.bookmarks) offsets.push_back(mark.charOffset);
    return offsets;
}

/// Закладка: одна кнопка ставит и снимает; список всегда по порядку книги,
/// куда бы ни встала новая.
void testBookmarkToggles() {
    std::printf("\n=== закладка: поставить и снять ===\n");

    BookState state;

    check(state.toggleBookmark(300, u16_text{u"триста"}), "на пустом месте — поставлена");
    check(state.hasBookmark(300) && state.bookmarks.size() == 1, "поставленная видна");

    state.toggleBookmark(100, u16_text{u"сто"});
    check(state.toggleBookmark(200, u16_text{u"двести"}), "между двумя — поставлена");
    check(offsetsOf(state) == std::vector<uint32_t>{100, 200, 300}, "вставка в середину держит порядок книги");
    check(state.bookmarks[1].hint == L"двести", "подсказка — у своей закладки");

    check(!state.toggleBookmark(200, u16_text{u"другие слова"}), "на том же месте — снята");
    check(!state.hasBookmark(200) && offsetsOf(state) == std::vector<uint32_t>{100, 300},
          "снята только она");

    check(state.toggleBookmark(0, u16_text{}), "в самом начале книги — тоже ставится");
    check(offsetsOf(state) == std::vector<uint32_t>{0, 100, 300}, "начало книги — первой");

    check(state.toggleBookmark(200, u16_text{u"снова"}), "снятая ставится снова");
    check(offsetsOf(state) == std::vector<uint32_t>{0, 100, 200, 300} && state.bookmarks[2].hint == L"снова",
          "на своё место и с новой подсказкой");
}

/// Сбой сценария словами: операция с файлом — причиной системы, исключение с
/// текстом — его текстом, если он UTF-8, всё прочее — фактом.
void testNoticeOfFailure() {
    std::printf("\n=== слова о сбое сценария ===\n");

    const system_exception failure("ReadFile", static_cast<int>(ERROR_FILE_NOT_FOUND));
    const Notice onFile = noticeOf(std::make_exception_ptr(failure));
    check(onFile.headline == L"Не удалось выполнить операцию с файлом",
          "операция с файлом — заголовок о файле, а не «Ошибка»");
    check(onFile.details == reasonOf(failure), "подробности — причина словами системы");
    check(onFile.details.find(L"ReadFile") != std::wstring::npos && onFile.details.find(L"= 2") != std::wstring::npos,
          "в причине — что делали и код системы");

    const Notice withPath = noticeOf(L"Не удалось прочитать файл книги", L"C:\\книги\\роман.fb3", failure);
    check(withPath.headline == L"Не удалось прочитать файл книги" &&
              withPath.details == L"C:\\книги\\роман.fb3\n\n" + reasonOf(failure),
          "сбой с файлом: что не удалось, путь и причина через пустую строку");
    check(noticeOf(L"Не удалось", L"", failure).details == reasonOf(failure),
          "без пути — одна причина, без пустых строк впереди");

    const Notice said = noticeOf(std::make_exception_ptr(std::runtime_error("Книга испорчена: нет описания")));
    check(said.headline == L"Ошибка" && said.details == L"Книга испорчена: нет описания",
          "исключение с текстом UTF-8 — его текст");

    const Notice garbled = noticeOf(std::make_exception_ptr(std::runtime_error("\xFF\xFE broken")));
    check(garbled.headline == L"Ошибка" && garbled.details == L"(сообщение не в UTF-8)",
          "текст не в UTF-8 — так и сказано, без мусора");

    const Notice unknown = noticeOf(std::make_exception_ptr(42));
    check(unknown.headline == L"Неизвестная ошибка" && unknown.details == L"Сценарий прерван.",
          "брошено не исключение — просто факт");
}

/// Слот прогретой книги и гонка «открыли, пока грелось». Книга настоящая:
/// «есть ли книга в слоте» — это указатель, и пустой слот за книгу не считает.
void testWarmSlotRace() {
    std::printf("\n=== слот прогретой книги ===\n");

    const std::filesystem::path a = testdata / L"Turgenev_I._Spisokshkolnoy._Otcyi_I_Deti.fb3";
    const std::filesystem::path b = testdata / L"nightmare_example.fb3";

    std::string bytes = bytesOf(a);
    const uint64_t size = bytes.size();
    const auto book = std::make_shared<Book>(a, std::move(bytes), dwriteFactory());

    // Открыли, пока грелось: открытие сняло заказ, прогрев вернулся позже.
    WarmSlot slot;
    slot.expect(a);
    slot.cancel();
    slot.fill(a, book, size);
    check(!slot.holds(a) && !slot.take(a), "открыли, пока грелось, — прогретое выброшено");
    check(book.use_count() == 1, "выброшенную книгу слот не держит — второй копии в памяти нет");

    // Прогрелась вовремя: отдаётся только своему пути и один раз.
    slot.expect(a);
    slot.fill(a, book, size);
    check(slot.holds(a), "прогретая лежит под своим путём");
    check(!slot.holds(b) && !slot.take(b), "другой книге прогретую не отдают");
    check(slot.holds(a), "чужая просьба слот не опустошает");

    const std::optional<Warmed> taken = slot.take(a);
    check(taken && taken->book == book && taken->fileSize == size, "своей — та же книга и её размер");
    check(!slot.holds(a) && !slot.take(a), "взятая — слот пуст");

    // Заказ сменился, пока грелась прежняя: прежняя не ложится.
    slot.expect(a);
    slot.expect(b);
    slot.fill(a, book, size);
    check(!slot.holds(a) && !slot.holds(b), "заказ сменился — прежний прогрев выброшен");

    // Новый заказ начинает с пустого слота: держится одна книга.
    slot.expect(a);
    slot.fill(a, book, size);
    slot.expect(a);
    check(!slot.holds(a), "новый заказ выбрасывает прогретое прежде");
}

/// Строка карточки полки: проценты, «не открывалась», закладки.
void testShelfLine() {
    std::printf("\n=== строка карточки полки ===\n");

    check(progressLine(420, 1000) == L"прочитано 42%", "прочитано 42%");
    check(progressLine(1000, 1000) == L"прочитано 100%", "дочитана — 100%");
    check(progressLine(1, 1000) == L"прочитано 0%", "только начата — 0%, а не «не открывалась»");
    check(progressLine(0, 1000) == L"не открывалась", "место в самом начале — не открывалась");
    check(progressLine(500, 0) == L"не открывалась", "знаков ноль — не открывалась, деления на ноль нет");

    check(bookmarksLine(0).empty(), "нет закладок — пусто");

    // Подпись с двоеточием: число после слова, и форма одна при любом числе.
    struct Count {
        size_t count;
        std::wstring_view said;
    };
    const Count counts[] = {
        {1, L"закладок: 1"}, {2, L"закладок: 2"}, {5, L"закладок: 5"}, {11, L"закладок: 11"}, {21, L"закладок: 21"},
    };
    for (const Count& one : counts) {
        check(bookmarksLine(one.count) == one.said, "закладки числом " + std::to_string(one.count));
    }

    check(shelfLine(420, 1000, 3) == L"прочитано 42%    закладок: 3", "прогресс и закладки — через отбивку");
    check(shelfLine(420, 1000, 0) == L"прочитано 42%", "без закладок — только прогресс");
    check(shelfLine(0, 1000, 2) == L"не открывалась", "«не открывалась» закладок не показывает");
}

/// Запрос поиска: что набрал читатель и что из этого ищется.
void testSearchQuery() {
    std::printf("\n=== запрос поиска ===\n");

    check(!searchQuery(u""), "пустое поле — не искать");
    check(!searchQuery(u"   \t  "), "одни пробелы и табуляции — не искать");

    std::u16string lone(u"слово");
    lone.push_back(static_cast<char16_t>(0xD800));
    check(!searchQuery(lone), "одиночная половина суррогатной пары — не искать");

    const std::u16string typed(u"  отцы и дети ");
    const std::optional<u16_view> query = searchQuery(typed);
    check(query && *query == L"  отцы и дети ", "слово — искать как набрано, с краями");
    check(query && query->data() == typed.data(), "запрос — вид в набранное, без копии");
}

}  // namespace

void runPiecesTests() {
    testBookmarkToggles();
    testNoticeOfFailure();
    testWarmSlotRace();
    testShelfLine();
    testSearchQuery();
}
