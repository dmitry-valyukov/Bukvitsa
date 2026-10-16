#include <algorithm>
#include <cmath>
#include <filesystem>
#include <vector>

// dwrite.h первым: он приводит guiddef.h с DEFINE_GUID, без которого
// d2d1effects.h не раскрывает CLSID эффектов.
#include <dwrite.h>

#include <d2d1effects.h>

// Родной размер снимка подложки в пикселях — до создания поверхности,
// поэтому нужен настоящий интерфейс WIC, а не только его объявление.
#include <wincodec.h>

// Заголовки проекта после всех стандартных: они ведут к импорту модуля книги,
// а стандартный заголовок после импорта MSVC уже не принимает. Свой первым:
// он единственный тянет за собой стандартные заголовки, которых нет здесь, —
// и уже он ведёт к импорту модуля книги.
#include "book_view.h"
#include "leaf_turn_animation.h"

#include "bukvitsa/reader/imaging.h"

#include "bukvitsa/typography/block.h"
#include "bukvitsa/typography/glyph_painter.h"
#include "bukvitsa/typography/system_failure.h"

import wxl.fmt;

namespace bukvitsa::reader {

using namespace wxl;

namespace {

/// Размытие набора под конфигуратором изгиба, в DIP. Лёгкое, не туман:
/// тексту достаточно отступить на второй план, чтобы направляющие мастера
/// читались чётче, — но кривые укладывают по строкам, и строки должны
/// оставаться различимыми.
constexpr float kPreviewBlur = 1.5f;

/// Цвет тени темы с нужной прозрачностью — для Direct2D, которым рисуется
/// средник. Тени переворота красятся тем же тоном (LeafTurnAnimation).
D2D1_COLOR_F tintedF(const D2D1_COLOR_F& color, float alpha) {
    return {color.r, color.g, color.b, alpha};
}

/// Кнопка щелчка — словами карты: правая, левая или иная.
PointerButton buttonOf(PointerPoint const& point) {
    auto const buttons = point.properties();
    if (buttons.isRightButtonPressed()) return PointerButton::Right;
    if (buttons.isLeftButtonPressed()) return PointerButton::Left;
    return PointerButton::Other;
}

}  // namespace

BookView::BookView(const CompositionWindow& window, BookPresenter& presenter, Settings& settings,
                   async::cancellation_token parent)
    : window_(window),
      presenter_(presenter),
      settings_(settings),
      compositor_(window.compositor()),
      // Страницы разворота встают на сцену окна сразу, под всем, что на ней
      // будет; рисует в его растры полоса — то, что показывает поток страниц
      // ведущего.
      spreadView_(compositor_, window.contentVisual(),
                  [this](ID2D1DeviceContext* context) { paintSpread(context); }),
      // Контейнер листов — сразу, от композитора окна; buildTree() его одевает
      // и ставит на сцену над страницами.
      leafLayer_(compositor_.createContainerVisual()),
      // Всплывашка сноски — XAML-остров, её рисунок висит в дереве острова, а не
      // на сцене: ей нужен композитор острова, а не окна.
      note_(window.chromeCompositor()),
      stop_(std::move(parent)),
      readingScope_(stop_.token()),
      // Стратегия переворота — до первой меры: мера доводит до конца всё, что
      // летит, и спрашивает у стратегии, куда рисовать. Всё, что она читает, —
      // разворот, источники, настройки, поток страниц и тон бумаги ведущего —
      // объявлено или построено раньше неё.
      turnAnimation_(makeTurnAnimation()),
      // Анимация листания — настройка читателя, и меняется на ходу: флажок
      // панели привязан к тому же полю. Слушатель меняет стратегию и потому
      // подписывается, когда она уже есть: брось её создание, слушателя с
      // нашим this в поле, которое живёт дольше полосы, не будет. Снимается
      // он сам, умирая.
      animateTurnsWatch_(settings_.animateTurns, *this),
      // Мера полосы — у окна, не у острова. ClientSizeChanged приходит из
      // WM_SIZE, раньше вёрстки XAML, и несёт размер в пикселях вместе с
      // масштабом экрана — тем же числом, которым остров переводит DIP в
      // пиксели; переезд на монитор с другим DPI (WM_DPICHANGED) приходит сюда
      // же, новым масштабом. Остров полосы занимает окно целиком, так что это и
      // его размер, а спрашивать остров (SizeChanged, XamlRoot) не нужно — и
      // нельзя было до Loaded. Событие приходит из цикла сообщений, то есть не
      // раньше, чем полоса достроится.
      sizeWatch_(window_, *this),
      // Холстом ведущего полоса становится последним членом: всё, о чём
      // ведущий её попросит, к этой минуте построено.
      canvasAttachment_(presenter_.attachCanvas(*this)) {
    // Полоса ещё не экран: под источником чтения ждать нечему, пока
    // activate(true) не заведёт новый. Серия стратегии, заведённая под ним
    // выше, отменяется вместе с ним.
    readingScope_.cancel();

    buildTree();

    // Колонцифра — единственный текст в читалке, который рисуется не своей
    // вёрсткой: у него нет ни переносов, ни выключки по формату — одна строка,
    // выключенная по центру, — и ради неё заводить абзац незачем. До первой
    // меры: та уже рисует разворот. Отказ DirectWrite — остановка
    // (system_failure.h): полоса без колонцифры молчала бы о причине.
    typography::stopIfFailed(dwriteFactory()->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                                               DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                                               12.0f, L"ru-RU", &statusFormat_),
                             "BookView: не создался формат колонцифры");
    typography::stopIfFailed(statusFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER),
                             "BookView: колонцифре не задана выключка");
    typography::stopIfFailed(statusFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER),
                             "BookView: колонцифре не задано место по высоте");

    // Первый WM_SIZE окно пережило до нас: текущая мера снимается сразу. Мера
    // — уже вёрстка, и ведущий рисует ею через холст, под которым полоса
    // стоит с конца списка инициализации. Сбой в ней — остановка, как и в
    // мере, пришедшей событием: applySize — noexcept.
    applySize({window_.clientSize(), window_.rasterizationScale()});
}

