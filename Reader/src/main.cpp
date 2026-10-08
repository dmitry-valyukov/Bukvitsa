// Reader — читалка Буквицы на wxl.ui.
//
// Здесь нет ни wWinMain, ни поднятия Windows App Runtime, ни наследника
// Application, ни XAML: всё это делает wxl и потом зовёт эту функцию.
//
// Что здесь есть — сборка приложения из частей и решения о том, что чем
// сменяется: окно с родной рамкой и запомненным местом, стартовый экран, витрина
// хранилища, полоса набора, реестр книг и полный экран по F11.

// Свои заголовки со стандартными внутри — до всего, что тянет import
// wxl.core: заголовок, включённый после импорта, MSVC уже не принимает.
#include "file_dialog.h"
#include "keys.h"

#include "ApplicationFolder.h"
#include "CompositionWindow.h"
#include "ShowDialog.h"
#include "skin_wizard.h"
#include "start_screen.h"

// Последними: они ведут к модели книги и реестру, а те импортируют wxl.core,
// после чего стандартный заголовок MSVC уже не принимает.
#include "bukvitsa/reader/book.h"
#include "book_view.h"
#include "bukvitsa/reader/library.h"
#include "library_screen.h"
#include "reader_panel.h"
#include "bukvitsa/reader/settings.h"
#include "bukvitsa/reader/store.h"
#include "bukvitsa/reader/theme_list.h"
#include "bukvitsa/reader/workspace.h"

// Импорт последним, после всех обычных заголовков.
import wxl.async;
import wxl.core;

using namespace wxl;
using namespace std::chrono_literals;

namespace {

using namespace bukvitsa::reader;

using wxl::async::detached_task;
using wxl::async::system_exception;

// Пространство имён книги: `using namespace bukvitsa::reader` его не приносит,
// а обход каталога разбирает документ сам.
namespace fb3 = bukvitsa::fb3;

// Каким окно открывается, когда запоминать ещё нечего.
constexpr int32_t kInitialWidth = 1280;
constexpr int32_t kInitialHeight = 860;

// Сколько заставка стоит одна, прежде чем на неё проступят кнопки. Не таймаут
// загрузки, а пауза ради самой заставки: приложению без книги нечего грузить,
// и без неё кнопки появились бы в тот же кадр, что и картинка.
constexpr auto kSplashHold = 1000ms;

// Пауза, после которой перемещение окна попадает на диск. Окно таскают
// непрерывно, а писать на каждый пиксель незачем; при закрытии сохранение
// безусловное, так что пауза ничего не теряет — она только страхует от того,
// что до закрытия дело не дойдёт.
constexpr auto kSaveQuiet = 800ms;

// То же для места чтения: страницы листают подряд, а файл на книгу один.
constexpr auto kPositionQuiet = 1500ms;

// ---- то, из чего собрано приложение ---------------------------------------
//
// Одна связка на всех, потому что корутины ниже держат её у себя в кадре, а
// девять отдельных параметров у каждой — это девять мест, где однажды забудут
// один. Всё внутри — либо shared_ptr, либо обёртка wxl, то есть хендл; копия
// такой связки ничего не копирует по существу.
/// Прогретая книга — та, которую читатель скорее всего откроет следующей:
/// прочитанная с диска и уже разобранная, пока он смотрит на заставку. Нажатие
/// «Продолжить чтение» после этого не ждёт ни диска, ни разбора — самой долгой
/// части открытия.
///
/// Держится ровно одна: греть больше нечего (продолжают одну книгу), а
/// разобранная книга — это десятки мегабайт. `wanted` говорит, чью книгу ждём:
/// прогрев кладёт туда путь, прежде чем уйти к диску, и, вернувшись, отдаёт
/// разобранное только если путь всё ещё тот. Открытие книги путь снимает —
/// и прогрев, шедший для неё же, выбросит своё, вместо того чтобы оставить в
/// памяти вторую копию уже открытой книги.
struct WarmBook {
    std::filesystem::path wanted;   ///< чью книгу греем или уже прогрели
    std::shared_ptr<Book> book;     ///< она же разобранная; пусто, пока прогрев идёт
    uint64_t fileSize = 0;          ///< размер файла: его записывает реестр
};

/// Сообщения читателю — диалогом над показанным экраном, одним на всё
/// приложение. XAML не держит двух ContentDialog разом (второй ShowAsync —
/// исключение), а сбои приходят и по два: сценарий упал, пока читатель читает
/// о прошлом. Потому, пока сообщение открыто, новое дописывается в него, а не
/// открывает второе; пока открыт чужой диалог (вопрос об удалении обложки) или
/// экрана ещё нет, сообщение ждёт в очереди и выходит по flush(). Ответа не
/// ждём: кнопка одна, и сценарий, которому не удалось, на этом и так кончился.
/// Щёлкнуть мимо открытого диалога нельзя — он заслоняет остров, — так что
/// второй диалог может прийти только от сценария по таймеру.
class Notices {
public:
    /// Ставит себя обработчиком сбоев сценариев: сценарий, кончившийся
    /// исключением, которого сам не поймал, -- не повод ронять читалку;
    /// исключение приходит в поток окна, и читатель видит, что именно не
    /// удалось, а кадр уходит, как у всякой detached_task. Без обработчика wxl
    /// завершила бы процесс. Обработчик wxl — указатель на функцию, без
    /// захвата, потому экземпляр один на процесс и находится по instance_.
    explicit Notices(wxl::CompositionWindow window) : window_(std::move(window)) {
        instance_ = this;
        previous_ = wxl::async::on_detached_task_failure();
        wxl::async::on_detached_task_failure() = [](std::exception_ptr error) noexcept {
            if (instance_) instance_->failed(std::move(error));
        };
    }

    ~Notices() {
        if (instance_ != this) return;
        wxl::async::on_detached_task_failure() = previous_;
        instance_ = nullptr;
    }

    Notices(const Notices&) = delete;
    Notices& operator=(const Notices&) = delete;

