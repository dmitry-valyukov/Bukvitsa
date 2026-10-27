#include <algorithm>
#include <utility>

#include <objbase.h>

// «book.h» здесь нет: реестру хватает описания книги и копии её обложки.

// Последними: реестр и хранилище импортируют wxl.core.
#include "bukvitsa/reader/library.h"
#include "bukvitsa/reader/shelf_text.h"
#include "bukvitsa/reader/store.h"

#include "bukvitsa/typography/system_failure.h"

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

/// Определена ниже, рядом с тем, что делает, — а нужна уже в entryFor().
BookEntry describe(const fb3::Description& description, uint32_t characterCount, const BookFile& file);

/// Карточка записи. Самосчитающая рождается со счётчиком 1, и указатель эту
/// ссылку принимает, а не добавляет свою.
intrusive_ptr<ShelfCard> cardOf(BookEntry entry) {
    return intrusive_ptr<ShelfCard>{new ShelfCard{std::move(entry)}, false};
}

/// Время правки из атрибута: тики `file_clock` целым со знаком. Нет атрибута
/// или в нём не число — неизвестно, и перепроверка прочтёт файл заново:
/// испорченное время стоит одного чтения, а не книги.
std::chrono::file_clock::time_point modifiedOf(const wxl::xml::node& element) {
    std::chrono::file_clock::rep ticks = 0;
    if (const auto value = element.attribute("modified")) try_parse(value->chars(), ticks);
    return std::chrono::file_clock::time_point{std::chrono::file_clock::duration{ticks}};
}

}  // namespace

bool samePath(const std::filesystem::path& one, const std::filesystem::path& other) {
    const std::wstring& first = one.native();
    const std::wstring& second = other.native();
    return first.size() == second.size() &&
           ::CompareStringOrdinal(first.c_str(), static_cast<int>(first.size()), second.c_str(),
                                  static_cast<int>(second.size()), TRUE) == CSTR_EQUAL;
}

bool sameBookId(u16_view one, u16_view other) {
    return !one.empty() && one == other;
}

bool samePlace(u16_view oneBookId, const std::filesystem::path& onePath, u16_view otherBookId,
               const std::filesystem::path& otherPath) {
    return (oneBookId.empty() || otherBookId.empty()) && samePath(onePath, otherPath);
}

ShelfCard::ShelfCard(BookEntry book)
    : entry(std::move(book)), progress(entry.characterCount == 0 ? progressLine(0, 0) : u16_text{}) {}

void ShelfCard::showState(const BookState& state) {
    if (state.charOffset == 0 && state.bookmarks.empty()) return;

    progress.set(shelfLine(state.charOffset, entry.characterCount, state.bookmarks.size()));
}

u16_text newGuid() {
    // Пустой guid — не guid: по нему запись реестра не найти и не отличить
    // от другой такой же. Отказ CoCreateGuid — беда системы, остановка.
    GUID guid{};
    typography::stopIfFailed(::CoCreateGuid(&guid), "newGuid: CoCreateGuid не выдал guid");

    // Ноль — буфер мал, а 40 знаков guid в скобках хватает всегда: ошибка в
    // коде, а не ответ.
    wchar_t text[40]{};
    const int written = ::StringFromGUID2(guid, text, static_cast<int>(std::size(text)));
    if (written == 0) wxl::core::abort("newGuid: guid не уместился в строку");

    // Скобки, шестнадцатеричные цифры и дефисы — ASCII по определению, так
    // что проверять здесь нечего. В `written` считается и завершающий ноль.
    return u16_text{unicode::assume_valid(std::wstring_view(text, static_cast<size_t>(written - 1)))};
}