void BookView::buildTree() {
    // Словарь синтаксиса нужен ровно здесь — и вносится ровно здесь: теги
    // называются как свойства (width, height, margin), и на уровне файла они
    // перекрыли бы одноимённые переменные во всём остальном коде.
    using namespace wxl::dsl;

    // Контейнер листов переворотов — над страницами. Клип контейнера не
    // даёт повёрнутому листу вылезти за полосу: клип живёт в координатах самого
    // визуала и применяется до его преобразования, поэтому повёрнутый лист
    // режется по прямоугольнику страницы, а не по описанному вокруг него. Сами
    // листы заводит по требованию стратегия переворота — при быстром листании
    // их в воздухе несколько разом. Страницы под ним уже на сцене — их
    // поставил разворот (spreadView_).
    leafLayer_.clip(compositor_.createInsetClip());

    // Листы переворотов — на сцене окна над страницами, привешены к
    // contentVisual() (над задником, под островом), а не всунуты в дерево XAML
    // через setElementChildVisual. Пока полоса не стала текущим экраном, её
    // сцена скрыта — activate(true) покажет при входе.
    window_.contentVisual().children().insertAtTop(leafLayer_);
    leafLayer_.isVisible(false);

    // Слой страницы — прозрачный, во всю полосу, первым ребёнком корня, под
    // сноской и оверлеями: указатель над ним — над самой страницей. Наведение и
    // курсор — у него, а не у корня: курсор предка WinUI показывает над всеми
    // потомками без своего, и рука корня легла бы на панель, мастер и поля
    // ввода в них, где нужен их собственный. Над сноской, панелью и мастером
    // указатель не над слоем — они лежат выше него, а не в нём, — так что
    // спрашивать, откуда пришло движение, не нужно: оно приходит только со
    // страницы. Щелчок и колесо со слоя всплывают к корню, как и прежде.
    Apply {
        pageLayer_,
        // Кисть прозрачная, но она есть: без кисти Grid в проверке попадания
        // не участвует, и движения над страницей не было бы вовсе.
        background = colors.transparent,

        // Стрелка с самого начала, а не пустой курсор («без перемены»): форма
        // слоя всегда одна из двух известных, а меняется только при смене
        // (updateCursor).
        cursor = InputSystemCursorShape::Arrow,

        // Указатель над знаком сноски — рукой, как над ссылкой: щелчок по
        // знаку откроет сноску, и это видно до щелчка.
        onPointerMoved = method(this, &BookView::pointerMoved),
        onPointerExited = method(this, &BookView::pointerExited),
    };

    Apply {
        root_,
        // Корень берёт фокус на себя: событие клавиши начинается у того, на
        // чём фокус, и пока фокуса нет ни на чём, ловить нечего.
        isTabStop = true,

        // Остров — в родной тёмной теме WinUI, при любой теме бумаги: панель,
        // мастер и сноска над страницей тёмные, и контролы в них — ползунки,
        // поле поиска, подсказки — должны быть той же породы. Тема бумаги
        // красит только полосу.
        requestedTheme = ElementTheme::Dark,

        // Кисть прозрачная: страница на сцене под этим островом, и сквозь него
        // должна быть видна она — бумагу вместе с набором несут
        // страницы-визуалы на сцене, а задник окна в чтении без кисти вовсе
        // (см. activate). Попадание по странице ловит слой страницы во всю
        // полосу, а щелчок и колесо с него всплывают сюда; самому корню кисть
        // для этого уже не нужна.
        background = colors.transparent,

        pageLayer_,
        note_.root(),

        onLoaded = method(this, &BookView::loaded),

        // Клавиш здесь нет: их разбирает приложение у этого же корня, на
        // пути вниз, по карте клавиш, а команды полосы отдаёт ведущему
        // (perform). Колесо и щелчок полоса ловит сама и отдаёт ведущему:
        // щелчку нужна сноска, которой карта не знает.
        onPointerWheelChanged = method(this, &BookView::wheelTurned),
        onPointerPressed = method(this, &BookView::pressed),
    };
}

void BookView::loaded(Grid const& self) {
    self.focus(FocusState::Programmatic);
}

void BookView::wheelTurned(Object const&, PointerRoutedEventArgs& args) {
    const int wheelDelta = args.getCurrentPoint(root_).properties().mouseWheelDelta();
    const bool controlHeld = (static_cast<uint32_t>(args.keyModifiers()) &
                              static_cast<uint32_t>(VirtualKeyModifiers::Control)) != 0;

    if (presenter_.wheel(wheelDelta, controlHeld)) args.handled(true);
}

void BookView::pressed(Object const&, PointerRoutedEventArgs& args) {
    const PointerPoint touchPoint = args.getCurrentPoint(root_);
    const Point touchPosition = touchPoint.position();

    // Поверх полосы лежит мастер: сюда доходят только щелчки мимо точек
    // сетки, и полосе из них остаётся листание (его решает ведущий) — ни
    // фокуса, ни пометки. Без мастера щелчок — полосы: корень берёт фокус —
    // клавиша начинается у элемента в фокусе, и без него полоса клавиш не
    // слышит, — а событие дальше не идёт.
    if (!presenter_.previewing()) {
        root_.focus(FocusState::Programmatic);
        args.handled(true);
    }
    presenter_.tap(layoutPointOf(PageWarp::Point{touchPosition.x, touchPosition.y}),
                   buttonOf(touchPoint));
}

