#include <shlobj.h>

#include <algorithm>
#include <exception>
#include <iterator>
#include <optional>

// Свои заголовки — после системных: они несут импорт, а заголовок после
// импорта MSVC принимает не всякий.
#include "bukvitsa/reader/imaging.h"
#include "bukvitsa/reader/workspace.h"

#include "bukvitsa/typography/system_failure.h"

import wxl.async;
import wxl.core;

using wxl::async::async_directory;
using wxl::async::async_file;
using wxl::async::cancellation_token;
using wxl::async::last_error;
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
bool absent(const last_error& failure) {
    return failure.code().value() == ERROR_FILE_NOT_FOUND || failure.code().value() == ERROR_PATH_NOT_FOUND;
}

/// Путь и причина для списка «не прочитались» обхода каталога.
std::wstring unreadLine(const std::filesystem::path& file, const last_error& failure) {
    return file.wstring() + L" — " + reasonOf(failure);
}

/// Копия испорченного или исправленного при чтении файла — рядом, под тем же
/// именем с «.bad».
std::filesystem::path brokenCopyOf(std::filesystem::path file) {
    file += L".bad";
    return file;
}

/// Кромка в родительном падеже — так её называют слова исправления.
const wchar_t* edgeWords(SkinEdge edge) {
    switch (edge) {
        case SkinEdge::Top:
            return L"верхней кромки";
        case SkinEdge::Bottom:
            return L"нижней кромки";
    }
    wxl::core::abort("edgeWords: кромка вне перечисления");
}

/// Что чтение исправило в обложке — словами читателю: что было в файле и что
/// стало на экране. Причина у кромки называет её, о точках — в числе по
/// тому, сколько их; выворот и пропуск — о всей обложке. Кромка или число
/// точек не у той причины — нарушение того, кто собрал исправление
/// (Skins::loadFrom): слова о таком исправлении соврали бы.
std::wstring correctionWords(const SkinCorrection& correction) {
    const bool wholeSkin = correction.fault == SkinFault::FoldsColumn || correction.fault == SkinFault::SkippedNoName ||
                           correction.fault == SkinFault::SkippedNoImage;
    const bool aboutPoints = correction.fault == SkinFault::OutsidePage ||
                             correction.fault == SkinFault::MissingCoordinate ||
                             correction.fault == SkinFault::ExtraPoints;
    if (wholeSkin == correction.edge.has_value() || aboutPoints == (correction.points == 0))
        wxl::core::abort("correctionWords: кромка или число точек не у той причины");

    const std::wstring edge = wholeSkin ? std::wstring{} : std::wstring{edgeWords(*correction.edge)};
    const bool one = correction.points == 1;
    switch (correction.fault) {
        case SkinFault::OutsidePage:
            return one ? L"у точки " + edge + L" координата не в долях страницы — на её месте точка начальной прямой"
                       : L"у точек " + edge + L" координаты не в долях страницы — на их местах точки начальной прямой";
        case SkinFault::MissingCoordinate:
            return one ? L"у точки " + edge + L" нет координаты — на её месте точка начальной прямой"
                       : L"у точек " + edge + L" нет координат — на их местах точки начальной прямой";
        case SkinFault::ExtraPoints:
            return one ? L"у " + edge + L" лишняя точка — отброшена" : L"у " + edge + L" лишние точки — отброшены";
        case SkinFault::SpineMoved:
            return L"корешок " + edge + L" не на середине — поставлен на середину";
        case SkinFault::Unordered:
            return L"с корешком на середине точки " + edge + L" идут не слева направо — кромка показана ровной";
        case SkinFault::FoldsColumn:
            return L"кромки выворачивают столбец — обложка показана ровной";
        case SkinFault::SkippedNoName:
            return L"пропущена — у неё нет имени";
        case SkinFault::SkippedNoImage:
            return L"пропущена — у неё нет снимка";
    }
    wxl::core::abort("correctionWords: исправление вне перечисления");
}