    /// @param headline что не удалось — заголовок диалога
    /// @param details  с чем и почему: путь, причина системы; может быть пусто
    void complain(std::wstring headline, std::wstring details) {
        using namespace wxl::dsl;

        if (lines_) {
            // Диалог открыт — дописываем. Заголовок второго сообщения — строкой
            // в содержимом: title у диалога один, и он принадлежит первому.
            lines_.value().children().append(
                TextBlock{headline, FontWeight{600}, Margin{0, 12, 0, 0}, textWrapping.wrap});
            if (!details.empty()) lines_.value().children().append(TextBlock{details, textWrapping.wrap});
            return;
        }

        // Над тем островом, что показан: диалог — не часть дерева, и без
        // XamlRoot показ падает; его ставит showDialog. Тема — показанного
        // экрана: полоса тёмная, полка и стартовый экран светлые, а корни их
        // все — Grid.
        UIElement const host = window_.content();
        if (!host) {
            // Показывать ещё не над чем: так падает чтение настроек и реестров
            // на старте, до первого экрана. Сообщение дождётся его (flush).
            pending_.emplace_back(std::move(headline), std::move(details));
            return;
        }

        auto lines = StackPanel{};
        if (!details.empty()) lines.children().append(TextBlock{details, textWrapping.wrap});

        auto dialog = ContentDialog{
            title = headline,
            content = lines,
            closeButtonText = u"Закрыть",
            defaultButton = ContentDialogButton::Close,
            onClosed = [this](ContentDialog const&) { lines_ = nullptr; },
        };
        dialog.requestedTheme(host.try_as<FrameworkElement>().actualTheme());

        // Открыт чужой диалог — XAML откажет исключением. Зовут отсюда и из
        // noexcept-обработчика сбоев, так что отказ — не ошибка, а очередь.
        try {
            showDialog(dialog, host);
        } catch (...) {
            pending_.emplace_back(std::move(headline), std::move(details));
            return;
        }
        lines_ = lines;
    }

    /// Экран показан: сообщения, пришедшие, пока показывать было не над чем,
    /// выходят теперь. Зовётся там же, где окну ставят содержимое.
    void flush() {
        std::vector<std::pair<std::wstring, std::wstring>> waiting = std::move(pending_);
        pending_.clear();
        for (auto& [headline, details] : waiting) complain(std::move(headline), std::move(details));
    }

private:
    /// Сбой сценария — сообщением: словами системы, чужим текстом после
    /// проверки или просто фактом.
    void failed(std::exception_ptr error) noexcept;

    inline static Notices* instance_ = nullptr;
    wxl::async::detached_task_failure_handler previous_ = nullptr;

    wxl::CompositionWindow window_;
    nullable<StackPanel> lines_ = nullptr;   ///< содержимое открытого диалога; пусто — диалога нет
    std::vector<std::pair<std::wstring, std::wstring>> pending_;   ///< до первого экрана
};

struct App {
    wxl::CompositionWindow window;
    std::shared_ptr<Workspace> ws;   ///< рабочее место: настройки, реестр, обложки и все файлы
    std::shared_ptr<BookState> state;
    std::shared_ptr<BookView> view;
    std::shared_ptr<ReaderPanel> panel;
    std::shared_ptr<LibraryScreen> shelf;
    std::shared_ptr<StartScreen> screen;
    std::shared_ptr<Screen> shown;
    std::shared_ptr<Screen> bookCameFrom;
    std::shared_ptr<WarmBook> warm;
    std::shared_ptr<Notices> notices;