void BookView::pointerMoved(Object const&, PointerRoutedEventArgs& args) {
    // Слой во всю полосу, так что его точка — та же, что у корня и у потока
    // страниц; в вёрстку её переводят там, где спрашивают ведущего.
    const Point hoverPosition = args.getCurrentPoint(pageLayer_).position();
    hoverPoint_ = PageWarp::Point{hoverPosition.x, hoverPosition.y};
    updateCursor();
}

void BookView::pointerExited(Object const&, PointerRoutedEventArgs&) {
    // Указатель ушёл со страницы — за край полосы или на сноску, панель,
    // мастер: точки над страницей больше нет.
    hoverPoint_.reset();
    updateCursor();
}

void BookView::updateCursor() {
    const InputSystemCursorShape hoverShape =
        hoverPoint_ && presenter_.pointsAtNoteMark(layoutPointOf(*hoverPoint_))
            ? InputSystemCursorShape::Hand
            : InputSystemCursorShape::Arrow;
    if (hoverShape == cursorShape_) return;

    cursorShape_ = hoverShape;
    pageLayer_.cursor(hoverShape);
}

PageWarp::Size BookView::stripSize() const {
    const PageFlow& flow = presenter_.flow();
    return PageWarp::Size{flow.width(), flow.height()};
}

PageFlow::Point BookView::layoutPointOf(PageWarp::Point screenPoint) const {
    auto const layoutPoint =
        pageWarp_ ? pageWarp_->toLayout(screenPoint, stripSize()) : screenPoint;
    return PageFlow::Point{layoutPoint.x, layoutPoint.y};
}

BookView::~BookView() {
    // Ожидания полосы — концы пакетов переворотов и паузы отпускания пула —
    // ей больше не нужны: отменяет их владелец, то есть она сама. Корутины
    // кончатся отменой на своём ожидании и ни полосы, ни стратегии уже не
    // тронут. Холст уходит из-под ведущего и подписки снимаются сами —
    // первыми из членов.
    stop_.cancel();
}

void BookView::addOverlay(const UIElement& element) {
    root_.children().append(element);
}

/* ---------------- холст ведущего ---------------- */

void BookView::turnStarting(bool forward) {
    turnAnimation_->turnStarting(forward);
}

void BookView::turnStarted(bool forward) {
    turnAnimation_->turnStarted(forward);
}

void BookView::finishTurns() {
    turnAnimation_->finishAll();
}

void BookView::activate(bool entersReading) {
    // Показаны страницы или нет, решается первым: при уходе посадка листа их
    // уже не одевает, при входе их одевает посадка и redraw.
    spreadView_.show(entersReading);

    // Страница в чтении — две страницы-визуала на сцене окна (spreadView_), а
    // задник окна без кисти: страницы кроют его целиком, и красить под ними
    // нечего. Входя, снимаем кисть и показываем страницы тем же кадром, что
    // redraw (его зовёт ведущий следом) рисует разворот и одевает их. Уходя —
    // наоборот: страницы прячутся, а задник на промежуток до фона следующего
    // экрана (заставка приходит асинхронно) берёт осевший разворот сам — иначе
    // окно сквозило бы на стол.
    if (entersReading) {
        // Всё, что летит, — в конечное состояние первым делом, тем же порядком,
        // что и при уходе: сперва посадка, потом источник чтения. Уход уже
        // посадил листы, и здесь это страховка: полоса могла быть экраном и
        // раньше (книгу открыли поверх), а новой серии нужен покой.
        turnAnimation_->finishAll();

        // Новое чтение — новый источник: прежний отменён уходом, а если полоса
        // уже была экраном, отменяется здесь; отменённый источник не
        // переиспользуется. Серии под прежним уже нет — её кончил finishAll.
        readingScope_.cancel();
        readingScope_ = async::cancellation_source{stop_.token()};

        window_.clearBackground();
    } else {
        // Уход из чтения: всё, что летит, садится сейчас, синхронно — листам
        // незачем лежать на скрытом слое с последними кадрами и выражениями
        // подъёма, которые композитор считал бы до возврата. Осевшим
        // становится разворот самого нового; страницы уже скрыты, и посадка их
        // не одевает — их оденет redraw при входе. Посадка же начинает заново
        // долгую паузу отпускания пула: оно живёт не под источником чтения и
        // отпустит листы и растры, пока читатель на полке. Потом отменяется
        // источник чтения: ожидания концов переворотов кончатся ходом очереди,
        // а корутины ничего не тронут.
        turnAnimation_->finishAll();
        readingScope_.cancel();

        // Задник берёт осевший растр — после посадки нынешний разворот живёт
        // в нём.
        if (spreadView_.measured())
            window_.background(spreadView_.raster(turnAnimation_->drawTarget()));

        // Страницы под указателем больше нет; вернувшись в чтение, полоса
        // узнает его точку с первым движением.
        hoverPoint_.reset();
        updateCursor();
    }

    // Листы в покое не нужны: разворот видно страницами. Они выходят на сцену
    // только на время переворота — показывает их стратегия, когда пускает
    // лист, и она же прячет обратно, когда сели все.
    leafLayer_.isVisible(false);
}

