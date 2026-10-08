#include <objbase.h>

// «book.h» больше не нужен: реестр работает с разобранным документом.
#include "settings.h"

// Последними: реестр и хранилище импортируют wxl.core.
#include "library.h"
#include "store.h"

// После своих заголовков: document.h тянет import wxl.core, а стандартный
// заголовок после импорта MSVC уже не принимает.
import wxl.core;
import wxl.fmt;
import wxl.xml;

namespace bukvitsa::reader {
namespace {

/// Расширение по типу содержимого части. Пустое значит «показать это нечем»:
/// обложку читает XAML, а он знает те же форматы, что и WIC.
u16_view coverExtension(std::string_view contentType) {
    if (contentType == "image/jpeg" || contentType == "image/jpg") return u16_view{u".jpg"};
    if (contentType == "image/png") return u16_view{u".png"};
    if (contentType == "image/gif") return u16_view{u".gif"};
    if (contentType == "image/bmp") return u16_view{u".bmp"};
    return {};
}

/// Один атрибут: имя, значение, экранирование. Отдельной функцией, потому что
/// в реестре их семь на запись, и повторять xmlValue() семь раз — значит однажды
/// забыть. Проверенный текст перед нами или путь, который надо чинить, --
/// разбирает xmlValue() по типу значения.
void attribute(text_builder<sta_allocator>& out, std::string_view name, const auto& value) {
    out.format(" {}=\"{}\"", name, xmlValue(value));
}

/// Определена ниже, рядом с тем, что делает, — а нужна уже в add().
BookEntry describe(const fb3::Document& document, const std::filesystem::path& path,
                   uint64_t fileSize);

}  // namespace

std::filesystem::path libraryPath() {
    return dataDirectory() / L"library.xml";
}

std::filesystem::path statePath(u16_view guid) {
    // Имя файла — guid и ничего больше: он наш, выдан CoCreateGuid, и в нём
    // не может оказаться ни разделителя пути, ни двоеточия. Названия книги
    // здесь нет намеренно — из него имя файла пришлось бы вычищать.
    std::filesystem::path file{guid.wchars()};
    file += L".xml";
    return dataDirectory() / L"books" / file;
}

u16_text newGuid() {
    GUID guid{};
    if (FAILED(::CoCreateGuid(&guid))) return {};

    wchar_t text[40]{};
    const int written = ::StringFromGUID2(guid, text, static_cast<int>(std::size(text)));
    if (written == 0) return {};

    // Скобки, шестнадцатеричные цифры и дефисы — ASCII по определению, так
    // что проверять здесь нечего. В `written` считается и завершающий ноль.
    return u16_text{unicode::assume_valid(std::wstring_view(text, static_cast<size_t>(written - 1)))};
}

bool Library::loadFrom(std::string xml) {
    books_.clear();

    if (xml.empty()) return true;

    try {
        wxl::xml::document document;
        const wxl::xml::node& root = document.load(std::move(xml));

        for (const wxl::xml::node& element : root.children_named("book")) {
            BookEntry entry;
            entry.guid = attributeOf(element, "guid");
            entry.path = std::filesystem::path(attributeOf(element, "path").wchars());
            entry.bookId = attributeOf(element, "bookId");
            entry.cover = attributeOf(element, "cover");
            entry.title = attributeOf(element, "title");
            entry.authors = attributeOf(element, "authors");
            entry.fileSize = numberOf(element, "size");
            entry.characterCount = static_cast<uint32_t>(numberOf(element, "characters"));

            // Без guid запись бесполезна: под ним лежит место чтения, и
            // выдать ей новый значило бы потерять прочитанное. Такого в файле,
            // который писали мы, не бывает — но файл могли и поправить руками.
            if (!entry.guid.empty() && !entry.path.empty()) books_.push_back(std::move(entry));
        }
    } catch (...) {
        // Битый реестр — это пустая витрина, а не отказ запуститься. Книги
        // при этом никуда не денутся: они лежат там, где лежали, и добавятся
        // снова. Сказать об этом читателю — дело вызывающего.
        books_.clear();
        return false;
    }
    return true;
}

std::string Library::toXml() const {
    // Аллокатор -- STA-пул: и этот формирователь, и bookStateXml ниже зовутся
    // из корутин между двумя `co_await`, то есть в интерфейсном потоке (см.
    // main.cpp); на рабочий поток уходят только готовые байты.
    text_builder<sta_allocator> out;

    out.append("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n");
    out.format("<library version=\"{}\">\n", kVersion);

    for (const BookEntry& entry : books_) {
        out.append("  <book");
        attribute(out, "guid", entry.guid);
        attribute(out, "title", entry.title);
        attribute(out, "authors", entry.authors);
        attribute(out, "bookId", entry.bookId);
        attribute(out, "cover", entry.cover);
        attribute(out, "path", entry.path.native());
        out.format(" size=\"{}\" characters=\"{}\"/>\n", entry.fileSize, entry.characterCount);
    }

    out.append("</library>\n");

    return std::string(out.view());
}

const BookEntry* Library::find(u16_view guid) const {
    for (const BookEntry& entry : books_) {
        if (entry.guid == guid) return &entry;
    }
    return nullptr;
}

const BookEntry* Library::findSame(const BookEntry& candidate) const {
    if (!candidate.bookId.empty()) {
        for (const BookEntry& entry : books_) {
            if (entry.bookId == candidate.bookId) return &entry;
        }
    }

    // По пути — только если UUID не помог. Сравнение без учёта регистра:
    // на Windows это один и тот же файл.
    const std::wstring& wanted = candidate.path.native();
    for (const BookEntry& entry : books_) {
        const std::wstring& known = entry.path.native();
        if (known.size() == wanted.size() &&
            ::CompareStringOrdinal(known.c_str(), static_cast<int>(known.size()), wanted.c_str(),
                                   static_cast<int>(wanted.size()), TRUE) == CSTR_EQUAL) {
            return &entry;
        }
    }
    return nullptr;
}

const BookEntry& Library::add(const fb3::Document& document, const std::filesystem::path& path,
                              uint64_t fileSize, u16_view rememberedGuid) {
    BookEntry entry = describe(document, path, fileSize);

    if (const BookEntry* known = findSame(entry)) {
        // Guid остаётся прежним: за ним место чтения, и книга, которую
        // переложили в другую папку, должна открыться там же, где закрылась.
        BookEntry& stored = books_[static_cast<size_t>(known - books_.data())];
        entry.guid = stored.guid;
        entry.cover = coverOf(document, entry.guid).name;
        stored = std::move(entry);
        return stored;
    }

    // Прежний guid — только свободный: занятый значил бы, что по пути из
    // настроек лежит уже другая книга, а эта запись — чья-то ещё.
    entry.guid = rememberedGuid.empty() || find(rememberedGuid) ? newGuid() : u16_text{rememberedGuid};
    entry.cover = coverOf(document, entry.guid).name;
    books_.push_back(std::move(entry));
    return books_.back();
}


std::filesystem::path coverDirectory() {
    return dataDirectory() / L"cache";
}

CoverBytes coverOf(const fb3::Document& document, u16_view guid) {
    const std::optional<uint32_t> index = document.description().coverImageIndex;
    if (!index || guid.empty()) return {};

    const fb3::ImagePart* part = document.image(*index);
    if (!part || part->bytes.empty()) return {};

    const u16_view extension = coverExtension(part->contentType.chars());
    if (extension.empty()) return {};   // svg и прочее, чего Image не покажет

    CoverBytes cover;
    cover.name = u16_text{guid};
    cover.name += extension;
    cover.bytes = part->bytes;

    return cover;
}

namespace {

BookEntry describe(const fb3::Document& document, const std::filesystem::path& path,
                   uint64_t fileSize) {
    const fb3::Description& description = document.description();

    BookEntry entry;
    entry.path = path;
    // Ни одного assume_valid: модель книги отдаёт проверенный текст, потому
    // что документ проверила wxl.xml, когда его открывала.
    entry.bookId = description.id.to_utf16();
    entry.title = description.title.to_utf16();
    entry.authors = description.authorsLine().to_utf16();
    entry.characterCount = document.characterCount();

    // Размер файла приходит снаружи: узнать его -- обращение к диску, а на
    // этом потоке их не бывает. Спрашивает его тот, кто читал сам файл, и
    // спрашивает заодно, одной операцией.
    entry.fileSize = fileSize;

    // Книга без названия бывает: в витрине лучше имя файла, чем пустая строка.
    // Имя файла Windows не обязано быть правильным UTF-16.
    if (entry.title.empty()) entry.title = unicode::repaired(path.filename().native());

    return entry;
}

}  // namespace

bool BookState::hasBookmark(uint32_t offset) const {
    for (const Bookmark& mark : bookmarks) {
        if (mark.charOffset == offset) return true;
    }
    return false;
}

BookState parseBookState(std::string xml) {
    BookState state;

    if (xml.empty()) return state;

    try {
        wxl::xml::document document;
        const wxl::xml::node& root = document.load(std::move(xml));

        if (const wxl::xml::node* reading = root.child("reading")) {
            state.charOffset = static_cast<uint32_t>(numberOf(*reading, "charOffset"));
        }
        if (const wxl::xml::node* marks = root.child("bookmarks")) {
            for (const wxl::xml::node& mark : marks->children_named("bookmark")) {
                Bookmark bookmark;
                bookmark.charOffset = static_cast<uint32_t>(numberOf(mark, "charOffset"));
                bookmark.hint = attributeOf(mark, "hint");
                state.bookmarks.push_back(std::move(bookmark));
            }
        }
    } catch (...) {
        // Битый файл состояния — это книга, открытая с начала, а не книга,
        // которая не открылась.
        return BookState{};
    }
    return state;
}

std::string bookStateXml(const BookState& state) {
    text_builder<sta_allocator> out;

    out.append("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n");
    out.format("<book version=\"{}\">\n", BookState::kVersion);
    // Позиция в символах книги, а не в страницах: страница меняется от кегля
    // и размера окна, символ — нет. Закладки меряются тем же.
    out.format("  <reading charOffset=\"{}\"/>\n", state.charOffset);

    if (!state.bookmarks.empty()) {
        out.append("  <bookmarks>\n");
        for (const Bookmark& mark : state.bookmarks) {
            out.format("    <bookmark charOffset=\"{}\" hint=\"{}\"/>\n", mark.charOffset,
                       xmlValue(mark.hint));
        }
        out.append("  </bookmarks>\n");
    }

    out.append("</book>\n");

    return std::string(out.view());
}

}  // namespace bukvitsa::reader
