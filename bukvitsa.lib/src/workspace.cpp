#include <shlobj.h>

// Свои заголовки — после системных: они несут импорт, а заголовок после
// импорта MSVC принимает не всякий.
#include "bukvitsa/reader/imaging.h"
#include "bukvitsa/reader/workspace.h"

import wxl.async;
import wxl.core;

using wxl::async::async_directory;
using wxl::async::async_file;
using wxl::async::system_exception;
using wxl::async::task;

namespace bukvitsa::reader {

namespace {

/// Путь читалки, переведённый в путь wxl. Строится в потоке вызова -- там, где
/// STA-пул, -- и уезжает в операцию копией.
path poolPath(const std::filesystem::path& system) {
    return path(std::wstring_view(system.native()));
}

/// Файла нет — обычное дело для настроек при первом запуске и для состояния
/// книги, которую ещё не открывали; всё остальное -- настоящая ошибка, у
/// которой есть имя.
bool absent(const system_exception& failure) {
    return failure.err_code() == ERROR_FILE_NOT_FOUND || failure.err_code() == ERROR_PATH_NOT_FOUND;
}

/// Имя и причина для списка «не прочитались» обхода каталога.
std::wstring unreadLine(const std::wstring& name, const system_exception& failure) {
    return name + L" — " + reasonOf(failure);
}

}  // namespace

std::filesystem::path standardDataDirectory() {
    // Подопытный каталог: сценарии прогона портят settings.xml и реестры, и
    // гонять их на настоящих данных читателя нельзя. Переменная подменяет
    // каталог целиком. Длина не ограничена MAX_PATH: путь к рабочей папке
    // сессии длиннее обычного, потому буфер спрашивается у самой Windows.
    if (const DWORD size = ::GetEnvironmentVariableW(L"BUKVITSA_DATA", nullptr, 0); size > 1) {
        std::wstring sandbox(size - 1, L'\0');
        if (::GetEnvironmentVariableW(L"BUKVITSA_DATA", sandbox.data(), size) == size - 1)
            return std::filesystem::path{std::move(sandbox)};
    }

    PWSTR folder = nullptr;
    if (FAILED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &folder))) {
        return {};
    }
    std::filesystem::path path{folder};
    ::CoTaskMemFree(folder);
    return path / L"Bukvitsa" / L"Reader";
}

Workspace::Workspace(std::filesystem::path directory) : directory_(std::move(directory)) {}

// ---- где что лежит --------------------------------------------------------

std::filesystem::path Workspace::settingsPath() const {
    return directory_ / L"settings.xml";
}

std::filesystem::path Workspace::libraryPath() const {
    return directory_ / L"library.xml";
}

std::filesystem::path Workspace::skinsPath() const {
    return directory_ / L"skins.xml";
}

std::filesystem::path Workspace::skinDirectory() const {
    return directory_ / L"skins";
}

std::filesystem::path Workspace::coverDirectory() const {
    return directory_ / L"cache";
}

std::filesystem::path Workspace::statePath(u16_view guid) const {
    std::filesystem::path file{guid.wchars()};
    file += L".xml";
    return directory_ / L"books" / file;
}

std::filesystem::path Workspace::skinImagePath(const Skin& skin) const {
    if (skin.image.empty()) return {};

    // У системной обложки снимок в Assets рядом с программой. Папку даёт
    // wxl.core, не wxl.ui: у тестов окна нет.
    return skin.system ? environment::application_folder() / L"Assets" / skin.image.wchars()
                       : skinDirectory() / skin.image.wchars();
}

// ---- запуск ---------------------------------------------------------------