    /// Темы и обложки одним списком — номер темы полосы ↔ имена в настройках.
    /// Заполняется там же и тем же, что список полосы (`setSkins`): при
    /// запуске, после сохранения и после удаления обложки.
    std::shared_ptr<ThemeList> themes = std::make_shared<ThemeList>();
};

// ---- корутины приложения --------------------------------------------------
//
// Каждая исполняется в интерфейсном потоке и уходит с него ровно на `co_await`
// — на время, пока файл читается или пишется. Между двумя co_await код
// обычный: он трогает XAML и общее состояние, потому что он и есть тот самый
// поток.
//
// Правило без исключений: **Reader диска не трогает вовсе.** Файлы читает и
// пишет рабочее место (`Workspace`, bukvitsa.lib) операциями wxl из своих
// корутин `task<T>`; сценарий ждёт их `co_await` и получает значение, а сбой
// операции — исключением `system_exception` с кодом системы. Разбор при этом
// идёт на потоке окна, в STA-пуле, — тоже у рабочего места.
//
// Сценарии — `detached_task`: кадр живёт, пока идёт сценарий, и уходит сам.
// Сценарий ловит то, на что у него есть ответ читателю, — файл книги, снимок,
// каталог; «файла нет» там, где его может и не быть, — не сбой. Остальное
// всплывает в `on_detached_task_failure` (см. wxl_launched): читатель видит
// окно с причиной, сценарий на этом кончается.

void Notices::failed(std::exception_ptr error) noexcept {
    try {
        std::rethrow_exception(error);
    } catch (const system_exception& failure) {
        complain(L"Не удалось выполнить операцию с файлом", reasonOf(failure));
    } catch (const std::exception& failure) {
        // Чужой текст: чей он и в какой кодировке, здесь неизвестно, потому
        // проверяется, а не принимается на веру.
        const std::optional<u8_view> said = unicode::checked(std::string_view(failure.what()));
        complain(L"Ошибка", said ? std::wstring(said->to_utf16().wchars()) : L"(сообщение не в UTF-8)");
    } catch (...) {
        complain(L"Неизвестная ошибка", L"Сценарий прерван.");
    }
}

/// Говорит читателю, что не удалось, с чем и почему — словами системы.
void complain(App const& app, std::wstring headline, std::wstring details,
              const system_exception& failure) {
    if (!details.empty()) details += L"\n\n";
    details += reasonOf(failure);
    app.notices->complain(std::move(headline), std::move(details));
}

/// Пишет настройки — сценарием без владельца, по таймеру или при закрытии:
/// текст рабочее место собирает в момент вызова, до ожидания.
detached_task saveSettingsLater(std::shared_ptr<Workspace> ws) {
    co_await ws->saveSettings();
}

/// Пишет состояние книги: место чтения и закладки.
detached_task saveStateLater(std::shared_ptr<Workspace> ws, u16_text guid, BookState state) {
    co_await ws->saveState(std::move(guid), std::move(state));
}

/// Достраивает полку: у каждой книги свой файл состояния, и читаются они по
/// одному, уже после того, как полка показана.
///
/// Это и есть «библиотека наполняется по мере чтения»: карточки встают сразу,
/// а «прочитано 42%» проступает на каждой, как только её файл прочитан. Полка
/// с сотней книг не ждёт сотни обращений к диску, чтобы показать первую.
detached_task fillProgress(std::shared_ptr<Workspace> ws, std::shared_ptr<LibraryScreen> shelf,
                           std::vector<BookEntry> books) {
    for (const BookEntry& book : books) {
        if (book.characterCount == 0) continue;   // не открывалась -- и читать нечего

        const BookState state = co_await ws->readState(book.guid);

        // Открывалась, а записать место не успела — показывать нечего.
        if (state.charOffset == 0 && state.bookmarks.empty()) continue;

        shelf->setProgress(book.guid, state.charOffset, state.bookmarks.size());
    }
}

/// Обходит каталог и добавляет из него книги — по одной, на глазах у читателя.
///
/// Здесь и видно, зачем всё это затевалось. Каталог перечисляется на рабочем
/// потоке; каждая книга читается там же; разбирается она здесь, между двумя
/// `co_await`, и сразу встаёт на полку. Ни один шаг не ждёт остальных: первая
/// книга появляется на полке, пока десятая ещё не прочитана, — а окно всё это
/// время отвечает, потому что интерфейсный поток каждый раз возвращается в
/// свой цикл сообщений.
///
/// Разбирается при этом `fb3::Document`, а не `Book`: реестру нужны метаданные
/// и обложка, а движок вёрстки с пагинатором книге, которую никто не открывал,
/// ни к чему.
detached_task addFolderFlow(App app, std::filesystem::path folder, std::function<void()> showLibrary) {
    // Полка -- прежде обхода: читатель, добавивший каталог, должен видеть, как
    // тот наполняется, а не пустой стартовый экран, за которым что-то
    // происходит.
    showLibrary();

    FolderAdded result;

    try {
        // Карточка — на каждую новую книгу сразу, пока обход идёт: одной
        // карточкой, а не пересборкой всей полки — та стоила бы квадрата от
        // числа книг и стирала бы прогресс, который уже проступил на соседях.
        result = co_await app.ws->addFolder(folder, [app](const BookEntry& entry) {
            if (*app.shown == Screen::Library) app.shelf->appendBook(entry);
        });
    } catch (const system_exception& failure) {
        complain(app, L"Не удалось добавить каталог", folder.wstring(), failure);
        co_return;
    }

    if (!result.unread.empty()) {
        std::wstring text;
        for (const std::wstring& line : result.unread) {
            if (!text.empty()) text += L"\n";
            text += line;
        }
        app.notices->complain(L"Не удалось прочитать", std::move(text));
    }
}

/// Сохраняет обложку из мастера: копия снимка, запись реестра, немедленное
/// применение — сохранённая обложка тут же становится текущей темой.
detached_task saveSkinFlow(App app, Skin skin, std::filesystem::path photo,
                  std::function<void()> leaveWizard) {
    const u16_text skinName = skin.name;

    try {
        co_await app.ws->saveSkin(std::move(skin), photo);
    } catch (const system_exception& failure) {
        complain(app, L"Не удалось сохранить обложку", photo.wstring(), failure);
        co_return;
    }

    // Список тем — раньше полосы: слушатель темы, которого может позвать
    // setSkins полосы, переводит номер в имена по нему.
    app.themes->setSkins(app.ws->skins.list());
    app.view->setSkins(app.ws->skins.list());
    app.panel->refreshThemes();

    // Номер — по полному списку, а не по реестру: номера считаются вместе с
    // системными обложками, которые стоят впереди реестровых.
    //
    // Имя обложки в настройки и их запись — дело слушателя темы (wxl_launched):
    // смена темы здесь ничем не отличается от смены клавишей T.
    app.view->setTheme(app.themes->afterSave(skinName, app.view->theme.get()));

    leaveWizard();
}

/// Приносит полосе снимок подложки, который она заказала: байты файла читает
/// операция wxl, раскодирует их полоса у себя. Не прочитался — полоса остаётся
/// при бумаге темы, как прежде оставалась при неудачном раскодировании: снимок
/// обложки не стоит окна с ошибкой при каждой смене темы.
detached_task loadBackdropFlow(App app, std::filesystem::path file) {
    try {
        std::string bytes = co_await app.ws->readBytes(file);

        app.view->setBackdrop(file, std::move(bytes));
    } catch (const system_exception&) {
    }
}

/// Проверяет снимок перед мастером обложек: читает файл и пробует раскодировать
/// его как картинку. Годится — `proceed`; нет — читателю говорится, что именно
/// не так. Прежде это делал сам мастер, читая диск в потоке окна; теперь ему
/// достаётся путь уже проверенного снимка, а байты для показа полоса закажет
/// у loadBackdropFlow сама: второе чтение того же файла дешевле, чем нести
/// байты через мастер в полосу.
detached_task checkImageFlow(App app, std::filesystem::path image, std::function<void()> proceed) {
    try {
        if (!co_await app.ws->isImage(image)) {
            app.notices->complain(L"Это не изображение", image.wstring());
            co_return;
        }
    } catch (const system_exception& failure) {
        complain(app, L"Не удалось открыть изображение", image.wstring(), failure);
        co_return;
    }

    proceed();
}

/// Убрать обложку из реестра и из полосы тем.
///
/// **Снимок при этом остаётся лежать в `skins\`.** Копия картинки не
/// принадлежит той обложке, которая её привела: мастер, открыв обложку и
/// сохранив её под другим именем, заводит вторую с тем же именем файла, — и
/// удаление одной унесло бы снимок из-под другой. Ссылок на файл никто не
/// считает, а цена ошибки несимметрична: лишний файл на диске — мусор,
/// который никого не касается, отсутствующий — сломанная обложка.
///
/// Тема после удаления ищется по имени, а не по номеру: номера всех обложек
/// за удаляемой сдвигаются, и «остаться на своей» значит найти её заново.
/// Удалили ту, что была на экране, — читатель возвращается на встроенную
/// тему, номер которой читалка держит в настройках ровно на этот случай.
detached_task deleteSkinFlow(App app, u16_text name) {
    if (!app.ws->skins.find(name)) co_return;   // реестр успел перемениться под руками

    // Удаление спрашивает: точки по снимку читатель расставлял руками, вернуть
    // их нечем, а корзина стоит вплотную к шестерёнке. Ответ по умолчанию —
    // «Отмена»: промах по соседней кнопке не должен ничего стоить. Вопрос
    // говорит ровно то, что произойдёт: снимок остаётся в `skins\`, и обещать
    // его пропажу значило бы соврать про собственное поведение.
    //
    // Диалог — над показанным экраном (XamlRoot) и в его теме; ответ ждётся
    // здесь же, окно всё это время живо. Открытое сообщение сюда не пустит —
    // оно заслоняет остров; а сообщение, пришедшее, пока вопрос открыт, ждёт в
    // Notices и выходит, когда вопрос закрыт.
    {
        using namespace wxl::dsl;

        UIElement const host = app.window.content();
        auto dialog = ContentDialog{
            xamlRoot = host.xamlRoot(),
            requestedTheme = host.try_as<FrameworkElement>().actualTheme(),
            title = L"Удалить обложку «" + std::wstring(name.wchars()) + L"»?",
            content = TextBlock{u"Расставленные по снимку точки пропадут; сам снимок останется.",
                                textWrapping.wrap},
            primaryButtonText = u"Удалить",
            closeButtonText = u"Отмена",
            defaultButton = ContentDialogButton::Close,
            onClosed = [notices = app.notices](ContentDialog const&) { notices->flush(); },
        };
        if (co_await dialog.showAsync() != ContentDialogResult::Primary) co_return;
    }

    if (!app.ws->skins.find(name)) co_return;   // реестр мог перемениться, пока спрашивали

    // Куда встать — спрашиваем, пока старый список цел: номер текущей темы
    // смотрит в него. Ключ встроенной темы из настроек — тоже сейчас: когда
    // номер текущей выпадает за укоротившийся список, setSkins полосы ставит
    // первую тему, и слушатель темы пишет её ключ в настройки поверх того,
    // к которому читатель должен вернуться.
    const int theme = app.themes->afterRemoval(name, app.view->theme.get(), app.ws->settings.theme);

    co_await app.ws->deleteSkin(name);

    app.themes->setSkins(app.ws->skins.list());
    app.view->setSkins(app.ws->skins.list());

    // Настройки — имя обложки или ключ темы — поправит и запишет слушатель
    // темы (wxl_launched).
    app.view->setTheme(theme);
    app.panel->refreshThemes();
}

/// «Продолжить чтение»: найти книгу, которую читали, и открыть её.
///
/// Путь в настройках — копия того, что в реестре, и она там ради быстрого
/// пути. Протух — спрашиваем реестр по guid; нет и там — читателю нечего
/// продолжать, и он хотел открыть книгу.
detached_task continueReading(std::shared_ptr<Workspace> ws, std::shared_ptr<WarmBook> warm,
                     std::function<void(std::filesystem::path)> openBook,
                     std::function<void()> addBook) {
    std::filesystem::path path = ws->settings.lastBookPath;

    // Прогретую книгу открываем не спрашивая диск вовсе: она уже в памяти
    // целиком, и файл ей больше не нужен — даже если его успели унести. Тем
    // самым у нажатия не остаётся ни одного ожидания: книга встаёт на экран в
    // том же обороте очереди.
    if (!path.empty() && warm->book && warm->wanted == path) {
        openBook(path);
        co_return;
    }

    path = co_await ws->lastBookPath();

    if (path.empty()) {
        addBook();
        co_return;
    }

    openBook(path);
}

/// Греет книгу под кнопкой «Продолжить чтение»: читает файл и разбирает его,
/// пока читатель смотрит на заставку и решает, чего он хочет. Нажмёт — книга
/// уже в памяти, и открытие обходится без диска и без разбора, то есть без
/// самой долгой своей части; не нажмёт — потеряна одна фоновая загрузка,
/// которая шла в те секунды, когда приложению всё равно нечего делать.
///
/// Вёрстки здесь нет и быть не может: она зависит от размера окна и настроек
/// вида. Да и незачем — открытие верстает не книгу целиком, а текущую главу до
/// видимого разворота, и хвост той же главы дочитывает уже в простое
/// (BookView::open → relayoutNow → paginateTail). Греется ровно то, что от
/// вида не зависит: байты файла и разобранный документ с блоками.
///
/// Разбор идёт в интерфейсном потоке, как и в openBookFlow, и иначе нельзя:
/// память разбора берётся из STA-пула, а он чужого потока не терпит. Поток на
/// это время занят — но занят он до нажатия, а не после.
detached_task warmBookFlow(App app, std::filesystem::path path) {
    Warmed warmed;

    try {
        warmed = co_await app.ws->readBook(path);
    } catch (std::exception const&) {
        // Молча: читатель ни о чём не просил, и жаловаться ему пока не на что.
        // Файл не прочитался или книга испорчена — он узнает об этом, когда
        // нажмёт, из openBookFlow, который прочитает её сам и скажет поимённо.
        app.warm->wanted.clear();
        co_return;
    }

    // Пока шло чтение, читатель мог открыть эту книгу и сам — тогда прогревать
    // нечего, и вторая её копия в памяти никому не нужна.
    if (app.warm->wanted != path) co_return;

    app.warm->book = std::move(warmed.book);
    app.warm->fileSize = warmed.fileSize;
}

/// Показывает полосу набора: страница — на сцену, экран — книга. Общее у двух
/// дорог сюда: открытия книги и возвращения к уже открытой.
void revealBook(App const& app) {
    // Страница уже сверстана и нарисована: меру полоса берёт у самого окна
    // (ClientSizeChanged), а не у экрана поверх него, и потому сверстана
    // всегда — и когда её остров ещё не показан, и при автооткрытии на старте,
    // когда никакого экрана нет вовсе. Пустого листа читатель не увидит.
    *app.bookCameFrom = *app.shown;
    *app.shown = Screen::Book;
    // Полоса становится текущим экраном: показать её страницу на сцене и увести
    // задник окна с заставки на бумагу темы. Только потом — остров ввода поверх.
    app.view->setActive(true);
    app.window.content(app.view->root());
    app.notices->flush();
}

/// Открывает книгу: от байтов на диске до страницы на экране.
///
/// Порядок здесь -- это порядок обязательств. Сначала книга разбирается (и
/// только если разобралась, старая уступает ей место), потом на диск уходит
/// место чтения предыдущей, и лишь затем реестр, обложка, настройки и
/// состояние новой. Каждый `co_await` -- это выход в цикл сообщений: окно всё
/// это время живо, отвечает и перерисовывается.
///
/// Две короткие дороги в начале: книга уже открыта (читатель вернулся к ней) —
/// показать; книга прогрета (warmBookFlow) — взять её из памяти и не трогать
/// диск.
detached_task openBookFlow(App app, std::filesystem::path path) {
    // Эта книга уже открыта — читатель просто вернулся к ней со стартового
    // экрана или с полки. Ни читать, ни разбирать заново нечего: полоса держит
    // её со всей вёрсткой и местом чтения, а реестр и настройки давно на неё
    // указывают. Остаётся показать.
    if (app.view->isOpen() && app.view->book()->path() == path) {
        revealBook(app);
        co_return;
    }

    // Прогретая книга — та, что прочиталась и разобралась, пока читатель
    // смотрел на заставку (warmBookFlow). Забираем её из слота целиком: слот
    // держит одну книгу, и держать в нём ту, что сейчас откроется, незачем.
    const bool warmed = app.warm->wanted == path && app.warm->book;

    std::shared_ptr<Book> book = warmed ? std::move(app.warm->book) : nullptr;
    uint64_t fileSize = warmed ? app.warm->fileSize : 0;

    // Прогрев для этой же книги мог ещё идти — пусть, вернувшись, выбросит
    // своё: книга открывается и без него, а вторая её копия в памяти не нужна.
    app.warm->wanted.clear();
    app.warm->book.reset();

    if (!book) {
        Warmed read;

        try {
            read = co_await app.ws->readBook(path);
        } catch (const system_exception& failure) {
            complain(app, L"Не удалось прочитать файл книги", path.wstring(), failure);
            co_return;
        } catch (std::exception const& failure) {
            // Разговор с читателем, а не запись в лог: он только что выбрал этот
            // файл и вправе узнать, что с ним не так.
            u16_text const reason = unicode::assume_valid(failure.what()).to_utf16();
            app.notices->complain(L"Не удалось открыть книгу",
                                  path.wstring() + L"\n\n" + std::wstring(reason.wchars()));
            co_return;
        }

        book = std::move(read.book);
        fileSize = read.fileSize;
    }

    // Место чтения предыдущей книги — на диск сразу: сейчас настройки укажут
    // на другую, и записывать станет некуда.
    if (!app.ws->settings.lastBookGuid.empty() && app.view->isOpen()) {
        app.state->charOffset = app.view->readingPosition();

        co_await app.ws->saveState(app.ws->settings.lastBookGuid, *app.state);
    }

    // Реестр, обложка, настройки, состояние — у рабочего места; здесь только
    // то, что видно: книга на полосе и её закладки в панели.
    Opened opened = co_await app.ws->openBook(*book, fileSize);

    *app.state = std::move(opened.state);

    app.view->open(std::move(book), app.state->charOffset);
    app.panel->setState(app.state.get());

    revealBook(app);
}

/// Запуск: настройки, реестр, первый экран и только потом -- показ окна.
///
/// Окно строится пустым и невидимым, а показывается в конце: место, куда его
/// поставить, лежит в настройках, и открыть его сначала посреди экрана, а
/// потом переставить -- значит показать читателю прыжок. Ждать при этом нечего:
/// файл настроек читает рабочий поток, а этот тем временем уже крутит цикл
/// сообщений.
detached_task startupFlow(App app, wxl::DispatcherQueueTimer splashTimer,
                 std::function<void(std::filesystem::path)> openBook,
                 std::function<void()> showStartScreen,
                 std::function<void(std::filesystem::path)> warmBook,
                 std::function<void()> watchSettings) {
    // Настройки, обложки, реестр — у рабочего места, одной корутиной; что
    // при этом не удалось, читатель узнаёт словами, а запуск идёт дальше с
    // умолчаниями.
    const Started started = co_await app.ws->start();

    for (const Notice& notice : started.notices) app.notices->complain(notice.headline, notice.details);

    // Запись на диск начинает слушать поля настроек только теперь:
    // прочитанное из файла — не перемена, которую надо записать обратно.
    watchSettings();

    // Прогрев книги — сразу, как только стало известно, какая она: разбор
    // длится дольше всего остального запуска вместе взятого. Заказ уходит и
    // возвращается тут же, так что строки ниже его не ждут.
    //
    // Только когда книга не открывается сама: при continueReading её откроет
    // startupFlow ниже, и греть значило бы разобрать её дважды. Протухший путь
    // безвреден — прогрев не прочитает файла и тихо кончится, а «Продолжить
    // чтение» найдёт книгу по guid в реестре и откроет обычной дорогой.
    if (!app.ws->settings.continueReading.get() && !app.ws->settings.lastBookPath.empty())
        warmBook(app.ws->settings.lastBookPath);

    // Обложки — раньше темы: выбранной темой может оказаться обложка, а её
    // индекс продолжает список за встроенными и без реестра не существует.
    app.themes->setSkins(app.ws->skins.list());
    app.view->setSkins(app.ws->skins.list());
    app.panel->refreshThemes();

    // Обложка по имени, а нет такой — встроенная по ключу: номера считаются
    // по полному списку, вместе с системными обложками впереди реестровых.
    app.view->setTheme(app.themes->indexFromSettings(app.ws->settings.theme, app.ws->settings.skin));

    const std::filesystem::path& lastBook = started.lastBook;

    app.window.resize({kInitialWidth, kInitialHeight});

    // Размер по умолчанию ставится всегда, и лишь потом накрывается
    // запомненным. Иначе испорченная строка в настройках оставила бы окно
    // таким, каким его открыл WinUI: placement молча ничего не делает, когда
    // разбирать нечего, — и это правильно, но своё умолчание к тому моменту
    // должно быть уже на месте.
    if (!app.ws->settings.windowPlacement.empty()) app.window.placement(app.ws->settings.windowPlacement);

    if (app.ws->settings.continueReading.get() && !lastBook.empty()) {
        openBook(lastBook);
    } else {
        showStartScreen();
        splashTimer.start();
    }

    app.window.activate();
}

}  // namespace