void BookView::restyle() {
    turnAnimation_->tintShadows(presenter_.paper().shadow);   // тени покрашены прежней бумагой

    // Карта держит форму прежних кривых — новому виду она не годится.
    warpMap_.Reset();
    warpFlat_ = false;

    // Слой изгиба нужен только странице со снимком, а весит как две полосы —
    // на ровных темах он отпускается вместе со снимком. Контекст и эффекты
    // мелкие и остаются. Снимок обложки отпускает и приносит владелец: ему
    // видно, тот же это файл или другой (две обложки могут делить снимок).
    if (!hasBackdrop()) {
        warpLayer_.Reset();
        warpPixels_ = {};
        dropBackdrop();
    }
}

bool BookView::showNote(const PageFlow::NoteHit& noteHit) {
    const PageFlow& flow = presenter_.flow();

    // Якорь — в координатах вёрстки, а всплывашка встаёт под знаком там, где
    // он нарисован: под изогнутой обложкой у корешка это десятки DIP разницы.
    auto const layoutAnchor = PageWarp::Point{noteHit.anchor.x, noteHit.anchor.y};
    auto const screenAnchor =
        pageWarp_ ? pageWarp_->toScreen(layoutAnchor, stripSize()) : layoutAnchor;

    note_.show(*flow.book(), noteHit.body, Point{screenAnchor.x, screenAnchor.y},
               {flow.width(), flow.height()}, presenter_.paper(), flow.fontSize(),
               presenter_.scale());
    return note_.visible();
}

void BookView::hideNote() {
    note_.hide();
}

void BookView::redraw() noexcept {
    const PageFlow& flow = presenter_.flow();
    if (!spreadView_.measured() || flow.width() <= 0.0f || flow.height() <= 0.0f) return;

    // Разворот собран потоком страниц, когда он менялся (перевёрстка, прыжок,
    // переворот): рисование и попадание по сноске читают уже готовый.
    //
    // Посреди книжного переворота нынешний разворот несёт самый новый лист, а
    // не осевший: рисуем в его растр — перелистываемая страница и приходящий
    // лист смотрят на него и обновятся сами, — а неперелистываемую не трогаем:
    // ей до посадки листа положено старое. Иначе глава, досчитанная в полёте
    // («из …» стало «из M»), одевала бы обе страницы в новое, и летящий лист
    // пропадал бы на глазах — на тех же точках под ним. Разворот одевает
    // страницы, только когда рисует в осевший растр (SpreadView::redraw).
    spreadView_.redraw(turnAnimation_->drawTarget());

    // Разворот мог смениться под неподвижной мышью — переворот с клавиш,
    // прыжок, перевёрстка, мастер поверх: указатель — по новому.
    updateCursor();
}

/* ---------------- своё ---------------- */

void BookView::animateTurnsChanged(bool) noexcept {
    // Сменить стратегию посреди переворота — значит сперва довести до конца
    // всё, что летит: страницы, осевший разворот и задник после этого — те же,
    // что после любой посадки, и новой стратегии достаётся покой. Листы старой
    // уходят со сцены, а её свободные растры — из пула: без анимации растр
    // нужен один, а листы заведут свои, когда понадобятся.
    turnAnimation_->finishAll();
    leafLayer_.children().removeAll();
    turnAnimation_ = makeTurnAnimation();
    spreadView_.dropSpareRasters();
}

std::unique_ptr<TurnAnimation> BookView::makeTurnAnimation() {
    std::unique_ptr<TurnAnimation> chosenAnimation;
    if (settings_.animateTurns.get()) {
        chosenAnimation = std::make_unique<LeafTurnAnimation>(
            compositor_, leafLayer_, window_.dispatcherQueue(), spreadView_, presenter_.flow(),
            readingScope_, stop_.token());
    } else {
        chosenAnimation = std::make_unique<InstantTurnAnimation>(spreadView_);
    }

    // Тени переворота — тоном нынешней бумаги.
    chosenAnimation->tintShadows(presenter_.paper().shadow);
    return chosenAnimation;
}

void BookView::applySize(const ClientSize& client) noexcept {
    const SizeInt32 pixels = client.size;
    const float scale = client.scale > 0.0f ? client.scale : 1.0f;

    // Пиксели — мера; DIP, в которых верстается и рисуется полоса, — их доля.
    // Та же мера — ни растров, ни вёрстки: событие приходит и без перемены.
    const float width = static_cast<float>(pixels.width) / scale;
    const float height = static_cast<float>(pixels.height) / scale;
    const PageFlow& flow = presenter_.flow();
    if (width == flow.width() && height == flow.height() && scale == presenter_.scale() &&
        spreadView_.measured())
        return;

    // Растры разворота меняют размер, а не пересоздаются: кисти, которые их
    // уже показывают (фон окна, страницы, листы), продолжают показывать их же.
    // Листы мерит сама стратегия — на каждый переворот. Раньше перевёрстки:
    // ведущий рисует ею в растры новой меры.
    if (pixels.width > 0 && pixels.height > 0) {
        spreadView_.resize(pixels);
        leafLayer_.size({width, height});
    }

    // Пустая мера — окно свёрнуто или ещё не показано: рисовать некуда, и
    // изгиба нарисованной страницы больше нет — переводить точку указателя не
    // в чем.
    if (width <= 0.0f || height <= 0.0f) pageWarp_.reset();

    // Размер в DIP и масштаб — ведущему: поток страниц берёт размер, и полоса
    // перевёрстывается, доведя до конца всё, что летит. Гистерезис колонок —
    // его, по этой дороге и только по ней.
    presenter_.resize(width, height, scale);
}