bool Library::loadFrom(std::string xml) {
    if (xml.empty()) {
        cards_.clear();
        folders_.clear();
        return true;
    }

    // Карточки собираются в стороне и встают разом: полка слышит один сброс,
    // а не книгу за книгой, и битый файл не оставляет на ней половины реестра.
    // Каталоги — так же: битый файл не оставляет и половины их.
    sta_vector<intrusive_ptr<ShelfCard>> cards;
    std::vector<std::filesystem::path> folders;

    try {
        wxl::xml::document document;
        const wxl::xml::node& root = document.load(std::move(xml));

        // Раздела каталогов у первой версии нет, как и атрибутов folder,
        // modified и missing у книг: её записи читаются без них — время правки
        // неизвестно, и первая перепроверка каталога прочтёт книги заново.
        // Каталог, названный дважды (файл правили руками), — один каталог.
        if (const wxl::xml::node* watched = root.child("folders")) {
            for (const wxl::xml::node& element : watched->children_named("folder")) {
                std::filesystem::path folder(attributeOf(element, "path").wchars());
                const auto known = [&folder](const std::filesystem::path& other) { return samePath(other, folder); };
                if (!folder.empty() && std::ranges::none_of(folders, known)) folders.push_back(std::move(folder));
            }
        }

        for (const wxl::xml::node& element : root.children_named("book")) {
            BookEntry entry;
            entry.guid = attributeOf(element, "guid");
            entry.path = std::filesystem::path(attributeOf(element, "path").wchars());
            entry.bookId = attributeOf(element, "bookId");
            entry.cover = attributeOf(element, "cover");
            entry.title = attributeOf(element, "title");
            entry.authors = attributeOf(element, "authors");
            entry.fileSize = numberOf(element, "size");
            entry.modified = modifiedOf(element);
            entry.folder = std::filesystem::path(attributeOf(element, "folder").wchars());
            entry.characterCount = static_cast<uint32_t>(numberOf(element, "characters"));

            // Без guid запись бесполезна: под ним лежит место чтения, и
            // выдать ей новый значило бы потерять прочитанное. Такого в файле,
            // который писали мы, не бывает — но файл могли и поправить руками.
            if (entry.guid.empty() || entry.path.empty()) continue;

            // Пометка — `missing="true"`, и только так: пишется атрибут лишь у
            // ненайденной книги. Иное значение, хотя бы и «false», вписанное
            // руками, — не пометка: книга на месте, пока перепроверка не скажет
            // другого.
            const std::optional<u8_view> missing = element.attribute("missing");
            intrusive_ptr<ShelfCard> card = cardOf(std::move(entry));
            card->missing_.set(missing && missing->chars() == "true");
            cards.push_back(std::move(card));
        }
    } catch (...) {
        // Битый реестр — это пустая витрина, а не отказ запуститься. Книги
        // при этом никуда не денутся: они лежат там, где лежали, и добавятся
        // снова. Сказать об этом читателю — дело вызывающего.
        cards_.clear();
        folders_.clear();
        return false;
    }

    folders_ = std::move(folders);
    cards_.assign(std::move(cards));
    return true;
}

std::string Library::toXml() const {
    // Аллокатор -- STA-пул: и этот формирователь, и bookStateXml ниже зовут
    // корутины рабочего места (`Workspace`) до своего `co_await`, то есть на
    // потоке вызова -- у читалки интерфейсном; на рабочий поток уходят только
    // готовые байты.
    text_builder<sta_allocator> out;

    out.append("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n");
    out.format("<library version=\"{}\">\n", kVersion);

    // Раздел каталогов — только когда они есть: пустых узлов реестр не пишет.
    if (!folders_.empty()) {
        out.append("  <folders>\n");
        for (const std::filesystem::path& folder : folders_) {
            out.append("    <folder");
            attribute(out, "path", folder.native());
            out.append("/>\n");
        }
        out.append("  </folders>\n");
    }

    for (const intrusive_ptr<ShelfCard>& card : cards_) {
        const BookEntry& entry = card->entry;
        out.append("  <book");
        attribute(out, "guid", entry.guid);
        attribute(out, "title", entry.title);
        attribute(out, "authors", entry.authors);
        attribute(out, "bookId", entry.bookId);
        attribute(out, "cover", entry.cover);
        attribute(out, "path", entry.path.native());

        // Каталог, время правки и пометка — только у той книги, у которой они
        // есть: добавленная файлом каталога не знает, открытая файлом не знает
        // времени, а найденная пометки не носит — её отсутствие и значит
        // «на месте».
        if (!entry.folder.empty()) attribute(out, "folder", entry.folder.native());
        out.format(" size=\"{}\"", entry.fileSize);
        if (entry.modified != kUnknownTime) out.format(" modified=\"{}\"", entry.modified.time_since_epoch().count());
        out.format(" characters=\"{}\"", entry.characterCount);
        if (card->missing_.get()) out.append(" missing=\"true\"");
        out.append("/>\n");
    }

    out.append("</library>\n");

    return std::string(out.view());
}