/// Обложки, которые чтение реестра исправило, — одним сообщением, строкой на
/// обложку, как непрочитанные книги обхода (`ScanReport::unread`), — и прямо о
/// том, что станет с правкой: первая же запись реестра обложек перепишет файл
/// тем, что показано, и правка рук останется только в копии.
Notice correctedNotice(const std::vector<CorrectedSkin>& correctedSkins, const std::filesystem::path& registryFile,
                       const std::filesystem::path& registryCopy) {
    std::wstring details;
    for (const CorrectedSkin& correctedSkin : correctedSkins) {
        details += correctedSkin.name.empty() ? std::wstring{L"Обложка без имени"}
                                              : L"Обложка «" + std::wstring{correctedSkin.name.wchars()} + L"»";
        details += L" в skins.xml: ";
        for (size_t at = 0; at < correctedSkin.corrections.size(); ++at) {
            if (at > 0) details += L"; ";
            details += correctionWords(correctedSkin.corrections[at]);
        }
        details += L".\n";
    }
    // Что станет с файлом при первой записи: исправленные обложки лягут в
    // него такими, как показаны, а пропущенные из него исчезнут — и те и
    // другие в своём прежнем виде останутся только в копии.
    const auto skipped = [](const CorrectedSkin& correctedSkin) {
        return std::ranges::any_of(correctedSkin.corrections, [](const SkinCorrection& correction) {
            return correction.fault == SkinFault::SkippedNoName || correction.fault == SkinFault::SkippedNoImage;
        });
    };
    const bool anySkipped = std::ranges::any_of(correctedSkins, skipped);
    const bool anyShown = !std::ranges::all_of(correctedSkins, skipped);
    details += L"\nФайл: " + registryFile.wstring() + L"\nКопия: " + registryCopy.wstring() +
               L"\n\nПервое же сохранение обложки в мастере или удаление обложки перепишет skins.xml";
    if (anyShown) details += L" исправленными кромками";
    if (anySkipped)
        details += anyShown ? L", а пропущенные обложки из него исчезнут" : L", и пропущенные обложки из него исчезнут";
    details += L"; правка рук сохранена в копии.";

    // Заголовок — в числе по обложкам, а не по исправлениям; когда все они
    // пропущены, исправлять было нечего, и заголовок говорит о пропуске.
    const bool single = correctedSkins.size() == 1;
    std::wstring headline = anyShown ? (single ? L"Обложка из skins.xml исправлена при чтении"
                                               : L"Обложки из skins.xml исправлены при чтении")
                                     : (single ? L"Обложка из skins.xml пропущена при чтении"
                                               : L"Обложки из skins.xml пропущены при чтении");
    return Notice{std::move(headline), std::move(details)};
}

/// Отчёт одного каталога — в общий отчёт перепроверки.
void addUp(ScanReport& total, ScanReport part) {
    total.listed += part.listed;
    total.parsed += part.parsed;
    total.added += part.added;
    total.marked += part.marked;
    total.restored += part.restored;
    total.unread.insert(total.unread.end(), std::make_move_iterator(part.unread.begin()),
                        std::make_move_iterator(part.unread.end()));
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

    // Без каталога данных читалке негде жить: пустой путь сделал бы рабочим
    // местом текущий каталог процесса. Отказ системы — остановка.
    PWSTR folder = nullptr;
    typography::stopIfFailed(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &folder),
                             "standardDataDirectory: нет каталога LocalAppData");
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

