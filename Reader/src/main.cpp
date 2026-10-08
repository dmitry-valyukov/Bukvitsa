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
#include "skin_wizard.h"
#include "start_screen.h"

// Последними: они ведут к модели книги и реестру, а те импортируют wxl.core,
// после чего стандартный заголовок MSVC уже не принимает.
#include "bukvitsa/reader/book.h"
#include "book_view.h"
#include "bukvitsa/reader/library.h"
#include "library_screen.h"
#include "notices.h"
#include "reader_panel.h"
#include "bukvitsa/reader/settings.h"
#include "bukvitsa/reader/store.h"
#include "bukvitsa/reader/theme_list.h"
#include "bukvitsa/reader/warm_slot.h"
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
// Один объект со счётчиком: окно, рабочее место, модели и экраны — его поля,
// по значению. Выделение одно, из STA-пула, счётчик без interlocked — поток
// один (`sta_refcounted`); корутины держат его в кадре одной ссылкой.
//
// Поля — в порядке жизни: строятся сверху вниз, умирают снизу вверх. Рабочее
// место выше экранов: они берут его настройки и пути, а полоса снимает своих
// слушателей с его настроек в деструкторе. Тема показанного экрана выше
// сообщений: они берут её при каждом показе. Полоса выше панели: панель держит
// ссылку на неё и снимает слушателя с её темы. Экраны неперемещаемы — держат
// `this` в своих подписках — и строятся прямо на месте.
//
// Правило захвата: поле хранит `App*`; сценарий, переживающий `co_await`,
// получает `AppPtr`, сделанный из того же `App*` в момент запуска — счётчик в
// самом объекте. Обработчик лежит в экране, экран — в App, и сильная ссылка из
// обработчика на владельца была бы кольцом: App не умер бы никогда.
class App : public sta_refcounted, private noncopyable {
public:
    /// Строит всё на месте. Окно приходит уже созданным: рамку и заставку ему
    /// ставят раньше всего остального.
    explicit App(CompositionWindow frame);

    CompositionWindow const window;

    /// Рабочее место: настройки, реестр, обложки и все файлы. Пустое: его
    /// наполнит запуск, и наполнит асинхронно — в этом потоке к диску не
    /// обращаются вовсе.
    Workspace ws;

    /// Темы и обложки одним списком — номер темы полосы ↔ имена в настройках.
    /// Заполняется там же и тем же, что список полосы (`setSkins`): при
    /// запуске, после сохранения и после удаления обложки.
    ThemeList themes;

    /// Слот прогретой книги: в него запуск кладёт ту, что стоит на кнопке
    /// «Продолжить чтение», разобрав её заранее.
    WarmSlot warm;

    /// Тема XAML показанного экрана: полоса тёмная, полка и заставка светлые —
    /// так их корни и задают. Диалог — не часть дерева экрана и его темы не
    /// наследует: сообщения и вопрос об удалении обложки берут её отсюда.
    observable<ElementTheme> screenTheme{ElementTheme::Light};

    /// Сообщения читателю и обработчик сбоев сценариев — после окна: сообщение
    /// идёт диалогом над показанным экраном, а первый сценарий начнётся позже,
    /// со startupFlow.
    Notices notices;

    /// Состояние открытой книги: место чтения и закладки. Читается и пишется
    /// целиком, потому что файл переписывается заменой — «дописать одно поле»
    /// всё равно значит написать его весь.
    BookState state;

    StartScreen screen;
    BookView view;
    LibraryScreen shelf;
    SkinWizard wizard;   ///< оверлей поверх полосы и панели
    ReaderPanel panel;   ///< ящики поверх страницы — внутри полосы, а не рядом с ней

    /// Три экрана — заставка, витрина и полоса набора — это три содержимого
    /// одного окна, и переключение между ними одно присваивание. Второго окна
    /// нет намеренно: оно завело бы вторую кнопку на панели задач.
    ///
    /// Куда возвращает Escape, знает `shown`, а `bookCameFrom` помнит, откуда
    /// книгу открыли: читатель, выбравший её на полке, ждёт полку обратно, а
    /// не заставку.
    observable<Screen> shown{Screen::Start};
    observable<Screen> bookCameFrom{Screen::Start};

    /// Отложенная запись места чтения, настроек вида, места окна; пауза заставки.
    DispatcherQueueTimer positionTimer;
    DispatcherQueueTimer viewTimer;
    DispatcherQueueTimer saveTimer;
    DispatcherQueueTimer splashTimer;

    /// Экран стал текущим: что показано — для клавиш и полки, его тема — для
    /// диалогов над ним. Одно место на обе перемены, чтобы они не разошлись.
    void show(Screen next);
    void showStartScreen();
    void showLibrary();
    void closePanel();
    void openBook(std::filesystem::path const& path);
    void warmBook(std::filesystem::path const& path);
    void addBook();
    void addFolder();
    void rememberPosition();
    void rememberWindow();
    void persistLater() noexcept;
    void watchSettings();
    void previewSkin();
    void leaveWizard();
    void installKeys(UIElement const& element);
};