bool Library::addFolder(std::filesystem::path folder) {
    const auto known = [&folder](const std::filesystem::path& other) { return samePath(other, folder); };
    if (std::ranges::any_of(folders_, known)) return false;

    folders_.push_back(std::move(folder));
    return true;
}

const BookEntry* Library::find(u16_view guid) const {
    const std::optional<uint32_t> at = placeOf(guid);
    return at ? &cards_[*at]->entry : nullptr;
}

const BookEntry* Library::findFile(const std::filesystem::path& path) const {
    for (const intrusive_ptr<ShelfCard>& card : cards_) {
        if (samePath(card->entry.path, path)) return &card->entry;
    }
    return nullptr;
}

const BookEntry* Library::findSame(const BookEntry& candidate) const {
    const std::optional<uint32_t> at = placeOfSame(candidate);
    return at ? &cards_[*at]->entry : nullptr;
}

std::optional<uint32_t> Library::placeOfSame(const BookEntry& candidate) const {
    // Сперва UUID, по всему реестру: запись с тем же UUID — та же книга, даже
    // если раньше неё стоит запись без UUID по тому же пути.
    for (uint32_t at = 0; at < cards_.size(); ++at) {
        if (sameBookId(cards_[at]->entry.bookId, candidate.bookId)) return at;
    }

    // По пути — только если UUID не помог и не опровергает: у одной из двух
    // записей его нет. Два разных UUID по одному пути — другая книга на месте
    // прежней; сведённые путём, они отдали бы новой guid прежней, и новая
    // открылась бы на чужой странице.
    for (uint32_t at = 0; at < cards_.size(); ++at) {
        const BookEntry& stored = cards_[at]->entry;
        if (samePlace(stored.bookId, stored.path, candidate.bookId, candidate.path)) return at;
    }
    return std::nullopt;
}

std::optional<uint32_t> Library::placeOf(u16_view guid) const {
    for (uint32_t at = 0; at < cards_.size(); ++at) {
        if (cards_[at]->entry.guid == guid) return at;
    }
    return std::nullopt;
}

BookEntry Library::entryFor(const fb3::Description& description, uint32_t characterCount,
                            const std::optional<BookCover>& cover, const BookFile& file,
                            u16_view rememberedGuid) const {
    BookEntry entry = describe(description, characterCount, file);

    if (const std::optional<uint32_t> known = placeOfSame(entry)) {
        // Guid остаётся прежним: за ним место чтения, и книга, которую
        // переложили в другую папку, должна открыться там же, где закрылась.
        const BookEntry& stored = cards_[*known]->entry;
        entry.guid = stored.guid;

        // Каталог и время правки знает обход, а открытие файлом — нет: у того
        // же файла неизвестное остаётся от записи. Время — только при том же
        // размере: другой размер — другой файл, и его время неизвестно.
        // Переложенная книга не берёт ни того, ни другого: её каталог и время
        // остались у прежнего пути.
        if (samePath(stored.path, entry.path)) {
            if (entry.folder.empty()) entry.folder = stored.folder;
            if (entry.modified == kUnknownTime && entry.fileSize == stored.fileSize) entry.modified = stored.modified;
        }
    } else {
        // Прежний guid — только свободный: занятый значил бы, что по пути из
        // настроек лежит уже другая книга, а эта запись — чья-то ещё. Занят он
        // и тогда, когда эта книга легла на место той: add уберёт прежнюю
        // запись, но guid её с местом чтения новой не достанется.
        entry.guid = rememberedGuid.empty() || find(rememberedGuid) ? newGuid() : u16_text{rememberedGuid};
    }

    entry.cover = coverOf(cover, entry.guid).name;
    return entry;
}

