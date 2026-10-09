// Приложение: сборка, запуск, навигация между экранами и клавиши.
//
// Здесь нет ни wWinMain, ни поднятия Windows App Runtime, ни наследника
// Application, ни XAML: всё это делает wxl и потом зовёт wxl_launched
// (main.cpp), а та — этот конструктор и start().
//
// Сценарии исполняются в интерфейсном потоке и уходят с него ровно на
// `co_await` — на время, пока файл читается или пишется. Между двумя co_await
// код обычный: он трогает XAML и общее состояние, потому что он и есть тот
// самый поток. Reader диска не трогает вовсе: файлы читает и пишет рабочее
// место (`Workspace`) из своих корутин `task<T>`, сбой операции приходит
// исключением `system_exception`. Сценарий ловит то, на что у него есть ответ
// читателю; остальное всплывает в `on_detached_task_failure` (его ставит
// `Notices`): читатель видит окно с причиной, сценарий на этом кончается.

// Свои заголовки со стандартными внутри — до всего, что тянет import
// wxl.core: заголовок, включённый после импорта, MSVC уже не принимает.
#include "keys.h"

#include "ApplicationFolder.h"
#include "app.h"

// Импорт последним, после всех обычных заголовков.
import wxl.async;
import wxl.core;

namespace bukvitsa::reader {

using namespace wxl;
using namespace std::chrono_literals;

using wxl::async::detached_task;

namespace {

// Каким окно открывается, когда запоминать ещё нечего.
constexpr int32_t kInitialWidth = 1280;
constexpr int32_t kInitialHeight = 860;

// Сколько заставка стоит одна, прежде чем на неё проступят кнопки. Не таймаут
// загрузки, а пауза ради самой заставки: приложению без книги нечего грузить,
// и без неё кнопки появились бы в тот же кадр, что и картинка.
constexpr auto kSplashHold = 1000ms;

/// Задник окна под заставкой и полкой.
std::filesystem::path splashImage() {
    return applicationFolder() / L"Assets/splash-screen-1k.png";
}

}  // namespace

// Окно — тегами в скобках, первым полем: своё окно верхнего уровня на
// композиторе, а не генерируемое wxl::Window. У того верхнее окно
// перенаправляемое, и при быстрой растяжке за угол в просвете белеет его
// GDI-поверхность, стёртая системной кистью; здесь окно с
// WS_EX_NOREDIRECTIONBITMAP — поверхности перенаправления нет вовсе. Теги — с
// квалификатором `dsl::`: открытые директивой, они спрятали бы обычные слова
// (C4459), а в списке инициализации открыть их негде.
//
// Экранам их намерения отдаёт `*this`: базы `Actions` построены раньше полей,
// ссылка на них годна, а зовут их экраны не раньше, чем конструктор кончится.
App::App()
    : window_{CompositionWindow {
          dsl::title = u"Буквица",
          dsl::minSize = SizeInt32{720, 520},
          // Закрытие пишет отложенное, пока окно ещё есть: Teardown приходит
          // уже без него, и места окна было бы не у кого спросить.
          dsl::onClosed = method(this, &App::close),
      }},
      ws_{standardDataDirectory()},
      notices_{window_, screenTheme_},
      start_{window_.chromeCompositor(), *this},
      view_{window_, ws_.settings, *this},
      shelf_{ws_.library, ws_.settings.continueReading, ws_.coverDirectory(), *this},
      wizard_{window_.chromeCompositor(), *this},
      panel_{window_.chromeCompositor(), view_, ws_.settings, state_, themes_, search_, *this},
      autosave_{*this, window_, ws_, themes_, view_, state_},
      splash_{window_.dispatcherQueue().createTimer()} {
    // Рамка окна тёмная, как и остров: системный светлый заголовок над тёмными
    // ящиками, мастером и полкой смотрелся бы чужим. Тега для неё в wxl пока
    // нет.
    window_.darkFrame(true);

    // Заставка — задником сцены: единственный визуал под XAML-островом. Он
    // ресайзится синхронно в WM_SIZE, оттого держится за рамку без отставания.
    // Асинхронно через Win2D/TextureCache, а не синхронным background(path):
    // тот рисует свою DrawingSurface отдельным D3D-устройством, и на сцене
    // оно не уживается с устройством Win2D того же кэша — DirectComposition
    // падает (dcompi). Тега для асинхронного фона в wxl пока нет.
    window_.backgroundAsync(splashImage());

    // Тема диалогов — за показанным экраном: книга тёмная, полка и заставка
    // светлые, как их корни.
    screenTheme_.follow(shown_, [](Screen shown) noexcept {
        return shown == Screen::Book ? ElementTheme::Dark : ElementTheme::Light;
    });

    // Панель живёт поверх полосы набора: «поверх страницы» — это внутри
    // полосы, а не рядом с ней. Мастер обложек — оверлей поверх полосы и
    // панели: под сеткой мастера читатель видит свою страницу, изогнутую
    // редактируемыми кривыми.
    view_.addOverlay(panel_.root());
    view_.addOverlay(wizard_.root());

    // Подложка полосы идёт за темой, предпросмотр — за правимой обложкой.
    // Слушатель поля — noexcept по контракту observable, и метод ему
    // отдаётся лямбдой: обёртка method() noexcept не переносит. Поля — свои,
    // слушатели уходят вместе с ними.
    static_cast<void>(view_.theme.on_change([this](int const&) noexcept { showBackdrop(); }));
    static_cast<void>(wizard_.skin().on_change([this](Skin const&) noexcept { preview(); }));

    // Клавиши, общие для всех экранов, — на корнях трёх экранов, после их
    // собственных: полоса и заставка разбирают свои раньше и метят их.
    start_.root().add_onPreviewKeyDown(method(this, &App::onKey));
    shelf_.root().add_onPreviewKeyDown(method(this, &App::onKey));
    view_.root().add_onPreviewKeyDown(method(this, &App::onKey));

    // Перетаскивание книги в окно. Пачку из нескольких файлов окно уже свело
    // к первому пути: читалка показывает одну книгу, а добавлять остальные в
    // реестр молча значило бы решать за читателя. Тега у окна для этого нет.
    window_.acceptFileDrops(method(this, &App::open));

    splash_.interval(kSplashHold);
    splash_.isRepeating(false);
    splash_.add_onTick(method(this, &App::revealStart));
}

/// Окно строится пустым и невидимым, а показывается в конце: место, куда его
/// поставить, лежит в настройках, и открыть его сначала посреди экрана, а
/// потом переставить — значит показать читателю прыжок. Ждать при этом нечего:
/// файл настроек читает рабочий поток, а этот тем временем уже крутит цикл
/// сообщений.
detached_task App::start() {
    // Настройки, обложки, реестр — у рабочего места, одной корутиной; что
    // при этом не удалось, читатель узнаёт словами, а запуск идёт дальше с
    // умолчаниями.
    const Started started = co_await ws_.start();

    for (const Notice& notice : started.notices) notices_.post(notice);

    // Запись на диск начинает слушать поля настроек только теперь:
    // прочитанное из файла — не перемена, которую надо записать обратно.
    autosave_.watchSettings();

    // Прогрев книги — сразу, как только стало известно, какая она: разбор
    // длится дольше всего остального запуска вместе взятого. Заказ уходит и
    // возвращается тут же, так что строки ниже его не ждут.
    //
    // Только когда книга не открывается сама: при continueReading её откроет
    // запуск ниже, и греть значило бы разобрать её дважды. Протухший путь
    // безвреден — прогрев не прочитает файла и тихо кончится, а «Продолжить
    // чтение» найдёт книгу по guid в реестре и откроет обычной дорогой.
    if (!ws_.settings.continueReading.get() && !ws_.settings.lastBookPath.empty())
        warmBook(ws_.settings.lastBookPath);

    // Обложки — раньше темы: выбранной темой может оказаться обложка, а её
    // индекс продолжает список за встроенными и без реестра не существует.
    setSkins();

    // Обложка по имени, а нет такой — встроенная по ключу: номера считаются
    // по полному списку, вместе с системными обложками впереди реестровых.
    view_.setTheme(themes_.indexFromSettings(ws_.settings.theme, ws_.settings.skin));

    window_.resize({kInitialWidth, kInitialHeight});

    // Размер по умолчанию ставится всегда, и лишь потом накрывается
    // запомненным. Иначе испорченная строка в настройках оставила бы окно
    // таким, каким его открыл WinUI: placement молча ничего не делает, когда
    // разбирать нечего, — и это правильно, но своё умолчание к тому моменту
    // должно быть уже на месте.
    if (!ws_.settings.windowPlacement.empty()) window_.placement(ws_.settings.windowPlacement);

    if (ws_.settings.continueReading.get() && !started.lastBook.empty()) {
        open(started.lastBook);
    } else {
        show(Screen::Start);
        splash_.start();
    }

    window_.activate();
}

void App::close() {
    autosave_.flush();
}

/* ---------------- навигация ---------------- */

void App::show(Screen next) {
    if (next == Screen::Book) {
        // Страница уже сверстана и нарисована: меру полоса берёт у самого
        // окна (ClientSizeChanged), а не у экрана поверх него, и потому
        // сверстана всегда — и когда её остров ещё не показан, и при
        // автооткрытии на старте. Пустого листа читатель не увидит.
        cameFrom_.set(shown_.get());
        shown_.set(Screen::Book);

        // Полоса становится текущим экраном: показать её страницу на сцене и
        // увести задник окна с заставки на бумагу темы. Только потом — остров
        // ввода поверх.
        view_.setActive(true);
        window_.content(view_.root());
        notices_.flush();
        return;
    }

    // Уходя из книги, место чтения пишем сразу: отложенная запись ждёт паузы,
    // а читатель уже ушёл — и, может быть, закроет приложение раньше, чем
    // таймер сработает. Полка тут же покажет свежий процент.
    panel_.close();
    autosave_.saveState();

    // Полоса перестаёт быть текущим экраном: её страница уходит со сцены, а
    // задником окна снова заставка.
    view_.setActive(false);
    window_.backgroundAsync(splashImage());

    if (next == Screen::Start) {
        // Большой кнопке — её книга: обложка, название, автор. На каждом
        // показе, потому что последняя открытая книга могла смениться, пока
        // экрана не было видно; при запуске реестр к этому моменту прочитан.
        if (const BookEntry* known = ws_.library.find(ws_.settings.lastBookGuid)) {
            start_.setContinueBook(known->title, known->authors,
                                   known->cover.empty()
                                       ? std::filesystem::path{}
                                       : ws_.coverDirectory() / known->cover.wchars());
        }

        shown_.set(Screen::Start);
        window_.content(start_.root());
        notices_.flush();
        return;
    }

    // Полка пересобирается на каждый показ: книга могла добавиться, а место
    // чтения — уехать с тех пор, как её видели в прошлый раз.
    shelf_.show();
    shown_.set(Screen::Library);
    window_.content(shelf_.root());
    notices_.flush();

    // Карточки уже стоят; проценты проступят на них по мере того, как рабочий
    // поток прочитает файлы состояния — по одному на книгу.
    fillProgress(ws_.library.books());
}

detached_task App::showLibrary() {
    show(Screen::Library);
    co_return;
}

detached_task App::back() {
    show(Screen::Start);
    co_return;
}

// Отмена стартового экрана — выход из приложения: обычное закрытие окна, со
// всем, что оно сохраняет по дороге (close).
detached_task App::quit() {
    window_.close();
    co_return;
}

// Правая кнопка по странице открывает ящик. Другой дороги к нему у мыши нет:
// у страницы книги нет ни полосы меню, ни кнопок — и не должно быть.
detached_task App::togglePanel() {
    panel_.toggle();
    co_return;
}

void App::revealStart() {
    splash_.stop();
    start_.reveal();
}

/* ---------------- клавиши ---------------- */

/// Полный экран по F11, выход из него по Escape. Перехват на пути вниз, а не
/// на всплытии: событие начинается у того, на чём фокус, и клавиша должна
/// работать независимо от того, на какой кнопке он сейчас стоит.
///
/// Что значит клавиша, решает карта клавиш (`resolve`, bukvitsa.lib): там вся
/// матрица — полоса, мастер, ползунок и поле ввода, приоритет Escape. Здесь —
/// только контекст для неё и исполнение ответа.
///
/// Флага «мы в полном экране» нет намеренно: он был бы вторым местом, где это
/// записано, — а состояние знает само окно (window_.fullScreen()).
void App::onKey(Object const&, KeyRoutedEventArgs& args) {
    // Листание, кегль и тему полоса пока разбирает сама — её обработчик стоит
    // на том же корне раньше этого и помечает свои клавиши. Помеченная сюда
    // не доходит, и команд полосы приложение не исполняет; карта отвечает за
    // них так же, как полоса.
    if (args.handled()) return;   // полоса набора своё уже разобрала

    const KeyPress press = keyPressOf(args.key());

    // Открыта ли сноска, полоса пока не говорит: dismissOverlays()
    // спрашивает и закрывает разом. Поэтому сноска считается открытой, пока
    // полоса не ответит, что закрывать было нечего, — тогда клавиша
    // разбирается заново без неё. Спрашивает только Escape над книгой без
    // панели: прочие клавиши от сноски не зависят.
    auto context = KeyContext{
        .shown = shown_.get(),
        .cameFrom = cameFrom_.get(),
        .wizardOpen = wizard_.isOpen(),
        .panelOpen = panel_.isOpen(),
        .noteOpen = true,
        .fullScreen = window_.fullScreen(),
        .origin = focusOriginOf(args.originalSource()),
        .bookOpen = view_.isOpen(),
    };

    Command resolved = resolve(press, context);
    if (resolved == Command::DismissNote && !view_.dismissOverlays()) {
        context.noteOpen = false;
        resolved = resolve(press, context);
    }

    switch (resolved) {
        case Command::ShowLibrary:
        case Command::BackToLibrary:
            show(Screen::Library);
            break;
        case Command::BackToStart:
            show(Screen::Start);
            break;
        case Command::OpenContents:
            panel_.open(ReaderPanel::Tab::Contents);
            break;
        case Command::OpenSearch:
            panel_.open(ReaderPanel::Tab::Search);
            break;
        case Command::OpenBookmarks:
            panel_.open(ReaderPanel::Tab::Bookmarks);
            break;
        case Command::TogglePanel:
            panel_.toggle();
            break;
        case Command::ClosePanel:
            panel_.close();   // фокус полосе возвращает сама панель
            break;
        case Command::DismissNote:
            break;   // сноску закрыла полоса — больше ничего не нужно
        case Command::ToggleFullScreen:
            window_.fullScreen(!window_.fullScreen());
            break;
        case Command::LeaveFullScreen:
            window_.fullScreen(false);
            break;

        // Команды полосы — фаза 2: их исполнит приложение, когда полоса
        // отдаст свой обработчик карте. Сейчас они сюда не доходят (см.
        // выше), а дошедшую — не метим.
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
}

}  // namespace bukvitsa::reader
