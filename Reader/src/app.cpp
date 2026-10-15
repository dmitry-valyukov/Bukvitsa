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
// исключением `last_error`. Сценарий ловит то, на что у него есть ответ
// читателю; остальное всплывает в `on_detached_task_failure` (его ставит
// `Notices`): читатель видит окно с причиной, сценарий на этом кончается.
//
// Отмена — не сбой. Сценарий заводит свой источник отмены, дочерний от общего
// `stop_`, и ждёт под его токеном; закрытие окна отменяет общий, а с ним и все
// дочерние. Отменённое ожидание кончается `operation_canceled_exception`;
// непойманная, она кончает сценарий молча — `detached_task` её глотает, до
// обработчика сбоев она не доходит. Ловить её раньше надо только там, где
// сценарий ловит всё (`std::exception`) и говорит об этом читателю, — иначе
// отмена дошла бы до него как ошибка.
//
// Состояние книги, настройки и реестр книг пишут три цикла-корутины,
// `task<>`-члены writeState, writeSettings и writeLibrary, — по одному
// писателю на файл; рабочее место эти модели только меняет в памяти. Держит
// циклы приложение (`DeferredWrite`), а не промис: цикл живёт, пока живо
// приложение, и на выходе держатель докручивает его синхронно. Записи токена
// не берут и доходят до конца; сбой записи цикл говорит читателю и ждёт
// дальше — следующая перемена запишет снова.

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

using wxl::async::cancellation_source;
using wxl::async::cancellation_token;
using wxl::async::detached_task;
using wxl::async::last_error;
using wxl::async::task;

