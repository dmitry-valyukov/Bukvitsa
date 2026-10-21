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
using wxl::async::cancellation_source;
using wxl::async::cancellation_token;
using wxl::async::last_error;
using wxl::async::operation_canceled_exception;
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

/// То же для отказа, который не файловый: книга или её обложка, не прочитавшиеся
/// из пакета.
std::wstring unreadLine(const std::filesystem::path& file, const std::exception& failure) {
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
    total.queued += part.queued;
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

Workspace::Workspace(std::filesystem::path directory, cancellation_token appStop)
    : directory_(std::move(directory)), appStop_(std::move(appStop)) {}

Workspace::~Workspace() {
    // Разборщик — поле рабочего места, и уход рабочего места посреди разбора
    // остановил бы его на шаге: книга, чей шаг идёт, и книги за ней итога не
    // получили бы, а их ждущие не узнали бы ни о книге, ни об отказе. Разбор
    // кончается всегда — шагом или отменой общего токена, — так что застать
    // его здесь — нарушенный порядок выхода.
    if (draining_) wxl::core::abort("~Workspace: разбор очереди регистраций ещё идёт");
}

// ---- очередь регистраций --------------------------------------------------

QueuedRegistration::QueuedRegistration(fb3::Description description, uint32_t characterCount,
                                       std::optional<BookCover> cover, BookFile file)
    : description_(std::move(description)),
      characterCount_(characterCount),
      cover_(std::move(cover)),
      file_(std::move(file)),
      bookId_(description_.id.to_utf16()) {}

void QueuedRegistration::take(fb3::Description description, uint32_t characterCount,
                              std::optional<BookCover> cover, BookFile file) {
    // Тот же файл, а новые входы его каталога или времени не знают — открытие
    // файлом после листинга: известное остаётся, тем же правилом, что у реестра
    // (`Library::entryFor`), — каталог всегда, время при том же размере. Иначе
    // книга, которую обход поставил, а открытие поставило снова, выпала бы из
    // каталога и читалась бы следующей перепроверкой заново.
    if (samePath(file.path, file_.path)) {
        if (file.folder.empty()) file.folder = file_.folder;
        if (file.modified == kUnknownTime && file.size == file_.size) file.modified = file_.modified;
    }

    description_ = std::move(description);
    characterCount_ = characterCount;
    cover_ = std::move(cover);
    file_ = std::move(file);
    bookId_ = description_.id.to_utf16();
}

const RegistrationOutcome& QueuedRegistration::outcome() const {
    if (!outcome_) wxl::core::abort("QueuedRegistration::outcome: шаг регистрации ещё не кончился");
    return *outcome_;
}

namespace {

/// Те же входы постановки: тот же файл тем же размером и временем правки —
/// шаг зарегистрировал бы то же самое.
bool sameInputs(const BookFile& one, const BookFile& other) {
    return samePath(one.path, other.path) && one.size == other.size && one.modified == other.modified;
}

}  // namespace

not_null<intrusive_ptr<QueuedRegistration>> Workspace::queueRegistration(fb3::Description description,
                                                                          uint32_t characterCount,
                                                                          std::optional<BookCover> cover,
                                                                          BookFile file) {
    // Та же книга — по правилу реестра и его же порядком (`Library::findSame`):
    // сперва UUID по всей очереди, затем путь при пустом UUID. Ключ — из того,
    // что поставлено, а не из записи, какой её сделает реестр: запись решает
    // шаг, по реестру, каким его оставили шаги впереди.
    //
    // Элементов одной книги в очереди может быть несколько. Повтор с теми же
    // входами не ставится — отдаётся найденный элемент; другие входы берёт
    // ждущий элемент, итога которого не ждёт никто; за элементом, чей шаг уже
    // идёт, и за ждущим, у которого стоит подписчик `ended()` (так открытие
    // ждёт конца шага своей книги — через `wxl::on_event`), новые входы
    // встают новым элементом. Поэтому из подходящих — позднейший: шаги идут
    // по порядку постановки, и запись книги в реестре оставляет шаг её
    // последнего элемента — входы, отданные элементу впереди него, этот шаг
    // затёр бы прежними.
    const u16_text bookId = description.id.to_utf16();
    std::optional<size_t> place;
    for (size_t at = 0; at < registrationQueue_.size(); ++at) {
        if (sameBookId(registrationQueue_[at]->bookId_, bookId)) place = at;
    }
    if (!place) {
        for (size_t at = 0; at < registrationQueue_.size(); ++at) {
            const QueuedRegistration& waiting = *registrationQueue_[at].get();
            if (samePlace(waiting.bookId_, waiting.file_.path, bookId, file.path)) place = at;
        }
    }

    if (place) {
        const not_null<intrusive_ptr<QueuedRegistration>> same = registrationQueue_[*place];

        // Те же входы — тот же шаг: повтор ничего не добавил бы.
        if (sameInputs(same->file_, file)) return same;

        // Другие, а элемент ждёт и итога его не ждёт никто — входы его: файл
        // прочитан позже, и регистрировать надо его — копию с другого пути,
        // каталог и время из листинга (`take` не затирает известное
        // неизвестным).
        if (!same->stepping_ && !same->ended_) {
            same->take(std::move(description), characterCount, std::move(cover), std::move(file));
            return same;
        }

        // Шаг идёт — его входы уже у реестра; итога ждёт подписчик — открытие
        // ждёт запись своего файла, и чужой файл ему не подменить. Новые входы
        // встают за ним своим элементом.
    }

    // Конструктор закрыт: элемент заводит только очередь. Указатель берёт
    // первую ссылку самосчитающего, а не добавляет свою.
    auto const queued = not_null<intrusive_ptr<QueuedRegistration>>{
        new QueuedRegistration{std::move(description), characterCount, std::move(cover), std::move(file)}};
    registrationQueue_.push_back(queued);

    // Разборщик один: живой дойдёт и до этой книги. Нет его — заводится здесь,
    // и шаг начинается в этом же вызове.
    if (!draining_) {
        draining_ = true;
        drain_ = drainRegistrations();
    }

    return queued;
}

task<> Workspace::drainRegistrations() {
    // Свой источник, дочерний от общего: выход приложения кончает разбор, а
    // больше его не кончает никто — шаг ждёт только записи обложки, которой
    // отмена не делит.
    auto const stop = cancellation_source{appStop_};

    while (!registrationQueue_.empty()) {
        // Копией: голова уходит из очереди раньше, чем её итог разойдётся по
        // подписчикам, а держать элемент до конца событий надо.
        const not_null<intrusive_ptr<QueuedRegistration>> head = registrationQueue_.front();

        if (stop.is_canceled()) {
            // Выход: книги, не дошедшие до шага, не регистрируются — каждая
            // кончается отменой, и её ждущие узнают это из итога.
            registrationQueue_.pop_front();
            auto const canceled = std::make_exception_ptr(
                operation_canceled_exception{"очередь регистраций кончилась раньше шага этой книги"});
            endRegistration(*head.get(), RegistrationOutcome{std::unexpect, canceled});
            continue;
        }

        head->stepping_ = true;

        std::optional<RegistrationOutcome> outcome;
        try {
            // Отдельной строкой, а не внутри emplace: значение шага — до
            // итога, и co_await в аргументах MSVC переносит не всякий.
            Registered registered = co_await registerQueued(*head.get());
            outcome.emplace(std::move(registered));
        } catch (const last_error&) {
            // Обложка не записалась — книга в реестр не встала; итог донесёт
            // сбой до того, кто его ждёт, или событие рабочего места, если не
            // ждёт никто, а очередь идёт дальше.
            outcome.emplace(std::unexpect, std::current_exception());
        } catch (const std::exception& failure) {
            // Всё прочее — не исход шага: шаг обещает только сбой записи
            // обложки, а иное (нехватка памяти и подобное) — нарушенное
            // обещание. Отпусти его дальше — разборщик кончился бы мимо
            // `draining_`, очередь встала бы навсегда, открытие ждало бы до
            // выхода, а выход остановился бы с неверным словом. Остановка —
            // здесь, с текстом исключения.
            //
            // Слова — без выделения: процесс на этом пути кончается, а
            // выделение само может не удаться — нехватка памяти и есть одна из
            // причин, — и его std::bad_alloc ушёл бы из этого обработчика мимо
            // остановки в задачу разборщика, которую никто не читает, с той же
            // вставшей очередью. Текст исключения — строкой wxl как есть, за ним
            // остановка с постоянными словами.
            wxl::core::report(failure.what());
            wxl::core::abort("Workspace::drainRegistrations: шаг регистрации кончился не сбоем записи обложки — "
                             "текст исключения строкой выше");
        } catch (...) {
            wxl::core::abort("Workspace::drainRegistrations: шаг регистрации кончился неизвестным исключением");
        }

        // Голова уходит до событий: проснувшийся подписчик, поставивший ту же
        // книгу снова, получит новый элемент, а не кончившийся.
        registrationQueue_.pop_front();
        endRegistration(*head.get(), std::move(*outcome));
    }

    draining_ = false;
}

void Workspace::endRegistration(QueuedRegistration& queued, RegistrationOutcome outcome) {
    queued.outcome_ = std::move(outcome);
    const RegistrationOutcome& ended = *queued.outcome_;

    // Реестр переменился — раньше конца шага у элемента: ждущий открытия,
    // проснувшись, будит запись реестра «сейчас», и она снимет уже и эту
    // перемену, а не отложит её на паузу.
    if (ended && (ended->isChanged || ended->restored)) bookRegistered_.event_(*ended);

    // Отказ, которого никто не ждёт: подписчика у конца шага нет — книга
    // перепроверки или добавленного каталога. Стоит подписчик — отказ его, и
    // говорит о нём он, — пока приложение не выходит. На выходе подписчик —
    // ожидание под токеном, который выход уже отменил (открытие — под
    // источником сценария, дочерним от общего): отменённое, оно стоит в
    // событии до хода очереди, а итог, пришедший раньше, не отдаёт — под
    // отменённым токеном ожидание wxl кончается отменой, что бы его ни
    // разбудило. Тогда об отказе говорит рабочее место, иначе отказ пропал бы
    // молча; дважды о нём не скажут — ожидание его не отдаст. Отмена — не
    // отказ: её приносит выход.
    if (!ended && (!queued.ended_ || appStop_.is_canceled())) {
        try {
            std::rethrow_exception(ended.error());
        } catch (const last_error& failure) {
            registrationRefused_.event_(unreadLine(queued.file_.path, failure));
        } catch (const operation_canceled_exception&) {
        }
    }

    queued.ended_(ended);
}

task<Registered> Workspace::registerQueued(const QueuedRegistration& queued) {
    // Guid, под которым настройки помнят этот файл: реестр мог книгу потерять
    // (битый library.xml), а её место чтения в books\{guid}.xml цело, и
    // «Продолжить чтение» не должно открывать книгу с начала. Другой файл —
    // пусто, и guid будет новым.
    const u16_view remembered =
        settings.lastBookPath == queued.file_.path ? u16_view{settings.lastBookGuid} : u16_view{};

    // Обложка — раньше записи в реестре. Карточка, встав в реестр, тут же
    // встаёт и на полку, а полка сразу берёт картинку из файла обложки: файл,
    // записанный после, опоздал бы, и картинка, не нашедшая его, осталась бы
    // пустой — сама она файла не ждёт. Потому сперва запись, какой она станет
    // (имя файлу обложки даёт её guid), затем обложка и лишь потом перемена
    // реестра.
    //
    // Обложку пишет только новая или изменённая запись: у той же записи в кэше
    // тот же файл, и переписывать его незачем.
    const BookEntry planned =
        library.entryFor(queued.description_, queued.characterCount_, queued.cover_, queued.file_, remembered);

    if (const BookEntry* stored = library.find(planned.guid); !stored || *stored != planned) {
        if (const CoverBytes cached = coverOf(queued.cover_, planned.guid); !cached.name.empty())
            co_await async_file::write_all(poolPath(coverDirectory() / cached.name.wchars()), std::string(cached.bytes));
    }

    // Пока обложка писалась, другой шаг идти не мог — очередь одна, и
    // разборщик один, — а прочие перемены реестра (пометки перепроверки,
    // каталоги) записей книг не меняют. Добавление решает заново и просит
    // guid задуманной записи, как guid из настроек, и обязано его отдать:
    // другой guid значил бы, что запись этой книги переменилась под шагом, а
    // обложка лежит под чужим именем.
    const AddedBook added = library.add(queued.description_, queued.characterCount_, queued.cover_, queued.file_,
                                        planned.guid);
    if (added.entry.guid != planned.guid)
        wxl::core::abort("Workspace::registerQueued: реестр отдал не тот guid, что задуман для обложки");

    // Запись — копией, а не ссылкой: между co_await реестр может дополниться,
    // и вектор переедет вместе со всеми ссылками в него.
    co_return Registered{added.entry, added.isNew, added.isChanged, added.restored};
}

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

task<Opened> Workspace::openBook(const BookEntry& entry, cancellation_token stop) {
    // Под отменённым токеном открытие не начинается. Дальше токен слушает
    // только чтение состояния — само чтение и вопрос сразу за ним, — и книга,
    // чьё открытие отменили, на полосу не встаёт, а настройки на неё не
    // указывают.
    stop.throw_if_canceled();

    // Состояние — отдельной строкой, а не внутри фигурной инициализации:
    // `co_await` в списке инициализаторов агрегата роняет бэкенд MSVC 14.51
    // (C1001).
    BookState state = co_await readState(entry.guid, stop);

    // Чтение, успевшее кончиться, отвечает состоянием и под отменой, пришедшей
    // на обратном пути, — ответ, успевший раньше отмены, — ответ, — но тот, кто
    // отменил, открытия больше не ждёт, что бы чтение ни ответило: дальше книга
    // встала бы на полосу. Тем же правилом токен спрашивают сразу за чтением
    // readBook и saveSkin.
    stop.throw_if_canceled();

    // Настройки, которые на книгу указывают, — в памяти и только после
    // последнего ожидания: от них до смены книги у вызывающего ожиданий нет,
    // и открытие, остановленное или отменённое на любом из своих ожиданий,
    // настроек не трогает — они не укажут на книгу, которой нет на полосе.
    // Файл настроек пишет цикл приложения, один на файл: запись отсюда шла бы
    // разом с его записью того же файла.
    settings.lastBookGuid = entry.guid;
    settings.lastBookPath = entry.path;

    Opened opened{entry, std::move(state)};

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

    // Листинг, успевший кончиться, отвечает и под отменой, пришедшей на
    // обратном пути; обход спрашивает её сам: дальше пометки меняют реестр.
    stop.throw_if_canceled();

    co_return co_await scanListing(std::move(folder), std::move(listing), std::move(stop));
}

task<ScanReport> Workspace::addFolder(std::filesystem::path folder, cancellation_token stop) {
    std::vector<async_directory::listed_entry> listing =
        co_await async_directory::list(poolPath(folder / L"*.fb3"), stop);

    // Листинг, успевший кончиться, отвечает и под отменой; её добавление
    // спрашивает само, до первой перемены реестра — как у перепроверки.
    stop.throw_if_canceled();

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

    // Отмена кончает обход на чтении следующей книги. Книги, поставленные до
    // неё, регистрирует очередь — их шагов отмена обхода не трогает, — и
    // реестр в памяти их узнает; файл реестра отсюда не пишется — его пишет
    // вызывающий, и после отмены тоже.
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
        // на обратном пути; её обход спрашивает сам: разбор дорогой, а книга,
        // начатая после отмены, встала бы в очередь и в реестр в памяти мимо
        // его последней записи — ту вызывающий делает, когда отменяет.
        stop.throw_if_canceled();

        // Размер — прочитанных байт: запись описывает то, что разобрано. Время
        // — из листинга, то есть не позже чтения: файл, сменившийся между ними,
        // в записи окажется старше себя, и следующая перепроверка прочтёт его
        // снова.
        BookFile file = book;
        file.size = bytes.size();

        // Разбор ловится вокруг разбора, а не вокруг всего шага: не книга, битая
        // книга, книга от будущего формата — не повод бросать обход. Файл,
        // который наверняка не книга FB3 (байты не zip или пакет без связи
        // Book), пропускается молча: о нём нечего сказать. Всякий иной отказ —
        // книга FB3, которая не разобралась, — в непрочитанных, с причиной:
        // молча пропавшая с полки, она была бы спрятанной ошибкой.
        std::optional<fb3::Document> document;

        try {
            document.emplace(std::move(bytes));
        } catch (const fb3::NotFb3Book&) {
            continue;
        } catch (const std::exception& failure) {
            report.unread.push_back(unreadLine(book.path, failure));
            continue;
        }

        // Реестру нужны описание, число знаков и обложка — их снимаем, и
        // документ умирает здесь же: держать разобранное тело, пока ждёт и
        // идёт шаг регистрации, незачем. Обложка — раньше описания: её индекс
        // в нём.
        //
        // Байтов картинок разбор не читает, и обложка — первое чтение части
        // картинки из пакета: испорченные сжатые байты отказывают здесь. Такая
        // книга — в непрочитанных обхода, с причиной, и в очередь не встаёт, а
        // обход идёт дальше.
        std::optional<BookCover> cover;

        try {
            cover = copyCover(*document);
        } catch (const std::exception& failure) {
            report.unread.push_back(unreadLine(book.path, failure));
            continue;
        }

        const uint32_t characterCount = document->characterCount();
        fb3::Description description = document->takeDescription();
        document.reset();

        // Полка растёт на каждой книге, а не в конце: в этом и смысл — читатель
        // видит, как она наполняется. Новая книга встаёт карточкой реестра
        // своим шагом в очереди, и полка, привязанная к карточкам, слышит её
        // сама. Шага обход не ждёт — читает следующую, пока пишется обложка
        // этой: что встало и что не встало, говорят события рабочего места, и
        // элемент обходу не нужен.
        static_cast<void>(
            queueRegistration(std::move(description), characterCount, std::move(cover), std::move(file)));
        ++report.queued;
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

    // Реестр в памяти меняется до записи, а не после: пишется он целиком, и
    // запись другого сценария, пошедшая за этой, несёт уже и эту обложку.
    // Не записался — память возвращается к тому, что на диске, иначе обложка
    // жила бы до выхода, а следующая запись реестра унесла бы на диск
    // сохранение, о котором читателю сказали, что оно не удалось. Копия
    // снимка остаётся: лишний файл — мусор, который никого не касается.
    std::optional<Skin> previous;
    if (const Skin* known = skins.find(skin.name)) previous = *known;
    const Skin placed = skin;

    skins.put(std::move(skin));

    std::string xml = skins.toXml();

    try {
        co_await async_file::write_all(poolPath(skinsPath()), std::move(xml));
    } catch (...) {
        // Откатывается только своё: под именем лежит не то, что положил этот
        // шаг, — его, пока шла запись, сменил или убрал другой сценарий, и
        // его перемена новее.
        if (const Skin* now = skins.find(placed.name); now && *now == placed) {
            if (previous)
                skins.put(std::move(*previous));
            else
                skins.remove(placed.name);
        }
        throw;
    }
}

task<> Workspace::deleteSkin(u16_view name, cancellation_token stop) {
    // Под отменённым токеном удаление не начинается: реестр в памяти не тронут.
    // Дальше токен не спрашивается — чтений нет, а строка, убранная из реестра
    // в памяти, должна уйти и с диска: иначе следующий запуск вернул бы её.
    stop.throw_if_canceled();

    const std::vector<Skin>& list = skins.list();
    const auto found = std::find_if(list.begin(), list.end(), [name](const Skin& skin) { return skin.name == name; });
    if (found == list.end()) co_return;   // реестр успел перемениться под руками

    // Убранная — с её местом, на случай отката (как у saveSkin: память — до
    // записи, отказ записи её возвращает). Копия: строка в реестре уходит.
    const std::size_t place = static_cast<std::size_t>(found - list.begin());
    Skin removed = *found;
    skins.remove(name);

    std::string xml = skins.toXml();

    try {
        co_await async_file::write_all(poolPath(skinsPath()), std::move(xml));
    } catch (...) {
        skins.restore(place, std::move(removed));
        throw;
    }
}

task<DecodedPicture> Workspace::probeImageAsync(std::filesystem::path image, cancellation_token stop) {
    // Не const: байты уходят телу раскодирования перемещением.
    std::string bytes = co_await async_file::read_all(poolPath(image), stop);

    // Чтение, успевшее кончиться раньше просьбы, отвечает байтами; под
    // отменённым токеном раскодирование не начинается и кончается отменой —
    // своего вопроса здесь не нужно.
    co_return co_await decodeImageAsync(std::move(bytes), kMaxPictureSide, std::move(stop));
}

// ---- общее ----------------------------------------------------------------

task<> Workspace::keepBrokenCopy(std::filesystem::path file, std::string bytes) {
    co_await async_file::write_all(poolPath(brokenCopyOf(std::move(file))), std::move(bytes));
}

}  // namespace bukvitsa::reader