task<Started> Workspace::start() {
    Started started;

    // Три файла по одной дороге: нет — первый запуск, молча; не читается —
    // слово читателю, а запуск идёт дальше с умолчаниями; прочитан, но не
    // разобран — слово и копия рядом. Разбор каждого — в его модель, у которой
    // наблюдаемые поля: о прочитанном контролы узнают сами.
    const auto read = [&](const std::filesystem::path& file, std::wstring whatFailed) -> task<std::string> {
        try {
            co_return co_await async_file::read_all(poolPath(file));
        } catch (const system_exception& failure) {
            if (!absent(failure))
                started.notices.push_back({std::move(whatFailed), file.wstring() + L"\n\n" + reasonOf(failure)});
            co_return std::string{};
        }
    };

    std::string settingsXmlText = co_await read(settingsPath(), L"Не удалось прочитать настройки");

    // Сам файл заменит первая же запись настроек, и ждать её не придётся: окно
    // встаёт на место по умолчанию, и таймер места окна запишет его через
    // секунды — сообщение читатель увидит уже над переписанным файлом, о чём
    // оно и говорит. Байты у копии свои, в памяти, так что порядок двух
    // записей безразличен.
    if (!readSettings(settingsXmlText, settings)) {
        started.notices.push_back({L"Настройки не прочитаны",
                                   settingsPath().wstring() +
                                       L"\n\nФайл испорчен: взяты умолчания, и они же сейчас лягут "
                                       L"в него — место окна пишется само, через секунды после "
                                       L"старта. Испорченный остаётся рядом копией "
                                       L"(settings.xml.bad)."});
        co_await keepBrokenCopy(settingsPath(), std::move(settingsXmlText));
    }

    // Обложки — раньше реестра книг и раньше темы у вызывающего: выбранной
    // темой может оказаться обложка, а её номер продолжает список за
    // встроенными и без реестра не существует.
    std::string skinsXmlText = co_await read(skinsPath(), L"Не удалось прочитать реестр обложек");

    if (!skins.loadFrom(skinsXmlText)) {
        started.notices.push_back({L"Реестр обложек не прочитан",
                                   skinsPath().wstring() +
                                       L"\n\nФайл испорчен: обложек нет. Копия лежит рядом "
                                       L"(skins.xml.bad), снимки в skins\\ целы, а первая же "
                                       L"запись реестра заменит сам файл."});
        co_await keepBrokenCopy(skinsPath(), std::move(skinsXmlText));
    }

    // Реестр читается всегда, а не только когда показывают полку: он
    // маленький, и без него не ответить на «продолжить чтение» по guid, если
    // путь в настройках протух.
    std::string libraryXmlText = co_await read(libraryPath(), L"Не удалось прочитать реестр книг");

    if (!library.loadFrom(libraryXmlText)) {
        started.notices.push_back({L"Реестр книг не прочитан",
                                   libraryPath().wstring() +
                                       L"\n\nФайл испорчен: полка пуста. Копия лежит рядом "
                                       L"(library.xml.bad), сами книги лежат там, где лежали; "
                                       L"первая же запись реестра — добавление книги или "
                                       L"«Продолжить чтение» — заменит сам файл, место чтения "
                                       L"последней книги при этом сохранится."});
        co_await keepBrokenCopy(libraryPath(), std::move(libraryXmlText));
    }

    started.lastBook = co_await lastBookPath();

    co_return started;
}

// ---- книги ----------------------------------------------------------------

task<Warmed> Workspace::readBook(std::filesystem::path path) {
    // Не const: байты уходят в книгу перемещением. Разбор — здесь, на потоке
    // вызова: память разбора берётся из STA-пула, а он чужого потока не терпит.
    std::string bytes = co_await async_file::read_all(poolPath(path));

    Warmed warmed;
    warmed.fileSize = bytes.size();
    warmed.book = std::make_shared<Book>(path, std::move(bytes), dwriteFactory());

    co_return warmed;
}

task<Registered> Workspace::registerBook(const fb3::Document& document, std::filesystem::path path,
                                         uint64_t fileSize) {
    // Guid, под которым настройки помнят этот файл: реестр мог книгу потерять
    // (битый library.xml), а её место чтения в books\{guid}.xml цело, и
    // «Продолжить чтение» не должно открывать книгу с начала. Другой файл —
    // пусто, и guid будет новым.
    const u16_view remembered = settings.lastBookPath == path ? u16_view{settings.lastBookGuid} : u16_view{};

    const size_t knownBefore = library.cards().size();

    // Копией, а не ссылкой: между co_await реестр может дополниться, и вектор
    // переедет вместе со всеми ссылками в него.
    Registered registered{library.add(document, path, fileSize, remembered), library.cards().size() != knownBefore};

    if (const CoverBytes cover = coverOf(document, registered.entry.guid); !cover.name.empty())
        co_await async_file::write_all(poolPath(coverDirectory() / cover.name.wchars()), std::string(cover.bytes));

    co_return registered;
}

task<Opened> Workspace::openBook(const Book& book, uint64_t fileSize) {
    // Порядок — это порядок обязательств: реестр с обложкой, затем настройки,
    // которые на книгу указывают, и лишь потом её состояние.
    const Registered registered = co_await registerBook(book.document(), book.path(), fileSize);

    co_await saveLibrary();

    settings.lastBookGuid = registered.entry.guid;
    settings.lastBookPath = registered.entry.path;

    co_await saveSettings();

    // Состояние — отдельной строкой, а не внутри фигурной инициализации:
    // `co_await` в списке инициализаторов агрегата роняет бэкенд MSVC 14.51
    // (C1001).
    BookState state = co_await readState(registered.entry.guid);

    Opened opened{registered.entry, std::move(state)};

    co_return opened;
}