namespace {

// Каким окно открывается, когда запоминать ещё нечего.
constexpr int32_t kInitialWidth = 1280;
constexpr int32_t kInitialHeight = 860;

// Сколько заставка стоит одна, прежде чем на неё проступят кнопки. Не таймаут
// загрузки, а пауза ради самой заставки: приложению без книги нечего грузить,
// и без неё кнопки появились бы в тот же кадр, что и картинка.
constexpr auto kSplashHold = 1000ms;

// Пауза, после которой настройки вида и место окна попадают на диск. Окно
// таскают и ползунок двигают непрерывно, а писать на каждый пиксель незачем.
// У реестра книг она та же, но паузы он не ждёт: его будят только «сейчас».
constexpr auto kSaveQuiet = 800ms;

// То же для места чтения: страницы листают подряд, а файл на книгу один.
constexpr auto kPositionQuiet = 1500ms;

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
//
// Циклы записи стартуют здесь же и сразу встают на ожидание таймера: лямбда
// только отдаёт держателю корутину-член, а до пробуждения цикл полей не
// трогает.
App::App()
    : window_{CompositionWindow {
          dsl::title = u"Буквица",
          dsl::minSize = SizeInt32{720, 520},
          // Закрытие отменяет сценарии и пишет отложенное, пока окно ещё есть:
          // Teardown приходит уже без него, и места окна было бы не у кого
          // спросить.
          dsl::onClosed = method(this, &App::close),
      }},
      ws_{standardDataDirectory()},
      notices_{window_, screenTheme_},
      start_{window_.chromeCompositor(), *this},
      view_{window_, ws_.settings, themes_, *this, stop_.token()},
      shelf_{ws_.library.cards(), ws_.settings.continueReading, ws_.coverDirectory(), *this},
      wizard_{window_.chromeCompositor(), *this},
      panel_{window_.chromeCompositor(), view_, ws_.settings, places_, themes_, search_, *this},
      stateWrite_{window_.dispatcherQueue(), kPositionQuiet, stop_.token(),
                  [this](WaitableTimer& timer, cancellation_token stop) {
                      return writeState(timer, std::move(stop));
                  }},
      settingsWrite_{window_.dispatcherQueue(), kSaveQuiet, stop_.token(),
                     [this](WaitableTimer& timer, cancellation_token stop) {
                         return writeSettings(timer, std::move(stop));
                     }},
      libraryWrite_{window_.dispatcherQueue(), kSaveQuiet, stop_.token(),
                    [this](WaitableTimer& timer, cancellation_token stop) {
                        return writeLibrary(timer, std::move(stop));
                    }},
      splash_{window_.dispatcherQueue().createTimer()},
      // Место чтения — отложенно, как и место окна: перелистывание — самое
      // частое действие в читалке, а файл состояния книги переписывается
      // целиком.
      positionWatch_{view_.position().on_change(method(this, &App::positionChanged))},
      // Тема — с самого начала: за ней идёт подложка полосы, а имена в
      // настройках слушатель пишет, только если они изменились, — до чтения
      // настроек тему никто не меняет.
      themeWatch_{view_.theme.on_change(method(this, &App::themeChanged))} {
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

    // Предпросмотр идёт за правимой обложкой. Поле — своё, слушатель уходит
    // вместе с ним.
    static_cast<void>(wizard_.skin().on_change(method(this, &App::preview)));

    // Клавиши — на корнях трёх экранов, на пути вниз: что значит клавиша,
    // решает карта, исполняет onKey. Свой обработчик раньше этого — только у
    // заставки (Enter, Escape): она метит своё.
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

// Подписки снимаются в теле деструктора — оно идёт раньше, чем умирают поля.
// Слушатели будят циклы записи, а держатели циклов умирают раньше окна,
// полосы и настроек: перемена, пришедшая после них, будила бы мёртвое.
App::~App() {
    for (auto& [field, cookie] : flagWatches_) field->remove_change(cookie);
    for (auto& [field, cookie] : styleWatches_) field->remove_change(cookie);
    view_.theme.remove_change(themeWatch_);
    view_.position().remove_change(positionWatch_);
    if (geometryWatch_) window_.remove_onGeometryChanged(geometryWatch_);
}

/// Окно строится пустым и невидимым, а показывается в конце: место, куда его
/// поставить, лежит в настройках, и открыть его сначала посреди экрана, а
/// потом переставить — значит показать читателю прыжок. Ждать при этом нечего:
/// файл настроек читает рабочий поток, а этот тем временем уже крутит цикл
/// сообщений.
detached_task App::start() {
    auto const stop = cancellation_source{stop_.token()};

    // Настройки, обложки, реестр — у рабочего места, одной корутиной; что
    // при этом не удалось, читатель узнаёт словами, а запуск идёт дальше с
    // умолчаниями.
    const Started started = co_await ws_.start(stop.token());

    for (const Notice& notice : started.notices) notices_.post(notice);

    // Запись на диск начинает слушать поля настроек только теперь:
    // прочитанное из файла — не перемена, которую надо записать обратно.
    watchSettings();

    // Предзагрузка книги — сразу, как только стало известно, какая она: разбор
    // длится дольше всего остального запуска вместе взятого, и пусть он идёт,
    // пока читатель смотрит на заставку. Задача уходит в поле, а вызов
    // возвращается тут же, так что строки ниже её не ждут.
    //
    // Только когда книга не открывается сама: при continueReading её откроет
    // запуск ниже, и предзагрузка разобрала бы её дважды. Протухший путь
    // безвреден — задача кончится ошибкой чтения, и «Продолжить чтение» найдёт
    // книгу по guid в реестре и откроет обычной дорогой.
    if (!ws_.settings.continueReading.get() && !ws_.settings.lastBookPath.empty())
        preloadBook(ws_.settings.lastBookPath);

    // Обложки — раньше темы: выбранной темой может оказаться обложка, а её
    // индекс продолжает список за встроенными и без реестра не существует.
    themes_.setSkins(ws_.skins.list());

    // Обложка по имени, а нет такой — встроенная по ключу: номера считаются
    // по полному списку, вместе с системными обложками впереди реестровых.
    applySkins(themes_.indexFromSettings(ws_.settings.theme, ws_.settings.skin));

    window_.resize({kInitialWidth, kInitialHeight});

    // Размер по умолчанию ставится всегда, и лишь потом накрывается
    // запомненным. Иначе испорченная строка в настройках оставила бы окно
    // таким, каким его открыл WinUI: placement молча ничего не делает, когда
    // разбирать нечего, — и это правильно, но своё умолчание к тому моменту
    // должно быть уже на месте.
    if (!ws_.settings.windowPlacement.empty()) window_.placement(ws_.settings.windowPlacement);

    // Место окна запоминается с этих пор, когда окно стоит там, где его
    // оставили. Размер по умолчанию выше — перемена наша, а не читателя:
    // место, снятое с неё, затёрло бы в настройках запомненное прежде, чем
    // его поставят.
    geometryWatch_ = window_.add_onGeometryChanged(method(this, &App::windowMoved));

    // Место, где окно встало, — в настройки сразу, как всякая перемена: окно
    // могло подогнаться под сегодняшние мониторы, а испорченный файл
    // настроек этой записью и заменяется, о чём запуск уже сказал читателю.
    windowMoved();

    if (ws_.settings.continueReading.get() && !started.lastBook.empty()) {
        open(started.lastBook);
    } else {
        show(Screen::Start);
        splash_.start();
    }

    window_.activate();
}

void App::close() {
    // Место окна — у него самого, пока оно есть: после Closed спросить будет
    // не у кого.
    windowMoved();

    // Сначала отмена: сценарий, который ждёт под токеном, кончится на этом
    // ожидании и до записей за ним не дойдёт; запись, которая уже идёт,
    // токена не берёт и доходит до конца. С общим источником отменены и
    // источники циклов записи, так что «сейчас» следом будит каждый цикл на
    // последнюю запись: он просыпается внутри вызова, снимает состояние этого
    // мига и уходит писать. Отмена после «сейчас» стоила бы лишней записи:
    // проснувшийся до неё цикл записал бы и пошёл на круг ещё раз. Цикл,
    // который в этот миг пишет прежнее, допишет и запишет снова — уже
    // последний раз. Дожидаются циклов их держатели, умирая с приложением.
    stop_.cancel();
    stateWrite_.signal();
    settingsWrite_.signal();
    libraryWrite_.signal();
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
    // таймер сработает. Полка тут же покажет свежий процент. Записать сейчас —
    // то же ожидание цикла, разбуженное владельцем.
    panel_.close();
    stateWrite_.signal();

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

    // Полка не пересобирается: она привязана к карточкам реестра и уже знает
    // каждую книгу. А место чтения могло уехать с тех пор, как её видели в
    // прошлый раз, — проценты проступят на карточках по мере того, как рабочий
    // поток прочитает файлы состояния, по одному на книгу.
    shown_.set(Screen::Library);
    window_.content(shelf_.root());
    notices_.flush();

    fillProgress();
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

// Прежний ключ при обложке остаётся как то, куда вернуться, если реестр
// обложек пропадёт. Запуск ставит ту же тему, что в файле, — её писать
// незачем.
void App::themeChanged(int index) noexcept {
    ThemeList::Persisted names = themes_.persist(index, ws_.settings.theme);
    if (names.skin != ws_.settings.skin || names.theme != ws_.settings.theme) {
        ws_.settings.skin = std::move(names.skin);
        ws_.settings.theme = std::move(names.theme);
        settingsWrite_.restart();
    }

    showBackdrop();
}

void App::revealStart() {
    splash_.stop();
    start_.reveal();
}

/* ---------------- отложенные записи ---------------- */

/// Место чтения и закладки книги на полосе. Аргументы записи вычисляются в
/// момент вызова, до ожидания: «сейчас» будит ждущий цикл внутри своего
/// вызова, и место снимается тем же ходом. Книга закрыта — писать нечего:
/// место чтения принадлежит ей, а не окну, и ноль незанятой полосы стёр бы
/// то, что уже записано. Пишется под guid книги на полосе — его знают её
/// места (`BookPlaces::guid`), — а не последней книги из настроек: те при
/// открытии меняются раньше полосы.
task<> App::writeState(WaitableTimer& timer, cancellation_token stop) {
    for (;;) {
        co_await timer;

        // Отмена, пришедшая до записи, делает её последней. Пришедшая посреди
        // записи — ещё один круг: перемена или «сейчас», заставшие запись,
        // иначе не записались бы вовсе, а под отменённым токеном ожидание не
        // приостанавливается.
        bool const last = stop.is_canceled();

        if (view_.isOpen()) {
            // Путь — для слов читателю, и снят до записи: пока она идёт, на
            // полосу может встать другая книга.
            std::filesystem::path const book = view_.book()->path();

            try {
                co_await ws_.saveState(places_.guid(), places_.stateAt(view_.position().get()));
            } catch (const last_error& failure) {
                notices_.post(noticeOf(L"Не удалось сохранить место чтения", book.wstring(), failure));
            }
        }

        if (last) co_return;
    }
}

/// Настройки целиком: вид, тема, место окна. Текст рабочее место собирает в
/// момент вызова, до ожидания, — на диск уходит то, что в полях к этому часу.
task<> App::writeSettings(WaitableTimer& timer, cancellation_token stop) {
    for (;;) {
        co_await timer;

        // Отмена посреди записи — ещё один круг, как у места чтения.
        bool const last = stop.is_canceled();

        try {
            co_await ws_.saveSettings();
        } catch (const last_error& failure) {
            notices_.post(noticeOf(L"Не удалось сохранить настройки", ws_.settingsPath().wstring(), failure));
        }

        if (last) co_return;
    }
}

/// Реестр книг целиком. Текст рабочее место собирает в момент вызова, до
/// ожидания: «сейчас» будит ждущий цикл внутри своего вызова, и на диск уходит
/// реестр этого мига.
task<> App::writeLibrary(WaitableTimer& timer, cancellation_token stop) {
    for (;;) {
        co_await timer;

        // Отмена посреди записи — ещё один круг, как у места чтения.
        bool const last = stop.is_canceled();

        try {
            co_await ws_.saveLibrary();
        } catch (const last_error& failure) {
            notices_.post(noticeOf(L"Не удалось сохранить реестр книг", ws_.libraryPath().wstring(), failure));
        }

        if (last) co_return;
    }
}

/// Настройки вида — наблюдаемые поля: ползунки панели привязаны к ним (Bind),
/// полоса пишет в них с колеса и клавиш и сама их слушает, галочка полки
/// привязана к continueReading, флажок панели — к animateTurns. Здесь
/// остаётся одна запись на диск — с паузой: ползунок шлёт перемену на каждый
/// сдвиг, а файл переписывается целиком.
void App::watchSettings() {
    Settings& settings = ws_.settings;
    for (observable<double>* field : {&settings.fontSize, &settings.lineHeight, &settings.margin}) {
        styleWatches_.emplace_back(field, field->on_change(method(this, &App::styleChanged)));
    }
    for (observable<bool>* field : {&settings.continueReading, &settings.animateTurns}) {
        flagWatches_.emplace_back(field, field->on_change(method(this, &App::flagChanged)));
    }
}

/// Место отдаётся строкой WinRT; к нам она приходит чужим текстом и
/// проверенным становится так же, как любой другой чужой. Снимается оно в миг
/// перемены, а не в цикле записи: тот может проснуться и после Closed, когда
/// окна уже нет.
void App::windowMoved() {
    ws_.settings.windowPlacement = unicode::repaired(window_.placement());
    settingsWrite_.restart();
}

void App::positionChanged(uint32_t) noexcept {
    stateWrite_.restart();
}

void App::styleChanged(double) noexcept {
    settingsWrite_.restart();
}

void App::flagChanged(bool) noexcept {
    settingsWrite_.restart();
}

/* ---------------- клавиши ---------------- */

/// Полный экран по F11, выход из него по Escape. Перехват на пути вниз, а не
/// на всплытии: событие начинается у того, на чём фокус, и клавиша должна
/// работать независимо от того, на какой кнопке он сейчас стоит.
///
/// Что значит клавиша, решает карта клавиш (`resolve`, bukvitsa.lib): там вся
/// матрица — полоса, мастер, ползунок и поле ввода, приоритет Escape. Здесь —
/// только контекст для неё и исполнение ответа. Над книгой это единственный
/// обработчик клавиш у корня: листание, кегль, тему и сноску исполняет полоса
/// (perform), тем же методом, что её колесо и щелчок.
///
/// Флага «мы в полном экране» нет намеренно: он был бы вторым местом, где это
/// записано, — а состояние знает само окно (window_.fullScreen()). Так же и
/// мастер: открыт ли он, знает он сам, а не предпросмотр полосы.
void App::onKey(Object const&, KeyRoutedEventArgs& args) {
    if (args.handled()) return;   // заставка своё уже разобрала

    auto const context = KeyContext {
        .shown = shown_.get(),
        .cameFrom = cameFrom_.get(),
        .wizardOpen = wizard_.isOpen(),
        .panelOpen = panel_.isOpen(),
        .noteOpen = view_.noteOpen(),
        .fullScreen = window_.fullScreen(),
        .origin = focusOriginOf(args.originalSource()),
        .bookOpen = view_.isOpen(),
    };

    const Command command = resolve(keyPressOf(args.key()), context);
    switch (command) {
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
        case Command::ToggleFullScreen:
            window_.fullScreen(!window_.fullScreen());
            break;
        case Command::LeaveFullScreen:
            window_.fullScreen(false);
            break;

        // Полосы: листание, начало и конец книги, кегль, тема, сноска.
        case Command::TurnForward:
        case Command::TurnBackward:
        case Command::GoToStart:
        case Command::GoToEnd:
        case Command::FontLarger:
        case Command::FontSmaller:
        case Command::FontReset:
        case Command::NextTheme:
        case Command::DismissNote:
            view_.perform(command);
            break;

        case Command::None:
            return;   // не наша клавиша: пусть идёт дальше
    }
    args.handled(true);
}

}  // namespace bukvitsa::reader