AddedBook Library::add(const fb3::Description& description, uint32_t characterCount,
                       const std::optional<BookCover>& cover, const BookFile& file, u16_view rememberedGuid) {
    BookEntry entry = entryFor(description, characterCount, cover, file, rememberedGuid);

    // У пути одна запись. Другая запись по пути этой книги — другой guid —
    // значит, её файл перезаписан: на нём другая книга с другим UUID или эта
    // же, узнанная по UUID и переложенная поверх файла книги без UUID. Той
    // книги больше нет, и запись её уходит раньше, чем встанет эта, — вместе
    // с местом чтения: оно было местом в той книге. Это не «не найдена» —
    // пометка обещала бы, что файл вернётся. Оставь её — у пути было бы две
    // записи: та никогда бы не пометилась (путь в листинге есть), а поиск по
    // пути находил бы то одну, то другую.
    for (uint32_t at = cards_.size(); at-- > 0;) {
        const BookEntry& stored = cards_[at]->entry;
        if (stored.guid != entry.guid && samePath(stored.path, entry.path)) cards_.erase(at);
    }

    // Запись и место решены одним ходом: entryFor отдал guid той же книги, если
    // она в реестре, а свободный — если нет.
    if (const std::optional<uint32_t> known = placeOf(entry.guid)) {
        ShelfCard& stored = *cards_[*known];

        // Файл только что прочитан — значит, он на месте, какой бы ни вышла
        // запись.
        const bool restored = stored.missing_.get();

        // Та же запись — та же карточка: книгу открыли снова, и полке нечего
        // перестраивать, — снимается разве что пометка. Другая — новая
        // карточка на том же месте, без пометки; строку прогресса она берёт у
        // прежней, пока полка не перечитает файл состояния.
        if (entry == stored.entry) {
            stored.missing_.set(false);
            return AddedBook{stored.entry, false, false, restored};
        }

        intrusive_ptr<ShelfCard> card = cardOf(std::move(entry));
        card->progress.set(stored.progress.get());
        cards_.replace(*known, std::move(card));
        return AddedBook{cards_[*known]->entry, false, true, restored};
    }

    cards_.push_back(cardOf(std::move(entry)));
    return AddedBook{cards_[cards_.size() - 1]->entry, true, true, false};
}

bool Library::setMissing(u16_view guid, bool missing) {
    const std::optional<uint32_t> at = placeOf(guid);
    if (!at || cards_[*at]->missing_.get() == missing) return false;

    cards_[*at]->missing_.set(missing);
    return true;
}

bool Library::erase(u16_view guid) {
    const std::optional<uint32_t> at = placeOf(guid);
    if (!at) return false;

    cards_.erase(*at);
    return true;
}


std::optional<BookCover> copyCover(const fb3::Document& document) {
    const std::optional<uint32_t> index = document.description().coverImageIndex;
    if (!index) return std::nullopt;

    // image(), а не images()[index]: байты части читаются при первом
    // обращении, и в списке их может ещё не быть.
    const fb3::ImagePart* part = document.image(*index);
    if (!part || part->bytes.empty()) return std::nullopt;

    return BookCover{part->contentType, part->bytes};
}

CoverBytes coverOf(const std::optional<BookCover>& cover, u16_view guid) {
    if (!cover || guid.empty()) return {};

    const u16_view extension = coverExtension(cover->contentType.chars());
    if (extension.empty()) return {};   // svg и прочее, чего Image не покажет

    CoverBytes cached;
    cached.name = u16_text{guid};
    cached.name += extension;
    cached.bytes = cover->bytes;

    return cached;
}

namespace {

BookEntry describe(const fb3::Description& description, uint32_t characterCount, const BookFile& file) {
    BookEntry entry;
    entry.path = file.path;
    // Ни одного assume_valid: модель книги отдаёт проверенный текст, потому
    // что документ проверила wxl.xml, когда его открывала.
    entry.bookId = description.id.to_utf16();
    entry.title = description.title.to_utf16();
    entry.authors = description.authorsLine().to_utf16();
    entry.characterCount = characterCount;

    // Размер, время правки и каталог приходят снаружи: узнать их -- обращение к
    // диску, а на этом потоке их не бывает. Размер спрашивает тот, кто читал
    // сам файл, время и каталог — обход, у которого они в листинге.
    entry.fileSize = file.size;
    entry.modified = file.modified;
    entry.folder = file.folder;

    // Книга без названия бывает: в витрине лучше имя файла, чем пустая строка.
    // Имя файла Windows не обязано быть правильным UTF-16.
    if (entry.title.empty()) entry.title = unicode::repaired(file.path.filename().native());

    return entry;
}

}  // namespace

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