void BookView::paintSpread(ID2D1DeviceContext* context) {
    const PageFlow& flow = presenter_.flow();
    const float scale = presenter_.scale();

    // Поверхность в пикселях, а вёрстка в DIP: масштаб домножается к тому
    // смещению атласа, которое wxl уже поставила, — иначе поверхность легла бы
    // поверх чужой.
    D2D1_MATRIX_3X2_F atlas{};
    context->GetTransform(&atlas);
    context->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale) *
                          *D2D1::Matrix3x2F::ReinterpretBaseType(&atlas));

    // Классический ClearType здесь не годится: он рассчитан на неподвижный
    // текст на непрозрачном фоне и рассыпается на цветную бахрому при
    // повороте, дробном масштабе и на OLED. Серая сглаженность с
    // субпиксельным позиционированием выглядит одинаково везде.
    context->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    context->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    if (!flow.spread().pages.empty()) {
        drawPage(context, flow.width(), flow.height());
    } else {
        pageWarp_.reset();   // приглашение не гнётся
        drawInvitation(context, flow.width(), flow.height());
    }
}

void BookView::dropBackdrop() {
    // Кэш битмапа устройства заведёт заново drawThemeBackdrop, вкомпоновывая
    // фото прямо в поверхность страницы.
    backdropSource_.Reset();
    photoBitmap_.Reset();
}

void BookView::setBackdrop(std::string bytes) {
    // Нечего ни отпускать, ни показывать — и перерисовывать незачем.
    if (bytes.empty() && !backdropSource_) return;

    // Раскодируется сразу и целиком: байты после этого не нужны, а битые
    // данные — бумага темы сейчас, а не отказ на каждом кадре.
    dropBackdrop();
    if (!bytes.empty()) backdropSource_ = decodeImage(bytes);
    redraw();
}