wxl::Teardown wxl_launched() {
    // Теги разметки открыты только здесь, где она и пишется. На уровне файла
    // они накрыли бы обычные слова — text, size, state, entry — и под /W4
    // каждое такое имя, вплоть до параметров шаблонов самой wxl, стало бы
    // «прячущим глобальное» (C4459).
    using namespace wxl::dsl;

    // Пустые: их наполнит запуск, и наполнит асинхронно. Ни настройки, ни
    // реестр здесь не читаются — в этом потоке к диску не обращаются вовсе.
    auto ws = std::make_shared<Workspace>(standardDataDirectory());

    // Своё окно верхнего уровня на композиторе, а не генерируемое wxl::Window:
    // у того верхнее окно перенаправляемое, и при быстрой растяжке за угол в
    // просвете белеет его GDI-поверхность, стёртая системной кистью. Здесь окно
    // с WS_EX_NOREDIRECTIONBITMAP — поверхности перенаправления нет вовсе, а
    // содержимое целиком даёт композитор. Окно — хендл: App и обработчики держат
    // его копии, а само окно живёт до WM_NCDESTROY.
    CompositionWindow const window{u"Буквица", SizeInt32{720, 520}};

    // Рамка окна тёмная, как и остров: системный светлый заголовок над тёмными
    // ящиками, мастером и полкой смотрелся бы чужим.
    window.darkFrame(true);

    // Сообщения читателю и обработчик сбоев сценариев — после окна: сообщение
    // идёт диалогом над показанным экраном, а первый сценарий начнётся позже,
    // со startupFlow.
    auto notices = std::make_shared<Notices>(window);

    // Заставка — задником сцены: единственный визуал под XAML-островом
    // оснастки. Он ресайзится синхронно в WM_SIZE, оттого держится за рамку без
    // отставания, и в просвете при быстрой растяжке видна заставка, а не белое.
    // Грузим асинхронно через Win2D/TextureCache, а не синхронным
    // background(path): тот рисует свою DrawingSurface отдельным
    // D3D-устройством, и на сцене-композиторе оно не уживается с устройством
    // Win2D того же кэша — DirectComposition падает (dcompi). Асинхронная
    // загрузка идёт тем же устройством Win2D и не падает.
    window.backgroundAsync(applicationFolder() / L"Assets/splash-screen-1k.png");

    auto screen = std::make_shared<StartScreen>(window.chromeCompositor());
    auto view = std::make_shared<BookView>(window, *ws);
    auto shelf = std::make_shared<LibraryScreen>(*ws);
    auto wizard = std::make_shared<SkinWizard>(window.chromeCompositor());

    // Панель живёт поверх полосы набора: «поверх страницы» — это внутри полосы,
    // а не рядом с ней.
    auto panel = std::make_shared<ReaderPanel>(window.chromeCompositor(), *view, ws->settings);
    view->addOverlay(panel->root());

    // Состояние открытой книги: место чтения и закладки. Читается и пишется
    // целиком, потому что файл переписывается заменой — «дописать одно поле»
    // всё равно значит написать его весь.
    auto bookState = std::make_shared<BookState>();

    // Слот прогретой книги: в него запуск кладёт ту, что стоит на кнопке
    // «Продолжить чтение», разобрав её заранее.
    auto warm = std::make_shared<WarmBook>();

    // Настройки чтения общие для всех книг: читателю нужен один привычный вид,
    // а не разный шрифт в каждой книге. Ставит их запуск, когда прочитает файл.

    // ---- сохранение места чтения ----
    //
    // Отложенно, как и место окна: перелистывание — самое частое действие в
    // читалке, а файл состояния книги переписывается целиком.
    auto positionTimer = window.dispatcherQueue().createTimer();
    positionTimer.interval(kPositionQuiet);
    positionTimer.isRepeating(false);

    auto const rememberPosition = [view, ws, bookState] {
        // Книга закрыта -- писать нечего: место чтения принадлежит ей, а не
        // окну, и ноль незанятой полосы стёр бы то, что уже записано.
        if (ws->settings.lastBookGuid.empty() || !view->isOpen()) return;
        bookState->charOffset = view->readingPosition();
        saveStateLater(ws, ws->settings.lastBookGuid, *bookState);
    };

    positionTimer.add_onTick([positionTimer, rememberPosition](Object const&, Object const&) {
        positionTimer.stop();
        rememberPosition();
    });

    view->onPositionChanged = [positionTimer](uint32_t) {
        positionTimer.stop();
        positionTimer.start();
    };

    // ---- смена содержимого окна ----
    //
    // Три экрана — заставка, витрина и полоса набора — это три содержимого
    // одного окна, и переключение между ними одно присваивание. Второго окна
    // нет намеренно: оно завело бы вторую кнопку на панели задач.
    //
    // Куда возвращает Escape, знает `shown`, а `bookCameFrom` помнит, откуда
    // книгу открыли: читатель, выбравший её на полке, ждёт полку обратно, а
    // не заставку.
    auto shown = std::make_shared<Screen>(Screen::Start);
    auto bookCameFrom = std::make_shared<Screen>(Screen::Start);

    auto const closePanel = [panel] { panel->close(); };

    auto const showStartScreen = [window, screen, view, ws, shown, rememberPosition, closePanel,
                                  notices] {
        closePanel();
        // Уходя из книги, место чтения пишем сразу: отложенная запись ждёт
        // паузы, а читатель уже ушёл -- и, может быть, закроет приложение
        // раньше, чем таймер сработает.
        rememberPosition();

        // Полоса перестаёт быть текущим экраном: убрать её страницу со сцены и
        // вернуть задником окна заставку — под стартовым экраном она, а не
        // бумага книги. Задел на будущее (кэш texture держит снимок) — второй
        // показ заставки идёт без загрузки.
        view->setActive(false);
        window.backgroundAsync(applicationFolder() / L"Assets/splash-screen-1k.png");

        // Большой кнопке — её книга: обложка, название, автор. На каждом
        // показе, потому что последняя открытая книга могла смениться, пока
        // экрана не было видно; при запуске реестр к этому моменту прочитан.
        if (const BookEntry* known = ws->library.find(ws->settings.lastBookGuid)) {
            screen->setContinueBook(known->title, known->authors,
                                    known->cover.empty() ? std::filesystem::path{}
                                                         : ws->coverDirectory() / known->cover.wchars());
        }

        *shown = Screen::Start;
        window.content(screen->root());
        notices->flush();
    };

    // Всё, из чего собрано приложение, одной связкой: её берут корутины.
    App const app{window, ws, bookState, view, panel, shelf, screen, shown, bookCameFrom, warm, notices};

    auto const showLibrary = [app, window, shelf, ws, shown, rememberPosition, closePanel] {
        closePanel();
        rememberPosition();   // и полка тут же покажет свежий процент

        // Как и на стартовом экране: полоса перестаёт быть текущим экраном —
        // её страница уходит со сцены, а задником окна снова заставка.
        app.view->setActive(false);
        window.backgroundAsync(applicationFolder() / L"Assets/splash-screen-1k.png");

        // Полка пересобирается на каждый показ: книга могла добавиться, а
        // место чтения — уехать с тех пор, как её видели в прошлый раз.
        shelf->show(ws->library);
        *shown = Screen::Library;
        window.content(shelf->root());
        app.notices->flush();

        // Карточки уже стоят; проценты проступят на них по мере того, как
        // рабочий поток прочитает файлы состояния — по одному на книгу.
        fillProgress(ws, shelf, ws->library.books());
    };

    auto const openBook = [app](std::filesystem::path const& path) {
        openBookFlow(app, path);
    };

    // Заказ на прогрев: чью книгу ждём, записывается здесь и сразу — до того,
    // как чтение уйдёт к диску. По этой записи прогрев потом и узнаёт, нужен
    // ли ещё его результат.
    auto const warmBook = [app](std::filesystem::path const& path) {
        app.warm->wanted = path;
        app.warm->book.reset();
        warmBookFlow(app, path);
    };

    auto const addBook = [window, openBook] {
        std::filesystem::path const path = askForBook(window.handle());
        if (!path.empty()) openBook(path);
    };

    screen->onAddBook = addBook;
    auto const addFolder = [app, window, showLibrary] {
        std::filesystem::path const folder = askForFolder(window.handle());

        if (!folder.empty()) addFolderFlow(app, folder, showLibrary);
    };

    screen->onAddFolder = addFolder;
    screen->onLibrary = showLibrary;

    // Отмена стартового экрана — выход из приложения: обычное закрытие окна,
    // со всем, что оно сохраняет по дороге.
    screen->onExit = [window] { window.close(); };

    shelf->onAddBook = addBook;
    shelf->onBack = showStartScreen;
    shelf->onOpen = [ws, openBook](u16_text guid) {
        if (BookEntry const* known = ws->library.find(guid)) openBook(known->path);
    };
    panel->onStateChanged = [ws, bookState] {
        saveStateLater(ws, ws->settings.lastBookGuid, *bookState);
    };

    panel->onLibrary = showLibrary;

    // Правая кнопка по странице открывает ящик. Другой дороги к нему у мыши
    // нет: у страницы книги нет ни полосы меню, ни кнопок — и не должно быть.
    view->onPanelRequested = [panel] { panel->toggle(); };

    // Снимок подложки полоса заказывает, а не читает: диск — только операциями wxl.
    view->onBackdropNeeded = [app](std::filesystem::path file) {
        loadBackdropFlow(app, std::move(file));
    };

    // Настройки вида — наблюдаемые поля settings: ползунки панели привязаны к
    // ним (Bind), полоса пишет в них с колеса и клавиш и сама их слушает,
    // галочка полки привязана к continueReading. Здесь остаётся одна запись на
    // диск — с паузой, как у места окна: ползунок шлёт перемену на каждый
    // сдвиг, файл переписывается целиком, а две записи одного файла не должны
    // идти одновременно — `write_all` пишет через один временный файл. Сами
    // значения уже в settings: закрытие окна пишет их своей записью, не
    // дожидаясь паузы.
    auto viewTimer = window.dispatcherQueue().createTimer();
    viewTimer.interval(kSaveQuiet);
    viewTimer.isRepeating(false);
    viewTimer.add_onTick([viewTimer, ws](Object const&, Object const&) {
        viewTimer.stop();
        saveSettingsLater(ws);
    });

    // Слушатели поля — noexcept по контракту observable: им некому отдать
    // исключение. Настройки держат слушателей сами, снимать их незачем: они
    // живут столько же, сколько окно. Ставит их запуск (startupFlow) сразу
    // после чтения файла: прочитанное — не перемена, и писать его обратно
    // незачем. Указатели, а не shared_ptr: слушатель лежит в самом поле, и
    // держать им своего хозяина значило бы кольцо.
    auto const persistLater = [viewTimer]() noexcept {
        viewTimer.stop();
        viewTimer.start();
    };
    auto const watchSettings = [prefs = &ws->settings, page = view.get(), themes = app.themes,
                                persistLater] {
        static_cast<void>(prefs->fontSize.on_change([persistLater](double) noexcept { persistLater(); }));
        static_cast<void>(prefs->lineHeight.on_change([persistLater](double) noexcept { persistLater(); }));
        static_cast<void>(prefs->margin.on_change([persistLater](double) noexcept { persistLater(); }));
        static_cast<void>(
            prefs->continueReading.on_change([persistLater](bool) noexcept { persistLater(); }));

        // Тема — поле полосы, а в настройках она лежит именем: обложка — своим,
        // встроенная тема — ключом; прежний ключ при обложке остаётся как то,
        // куда вернуться, если реестр обложек пропадёт. Слушатель переводит
        // номер в имена по списку тем и пишет файл, только если имена
        // изменились: запуск ставит ту же тему, что в файле. Список тем
        // слушатель держит сам: тот о полосе не знает, кольца нет.
        static_cast<void>(page->theme.on_change([prefs, themes, persistLater](int index) noexcept {
            ThemeList::Persisted names = themes->persist(index, prefs->theme);
            if (names.skin == prefs->skin && names.theme == prefs->theme) return;
            prefs->skin = std::move(names.skin);
            prefs->theme = std::move(names.theme);
            persistLater();
        }));
    };

    // ---- мастер обложек ----
    //
    // Оверлей поверх полосы: под сеткой мастера читатель видит свою страницу,
    // изогнутую редактируемыми кривыми. Дорога туда — кнопки в панели «Вид»,
    // дорога обратно — его собственные кнопки; Escape мастером не занимается.
    view->addOverlay(wizard->root());

    // Предпросмотр: полоса рисуется со снимком и кривыми мастера. Он же —
    // пересчёт после отпускания точки.
    auto const previewSkin = [view, wizard] {
        view->setPreview(&wizard->skin(), wizard->imagePath());
    };

    auto const leaveWizard = [view, wizard] {
        wizard->hide();
        view->setPreview(nullptr, {});
        view->root().focus(FocusState::Programmatic);
    };

    panel->onAddSkin = [window, app, wizard, closePanel, previewSkin] {
        std::filesystem::path const path = askForImage(window.handle());

        if (path.empty()) return;

        checkImageFlow(
            app, path,
            [wizard, path, closePanel, previewSkin] {
                wizard->openNew(path);
                closePanel();
                wizard->show();
                previewSkin();
            });
    };

    panel->onEditSkin = [app, wizard, closePanel, previewSkin](u16_text skinName) {
        // По полному списку тем, а не по реестру: системные обложки живут
        // только в нём, а шестерёнка есть и у них — правка «на основе».
        const std::optional<int> index = app.themes->indexOfSkin(skinName);
        const Skin* known = index ? app.themes->skinAt(*index) : nullptr;
        if (!known) return;   // список успел перемениться под руками

        // Копия, а не указатель: пока снимок читают, список тем может
        // перемениться, и указатель в него протухнет.
        const Skin skin = *known;

        const std::filesystem::path image = app.ws->skinImagePath(skin);

        checkImageFlow(
            app, image,
            [wizard, skin, image, closePanel, previewSkin] {
                wizard->openEdit(skin, image);
                closePanel();
                wizard->show();
                previewSkin();
            });
    };

    // Спрашивает и удаляет сам сценарий: вопрос — диалог, которого ждут.
    panel->onDeleteSkin = [app](u16_text skinName) { deleteSkinFlow(app, std::move(skinName)); };

    wizard->onCurvesChanged = previewSkin;

    wizard->onChooseAnother = [window, app, wizard, previewSkin] {
        std::filesystem::path const path = askForImage(window.handle());

        // Отказался — остаёмся на прежнем снимке: читатель ничего не терял.
        if (path.empty()) return;

        checkImageFlow(
            app, path,
            [wizard, path, previewSkin] {
                wizard->openNew(path);
                previewSkin();
            });
    };

    wizard->onExit = leaveWizard;

    wizard->onSave = [app, leaveWizard](Skin skin, std::filesystem::path photo) {
        saveSkinFlow(app, std::move(skin), std::move(photo), leaveWizard);
    };

    screen->onContinueReading = [ws, warm, openBook, addBook] {
        continueReading(ws, warm, openBook, addBook);
    };

    // ---- клавиши, общие для обоих экранов ----
    //
    // Полный экран по F11, выход из него по Escape. Перехват на пути вниз, а
    // не на всплытии: событие начинается у того, на чём фокус, и клавиша
    // должна работать независимо от того, на какой кнопке он сейчас стоит.
    //
    // Что значит клавиша, решает карта клавиш (`resolve`, bukvitsa.lib): там
    // вся матрица — полоса, мастер, ползунок и поле ввода, приоритет Escape.
    // Здесь — только контекст для неё и исполнение ответа.
    //
    // Флага «мы в полном экране» нет намеренно: он был бы вторым местом, где это
    // записано, — а состояние знает само окно (window.fullScreen()), и второй
    // его слепок разошёлся бы с первым.
    auto const isFullScreen = [window] { return window.fullScreen(); };

    auto const setFullScreen = [window](bool on) { window.fullScreen(on); };

    auto const installKeys = [setFullScreen, isFullScreen, shown, bookCameFrom, showStartScreen,
                              showLibrary, panel, view, wizard](UIElement const& element) {
        element.add_onPreviewKeyDown([=](Object const&, KeyRoutedEventArgs& args) {
            // Листание, кегль и тему полоса пока разбирает сама — её
            // обработчик стоит на том же корне раньше этого и помечает свои
            // клавиши. Помеченная сюда не доходит, и команд полосы приложение
            // не исполняет; карта отвечает за них так же, как полоса.
            if (args.handled()) return;   // полоса набора своё уже разобрала

            const KeyPress press = keyPressOf(args.key());

            // Открыта ли сноска, полоса пока не говорит: dismissOverlays()
            // спрашивает и закрывает разом. Поэтому сноска считается открытой,
            // пока полоса не ответит, что закрывать было нечего, — тогда
            // клавиша разбирается заново без неё. Спрашивает только Escape над
            // книгой без панели: прочие клавиши от сноски не зависят.
            KeyContext context{
                .shown = *shown,
                .cameFrom = *bookCameFrom,
                .wizardOpen = wizard->isOpen(),
                .panelOpen = panel->isOpen(),
                .noteOpen = true,
                .fullScreen = isFullScreen(),
                .origin = focusOriginOf(args.originalSource()),
                .bookOpen = view->isOpen(),
            };

            Command command = resolve(press, context);
            if (command == Command::DismissNote && !view->dismissOverlays()) {
                context.noteOpen = false;
                command = resolve(press, context);
            }

            switch (command) {
                case Command::ShowLibrary:
                case Command::BackToLibrary:
                    showLibrary();
                    break;
                case Command::BackToStart:
                    showStartScreen();
                    break;
                case Command::OpenContents:
                    panel->open(ReaderPanel::Tab::Contents);
                    break;
                case Command::OpenSearch:
                    panel->open(ReaderPanel::Tab::Search);
                    break;
                case Command::OpenBookmarks:
                    panel->open(ReaderPanel::Tab::Bookmarks);
                    break;
                case Command::TogglePanel:
                    panel->toggle();
                    break;
                case Command::ClosePanel:
                    panel->close();   // фокус полосе возвращает сама панель
                    break;
                case Command::DismissNote:
                    break;   // сноску закрыла полоса — больше ничего не нужно
                case Command::ToggleFullScreen:
                    setFullScreen(!isFullScreen());
                    break;
                case Command::LeaveFullScreen:
                    setFullScreen(false);
                    break;

                // Команды полосы — фаза 2: их исполнит приложение, когда
                // полоса отдаст свой обработчик карте. Сейчас они сюда не
                // доходят (см. выше), а дошедшую — не метим.
                case Command::TurnForward:
                case Command::TurnBackward:
                case Command::GoToStart:
                case Command::GoToEnd:
                case Command::FontLarger:
                case Command::FontSmaller:
                case Command::FontReset:
                case Command::NextTheme:
                case Command::None:
                    return;   // не наша клавиша: пусть идёт дальше
            }
            args.handled(true);
        });
    };

    installKeys(screen->root());
    installKeys(shelf->root());
    installKeys(view->root());

    // Перетаскивание книги в окно. Регистрация цели идёт на HWND, который к
    // этому моменту уже есть -- окно создано, хотя ещё и не показано. Пачку из
    // нескольких файлов окно уже свело к первому пути: читалка показывает одну
    // книгу, а добавлять остальные в реестр молча значило бы решать за читателя.
    window.acceptFileDrops([openBook](std::filesystem::path path) { openBook(path); });

    // ---- сохранение места окна ----
    auto saveTimer = window.dispatcherQueue().createTimer();
    saveTimer.interval(kSaveQuiet);
    saveTimer.isRepeating(false);

    auto const rememberWindow = [window, ws] {
        // Место отдаётся строкой WinRT; к нам она приходит чужим текстом, и
        // проверенным становится так же, как любой другой чужой.
        ws->settings.windowPlacement = unicode::repaired(window.placement());
        saveSettingsLater(ws);
    };

    saveTimer.add_onTick([saveTimer, rememberWindow](Object const&, Object const&) {
        saveTimer.stop();
        rememberWindow();
    });

    // Геометрия сменилась — двигали, растягивали, максимизировали или
    // восстановили: всё, что мы запоминаем. Окно сводит это в один колбэк.
    window.onGeometryChanged([saveTimer] {
        saveTimer.stop();   // каждое движение отодвигает запись
        saveTimer.start();
    });

    window.onClosed([saveTimer, positionTimer, viewTimer, rememberWindow, rememberPosition] {
        saveTimer.stop();
        positionTimer.stop();
        viewTimer.stop();   // окно уходит с полными настройками: rememberWindow пишет их
        rememberWindow();
        rememberPosition();
    });

    // Пауза заставки отсчитывается таймером очереди UI, а не сном: поток, на
    // котором стоит окно, обязан оставаться свободным.
    auto splashTimer = window.dispatcherQueue().createTimer();
    splashTimer.interval(kSplashHold);
    splashTimer.isRepeating(false);
    splashTimer.add_onTick([screen, splashTimer](Object const&, Object const&) {
        splashTimer.stop();
        screen->reveal();
    });

    // ---- запуск ----
    //
    // Всё, что читалка знает о себе, читается отсюда и асинхронно: настройки,
    // реестр, книга, на которой остановились. Окно показывается в конце этой
    // цепочки — уже на своём месте и с уже выбранным экраном.
    startupFlow(app, splashTimer, openBook, showStartScreen, warmBook, watchSettings);

    // Захват держит экраны, модели и таймеры живыми, пока идёт приложение:
    // на них ссылаются обработчики окна и полосы. Само окно держит себя до
    // WM_NCDESTROY и в захвате не нуждается. Пул STA не наш: его строит и
    // держит сама wxl в своей точке входа, и второй такой падает.
    //
    //
    // Сценарии держат себя сами (`detached_task`), а петлю ввода-вывода wxl
    // гасит уже после этого обработчика, дождавшись операций в полёте.
    return [screen, shelf, view, ws, wizard, saveTimer, positionTimer, viewTimer,
            splashTimer](TeardownReason) {};
}