task<FolderAdded> Workspace::addFolder(std::filesystem::path folder, function<void(const BookEntry&)> onNew) {
    FolderAdded result;

    const std::vector<async_directory::listed_entry> found =
        co_await async_directory::list(poolPath(folder / L"*.fb3"));

    for (const async_directory::listed_entry& entry : found) {
        if (entry.is_directory) continue;

        const std::filesystem::path path = folder / entry.name;

        std::string bytes;

        try {
            bytes = co_await async_file::read_all(poolPath(path));
        } catch (const system_exception& failure) {
            // Файлы, которые не прочитались, называются вызывающему разом в
            // конце: каталог с сотней книг не должен спотыкаться об один файл,
            // но и молчать о нём нельзя.
            result.unread.push_back(unreadLine(entry.name, failure));
            continue;
        }

        const uint64_t fileSize = bytes.size();

        // Разбор ловится вокруг разбора, а не вокруг всего шага: не книга, битая
        // книга, книга от будущего формата — не повод бросать обход.
        std::optional<fb3::Document> document;

        try {
            document.emplace(std::move(bytes));
        } catch (const std::exception&) {
            continue;
        }

        const Registered registered = co_await registerBook(*document, path, fileSize);

        result.added = true;

        // Полка растёт на каждой книге, а не в конце: в этом и смысл — читатель
        // видит, как она наполняется.
        if (registered.isNew) onNew(registered.entry);
    }

    if (result.added) co_await saveLibrary();

    co_return result;
}

task<std::filesystem::path> Workspace::lastBookPath() {
    // Путь в настройках — копия того, что в реестре, ради быстрой дороги;
    // протух — спрашиваем реестр по guid.
    std::filesystem::path last = settings.lastBookPath;

    if (!last.empty() && !co_await async_file::exists(poolPath(last))) last.clear();

    if (last.empty()) {
        if (const BookEntry* entry = library.find(settings.lastBookGuid)) {
            last = entry->path;

            if (!co_await async_file::exists(poolPath(last))) last.clear();
        }
    }

    co_return last;
}

task<BookState> Workspace::readState(u16_view guid) {
    // Состояния у книги может и не быть: её только что добавили, или место
    // чтения ещё не записывалось. Это не ошибка, а чистый лист.
    try {
        co_return parseBookState(co_await async_file::read_all(poolPath(statePath(guid))));
    } catch (const system_exception& failure) {
        if (!absent(failure)) throw;
        co_return BookState{};
    }
}

task<> Workspace::saveState(u16_text guid, BookState state) {
    if (guid.empty()) co_return;

    co_await async_file::write_all(poolPath(statePath(guid)), bookStateXml(state));
}

task<> Workspace::saveSettings() {
    std::string xml = settingsXml(settings);

    co_await async_file::write_all(poolPath(settingsPath()), std::move(xml));
}

task<> Workspace::saveLibrary() {
    std::string xml = library.toXml();

    co_await async_file::write_all(poolPath(libraryPath()), std::move(xml));
}

// ---- обложки читателя -----------------------------------------------------

task<> Workspace::saveSkin(Skin skin, std::filesystem::path photo) {
    if (skin.image.empty()) {
        std::string bytes = co_await async_file::read_all(poolPath(photo));

        // Расширение — от файла снимка, то есть от файловой системы: имя файла
        // Windows не обязано быть правильным UTF-16, потому чинится.
        u16_text file = newGuid();
        file += unicode::repaired(photo.extension().native());

        co_await async_file::write_all(poolPath(skinDirectory() / file.wchars()), std::move(bytes));

        skin.image = std::move(file);
    }

    skins.put(std::move(skin));

    std::string xml = skins.toXml();

    co_await async_file::write_all(poolPath(skinsPath()), std::move(xml));
}

task<> Workspace::deleteSkin(u16_view name) {
    if (!skins.remove(name)) co_return;   // реестр успел перемениться под руками

    std::string xml = skins.toXml();

    co_await async_file::write_all(poolPath(skinsPath()), std::move(xml));
}

task<bool> Workspace::isImage(std::filesystem::path image) {
    const std::string bytes = co_await async_file::read_all(poolPath(image));

    co_return !!decodeImage(bytes);   // ComPtr отвечает на `!`, как в прежней проверке мастера
}

task<std::string> Workspace::readBytes(std::filesystem::path file) {
    co_return co_await async_file::read_all(poolPath(file));
}

// ---- общее ----------------------------------------------------------------

task<> Workspace::keepBrokenCopy(std::filesystem::path file, std::string bytes) {
    file += L".bad";
    co_await async_file::write_all(poolPath(file), std::move(bytes));
}

}  // namespace bukvitsa::reader