bool BookView::ensureWarp(ID2D1DeviceContext* context) {
    const PageFlow& flow = presenter_.flow();
    const float scale = presenter_.scale();

    const D2D1_SIZE_U pixels{static_cast<UINT32>(flow.width() * scale + 0.5f),
                             static_cast<UINT32>(flow.height() * scale + 0.5f) * 2};
    if (pixels.width == 0 || pixels.height == 0) return false;

    // Слой двойной высоты, которого устройство не держит одним битмапом, —
    // предел устройства, известный заранее, а не сбой: на Windows 10 бывают
    // устройства уровня 9.x с пределом 2048–4096 пикселей. Страница тогда
    // рисуется без изгиба; прежний слой — другой меры — отпускается, чтобы
    // ни изгиб, ни предпросмотр под конфигуратором на нём не рисовали.
    if (const UINT32 deviceLimit = context->GetMaximumBitmapSize();
        pixels.width > deviceLimit || pixels.height > deviceLimit) {
        warpLayer_.Reset();
        warpMap_.Reset();
        warpFlat_ = false;
        return false;
    }

    if (!warpLayer_ || warpPixels_.width != pixels.width || warpPixels_.height != pixels.height) {
        if (!warpContext_) {
            Microsoft::WRL::ComPtr<ID2D1Device> device;
            context->GetDevice(&device);
            typography::stopIfFailed(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &warpContext_),
                                     "BookView: не создался контекст слоя изгиба");
            // Те же режимы, что redraw() ставит поверхности: слой — та же
            // страница, только в другой битмап.
            warpContext_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            warpContext_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        }

        warpLayer_.Reset();
        warpMap_.Reset();
        warpFlat_ = false;

        const D2D1_BITMAP_PROPERTIES1 layerProps{
            {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED},
            96.0f, 96.0f, D2D1_BITMAP_OPTIONS_TARGET, nullptr};
        typography::stopIfFailed(warpContext_->CreateBitmap(pixels, nullptr, 0, &layerProps, &warpLayer_),
                                 "BookView: не создался слой изгиба");
        warpPixels_ = pixels;
    }

    // Эффекты — раньше проверки плоскости: плоскому предпросмотру под
    // конфигуратором ужатие нужно тоже — его слой идёт мимо смещения, но
    // через ту же вертикальную двойку. Подключение входов здесь не делается:
    // кто за кем стоит в цепочке, каждый кадр решает drawPage().
    if (!warpDisplace_) {
        typography::stopIfFailed(warpContext_->CreateEffect(CLSID_D2D1DisplacementMap, &warpDisplace_),
                                 "BookView: не создалось смещение слоя изгиба");
        typography::stopIfFailed(warpContext_->CreateEffect(CLSID_D2D1Scale, &warpShrink_),
                                 "BookView: не создалось ужатие слоя изгиба");
        typography::stopIfFailed(
            warpDisplace_->SetValue(D2D1_DISPLACEMENTMAP_PROP_X_CHANNEL_SELECT, D2D1_CHANNEL_SELECTOR_R),
            "BookView: смещению не задан канал X");
        typography::stopIfFailed(
            warpDisplace_->SetValue(D2D1_DISPLACEMENTMAP_PROP_Y_CHANNEL_SELECT, D2D1_CHANNEL_SELECTOR_G),
            "BookView: смещению не задан канал Y");
        typography::stopIfFailed(warpShrink_->SetValue(D2D1_SCALE_PROP_SCALE, D2D1::Vector2F(1.0f, 0.5f)),
                                 "BookView: ужатию не задан масштаб");
        typography::stopIfFailed(warpShrink_->SetValue(D2D1_SCALE_PROP_INTERPOLATION_MODE,
                                                       D2D1_SCALE_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC),
                                 "BookView: ужатию не задана интерполяция");
    }

    // Обложка с нетронутыми точками — плоская: гнуть нечего, полоса рисуется
    // напрямую. Ответ запомнен, чтобы не пересчитывать кривые на каждый кадр;
    // сбрасывают его смена темы, реестра обложек и размера полосы.
    if (warpFlat_) return false;

    if (!warpMap_) {
        // Чьи кривые гнут полосу: предпросмотр мастера или выбранная обложка
        // (BookPresenter::pageSkin).
        // Формулы здесь больше нет. Купол синуса, стоявший за «Антиквариат»,
        // был одинаков для любого снимка, а у настоящей фотографии каждая из
        // четырёх границ своя — и «Антиквариат» теперь описан точками, как
        // всякая другая обложка, только описание лежит в коде (skins.h).
        const Skin* skin = presenter_.pageSkin();

        // Выбрана тема, а не обложка: бумаги на экране нет, гнуть нечего.
        if (!skin) {
            warpFlat_ = true;
            return false;
        }

        // Сдвиг по кромкам обложки — математика изгиба (PageWarp): ею же
        // полоса переводит точку указателя, так что знак под указателем — тот,
        // что нарисован. Обложка с нетронутыми точками — плоская: гнуть нечего.
        auto const warp = PageWarp{*skin};
        if (warp.flat()) {
            warpFlat_ = true;
            return false;
        }

        // Отклонения кромок от их прямых начальных линий, в долях высоты
        // полосы, — по столбцу карты, один раз на столбец, а не на каждый
        // пиксель. У каждой границы каждого листа изгиб свой; какой лист под
        // столбцом, кривая решает сама — по своей точке корешка.
        const int columns = static_cast<int>(pixels.width);
        std::vector<PageWarp::Column> edges(static_cast<size_t>(columns));
        for (int x = 0; x < columns; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(columns);
            edges[static_cast<size_t>(x)] = warp.column(u);
        }

        // Изгиб есть, но меньше, чем видно на столбцах карты, — гнуть тоже
        // нечего, а размах ушёл бы в знаменатель.
        float amplitude = 0.0f;
        for (const PageWarp::Column& edge : edges) {
            amplitude = std::max({amplitude, std::abs(edge.top), std::abs(edge.bottom)});
        }
        if (amplitude <= 0.0f) {
            warpFlat_ = true;
            return false;
        }
        warpAmplitude_ = amplitude;

        // Карта хранит чистую форму изгиба: канал G — доля сдвига по Y от
        // размаха эффекта, 128 — «не смещать». Сдвиг отсчитан в точке экрана и
        // обратный (эффект читает «откуда взять», а не «куда сдвинуть» —
        // PageWarp::shift), поэтому у строк, уезжающих вверх, в карте стоит
        // плюс — взять снизу.
        std::vector<uint8_t> bytes(size_t{pixels.width} * pixels.height * 4);
        for (UINT32 y = 0; y < pixels.height; ++y) {
            const float down = (static_cast<float>(y) + 0.5f) / static_cast<float>(pixels.height);
            uint8_t* row = &bytes[size_t{y} * pixels.width * 4];
            for (UINT32 x = 0; x < pixels.width; ++x) {
                const float shift = edges[x].at(down);
                uint8_t* px = row + size_t{x} * 4;
                px[0] = 128;   // B — не читается
                px[1] = static_cast<uint8_t>(127.5f * (1.0f - shift / amplitude) + 0.5f);
                px[2] = 128;   // R — канал X, нейтрально
                px[3] = 255;
            }
        }
        const D2D1_BITMAP_PROPERTIES1 mapProps{
            {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED},
            96.0f, 96.0f, D2D1_BITMAP_OPTIONS_NONE, nullptr};
        typography::stopIfFailed(
            warpContext_->CreateBitmap(pixels, bytes.data(), pixels.width * 4, &mapProps, &warpMap_),
            "BookView: не создалась карта изгиба");
    }

    warpDisplace_->SetInput(0, warpLayer_.Get());
    warpDisplace_->SetInput(1, warpMap_.Get());
    // Размах — в пикселях слоя: наибольшее отклонение краёв в долях высоты —
    // это warpAmplitude_·высота·масштаб пикселей поверхности и вдвое больше в
    // слое двойной высоты; ещё двойка — потому что карта отклоняется от
    // середины не дальше половины размаха.
    typography::stopIfFailed(warpDisplace_->SetValue(D2D1_DISPLACEMENTMAP_PROP_SCALE,
                                                     4.0f * warpAmplitude_ * flow.height() * scale),
                             "BookView: смещению не задан размах");
    return true;
}

void BookView::ensureBlur(not_null<ID2D1DeviceContext> layerContext) {
    if (warpBlur_) return;

    // HARD вместо мягкой кромки по умолчанию: у границ полосы размытие не
    // должно подмешивать прозрачность из-за края слоя.
    typography::stopIfFailed(layerContext->CreateEffect(CLSID_D2D1GaussianBlur, &warpBlur_),
                             "BookView: не создалось размытие предпросмотра");
    typography::stopIfFailed(warpBlur_->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD),
                             "BookView: размытию не задана кромка");
}