task<Started> Workspace::start(cancellation_token stop) {
    Started started;

    // Три файла по одной дороге: нет — первый запуск, молча; не читается —
    // слово читателю, а запуск идёт дальше с умолчаниями; прочитан, но не
    // разобран — слово и копия рядом. Разбор каждого — в его модель, у которой
    // наблюдаемые поля: о прочитанном контролы узнают сами. Без токена: из
    // этих моделей потом пишется всё, и недочитанная легла бы на диск
    // умолчаниями.
    const auto read = [&](const std::filesystem::path& file, std::wstring whatFailed) -> task<std::string> {
        try {
            co_return co_await async_file::read_all(poolPath(file));
        } catch (const last_error& failure) {
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

    // Обложку, чьи кромки в файле неверны, чтение показывает исправленной, а
    // читатель узнаёт об этом здесь, одним сообщением. Сам файл не
    // переписывается, но его заменит первая запись реестра обложек —
    // сохранение или удаление обложки читателем, — и правка рук пропала бы:
    // она остаётся копией рядом, как испорченный файл.
    const std::optional<std::vector<CorrectedSkin>> correctedSkins = skins.loadFrom(skinsXmlText);

    if (!correctedSkins) {
        started.notices.push_back({L"Реестр обложек не прочитан",
                                   skinsPath().wstring() +
                                       L"\n\nФайл испорчен: обложек нет. Копия лежит рядом "
                                       L"(skins.xml.bad), снимки в skins\\ целы, а первая же "
                                       L"запись реестра заменит сам файл."});
        co_await keepBrokenCopy(skinsPath(), std::move(skinsXmlText));
    } else if (!correctedSkins->empty()) {
        // Копия — раньше слова о ней: сообщение называет её как лежащую.
        co_await keepBrokenCopy(skinsPath(), std::move(skinsXmlText));
        started.notices.push_back(correctedNotice(*correctedSkins, skinsPath(), brokenCopyOf(skinsPath())));
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

    started.lastBook = co_await lastBookPath(std::move(stop));

    co_return started;
}

// ---- книги ----------------------------------------------------------------

task<LoadedBook> Workspace::readBook(std::filesystem::path path, cancellation_token stop) {
    // Не const: байты уходят в документ перемещением. Разбор — здесь, на потоке
    // вызова: память разбора берётся из STA-пула, а он чужого потока не терпит.
    std::string bytes = co_await async_file::read_all(poolPath(path), stop);

    // Чтение, успевшее кончиться, отвечает байтами, даже если просьба пришла,
    // пока они ехали обратно: отмену после ответа сценарий спрашивает сам. Разбор
    // дорогой, а книгу уже не ждут.
    stop.throw_if_canceled();

    const uint64_t fileSize = bytes.size();

    // Документ — только на время открытия: умирает в конце этой корутины, и
    // ожиданий до её конца больше нет.
    fb3::Document document{std::move(bytes)};

    // Что нужно регистрации, снимается раньше книги. Обложка — копией и первой:
    // её индекс — в описании, а байты картинок книга заберёт себе, и документ
    // читал бы их из пакета заново. Описание — перемещением: книге оно не
    // нужно, и строится она из документа, у которого его уже нет.
    std::optional<BookCover> cover = copyCover(document);
    fb3::Description description = document.takeDescription();

    auto book =
        not_null<intrusive_ptr<Book>>{make_refcounted<Book>(path, std::move(document), dwriteFactory())};

    co_return LoadedBook{std::move(book), std::move(description), std::move(cover), fileSize};
}

task<Registered> Workspace::registerBook(const fb3::Description& description, uint32_t characterCount,
                                         const std::optional<BookCover>& cover, BookFile file) {
    // Guid, под которым настройки помнят этот файл: реестр мог книгу потерять
    // (битый library.xml), а её место чтения в books\{guid}.xml цело, и
    // «Продолжить чтение» не должно открывать книгу с начала. Другой файл —
    // пусто, и guid будет новым.
    const u16_view remembered = settings.lastBookPath == file.path ? u16_view{settings.lastBookGuid} : u16_view{};

    // Обложка — раньше записи в реестре. Карточка, встав в реестр, тут же
    // встаёт и на полку, а полка сразу берёт картинку из файла обложки: файл,
    // записанный после, опоздал бы, и картинка, не нашедшая его, осталась бы
    // пустой — сама она файла не ждёт. Потому сперва запись, какой она станет
    // (имя файлу обложки даёт её guid), затем обложка и лишь потом перемена
    // реестра.
    //
    // Обложку пишет только новая или изменённая запись: у той же записи в кэше
    // тот же файл, и переписывать его незачем. Это снимает столкновение
    // перепроверки каталога и открытия той же неизменной книги — две записи
    // одной обложки разом через один временный файл. Одновременные регистрации
    // изменённой или новой книги этим не упорядочены и сталкиваются по-прежнему
    // (ограничение — в шапке workspace.h).
    const BookEntry planned = library.entryFor(description, characterCount, cover, file, remembered);

    if (const BookEntry* stored = library.find(planned.guid); !stored || *stored != planned) {
        if (const CoverBytes cached = coverOf(cover, planned.guid); !cached.name.empty())
            co_await async_file::write_all(poolPath(coverDirectory() / cached.name.wchars()), std::string(cached.bytes));
    }

    // Пока обложка писалась, реестр мог перемениться: ту же книгу могла
    // поставить в него другая регистрация. Добавление решает заново, а guid
    // задуманной записи просит, как guid из настроек: книге, которой в реестре
    // так и нет, достаётся он — и обложка под ним уже лежит; книга, которая
    // успела в него встать, остаётся под своим guid и при своей обложке, а
    // обложка под задуманным guid остаётся в кэше лишней.
    const AddedBook added = library.add(description, characterCount, cover, file, planned.guid);

    // Запись — копией, а не ссылкой: между co_await реестр может дополниться,
    // и вектор переедет вместе со всеми ссылками в него.
    co_return Registered{added.entry, added.isNew, added.restored};
}

task<Opened> Workspace::openBook(const LoadedBook& loaded, cancellation_token stop) {
    // Под отменённым токеном открытие не начинается — модель не тронута. Дальше
    // токен слушает только чтение состояния, а оно стоит последним: отмена,
    // пришедшая на записи обложки, реестра и настроек в памяти не разделяет —
    // меняются оба, — и кончает открытие на чтении.
    stop.throw_if_canceled();

    // Порядок — это порядок обязательств: регистрация (реестр — в памяти,
    // обложка — в кэш), затем настройки, которые на книгу указывают, — тоже в
    // памяти, и лишь потом её состояние. Файлы реестра и настроек пишут циклы
    // приложения, по одному на файл: запись отсюда шла бы разом с их записью
    // того же файла.
    //
    // Ни времени правки, ни каталога открытие не знает: у книги, которая уже в
    // реестре, они остаются от обхода каталога (`Library::entryFor`).
    auto const file = BookFile{.path = loaded.book->path(), .size = loaded.fileSize};
    const Registered registered =
        co_await registerBook(loaded.description, loaded.book->characterCount(), loaded.cover, file);

    settings.lastBookGuid = registered.entry.guid;
    settings.lastBookPath = registered.entry.path;

    // Состояние — отдельной строкой, а не внутри фигурной инициализации:
    // `co_await` в списке инициализаторов агрегата роняет бэкенд MSVC 14.51
    // (C1001).
    BookState state = co_await readState(registered.entry.guid, std::move(stop));

    Opened opened{registered.entry, std::move(state)};

    co_return opened;
}

task<ScanReport> Workspace::scanFolder(std::filesystem::path folder, cancellation_token stop) {
    std::vector<async_directory::listed_entry> listing;

    try {
        listing = co_await async_directory::list(poolPath(folder / L"*.fb3"), stop);
    } catch (const last_error& failure) {
        // Каталога нет — нет и его книг: съёмный диск не вставлен, каталог
        // унесли или переименовали. Пустой листинг помечает все его записи, а
        // вернётся каталог — пометки снимет следующая перепроверка. Прочие
        // сбои — не ответ «пусто», о записях они ничего не говорят и идут
        // вызывающему.
        if (!absent(failure)) throw;
    }

    co_return co_await scanListing(std::move(folder), std::move(listing), std::move(stop));
}

task<ScanReport> Workspace::addFolder(std::filesystem::path folder, cancellation_token stop) {
    std::vector<async_directory::listed_entry> listing =
        co_await async_directory::list(poolPath(folder / L"*.fb3"), stop);

    // Наблюдаемым каталог становится, когда перечислился: каталог, которого не
    // прочесть, реестр не запоминает — каждый запуск спотыкался бы о него
    // снова.
    library.addFolder(folder);

    co_return co_await scanListing(std::move(folder), std::move(listing), std::move(stop));
}

task<ScanReport> Workspace::rescanFolders(cancellation_token stop) {
    // Под отменённым токеном перепроверка не начинается — и без каталогов,
    // где ждать нечего.
    stop.throw_if_canceled();

    ScanReport total;

    // Копией: пока идёт перепроверка, читатель может добавить каталог, и
    // список вырастет под руками. Добавленный обходит его собственный
    // сценарий.
    const std::vector<std::filesystem::path> folders = library.folders();

    for (const std::filesystem::path& folder : folders) {
        try {
            ScanReport part = co_await scanFolder(folder, stop);
            addUp(total, std::move(part));
        } catch (const last_error& failure) {
            // Каталог есть, но не перечислился: сеть не отвечает, доступа нет.
            // Его записи не тронуты — о них ничего не известно; читателю —
            // строкой, остальные каталоги перепроверяются.
            total.unread.push_back(unreadLine(folder, failure));
        }
    }

    co_return total;
}

task<ScanReport> Workspace::scanListing(std::filesystem::path folder,
                                        std::vector<async_directory::listed_entry> listing,
                                        cancellation_token stop) {
    ScanReport report;

    // Книги каталога — путями, с размером и временем правки: путь сводит
    // листинг с записями реестра, размер и время говорят, тот ли это файл.
    std::vector<BookFile> books;
    for (const async_directory::listed_entry& entry : listing) {
        if (entry.is_directory) continue;
        books.push_back(BookFile{folder / entry.name, entry.size, entry.last_write, folder});
    }
    report.listed = static_cast<uint32_t>(books.size());

    // Пометки — одним ходом, до первого чтения: реестр сверяется с листингом
    // целиком, пока ни одна регистрация его не тронула. Запись этого каталога,
    // чьего файла нет, — «не найдена»; чей есть — найдена, даже если файл
    // потом не прочитается: он на месте.
    for (const intrusive_ptr<ShelfCard>& card : library.cards()) {
        const BookEntry& known = card->entry;
        if (!samePath(known.folder, folder)) continue;

        const bool listed =
            std::ranges::any_of(books, [&known](const BookFile& book) { return samePath(book.path, known.path); });

        if (!library.setMissing(known.guid, !listed)) continue;

        if (listed)
            ++report.restored;
        else
            ++report.marked;
    }

    // Отмена кончает обход на чтении следующей книги. Книги, зарегистрированные
    // до неё, полка уже показала, и реестр в памяти их знает; файл реестра
    // отсюда не пишется — его пишет вызывающий, и после отмены тоже.
    for (const BookFile& book : books) {
        // Тот же файл не читается вовсе: запись этого каталога с тем же
        // размером и тем же временем правки — тот же файл, и ни чтение, ни
        // разбор, ни обложка ему не нужны. Время неизвестно (запись первой
        // версии реестра, открытая файлом с другим размером, а то и файловая
        // система, которая времени не отдаёт и листинг называет нулём) — не
        // тот же: такой файл читается заново. Запись чужого каталога или без
        // каталога — тоже не та: файл перечитывается, и запись переходит к
        // этому каталогу, — иначе пометки ей ставил бы и снимал другой обход.
        if (const BookEntry* known = library.findFile(book.path);
            known && samePath(known->folder, folder) && known->fileSize == book.size &&
            known->modified != kUnknownTime && known->modified == book.modified)
            continue;

        std::string bytes;

        try {
            bytes = co_await async_file::read_all(poolPath(book.path), stop);
        } catch (const last_error& failure) {
            // Файлы, которые не прочитались, называются вызывающему разом в
            // конце: каталог с сотней книг не должен спотыкаться об один файл,
            // но и молчать о нём нельзя.
            report.unread.push_back(unreadLine(book.path, failure));
            continue;
        }

        // Чтение, успевшее кончиться, отвечает байтами и под отменой, пришедшей
        // на обратном пути; её обход спрашивает сам: разбор дорогой, а книгу,
        // начатую после отмены, регистрация положила бы в реестр в памяти мимо
        // его последней записи — ту вызывающий делает, когда отменяет.
        stop.throw_if_canceled();

        // Размер — прочитанных байт: запись описывает то, что разобрано. Время
        // — из листинга, то есть не позже чтения: файл, сменившийся между ними,
        // в записи окажется старше себя, и следующая перепроверка прочтёт его
        // снова.
        BookFile file = book;
        file.size = bytes.size();

        // Разбор ловится вокруг разбора, а не вокруг всего шага: не книга, битая
        // книга, книга от будущего формата — не повод бросать обход.
        std::optional<fb3::Document> document;

        try {
            document.emplace(std::move(bytes));
        } catch (const std::exception&) {
            continue;
        }

        // Реестру нужны описание, число знаков и обложка — их снимаем, и
        // документ умирает до ожидания: держать разобранное тело, пока пишется
        // обложка, незачем. Обложка — раньше описания: её индекс в нём.
        const std::optional<BookCover> cover = copyCover(*document);
        const uint32_t characterCount = document->characterCount();
        const fb3::Description description = document->takeDescription();
        document.reset();

        // Полка растёт на каждой книге, а не в конце: в этом и смысл — читатель
        // видит, как она наполняется. Новая книга встаёт карточкой реестра
        // прямо в регистрации, и полка, привязанная к карточкам, слышит её сама.
        try {
            const Registered registered = co_await registerBook(description, characterCount, cover, std::move(file));

            ++report.parsed;
            if (registered.isNew) ++report.added;
            if (registered.restored) ++report.restored;
        } catch (const last_error& failure) {
            // Обложка не записалась — и книга в реестр не встала: запись без
            // файла обложки показала бы пустую картинку, а та же запись потом
            // обложку не переписывает. Следующая перепроверка попробует снова.
            report.unread.push_back(unreadLine(book.path, failure));
        }
    }

    co_return report;
}

task<std::filesystem::path> Workspace::lastBookPath(cancellation_token stop) {
    // Путь в настройках — копия того, что в реестре, ради быстрой дороги;
    // протух — спрашиваем реестр по guid.
    std::filesystem::path last = settings.lastBookPath;

    if (!last.empty() && !co_await async_file::exists(poolPath(last), stop)) last.clear();

    if (last.empty()) {
        if (const BookEntry* entry = library.find(settings.lastBookGuid)) {
            last = entry->path;

            if (!co_await async_file::exists(poolPath(last), stop)) last.clear();
        }
    }

    co_return last;
}

task<BookState> Workspace::readState(u16_view guid, cancellation_token stop) {
    // Состояния у книги может и не быть: её только что добавили, или место
    // чтения ещё не записывалось. Это не ошибка, а чистый лист.
    try {
        co_return parseBookState(co_await async_file::read_all(poolPath(statePath(guid)), std::move(stop)));
    } catch (const last_error& failure) {
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

task<> Workspace::saveSkin(Skin skin, std::filesystem::path photo, cancellation_token stop) {
    // Под отменённым токеном сохранение не начинается — и у правки, где читать
    // нечего. Токен слушает только чтение снимка и спрашивается сразу за ним, до
    // записей: копия снимка без строки реестра — файл, на который никто не
    // ссылается, и с первой записи сохранение доходит до конца.
    stop.throw_if_canceled();

    if (skin.image.empty()) {
        std::string bytes = co_await async_file::read_all(poolPath(photo), stop);

        // Чтение, успевшее кончиться, отвечает снимком и под отменой, пришедшей
        // на обратном пути; её сценарий спрашивает сам.
        stop.throw_if_canceled();

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

task<> Workspace::deleteSkin(u16_view name, cancellation_token stop) {
    // Под отменённым токеном удаление не начинается: реестр в памяти не тронут.
    // Дальше токен не спрашивается — чтений нет, а строка, убранная из реестра
    // в памяти, должна уйти и с диска: иначе следующий запуск вернул бы её.
    stop.throw_if_canceled();

    if (!skins.remove(name)) co_return;   // реестр успел перемениться под руками

    std::string xml = skins.toXml();

    co_await async_file::write_all(poolPath(skinsPath()), std::move(xml));
}

task<PictureVerdict> Workspace::probeImage(std::filesystem::path image, cancellation_token stop) {
    const std::string bytes = co_await async_file::read_all(poolPath(image), std::move(stop));

    co_return decodeImage(bytes).verdict;   // пиксели пробе не нужны — битмап отпускается здесь же
}

task<std::string> Workspace::readBytes(std::filesystem::path file, cancellation_token stop) {
    co_return co_await async_file::read_all(poolPath(file), std::move(stop));
}

// ---- общее ----------------------------------------------------------------

task<> Workspace::keepBrokenCopy(std::filesystem::path file, std::string bytes) {
    co_await async_file::write_all(poolPath(brokenCopyOf(std::move(file))), std::move(bytes));
}

}  // namespace bukvitsa::reader