/// Сильная ссылка на приложение — у сценариев и у обработчика Teardown.
using AppPtr = intrusive_ptr<App>;

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
// Первым параметром — `AppPtr` (правило захвата — над App), дальше — только
// данные сценария. Сценарий ловит то, на что у него есть ответ читателю, —
// файл книги, снимок, каталог; «файла нет» там, где его может и не быть, — не
// сбой. Остальное всплывает в `on_detached_task_failure` (его ставит
// `Notices`): читатель видит окно с причиной, сценарий на этом кончается.

/// Пишет настройки — сценарием без владельца, по таймеру или при закрытии:
/// текст рабочее место собирает в момент вызова, до ожидания.
detached_task saveSettingsLater(AppPtr app) {
    co_await app->ws.saveSettings();
}

/// Пишет состояние книги: место чтения и закладки.
detached_task saveStateLater(AppPtr app, u16_text guid, BookState state) {
    co_await app->ws.saveState(std::move(guid), std::move(state));
}

/// Достраивает полку: у каждой книги свой файл состояния, и читаются они по
/// одному, уже после того, как полка показана.
///
/// Это и есть «библиотека наполняется по мере чтения»: карточки встают сразу,
/// а «прочитано 42%» проступает на каждой, как только её файл прочитан. Полка
/// с сотней книг не ждёт сотни обращений к диску, чтобы показать первую.
detached_task fillProgress(AppPtr app, std::vector<BookEntry> books) {
    for (const BookEntry& book : books) {
        if (book.characterCount == 0) continue;   // не открывалась -- и читать нечего

        const BookState state = co_await app->ws.readState(book.guid);

        // Открывалась, а записать место не успела — показывать нечего.
        if (state.charOffset == 0 && state.bookmarks.empty()) continue;

        app->shelf.setProgress(book.guid, state.charOffset, state.bookmarks.size());
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
detached_task addFolderFlow(AppPtr app, std::filesystem::path folder) {
    // Полка -- прежде обхода: читатель, добавивший каталог, должен видеть, как
    // тот наполняется, а не пустой стартовый экран, за которым что-то
    // происходит.
    app->showLibrary();

    FolderAdded result;

    try {
        // Карточка — на каждую новую книгу сразу, пока обход идёт: одной
        // карточкой, а не пересборкой всей полки — та стоила бы квадрата от
        // числа книг и стирала бы прогресс, который уже проступил на соседях.
        result = co_await app->ws.addFolder(folder, [app](const BookEntry& entry) {
            if (app->shown.get() == Screen::Library) app->shelf.appendBook(entry);
        });
    } catch (const system_exception& failure) {
        app->notices.post(noticeOf(L"Не удалось добавить каталог", folder.wstring(), failure));
        co_return;
    }

    if (!result.unread.empty()) {
        std::wstring text;
        for (const std::wstring& line : result.unread) {
            if (!text.empty()) text += L"\n";
            text += line;
        }
        app->notices.post({L"Не удалось прочитать", std::move(text)});
    }
}

/// Сохраняет обложку из мастера: копия снимка, запись реестра, немедленное
/// применение — сохранённая обложка тут же становится текущей темой.
detached_task saveSkinFlow(AppPtr app, Skin skin, std::filesystem::path photo) {
    const u16_text skinName = skin.name;

    try {
        co_await app->ws.saveSkin(std::move(skin), photo);
    } catch (const system_exception& failure) {
        app->notices.post(noticeOf(L"Не удалось сохранить обложку", photo.wstring(), failure));
        co_return;
    }

    // Список тем — раньше полосы: слушатель темы, которого может позвать
    // setSkins полосы, переводит номер в имена по нему.
    app->themes.setSkins(app->ws.skins.list());
    app->view.setSkins(app->ws.skins.list());
    app->panel.refreshThemes();

    // Номер — по полному списку, а не по реестру: номера считаются вместе с
    // системными обложками, которые стоят впереди реестровых.
    //
    // Имя обложки в настройки и их запись — дело слушателя темы (watchSettings):
    // смена темы здесь ничем не отличается от смены клавишей T.
    app->view.setTheme(app->themes.afterSave(skinName, app->view.theme.get()));

    app->leaveWizard();
}

/// Приносит полосе снимок подложки, который она заказала: байты файла читает
/// операция wxl, раскодирует их полоса у себя. Не прочитался — полоса остаётся
/// при бумаге темы, как прежде оставалась при неудачном раскодировании: снимок
/// обложки не стоит окна с ошибкой при каждой смене темы.
detached_task loadBackdropFlow(AppPtr app, std::filesystem::path file) {
    try {
        std::string bytes = co_await app->ws.readBytes(file);

        app->view.setBackdrop(file, std::move(bytes));
    } catch (const system_exception&) {
    }
}

/// Проверяет снимок перед мастером обложек: читает файл и пробует раскодировать
/// его как картинку. Годится — `proceed`; нет — читателю говорится, что именно
/// не так. Мастеру достаётся путь уже проверенного снимка, а байты для показа
/// полоса закажет у loadBackdropFlow сама: второе чтение того же файла дешевле,
/// чем нести байты через мастер в полосу.
///
/// `proceed` — не метод App: у трёх дорог в мастер продолжения разные. Голый
/// `App*` в них жив, пока жив кадр: кадр держит `app`.
detached_task checkImageFlow(AppPtr app, std::filesystem::path image, std::function<void()> proceed) {
    try {
        if (!co_await app->ws.isImage(image)) {
            app->notices.post({L"Это не изображение", image.wstring()});
            co_return;
        }
    } catch (const system_exception& failure) {
        app->notices.post(noticeOf(L"Не удалось открыть изображение", image.wstring(), failure));
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
detached_task deleteSkinFlow(AppPtr app, u16_text name) {
    if (!app->ws.skins.find(name)) co_return;   // реестр успел перемениться под руками

    // Удаление спрашивает: точки по снимку читатель расставлял руками, вернуть
    // их нечем, а корзина стоит вплотную к шестерёнке. Ответ по умолчанию —
    // «Отмена»: промах по соседней кнопке не должен ничего стоить. Вопрос
    // говорит ровно то, что произойдёт: снимок остаётся в `skins\`, и обещать
    // его пропажу значило бы соврать про собственное поведение.
    //
    // Диалог — над показанным экраном (XamlRoot) и в его теме (screenTheme);
    // ответ ждётся здесь же, окно всё это время живо. Открытое сообщение сюда
    // не пустит — оно заслоняет остров; а сообщение, пришедшее, пока вопрос
    // открыт, ждёт в Notices и выходит, когда вопрос закрыт.
    {
        using namespace wxl::dsl;

        UIElement const host = app->window.content();
        auto dialog = ContentDialog{
            xamlRoot = host.xamlRoot(),
            requestedTheme = app->screenTheme.get(),
            title = L"Удалить обложку «" + std::wstring(name.wchars()) + L"»?",
            content = TextBlock{u"Расставленные по снимку точки пропадут; сам снимок останется.",
                                textWrapping.wrap},
            primaryButtonText = u"Удалить",
            closeButtonText = u"Отмена",
            defaultButton = ContentDialogButton::Close,
            onClosed = [app](ContentDialog const&) { app->notices.flush(); },
        };
        if (co_await dialog.showAsync() != ContentDialogResult::Primary) co_return;
    }

    if (!app->ws.skins.find(name)) co_return;   // реестр мог перемениться, пока спрашивали

    // Куда встать — спрашиваем, пока старый список цел: номер текущей темы
    // смотрит в него. Ключ встроенной темы из настроек — тоже сейчас: когда
    // номер текущей выпадает за укоротившийся список, setSkins полосы ставит
    // первую тему, и слушатель темы пишет её ключ в настройки поверх того,
    // к которому читатель должен вернуться.
    const int theme = app->themes.afterRemoval(name, app->view.theme.get(), app->ws.settings.theme);

    co_await app->ws.deleteSkin(name);

    app->themes.setSkins(app->ws.skins.list());
    app->view.setSkins(app->ws.skins.list());

    // Настройки — имя обложки или ключ темы — поправит и запишет слушатель
    // темы (watchSettings).
    app->view.setTheme(theme);
    app->panel.refreshThemes();
}

/// «Продолжить чтение»: найти книгу, которую читали, и открыть её.
///
/// Путь в настройках — копия того, что в реестре, и она там ради быстрого
/// пути. Протух — спрашиваем реестр по guid; нет и там — читателю нечего
/// продолжать, и он хотел открыть книгу.
detached_task continueReading(AppPtr app) {
    std::filesystem::path path = app->ws.settings.lastBookPath;

    // Прогретую книгу открываем не спрашивая диск вовсе: она уже в памяти
    // целиком, и файл ей больше не нужен — даже если его успели унести. Тем
    // самым у нажатия не остаётся ни одного ожидания: книга встаёт на экран в
    // том же обороте очереди.
    if (!path.empty() && app->warm.holds(path)) {
        app->openBook(path);
        co_return;
    }

    path = co_await app->ws.lastBookPath();

    if (path.empty()) {
        app->addBook();
        co_return;
    }

    app->openBook(path);
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
detached_task warmBookFlow(AppPtr app, std::filesystem::path path) {
    Warmed warmed;

    try {
        warmed = co_await app->ws.readBook(path);
    } catch (std::exception const&) {
        // Молча: читатель ни о чём не просил, и жаловаться ему пока не на что.
        // Файл не прочитался или книга испорчена — он узнает об этом, когда
        // нажмёт, из openBookFlow, который прочитает её сам и скажет поимённо.
        app->warm.cancel();
        co_return;
    }

    // Пока шло чтение, читатель мог открыть эту книгу и сам — тогда слот её не
    // примет: вторая копия в памяти никому не нужна.
    app->warm.fill(path, std::move(warmed.book), warmed.fileSize);
}

/// Показывает полосу набора: страница — на сцену, экран — книга. Общее у двух
/// дорог сюда: открытия книги и возвращения к уже открытой.
void revealBook(App& app) {
    // Страница уже сверстана и нарисована: меру полоса берёт у самого окна
    // (ClientSizeChanged), а не у экрана поверх него, и потому сверстана
    // всегда — и когда её остров ещё не показан, и при автооткрытии на старте,
    // когда никакого экрана нет вовсе. Пустого листа читатель не увидит.
    app.bookCameFrom.set(app.shown.get());
    app.show(Screen::Book);
    // Полоса становится текущим экраном: показать её страницу на сцене и увести
    // задник окна с заставки на бумагу темы. Только потом — остров ввода поверх.
    app.view.setActive(true);
    app.window.content(app.view.root());
    app.notices.flush();
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
detached_task openBookFlow(AppPtr app, std::filesystem::path path) {
    // Эта книга уже открыта — читатель просто вернулся к ней со стартового
    // экрана или с полки. Ни читать, ни разбирать заново нечего: полоса держит
    // её со всей вёрсткой и местом чтения, а реестр и настройки давно на неё
    // указывают. Остаётся показать.
    if (app->view.isOpen() && app->view.book()->path() == path) {
        revealBook(*app);
        co_return;
    }

    // Прогретая книга — та, что прочиталась и разобралась, пока читатель
    // смотрел на заставку (warmBookFlow). Забираем её из слота целиком: слот
    // держит одну книгу, и держать в нём ту, что сейчас откроется, незачем.
    std::optional<Warmed> warmed = app->warm.take(path);

    std::shared_ptr<Book> book = warmed ? std::move(warmed->book) : nullptr;
    uint64_t fileSize = warmed ? warmed->fileSize : 0;

    // Прогрев для этой же книги мог ещё идти — пусть, вернувшись, выбросит
    // своё: книга открывается и без него, а вторая её копия в памяти не нужна.
    // Прогретая для другой книги — тоже прочь: открывается эта.
    app->warm.cancel();

    if (!book) {
        Warmed read;

        try {
            read = co_await app->ws.readBook(path);
        } catch (const system_exception& failure) {
            app->notices.post(noticeOf(L"Не удалось прочитать файл книги", path.wstring(), failure));
            co_return;
        } catch (std::exception const& failure) {
            // Разговор с читателем, а не запись в лог: он только что выбрал этот
            // файл и вправе узнать, что с ним не так.
            u16_text const reason = unicode::assume_valid(failure.what()).to_utf16();
            app->notices.post({L"Не удалось открыть книгу",
                               path.wstring() + L"\n\n" + std::wstring(reason.wchars())});
            co_return;
        }

        book = std::move(read.book);
        fileSize = read.fileSize;
    }

    // Место чтения предыдущей книги — на диск сразу: сейчас настройки укажут
    // на другую, и записывать станет некуда.
    if (!app->ws.settings.lastBookGuid.empty() && app->view.isOpen()) {
        app->state.charOffset = app->view.readingPosition();

        co_await app->ws.saveState(app->ws.settings.lastBookGuid, app->state);
    }

    // Реестр, обложка, настройки, состояние — у рабочего места; здесь только
    // то, что видно: книга на полосе и её закладки в панели.
    Opened opened = co_await app->ws.openBook(*book, fileSize);

    app->state = std::move(opened.state);

    app->view.open(std::move(book), app->state.charOffset);
    app->panel.setState(&app->state);

    revealBook(*app);
}

/// Запуск: настройки, реестр, первый экран и только потом -- показ окна.
///
/// Окно строится пустым и невидимым, а показывается в конце: место, куда его
/// поставить, лежит в настройках, и открыть его сначала посреди экрана, а
/// потом переставить -- значит показать читателю прыжок. Ждать при этом нечего:
/// файл настроек читает рабочий поток, а этот тем временем уже крутит цикл
/// сообщений.
detached_task startupFlow(AppPtr app) {
    // Настройки, обложки, реестр — у рабочего места, одной корутиной; что
    // при этом не удалось, читатель узнаёт словами, а запуск идёт дальше с
    // умолчаниями.
    const Started started = co_await app->ws.start();

    for (const Notice& notice : started.notices) app->notices.post(notice);

    // Запись на диск начинает слушать поля настроек только теперь:
    // прочитанное из файла — не перемена, которую надо записать обратно.
    app->watchSettings();

    // Прогрев книги — сразу, как только стало известно, какая она: разбор
    // длится дольше всего остального запуска вместе взятого. Заказ уходит и
    // возвращается тут же, так что строки ниже его не ждут.
    //
    // Только когда книга не открывается сама: при continueReading её откроет
    // startupFlow ниже, и греть значило бы разобрать её дважды. Протухший путь
    // безвреден — прогрев не прочитает файла и тихо кончится, а «Продолжить
    // чтение» найдёт книгу по guid в реестре и откроет обычной дорогой.
    if (!app->ws.settings.continueReading.get() && !app->ws.settings.lastBookPath.empty())
        app->warmBook(app->ws.settings.lastBookPath);

    // Обложки — раньше темы: выбранной темой может оказаться обложка, а её
    // индекс продолжает список за встроенными и без реестра не существует.
    app->themes.setSkins(app->ws.skins.list());
    app->view.setSkins(app->ws.skins.list());
    app->panel.refreshThemes();

    // Обложка по имени, а нет такой — встроенная по ключу: номера считаются
    // по полному списку, вместе с системными обложками впереди реестровых.
    app->view.setTheme(app->themes.indexFromSettings(app->ws.settings.theme, app->ws.settings.skin));

    const std::filesystem::path& lastBook = started.lastBook;

    app->window.resize({kInitialWidth, kInitialHeight});

    // Размер по умолчанию ставится всегда, и лишь потом накрывается
    // запомненным. Иначе испорченная строка в настройках оставила бы окно
    // таким, каким его открыл WinUI: placement молча ничего не делает, когда
    // разбирать нечего, — и это правильно, но своё умолчание к тому моменту
    // должно быть уже на месте.
    if (!app->ws.settings.windowPlacement.empty())
        app->window.placement(app->ws.settings.windowPlacement);

    if (app->ws.settings.continueReading.get() && !lastBook.empty()) {
        app->openBook(lastBook);
    } else {
        app->showStartScreen();
        app->splashTimer.start();
    }

    app->window.activate();
}

// ---- App: сборка и связки -------------------------------------------------
//
// Метод отдаёт сценарию `this`: `AppPtr` со своей ссылкой из него делает вызов.

App::App(CompositionWindow frame)
    : window(std::move(frame)),
      ws(standardDataDirectory()),
      notices(window, screenTheme),
      screen(window.chromeCompositor()),
      view(window, ws),
      shelf(ws),
      wizard(window.chromeCompositor()),
      panel(window.chromeCompositor(), view, ws.settings),
      positionTimer(window.dispatcherQueue().createTimer()),
      viewTimer(window.dispatcherQueue().createTimer()),
      saveTimer(window.dispatcherQueue().createTimer()),
      splashTimer(window.dispatcherQueue().createTimer()) {
    // Панель живёт поверх полосы набора: «поверх страницы» — это внутри полосы,
    // а не рядом с ней.
    view.addOverlay(panel.root());

    // Мастер обложек — оверлей поверх полосы и панели: под сеткой мастера
    // читатель видит свою страницу, изогнутую редактируемыми кривыми. Дорога
    // туда — кнопки в панели «Вид», дорога обратно — его собственные кнопки;
    // Escape мастером не занимается.
    view.addOverlay(wizard.root());

    // Место чтения — отложенно, как и место окна: перелистывание — самое частое
    // действие в читалке, а файл состояния книги переписывается целиком.
    positionTimer.interval(kPositionQuiet);
    positionTimer.isRepeating(false);

    viewTimer.interval(kSaveQuiet);
    viewTimer.isRepeating(false);

    saveTimer.interval(kSaveQuiet);
    saveTimer.isRepeating(false);

    // Пауза заставки отсчитывается таймером очереди UI, а не сном: поток, на
    // котором стоит окно, обязан оставаться свободным.
    splashTimer.interval(kSplashHold);
    splashTimer.isRepeating(false);
}

void App::show(Screen next) {
    shown.set(next);
    screenTheme.set(next == Screen::Book ? ElementTheme::Dark : ElementTheme::Light);
}

void App::closePanel() {
    panel.close();
}

void App::rememberPosition() {
    // Книга закрыта -- писать нечего: место чтения принадлежит ей, а не
    // окну, и ноль незанятой полосы стёр бы то, что уже записано.
    if (ws.settings.lastBookGuid.empty() || !view.isOpen()) return;
    state.charOffset = view.readingPosition();
    saveStateLater(this, ws.settings.lastBookGuid, state);
}

void App::rememberWindow() {
    // Место отдаётся строкой WinRT; к нам она приходит чужим текстом, и
    // проверенным становится так же, как любой другой чужой.
    ws.settings.windowPlacement = unicode::repaired(window.placement());
    saveSettingsLater(this);
}

void App::showStartScreen() {
    closePanel();
    // Уходя из книги, место чтения пишем сразу: отложенная запись ждёт
    // паузы, а читатель уже ушёл -- и, может быть, закроет приложение
    // раньше, чем таймер сработает.
    rememberPosition();

    // Полоса перестаёт быть текущим экраном: убрать её страницу со сцены и
    // вернуть задником окна заставку — под стартовым экраном она, а не
    // бумага книги. Задел на будущее (кэш texture держит снимок) — второй
    // показ заставки идёт без загрузки.
    view.setActive(false);
    window.backgroundAsync(applicationFolder() / L"Assets/splash-screen-1k.png");

    // Большой кнопке — её книга: обложка, название, автор. На каждом
    // показе, потому что последняя открытая книга могла смениться, пока
    // экрана не было видно; при запуске реестр к этому моменту прочитан.
    if (const BookEntry* known = ws.library.find(ws.settings.lastBookGuid)) {
        screen.setContinueBook(known->title, known->authors,
                               known->cover.empty() ? std::filesystem::path{}
                                                    : ws.coverDirectory() / known->cover.wchars());
    }

    show(Screen::Start);
    window.content(screen.root());
    notices.flush();
}

void App::showLibrary() {
    closePanel();
    rememberPosition();   // и полка тут же покажет свежий процент

    // Как и на стартовом экране: полоса перестаёт быть текущим экраном —
    // её страница уходит со сцены, а задником окна снова заставка.
    view.setActive(false);
    window.backgroundAsync(applicationFolder() / L"Assets/splash-screen-1k.png");

    // Полка пересобирается на каждый показ: книга могла добавиться, а
    // место чтения — уехать с тех пор, как её видели в прошлый раз.
    shelf.show(ws.library);
    show(Screen::Library);
    window.content(shelf.root());
    notices.flush();

    // Карточки уже стоят; проценты проступят на них по мере того, как
    // рабочий поток прочитает файлы состояния — по одному на книгу.
    fillProgress(this, ws.library.books());
}

void App::openBook(std::filesystem::path const& path) {
    openBookFlow(this, path);
}

/// Заказ на прогрев: чью книгу ждём, записывается здесь и сразу — до того,
/// как чтение уйдёт к диску. По этой записи прогрев потом и узнаёт, нужен ли
/// ещё его результат.
void App::warmBook(std::filesystem::path const& path) {
    warm.expect(path);
    warmBookFlow(this, path);
}

void App::addBook() {
    std::filesystem::path const path = askForBook(window.handle());
    if (!path.empty()) openBook(path);
}

void App::addFolder() {
    std::filesystem::path const folder = askForFolder(window.handle());
    if (!folder.empty()) addFolderFlow(this, folder);
}

void App::persistLater() noexcept {
    viewTimer.stop();
    viewTimer.start();
}

/// Настройки вида — наблюдаемые поля settings: ползунки панели привязаны к
/// ним (Bind), полоса пишет в них с колеса и клавиш и сама их слушает,
/// галочка полки привязана к continueReading. Здесь остаётся одна запись на
/// диск — с паузой, как у места окна: ползунок шлёт перемену на каждый
/// сдвиг, файл переписывается целиком, а две записи одного файла не должны
/// идти одновременно — `write_all` пишет через один временный файл. Сами
/// значения уже в settings: закрытие окна пишет их своей записью, не
/// дожидаясь паузы.
///
/// Слушатели поля — noexcept по контракту observable: им некому отдать
/// исключение. Настройки держат слушателей сами, снимать их незачем: они
/// живут столько же, сколько App. Ставит их запуск (startupFlow) сразу
/// после чтения файла: прочитанное — не перемена, и писать его обратно
/// незачем. Слушатель лежит в поле App и берёт `this`, а не `AppPtr`:
/// держать им своего хозяина значило бы кольцо.
void App::watchSettings() {
    static_cast<void>(ws.settings.fontSize.on_change([this](double) noexcept { persistLater(); }));
    static_cast<void>(ws.settings.lineHeight.on_change([this](double) noexcept { persistLater(); }));
    static_cast<void>(ws.settings.margin.on_change([this](double) noexcept { persistLater(); }));
    static_cast<void>(
        ws.settings.continueReading.on_change([this](bool) noexcept { persistLater(); }));

    // Тема — поле полосы, а в настройках она лежит именем: обложка — своим,
    // встроенная тема — ключом; прежний ключ при обложке остаётся как то,
    // куда вернуться, если реестр обложек пропадёт. Слушатель переводит
    // номер в имена по списку тем и пишет файл, только если имена
    // изменились: запуск ставит ту же тему, что в файле.
    static_cast<void>(view.theme.on_change([this](int index) noexcept {
        ThemeList::Persisted names = themes.persist(index, ws.settings.theme);
        if (names.skin == ws.settings.skin && names.theme == ws.settings.theme) return;
        ws.settings.skin = std::move(names.skin);
        ws.settings.theme = std::move(names.theme);
        persistLater();
    }));
}

/// Предпросмотр: полоса рисуется со снимком и кривыми мастера. Он же —
/// пересчёт после отпускания точки.
void App::previewSkin() {
    view.setPreview(&wizard.skin(), wizard.imagePath());
}

void App::leaveWizard() {
    wizard.hide();
    view.setPreview(nullptr, {});
    view.root().focus(FocusState::Programmatic);
}

/// Клавиши, общие для всех экранов.
///
/// Полный экран по F11, выход из него по Escape. Перехват на пути вниз, а
/// не на всплытии: событие начинается у того, на чём фокус, и клавиша
/// должна работать независимо от того, на какой кнопке он сейчас стоит.
///
/// Что значит клавиша, решает карта клавиш (`resolve`, bukvitsa.lib): там
/// вся матрица — полоса, мастер, ползунок и поле ввода, приоритет Escape.
/// Здесь — только контекст для неё и исполнение ответа.
///
/// Флага «мы в полном экране» нет намеренно: он был бы вторым местом, где это
/// записано, — а состояние знает само окно (window.fullScreen()), и второй
/// его слепок разошёлся бы с первым.
void App::installKeys(UIElement const& element) {
    // Обработчик лежит в дереве экрана, экран — в App: `this`, не `AppPtr`.
    element.add_onPreviewKeyDown([this](Object const&, KeyRoutedEventArgs& args) {
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
        auto context = KeyContext{
            .shown = shown.get(),
            .cameFrom = bookCameFrom.get(),
            .wizardOpen = wizard.isOpen(),
            .panelOpen = panel.isOpen(),
            .noteOpen = true,
            .fullScreen = window.fullScreen(),
            .origin = focusOriginOf(args.originalSource()),
            .bookOpen = view.isOpen(),
        };

        Command resolved = resolve(press, context);
        if (resolved == Command::DismissNote && !view.dismissOverlays()) {
            context.noteOpen = false;
            resolved = resolve(press, context);
        }

        switch (resolved) {
            case Command::ShowLibrary:
            case Command::BackToLibrary:
                showLibrary();
                break;
            case Command::BackToStart:
                showStartScreen();
                break;
            case Command::OpenContents:
                panel.open(ReaderPanel::Tab::Contents);
                break;
            case Command::OpenSearch:
                panel.open(ReaderPanel::Tab::Search);
                break;
            case Command::OpenBookmarks:
                panel.open(ReaderPanel::Tab::Bookmarks);
                break;
            case Command::TogglePanel:
                panel.toggle();
                break;
            case Command::ClosePanel:
                panel.close();   // фокус полосе возвращает сама панель
                break;
            case Command::DismissNote:
                break;   // сноску закрыла полоса — больше ничего не нужно
            case Command::ToggleFullScreen:
                window.fullScreen(!window.fullScreen());
                break;
            case Command::LeaveFullScreen:
                window.fullScreen(false);
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
}

}  // namespace

wxl::Teardown wxl_launched() {
    // Теги разметки открыты только здесь, где она и пишется. На уровне файла
    // они накрыли бы обычные слова — text, size, state, entry — и под /W4
    // каждое такое имя, вплоть до параметров шаблонов самой wxl, стало бы
    // «прячущим глобальное» (C4459).
    using namespace wxl::dsl;

    // Своё окно верхнего уровня на композиторе, а не генерируемое wxl::Window:
    // у того верхнее окно перенаправляемое, и при быстрой растяжке за угол в
    // просвете белеет его GDI-поверхность, стёртая системной кистью. Здесь окно
    // с WS_EX_NOREDIRECTIONBITMAP — поверхности перенаправления нет вовсе, а
    // содержимое целиком даёт композитор. Окно — хендл: App держит его копию,
    // а само окно живёт до WM_NCDESTROY.
    auto const window = CompositionWindow{
        title = u"Буквица",
        minSize = SizeInt32{720, 520},
    };

    // Рамка окна тёмная, как и остров: системный светлый заголовок над тёмными
    // ящиками, мастером и полкой смотрелся бы чужим.
    window.darkFrame(true);

    // Заставка — задником сцены: единственный визуал под XAML-островом
    // оснастки. Он ресайзится синхронно в WM_SIZE, оттого держится за рамку без
    // отставания, и в просвете при быстрой растяжке видна заставка, а не белое.
    // Грузим асинхронно через Win2D/TextureCache, а не синхронным
    // background(path): тот рисует свою DrawingSurface отдельным
    // D3D-устройством, и на сцене-композиторе оно не уживается с устройством
    // Win2D того же кэша — DirectComposition падает (dcompi). Асинхронная
    // загрузка идёт тем же устройством Win2D и не падает.
    window.backgroundAsync(applicationFolder() / L"Assets/splash-screen-1k.png");

    // Всё, из чего собрано приложение, — один объект. Он рождается со счётчиком
    // 1, и `make_refcounted` эту ссылку принимает, а не добавляет свою: иначе
    // App не умер бы никогда. Обработчики ниже лежат в полях App и берут голый
    // `app`; сценарию `AppPtr` из него делает сам вызов.
    auto const keeper = make_refcounted<App>(window);
    App* const app = keeper.get();

    // ---- сохранение места чтения ----
    app->positionTimer.add_onTick([app](Object const&, Object const&) {
        app->positionTimer.stop();
        app->rememberPosition();
    });

    app->view.onPositionChanged = [app](uint32_t) {
        app->positionTimer.stop();
        app->positionTimer.start();
    };

    // ---- экраны ----
    app->screen.onContinueReading = [app] { continueReading(app); };
    app->screen.onAddBook = [app] { app->addBook(); };
    app->screen.onAddFolder = [app] { app->addFolder(); };
    app->screen.onLibrary = [app] { app->showLibrary(); };

    // Отмена стартового экрана — выход из приложения: обычное закрытие окна,
    // со всем, что оно сохраняет по дороге.
    app->screen.onExit = [app] { app->window.close(); };

    app->shelf.onAddBook = [app] { app->addBook(); };
    app->shelf.onBack = [app] { app->showStartScreen(); };
    app->shelf.onOpen = [app](u16_text guid) {
        if (BookEntry const* known = app->ws.library.find(guid)) app->openBook(known->path);
    };

    app->panel.onStateChanged = [app] {
        saveStateLater(app, app->ws.settings.lastBookGuid, app->state);
    };
    app->panel.onLibrary = [app] { app->showLibrary(); };

    // Правая кнопка по странице открывает ящик. Другой дороги к нему у мыши
    // нет: у страницы книги нет ни полосы меню, ни кнопок — и не должно быть.
    app->view.onPanelRequested = [app] { app->panel.toggle(); };

    // Снимок подложки полоса заказывает, а не читает: диск — только операциями wxl.
    app->view.onBackdropNeeded = [app](std::filesystem::path file) {
        loadBackdropFlow(app, std::move(file));
    };

    // Запись настроек вида — с паузой (watchSettings).
    app->viewTimer.add_onTick([app](Object const&, Object const&) {
        app->viewTimer.stop();
        saveSettingsLater(app);
    });

    // ---- мастер обложек ----
    app->panel.onAddSkin = [app] {
        std::filesystem::path const path = askForImage(app->window.handle());

        if (path.empty()) return;

        checkImageFlow(app, path, [app, path] {
            app->wizard.openNew(path);
            app->closePanel();
            app->wizard.show();
            app->previewSkin();
        });
    };

    app->panel.onEditSkin = [app](u16_text skinName) {
        // По полному списку тем, а не по реестру: системные обложки живут
        // только в нём, а шестерёнка есть и у них — правка «на основе».
        const std::optional<int> index = app->themes.indexOfSkin(skinName);
        const Skin* known = index ? app->themes.skinAt(*index) : nullptr;
        if (!known) return;   // список успел перемениться под руками

        // Копия, а не указатель: пока снимок читают, список тем может
        // перемениться, и указатель в него протухнет.
        const Skin skin = *known;

        const std::filesystem::path image = app->ws.skinImagePath(skin);

        checkImageFlow(app, image, [app, skin, image] {
            app->wizard.openEdit(skin, image);
            app->closePanel();
            app->wizard.show();
            app->previewSkin();
        });
    };

    // Спрашивает и удаляет сам сценарий: вопрос — диалог, которого ждут.
    app->panel.onDeleteSkin = [app](u16_text skinName) { deleteSkinFlow(app, std::move(skinName)); };

    app->wizard.onCurvesChanged = [app] { app->previewSkin(); };

    app->wizard.onChooseAnother = [app] {
        std::filesystem::path const path = askForImage(app->window.handle());

        // Отказался — остаёмся на прежнем снимке: читатель ничего не терял.
        if (path.empty()) return;

        checkImageFlow(app, path, [app, path] {
            app->wizard.openNew(path);
            app->previewSkin();
        });
    };

    app->wizard.onExit = [app] { app->leaveWizard(); };

    app->wizard.onSave = [app](Skin skin, std::filesystem::path photo) {
        saveSkinFlow(app, std::move(skin), std::move(photo));
    };

    // ---- клавиши, общие для всех экранов ----
    app->installKeys(app->screen.root());
    app->installKeys(app->shelf.root());
    app->installKeys(app->view.root());

    // Перетаскивание книги в окно. Регистрация цели идёт на HWND, который к
    // этому моменту уже есть -- окно создано, хотя ещё и не показано. Пачку из
    // нескольких файлов окно уже свело к первому пути: читалка показывает одну
    // книгу, а добавлять остальные в реестр молча значило бы решать за читателя.
    window.acceptFileDrops([app](std::filesystem::path path) { app->openBook(path); });

    // ---- сохранение места окна ----
    app->saveTimer.add_onTick([app](Object const&, Object const&) {
        app->saveTimer.stop();
        app->rememberWindow();
    });

    // Геометрия сменилась — двигали, растягивали, максимизировали или
    // восстановили: всё, что мы запоминаем. Окно сводит это в один колбэк.
    window.onGeometryChanged([app] {
        app->saveTimer.stop();   // каждое движение отодвигает запись
        app->saveTimer.start();
    });

    window.onClosed([app] {
        app->saveTimer.stop();
        app->positionTimer.stop();
        app->viewTimer.stop();   // окно уходит с полными настройками: rememberWindow пишет их
        app->rememberWindow();
        app->rememberPosition();
    });

    app->splashTimer.add_onTick([app](Object const&, Object const&) {
        app->splashTimer.stop();
        app->screen.reveal();
    });

    // ---- запуск ----
    //
    // Всё, что читалка знает о себе, читается отсюда и асинхронно: настройки,
    // реестр, книга, на которой остановились. Окно показывается в конце этой
    // цепочки — уже на своём месте и с уже выбранным экраном.
    startupFlow(keeper);

    // Захват держит App — экраны, модели и таймеры — живым, пока идёт
    // приложение: обработчики окна и экранов ссылаются на него голым
    // указателем. Окно в нём — поле App; сам обработчик окна не захватывает,
    // оно держит себя до WM_NCDESTROY. Пул STA не наш: его строит и держит
    // сама wxl в своей точке входа, и второй такой падает; App из пула
    // отпускается вместе с этим обработчиком, до разборки пула.
    //
    // Сценарии держат App сами (`AppPtr` в кадре), а петлю ввода-вывода wxl
    // гасит уже после этого обработчика, дождавшись операций в полёте.
    return [keeper](TeardownReason) {};
}