void BookView::drawThemeBackdrop(ID2D1DeviceContext* context, float width, float height) {
    if (!backdropSource_) return;

    // Битмап устройства заводится раз на снимок и держится: CreateBitmapFromWicBitmap
    // на каждый кадр стоил бы дорого, а перерисовка бывает лишь на листании и
    // растяжке. Устройство поверхности стабильно, пока не потеряно. Снимок
    // больше предела устройства уменьшается до него, а отказ Direct2D —
    // остановка (deviceBitmapOf).
    if (!photoBitmap_) photoBitmap_ = deviceBitmapOf(context, backdropSource_.Get());

    // На всю полосу, без сохранения пропорций (как прежняя кисть Fill): снимок
    // скомпонован под полосу — книга по центру, стол по краям.
    context->DrawBitmap(photoBitmap_.Get(), D2D1::RectF(0.0f, 0.0f, width, height), 1.0f,
                        D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
}

void BookView::drawPage(ID2D1DeviceContext* context, float width, float height) {
    const float scale = presenter_.scale();

    // Ровная тема кроет лист бумагой; тема с подложкой начинает лист с самой
    // фотоподложки, а текст ложится поверх неё — всё в одну поверхность, одним
    // битмапом, без отдельного визуала под страницей.
    if (!hasBackdrop()) {
        pageWarp_.reset();   // ровная тема: набор там, где его положила вёрстка
        context->Clear(presenter_.paper().background);
        drawPageContent(context, width, height);
        return;
    }
    // Фото-тема: фотоподложка — в ту же поверхность, под текстом, чтобы страница
    // осталась одним битмапом. Не раскодировалась — бумага темы, чтобы задник не
    // сквозил.
    if (backdropSource_) {
        context->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
        drawThemeBackdrop(context, width, height);
    } else {
        context->Clear(presenter_.paper().background);
    }

    const bool warped = ensureWarp(context);

    // Точку указателя полоса переводит тем изгибом, которым страница
    // нарисована: гнуть нечего — набор на экране там же, где в вёрстке.
    // Обложка та же, по которой ensureWarp строил карту: её смену полоса
    // узнаёт restyle, и карта строится заново.
    if (warped) {
        pageWarp_.emplace(*presenter_.pageSkin());
    } else {
        pageWarp_.reset();
    }

    // Под конфигуратором изгиба набор слегка размывается: текст отступает на
    // второй план, и направляющие мастера читаются чётче. Ещё не согнутая —
    // плоская — обложка идёт тем же слоем, но мимо смещения: размытию нужен
    // образ страницы, а прямому рисунку в поверхность его не дать.
    const bool blurred = presenter_.previewing() && (warped || warpFlat_);
    if (blurred) ensureBlur(not_null<ID2D1DeviceContext>{warpContext_.Get()});

    if (!warped && !blurred) {
        drawPageContent(context, width, height);
        return;
    }

    warpContext_->SetTarget(warpLayer_.Get());
    warpContext_->BeginDraw();
    warpContext_->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
    // Вертикальная двойка растит слой, горизонтальный масштаб — тот же, что
    // redraw() даёт поверхности: содержимое рисуется в DIP, слой — в пикселях.
    warpContext_->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale * 2.0f));

    drawPageContent(warpContext_.Get(), width, height);
    typography::stopIfFailed(warpContext_->EndDraw(), "BookView: не закрылся рисунок слоя изгиба");

    // Хвост цепочки собирается по месту: изгиб, если есть что гнуть, потом
    // ужатие двойной высоты, потом размытие, если открыт конфигуратор.
    if (warped) {
        warpShrink_->SetInputEffect(0, warpDisplace_.Get());
    } else {
        warpShrink_->SetInput(0, warpLayer_.Get());
    }

    ID2D1Effect* tail = warpShrink_.Get();
    if (blurred) {
        warpBlur_->SetInputEffect(0, tail);
        typography::stopIfFailed(
            warpBlur_->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, kPreviewBlur * scale),
            "BookView: размытию не задана сила");
        tail = warpBlur_.Get();
    }

    // Выход эффектов — в пикселях поверхности, поэтому масштаб DIP→пиксели из
    // трансформа на время вынимается: остаётся только смещение атласа.
    D2D1_MATRIX_3X2_F outer{};
    context->GetTransform(&outer);
    context->SetTransform(D2D1::Matrix3x2F::Scale(1.0f / scale, 1.0f / scale) *
                          *D2D1::Matrix3x2F::ReinterpretBaseType(&outer));
    Microsoft::WRL::ComPtr<ID2D1Image> page;
    tail->GetOutput(&page);
    context->DrawImage(page.Get());
    context->SetTransform(*D2D1::Matrix3x2F::ReinterpretBaseType(&outer));
}

void BookView::drawPageContent(ID2D1DeviceContext* context, float width, float height) {
    const PageFlow& flow = presenter_.flow();

    const Theme& shade = presenter_.paper();
    const float margin = flow.margin();
    const float top = flow.verticalMargin();

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> textBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> linkBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> dimBrush;
    typography::stopIfFailed(context->CreateSolidColorBrush(shade.text, &textBrush),
                             "BookView: не создалась кисть текста");
    typography::stopIfFailed(context->CreateSolidColorBrush(shade.link, &linkBrush),
                             "BookView: не создалась кисть знака сноски");
    typography::stopIfFailed(context->CreateSolidColorBrush(shade.dim, &dimBrush),
                             "BookView: не создалась кисть колонцифры");

    // Знак сноски — цветом ссылки: по нему щёлкают, и видно это должно быть
    // без мыши.
    auto const lineBrushes = typography::TextBrushes{not_null<ID2D1Brush>{textBrush.Get()},
                                                     not_null<ID2D1Brush>{linkBrush.Get()}};

    // Колонки разворота — готовая лента потока страниц: каждая своя
    // страница, идущие подряд, при нужде со стыка из следующей главы. Разворот
    // стоит по середине окна, остаток ширины уходит в поля поровну.
    const std::vector<const typography::Page*>& columns = flow.spread().pages;
    for (size_t column = 0; column < columns.size(); ++column) {
        const float left = flow.columnLeft(column);
        const typography::Page& page = *columns[column];

        for (const typography::PlacedImage& image : page.images) {
            if (ID2D1Bitmap1* bitmap = flow.book()->bitmap(image.imageIndex, context)) {
                const D2D1_RECT_F target =
                    D2D1::RectF(left + image.x, top + image.y, left + image.x + image.width,
                                top + image.y + image.height);
                context->DrawBitmap(bitmap, target, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            }
        }

        for (const typography::PlacedLine& placed : page.lines) {
            typography::drawLine(context, *placed.line, left + placed.x, top + placed.baseline,
                                 lineBrushes);
        }
    }

    // Средник — то, чем две колонки на экране становятся раскрытой книгой: у
    // корешка бумага уходит вглубь, и туда не достаёт свет. Рисуется вместе со
    // страницей, а не отдельным визуалом, и это главное в нём: средник
    // принадлежит развороту, а не перевороту. Поэтому он есть всегда — и до
    // первого листания, и после перевёрстки, — и едет вместе со всем, что этот
    // разворот показывает: и с уходящим листом, и с приходящим. Движущиеся
    // тени переворота в конце его же и подменяют.
    //
    // Поверх текста, а не под ним: тень у корешка ложится на страницу целиком,
    // вместе с набором.
    if (flow.columns() == 2) {
        const float centre = flow.spine();
        const float band = width * kBendOfWindow;
        const D2D1_COLOR_F& hue = presenter_.paper().shadow;
        const D2D1_COLOR_F clear = tintedF(hue, 0.0f);
        const D2D1_COLOR_F dense = tintedF(hue, alphaOf(kBendNear));
        const D2D1_COLOR_F mid = tintedF(hue, alphaOf(kBendMid));

        // Тень зеркальна относительно корешка: тот же спад, что у полутона
        // изгиба, но в обе стороны — потому в конце переворота обе половины и
        // сходятся с тем, что было нарисовано.
        const D2D1_GRADIENT_STOP ramp[] = {
            {0.0f, clear},
            {0.5f - kBendMidStop * 0.5f, mid},
            {0.5f, dense},
            {0.5f + kBendMidStop * 0.5f, mid},
            {1.0f, clear},
        };

        Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> stops;
        typography::stopIfFailed(
            context->CreateGradientStopCollection(ramp, static_cast<UINT32>(std::size(ramp)), &stops),
            "BookView: не создалась шкала тени корешка");
        Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> gutter;
        typography::stopIfFailed(context->CreateLinearGradientBrush(
                                     D2D1::LinearGradientBrushProperties(D2D1::Point2F(centre - band, 0.0f),
                                                                         D2D1::Point2F(centre + band, 0.0f)),
                                     stops.Get(), &gutter),
                                 "BookView: не создалась кисть тени корешка");
        context->FillRectangle(D2D1::RectF(centre - band, 0.0f, centre + band, height), gutter.Get());
    }

    // На развороте колонцифра называет обе страницы: читатель видит две, и
    // одна в счётчике расходилась бы с тем, что перед глазами.
    //
    // Номер левой колонки известен сразу — видимый разворот посчитан
    // начисто в тот же кадр. Общее же число страниц главы становится
    // известно, только когда её набор кончился; пока нет — многоточие:
    // число, которое сейчас сменится другим, хуже честного молчания.
    // Процент при этом верен всегда, потому что считается по символам
    // книги, а место чтения перевёрстка почти не двигает.
    // Номера — по левой главе: разворот на стыке кончается колонками
    // следующей, но «стр. X из M» называет ту главу, где стоит читатель, и
    // не залезает в номера соседней. Числа решает поток страниц (status);
    // здесь они только становятся строкой.
    const PageFlow::Status status = flow.status();
    const size_t first = status.first;
    const size_t own = status.own;
    const u16_text numbers = own <= 1 ? core::format(u"{}", first + 1)
                                      : core::format(u"{}–{}", first + 1, first + own);
    const u16_text total =
        status.total ? core::format(u"{}", *status.total) : u16_text{u"…"};

    // Номер и общее число — по главе: пагинатор знает лишь её, и «из M»
    // здесь значит «из стольких страниц в этой главе». Процент — по всей
    // книге, по символам; им читатель и меряет весь путь.
    const u16_text line = core::format(u"Глава {} · стр. {} из {}     {:.0f}%",
                                       status.chapter + 1, numbers, total,
                                       status.progress * 100.0f);
    context->DrawText(line.wchars().data(), static_cast<UINT32>(line.size()),
                      statusFormat_.Get(),
                      D2D1::RectF(margin, height - top, width - margin, height),
                      dimBrush.Get());
}

void BookView::drawInvitation(ID2D1DeviceContext* context, float width, float height) {
    const Theme& shade = presenter_.paper();
    context->Clear(!hasBackdrop() ? shade.background : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

    if (!invitationFormat_) {
        typography::stopIfFailed(dwriteFactory()->CreateTextFormat(L"Georgia", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                                                   DWRITE_FONT_STYLE_ITALIC,
                                                                   DWRITE_FONT_STRETCH_NORMAL, 22.0f, L"ru-RU",
                                                                   &invitationFormat_),
                                 "BookView: не создался формат приглашения");
        typography::stopIfFailed(invitationFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER),
                                 "BookView: приглашению не задана выключка");
        typography::stopIfFailed(invitationFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER),
                                 "BookView: приглашению не задано место по высоте");
    }

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    typography::stopIfFailed(context->CreateSolidColorBrush(shade.dim, &brush),
                             "BookView: не создалась кисть приглашения");

    static constexpr wchar_t kInvitation[] = L"Перетащите книгу FB3 в это окно";
    context->DrawText(kInvitation, static_cast<UINT32>(sizeof(kInvitation) / sizeof(wchar_t) - 1),
                      invitationFormat_.Get(), D2D1::RectF(0, 0, width, height), brush.Get());
}

}  // namespace bukvitsa::reader
