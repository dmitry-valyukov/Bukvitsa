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
#include "UiThread.h"
#include "book_view.h"
#include "leaf_turn_animation.h"

#include "bukvitsa/reader/imaging.h"

#include "bukvitsa/typography/block.h"
#include "bukvitsa/typography/glyph_painter.h"

import wxl.fmt;

namespace bukvitsa::reader {

using namespace wxl;
using namespace std::chrono_literals;

namespace {

/// Размытие набора под конфигуратором изгиба, в DIP. Лёгкое, не туман:
/// тексту достаточно отступить на второй план, чтобы направляющие мастера
/// читались чётче, — но кривые укладывают по строкам, и строки должны
/// оставаться различимыми.
constexpr float kPreviewBlur = 1.5f;

// Сколько времени пагинатору отдаётся за раз. Порция кончается не раньше
// срока, а позже — на цену одного блока: разорвать вёрстку абзаца нечем.
// Самый дорогой блок из четырёх тестовых книг стоит 27 мс, так что худшая
// порция укладывается в те сто миллисекунд, дольше которых читалка не вправе
// не отвечать на ввод. Роман в 650 тысяч знаков проходит за шесть порций.
constexpr auto kPaginationSlice = 50ms;

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

BookView::BookView(const CompositionWindow& window, Settings& settings, ThemeList& themes,
                   Actions& actions, async::cancellation_token parent)
    : window_(window),
      settings_(settings),
      themes_(themes),
      actions_(actions),
      compositor_(window.compositor()),
      // Страницы разворота встают на сцену окна сразу, под всем, что на ней
      // будет; рисует в его растры полоса — то, что показывает поток страниц.
      spread_(compositor_, window.contentVisual(),
              [this](ID2D1DeviceContext* context) { paintSpread(context); }),
      // Контейнер листов — сразу, от композитора окна; buildTree() его одевает
      // и ставит на сцену над страницами.
      leafLayer_(compositor_.createContainerVisual()),
      // Всплывашка сноски — XAML-остров, её рисунок висит в дереве острова, а не
      // на сцене: ей нужен композитор острова, а не окна.
      note_(window.chromeCompositor()),
      stop_(std::move(parent)),
      readingScope_(stop_.token()),
      // Анимация листания — настройка читателя, и меняется на ходу: флажок
      // панели привязан к тому же полю. Поле живёт дольше полосы — слушателя
      // полоса снимает за собой.
      animateTurnsWatch_(
          settings_.animateTurns.on_change(method(this, &BookView::animateTurnsChanged))) {
    // Полоса ещё не экран: под источником чтения ждать нечему, пока
    // setActive(true) не заведёт новый.
    readingScope_.cancel();

    buildTree();

    // Стратегия переворота — до первой меры: мера доводит до конца всё, что
    // летит, и спрашивает у стратегии, куда рисовать.
    turnAnimation_ = makeTurnAnimation();

    // Мера полосы — у окна, не у острова. ClientSizeChanged приходит из WM_SIZE,
    // раньше вёрстки XAML, и несёт размер в пикселях вместе с масштабом экрана
    // — тем же числом, которым остров переводит DIP в пиксели; переезд на
    // монитор с другим DPI (WM_DPICHANGED) приходит сюда же, новым масштабом.
    // Остров полосы занимает окно целиком, так что это и его размер, а
    // спрашивать остров (SizeChanged, XamlRoot) не нужно — и нельзя было до
    // Loaded. Первый WM_SIZE окно пережило до нас: текущая мера снимается сразу.
    sizeToken_ = window_.add_onClientSizeChanged([this](Object const&, ClientSize const& client) {
        // Гистерезис числа колонок — только здесь: окно тянут мышью, граница
        // меры проходит под курсором, и без него колонки защёлкают. Знака
        // хватает — дрожь бывает в доли знака, а не в четыре.
        if (applySize(client.size, client.scale)) requestRelayout(true);
    });
    applySize(window_.clientSize(), window_.rasterizationScale());

    // Настройки вида приходят из модели, а не ставятся снаружи: полоса слушает
    // поля и перевёрстывается сама; то же поле двигает ползунок панели (Bind),
    // а колесо и клавиши полосы пишут в него же — и панель узнаёт о них так же.
    // Интерлиньяж и поля в модели в процентах, здесь — множитель и доля.
    settingsWatches_.emplace_back(
        &settings_.fontSize, settings_.fontSize.on_change([this](double size) noexcept {
            applyFontSize(static_cast<float>(size));
        }));
    settingsWatches_.emplace_back(
        &settings_.lineHeight, settings_.lineHeight.on_change([this](double percent) noexcept {
            applyLineHeight(static_cast<float>(percent) / 100.0f);
        }));
    settingsWatches_.emplace_back(
        &settings_.margin, settings_.margin.on_change([this](double percent) noexcept {
            applyMargin(static_cast<float>(percent) / 100.0f);
        }));
    flow_.setStyle(static_cast<float>(settings_.fontSize.get()),
                   static_cast<float>(settings_.lineHeight.get()) / 100.0f,
                   static_cast<float>(settings_.margin.get()) / 100.0f);

    // Смена темы — своя перекраска; поле своё, его слушатель умрёт вместе с
    // полосой, снимать нечего.
    static_cast<void>(theme.on_change(method(this, &BookView::themeChanged)));

    IDWriteFactory* const dwrite = dwriteFactory();
    if (!dwrite) return;

    // Колонцифра — единственный текст в читалке, который рисуется не своей
    // вёрсткой: у него нет ни переносов, ни выключки по формату — одна строка,
    // выключенная по центру, — и ради неё заводить абзац незачем.
    dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"ru-RU",
                             &statusFormat_);
    if (statusFormat_) {
        statusFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        statusFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
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
    // поставил разворот (spread_).
    leafLayer_.clip(compositor_.createInsetClip());

    // Листы переворотов — на сцене окна над страницами, привешены к
    // contentVisual() (над задником, под островом), а не всунуты в дерево XAML
    // через setElementChildVisual. Пока полоса не стала текущим экраном, её
    // сцена скрыта — setActive(true) покажет при входе.
    window_.contentVisual().children().insertAtTop(leafLayer_);
    leafLayer_.isVisible(false);

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

        // Кисть прозрачная, но она есть: без кисти Grid не участвует в проверке
        // попадания вовсе, и щелчок по полосе не доходил бы никуда — ни до знака
        // сноски, ни до трети страницы. Прозрачная потому, что страница теперь
        // на сцене под этим островом, и сквозь него должна быть видна она.
        // Бумагу вместе с набором несут страницы-визуалы на сцене, а задник
        // окна в чтении без кисти вовсе (см. setActive).
        background = colors.transparent,

        note_.root(),

        onLoaded = method(this, &BookView::loaded),

        // Клавиш здесь нет: их разбирает приложение у этого же корня, на
        // пути вниз, по карте клавиш, а команды полосы отдаёт в perform.
        // Колесо и щелчок — полосы: щелчку нужна сноска, которой карта не
        // знает.
        onPointerWheelChanged = method(this, &BookView::wheelTurned),
        onPointerPressed = method(this, &BookView::pressed),
    };
}

void BookView::loaded(Grid const& self) {
    self.focus(FocusState::Programmatic);
}

void BookView::wheelTurned(Object const&, PointerRoutedEventArgs& args) {
    const int delta = args.getCurrentPoint(root_).properties().mouseWheelDelta();
    const bool control = (static_cast<uint32_t>(args.keyModifiers()) &
                          static_cast<uint32_t>(VirtualKeyModifiers::Control)) != 0;

    const Command command = resolveWheel(delta, control, pointerContext());
    if (command == Command::None) return;

    perform(command);
    args.handled(true);
}

void BookView::pressed(Object const&, PointerRoutedEventArgs& args) {
    const PointerPoint touch = args.getCurrentPoint(root_);
    const Point point = touch.position();
    const Command command = resolveTap(point.x, flow_.width(), buttonOf(touch), pointerContext());

    // Поверх полосы лежит мастер: из всего щелчка полосе остаётся листание по
    // третям — ни ящика, ни сносок, ни фокуса. Сюда доходят только щелчки мимо
    // точек сетки: попавшие мастер разобрал сам.
    if (preview_) {
        if (command != Command::None) perform(command);
        return;
    }
    root_.focus(FocusState::Programmatic);
    args.handled(true);

    // Правая кнопка — единственная дорога к ящику для того, кто держит мышь:
    // у страницы нет ни полосы меню, ни кнопок, и заводить их ради этого
    // значило бы завесить книгу обстановкой. Панель полосе не принадлежит —
    // её открывает владелец.
    if (command == Command::TogglePanel) {
        actions_.togglePanel();
        return;
    }

    // Знак сноски важнее перелистывания: он мелкий, и промах по нему из-за
    // того, что страница уже перевернулась, читателя злит.
    const std::optional<PageFlow::NoteHit> mark = flow_.noteAt(PageFlow::Point{point.x, point.y});
    if (mark) {
        note_.show(*flow_.book(), mark->body, Point{mark->anchor.x, mark->anchor.y},
                   {flow_.width(), flow_.height()}, paper(), flow_.fontSize(), scale_);
        return;
    }

    // Щелчок мимо знака закрывает то, что открыто поверх полосы: читатель
    // прочёл примечание и вернулся к книге.
    if (note_.visible()) {
        note_.hide();
        return;
    }

    if (command != Command::None) perform(command);
}

KeyContext BookView::pointerContext() const {
    return KeyContext {
        .shown = Screen::Book,
        .wizardOpen = previewing(),
        .noteOpen = noteOpen(),
        .bookOpen = isOpen(),
    };
}

void BookView::perform(Command command) {
    switch (command) {
        case Command::TurnForward:
            turnPage(1);
            return;
        case Command::TurnBackward:
            turnPage(-1);
            return;

        // Начало и конец карта отдаёт только при открытой книге: конец
        // известен лишь ей, и чистовой набор доводится до самого конца.
        case Command::GoToStart:
        case Command::GoToEnd:
            if (!flow_.isOpen()) core::abort("BookView::perform: начало или конец книги без книги");
            goToCharOffset(command == Command::GoToStart ? 0u : flow_.book()->characterCount());
            return;

        // Кегль — в настройки: к полосе перемена вернётся слушателем, как и к
        // ползунку панели.
        case Command::FontLarger:
            nudgeFontSize(kFontSizeStep);
            return;
        case Command::FontSmaller:
            nudgeFontSize(-kFontSizeStep);
            return;
        case Command::FontReset:
            settings_.fontSize.set(kFontSizeDefault);
            return;

        case Command::NextTheme:
            setTheme(theme.get() + 1);
            return;
        case Command::DismissNote:
            note_.hide();
            return;

        default:
            break;
    }

    // Экраны, ящики и полный экран — приложения, а `None` исполнять нечего:
    // дошедшая сюда — ошибка проводки, а не клавиша, которую можно пропустить.
    core::abort("BookView::perform: команда не полосы");
}

BookView::~BookView() {
    // Ожидания полосы — концы пакетов переворотов — ей больше не нужны:
    // отменяет их владелец, то есть она сама. Корутины кончатся отменой на
    // своём ожидании и ни полосы, ни стратегии уже не тронут.
    stop_.cancel();

    // Настройки и окно переживают полосу: слушатели с нашим this снимаются,
    // иначе следующая перемена кегля, анимации или размера позвала бы уже
    // разрушенную полосу.
    for (auto& [field, cookie] : settingsWatches_) field->remove_change(cookie);
    settings_.animateTurns.remove_change(animateTurnsWatch_);
    window_.remove_onClientSizeChanged(sizeToken_);
}

void BookView::addOverlay(const UIElement& element) {
    root_.children().append(element);
}

void BookView::open(not_null<Book> book, uint32_t charOffset) {
    note_.hide();
    flow_.open(book, charOffset);
    requestRelayout();
    position_.set(flow_.position());
}

void BookView::setActive(bool active) {
    // Показаны страницы или нет, решается первым: при уходе посадка листа их
    // уже не одевает, при входе их одевает посадка и redraw.
    spread_.show(active);

    // Страница в чтении — две страницы-визуала на сцене окна (spread_), а
    // задник окна без кисти: страницы кроют его целиком, и красить под ними
    // нечего. Входя, снимаем кисть и показываем страницы тем же кадром, что
    // redraw рисует разворот и одевает их. Уходя — наоборот: страницы
    // прячутся, а задник на промежуток до фона следующего экрана (заставка
    // приходит асинхронно) берёт нынешний разворот сам — иначе окно сквозило
    // бы на стол.
    if (active) {
        // Новое чтение — новый источник: прежний отменён уходом, а если полоса
        // уже была экраном (книгу открыли поверх), отменяется здесь; отменённый
        // источник не переиспользуется.
        readingScope_.cancel();
        readingScope_ = async::cancellation_source{stop_.token()};

        // Перевороты, брошенные уходом, садятся теперь — первым делом: осевшим
        // становится разворот самого нового, неперелистываемая страница
        // одевается в него, листы — в покой, и заводится отпускание пула.
        turnAnimation_->finishAll();

        window_.clearBackground();
        redraw();
    } else {
        // Уход из чтения — отмена его источника, и только: ожидания концов
        // переворотов кончатся ходом очереди, а корутины ничего не тронут.
        // Листы досядут при следующем входе — до него их никто не видит.
        readingScope_.cancel();

        // Задник берёт растр нынешнего разворота — там, где он живёт сейчас
        // (в полёте книжного листа — растр самого нового листа): кисть
        // смотрит на растр, а не на указатель осевшего, и поздняя посадка её
        // не меняет — после неё осевшим станет тот же растр.
        if (spread_.measured()) window_.background(spread_.raster(turnAnimation_->drawTarget()));
    }

    // Листы в покое не нужны: разворот видно страницами. Они выходят на сцену
    // только на время переворота — показывает их стратегия, когда пускает
    // лист, и она же прячет обратно, когда сели все.
    leafLayer_.isVisible(false);
}

void BookView::setTheme(int index) {
    // Приводится к списку по кругу и кладётся в поле; всё, что следует за
    // сменой темы, делает слушатель (themeChanged) — так же, как если бы поле
    // поменял кто-то другой. Та же тема, что и была, слушателя не зовёт.
    theme.set(themes_.normalize(index));
}

void BookView::themeChanged(int) noexcept {
    note_.hide();   // подложка всплывашки покрашена прошлой темой
    turnAnimation_->tintShadows(paper().shadow);   // тени тоже покрашены прошлой темой

    // Карта держит форму конкретной обложки — новой теме она не годится.
    warpMap_.Reset();
    warpFlat_ = false;

    // Слой изгиба нужен только теме с подложкой, а весит как две полосы —
    // на ровных темах он отпускается вместе со снимком. Контекст и эффекты
    // мелкие и остаются. Снимок обложки отпускает и приносит владелец: ему
    // видно, тот же это файл или другой (две обложки могут делить снимок).
    if (!hasBackdrop()) {
        warpLayer_.Reset();
        warpPixels_ = {};
        dropBackdrop();
    }
    redraw();
}

void BookView::animateTurnsChanged(bool) noexcept {
    // Сменить стратегию посреди переворота — значит сперва довести до конца
    // всё, что летит: страницы, осевший разворот и задник после этого — те же,
    // что после любой посадки, и новой стратегии достаётся покой. Листы старой
    // уходят со сцены, а её свободные растры — из пула: без анимации растр
    // нужен один, а листы заведут свои, когда понадобятся.
    turnAnimation_->finishAll();
    leafLayer_.children().removeAll();
    turnAnimation_ = makeTurnAnimation();
    spread_.dropSpareRasters();
}

std::unique_ptr<TurnAnimation> BookView::makeTurnAnimation() {
    std::unique_ptr<TurnAnimation> chosenAnimation;
    if (settings_.animateTurns.get()) {
        chosenAnimation = std::make_unique<LeafTurnAnimation>(
            compositor_, leafLayer_, window_.dispatcherQueue(), spread_, flow_, readingScope_);
    } else {
        chosenAnimation = std::make_unique<InstantTurnAnimation>(spread_);
    }

    // Тени переворота — тоном нынешней темы.
    chosenAnimation->tintShadows(paper().shadow);
    return chosenAnimation;
}

void BookView::skinsChanged(int index) {
    // Снимок и карта сделаны из прежней обложки: снимок принесёт владелец
    // заново, карта соберётся по новым кривым.
    dropBackdrop();
    warpMap_.Reset();
    warpFlat_ = false;

    // Другой номер перекрасит и перерисует слушатель темы (themeChanged). На
    // тот же номер слушатель молчит, а кривые под ним могли смениться —
    // перерисовать здесь.
    if (themes_.normalize(index) != theme.get()) {
        setTheme(index);
    } else {
        redraw();
    }
}

void BookView::setPreview(const Skin* skin) {
    if (skin) {
        preview_ = *skin;
    } else {
        preview_.reset();
    }

    // Карта держит форму прежних кривых; снимок, если он другой, принесёт
    // владелец.
    warpMap_.Reset();
    warpFlat_ = false;
    redraw();
}

void BookView::nudgeFontSize(double by) {
    settings_.fontSize.set(std::clamp(settings_.fontSize.get() + by, kFontSizeMin, kFontSizeMax));
}

void BookView::applyFontSize(float size) {
    if (flow_.setStyle(size, flow_.lineHeight(), flow_.marginFraction())) requestRelayout();
}

void BookView::applyLineHeight(float multiplier) {
    if (flow_.setStyle(flow_.fontSize(), multiplier, flow_.marginFraction())) requestRelayout();
}

void BookView::applyMargin(float fraction) {
    if (flow_.setStyle(flow_.fontSize(), flow_.lineHeight(), fraction)) requestRelayout();
}

bool BookView::applySize(SizeInt32 pixels, float scale) {
    if (scale <= 0.0f) scale = 1.0f;

    // Пиксели — мера; DIP, в которых верстается и рисуется полоса, — их доля.
    const float width = static_cast<float>(pixels.width) / scale;
    const float height = static_cast<float>(pixels.height) / scale;
    if (width == flow_.width() && height == flow_.height() && scale == scale_ && spread_.measured())
        return false;

    flow_.resize(width, height);
    scale_ = scale;

    if (pixels.width <= 0 || pixels.height <= 0) return false;

    // Растры разворота меняют размер, а не пересоздаются: кисти, которые их
    // уже показывают (фон окна, страницы, листы), продолжают показывать их же.
    // Листы мерит сама стратегия — на каждый переворот.
    spread_.resize(pixels);
    leafLayer_.size({width, height});

    // Окна кроя листов заданы в прежних числах и после смены размера
    // бессмысленны — все идущие перевороты в покой: доигрывать их по новым
    // размерам значило бы листать вслепую.
    turnAnimation_->finishAll();
    return true;
}

void BookView::requestRelayout(bool windowResize) {
    // Прямо здесь, в обработчике события: грязная страница стоит единицы
    // миллисекунд, и откладывать её значило бы показать читателю его же
    // движение мыши с опозданием на кадр без всякой на то причины.
    relayoutNow(windowResize);
}

void BookView::relayoutNow(bool windowResize) {
    // Всякая новая вёрстка отменяет досчёт, который шёл: считать книгу по
    // прежней полосе больше незачем. Сброс владельца уничтожает кадр корутины
    // вместе с её ожиданием простоя — очередь потока его уже не разбудит.
    paginationTail_.reset();

    // Перевёрстка меняет и корешок, и ширину страницы, а листы могли остаться
    // от прерванных переворотов с окнами кроя, посчитанными по прежним числам.
    // Все идущие перевороты — в покой: доигрывать их по новым размерам значило
    // бы листать вслепую.
    turnAnimation_->finishAll();

    // Глава до видимого разворота — начисто, тем же кадром; место чтения
    // перевёрстка не двигает (PageFlow::relayout). Без книги или меры вёрстки
    // нет — рисуется приглашение или ничего.
    const bool laidOut = flow_.relayout(windowResize);
    redraw();

    // Остаток главы — порциями в свободное время потока: с него узнаётся общее
    // число страниц.
    if (laidOut) startTail();
}

void BookView::startTail() {
    if (!flow_.needsTail()) return;   // досчитывать нечего
    if (paginationTail_ && !paginationTail_->done()) return;   // идёт — возьмёт и эту главу

    // Корутина доходит до первого ожидания ещё внутри вызова, так что в
    // paginationTail_ ложится кадр, который уже ждёт простоя. Кончившийся кадр,
    // если он там лежал, emplace уничтожает.
    paginationTail_.emplace(paginateTail());
}

async::task<> BookView::paginateTail() {
    // Порции продолжают счёт с курсора текущей главы, не начиная заново:
    // синхронно посчитанные страницы остаются на месте, а здесь добирается
    // лишь хвост главы ради общего числа страниц. Меняется от него только
    // число «из M» — и то один раз, когда глава досчитана; до тех пор
    // колонцифра показывает «из …», и перерисовывать по ходу нечего.
    //
    // «Текущей» — на момент порции: листание через границу меняет главу под
    // идущим досчётом, и он просто считает дальше ту, что стала текущей;
    // кончил — startTail заведёт новый, когда войдут в следующую недосчитанную.
    //
    // Ожидание простоя — это и есть «в свободное время»: поток сперва разберёт
    // ввод и покажет нарисованное, а уже потом возьмётся за книгу. Книга здесь
    // всегда есть: её меняет только open(), а тот перевёрстывает, и новая
    // вёрстка сбрасывает этот кадр раньше, чем он проснётся. Уход окна кончает
    // ожидание исключением; оно остаётся в кадре, читать его некому — хвост
    // просто не досчитан, как и всё остальное в закрывающемся окне.
    for (;;) {
        co_await UiThread::onIdle();
        if (!flow_.advanceTail(kPaginationSlice)) break;
    }

    redraw();   // глава досчитана: «из …» стало «из M»
}

void BookView::showColumn(const PageFlow::Column& target) {
    turnAnimation_->finishAll();
    note_.hide();

    flow_.show(target);
    startTail();   // глава могла смениться — её хвост тоже нужен

    redraw();
    position_.set(flow_.position());
}

void BookView::redraw() {
    if (!spread_.measured() || flow_.width() <= 0.0f || flow_.height() <= 0.0f) return;

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
    spread_.redraw(turnAnimation_->drawTarget());
}

void BookView::paintSpread(ID2D1DeviceContext* context) {
    // Поверхность в пикселях, а вёрстка в DIP: масштаб домножается к тому
    // смещению атласа, которое wxl уже поставила, — иначе поверхность легла бы
    // поверх чужой.
    D2D1_MATRIX_3X2_F atlas{};
    context->GetTransform(&atlas);
    context->SetTransform(D2D1::Matrix3x2F::Scale(scale_, scale_) *
                          *D2D1::Matrix3x2F::ReinterpretBaseType(&atlas));

    // Классический ClearType здесь не годится: он рассчитан на неподвижный
    // текст на непрозрачном фоне и рассыпается на цветную бахрому при
    // повороте, дробном масштабе и на OLED. Серая сглаженность с
    // субпиксельным позиционированием выглядит одинаково везде.
    context->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    context->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    if (!flow_.spread().pages.empty()) {
        drawPage(context, flow_.width(), flow_.height());
    } else {
        drawInvitation(context, flow_.width(), flow_.height());
    }
}

void BookView::turnPage(int delta) {
    if (delta == 0)
        return;
    const bool forward = delta > 0;

    // Следующий разворот ленты от нынешнего места. Лента непрерывна, так что
    // это обычный шаг: границу главы он проходит сам, не прыжком. Место чтения
    // и страницы двигаются сразу, а лист летит вдогонку — поэтому быстрые
    // нажатия в одну сторону пускают несколько листов внахлёст, а не ждут в
    // очереди; встречное сперва доводит летящие до конца (turnStarting).
    const std::optional<PageFlow::Column> target = flow_.turnTarget(forward);
    if (!target)
        return;   // нет книги или край книги — листать некуда

    startTurn(*target, forward);
}

void BookView::startTurn(const PageFlow::Column& target, bool forward) {
    note_.hide();   // страница ушла, а сноска на ней осталась бы висеть

    // Порядок держится инвариантом, и он важнее анимации: уходящий разворот
    // лист берёт, пока книга ещё стоит на нём; книга шагает; новый разворот
    // рисуется ровно один раз — туда, где он теперь живёт (drawTarget
    // стратегии), — и только потом что-нибудь начинает двигаться. Как выглядит
    // переворот, решает стратегия — до шага и после; что он делает, решает
    // полоса.
    turnAnimation_->turnStarting(forward);

    // Двигаем книгу на целевой разворот. Целевая колонка может лежать в соседней
    // главе: поток делает её текущей, не теряя вёрстки (лента разложила её как
    // соседнюю на стыке), ставит место чтения и собирает новый разворот.
    flow_.show(target);
    startTail();   // вошли в соседнюю главу — её «из M» тоже надо досчитать

    // Новый разворот — туда, где он теперь живёт: в книге в растр самого
    // нового листа, иначе в осевший растр, и страницы одеваются в него. Без
    // анимации на этом переворот и кончается — ровно как прыжок (showColumn).
    redraw();

    turnAnimation_->turnStarted(forward);

    // Место чтения — при нажатии, а не при посадке листа.
    position_.set(flow_.position());
}

void BookView::goToCharOffset(uint32_t charOffset) {
    if (!flow_.isOpen()) return;

    // Прыжок по закладке, оглавлению или находке поиска — это разрыв ленты, а
    // не листание: разворот встаёт с колонки, где лежит место, без анимации
    // перехода. Досчёт нужной главы делает columnOf; фоновые порции прежней
    // главы, если ушли в другую, устаревают — их отменяет relayout при первом
    // же движении, а до того они молча пройдут по новой текущей главе.
    showColumn(flow_.columnOf(charOffset));
}

void BookView::dropBackdrop() {
    // Сначала снимок, потом байты: он читает из них. Кэш битмапа устройства
    // заведёт заново drawThemeBackdrop, вкомпоновывая фото прямо в
    // поверхность страницы.
    backdropSource_.Reset();
    backdropBytes_.clear();
    photoBitmap_.Reset();
}

void BookView::setBackdrop(std::string bytes) {
    // Нечего ни отпускать, ни показывать — и перерисовывать незачем.
    if (bytes.empty() && backdropBytes_.empty()) return;

    dropBackdrop();
    backdropBytes_ = std::move(bytes);
    if (!backdropBytes_.empty()) backdropSource_ = decodeImage(backdropBytes_);
    redraw();
}

bool BookView::ensureWarp(ID2D1DeviceContext* context) {
    const D2D1_SIZE_U pixels{static_cast<UINT32>(flow_.width() * scale_ + 0.5f),
                             static_cast<UINT32>(flow_.height() * scale_ + 0.5f) * 2};
    if (pixels.width == 0 || pixels.height == 0) return false;

    if (!warpLayer_ || warpPixels_.width != pixels.width || warpPixels_.height != pixels.height) {
        if (!warpContext_) {
            Microsoft::WRL::ComPtr<ID2D1Device> device;
            context->GetDevice(&device);
            if (!device || FAILED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
                                                              &warpContext_)))
                return false;
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
        if (FAILED(warpContext_->CreateBitmap(pixels, nullptr, 0, &layerProps, &warpLayer_)))
            return false;
        warpPixels_ = pixels;
    }

    // Эффекты — раньше проверки плоскости: плоскому предпросмотру под
    // конфигуратором ужатие нужно тоже — его слой идёт мимо смещения, но
    // через ту же вертикальную двойку. Подключение входов здесь не делается:
    // кто за кем стоит в цепочке, каждый кадр решает drawPage().
    if (!warpDisplace_) {
        warpContext_->CreateEffect(CLSID_D2D1DisplacementMap, &warpDisplace_);
        warpContext_->CreateEffect(CLSID_D2D1Scale, &warpShrink_);
        if (!warpDisplace_ || !warpShrink_) {
            warpDisplace_.Reset();
            warpShrink_.Reset();
            return false;
        }
        warpDisplace_->SetValue(D2D1_DISPLACEMENTMAP_PROP_X_CHANNEL_SELECT,
                                D2D1_CHANNEL_SELECTOR_R);
        warpDisplace_->SetValue(D2D1_DISPLACEMENTMAP_PROP_Y_CHANNEL_SELECT,
                                D2D1_CHANNEL_SELECTOR_G);
        warpShrink_->SetValue(D2D1_SCALE_PROP_SCALE, D2D1::Vector2F(1.0f, 0.5f));
        warpShrink_->SetValue(D2D1_SCALE_PROP_INTERPOLATION_MODE,
                              D2D1_SCALE_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
    }

    // Обложка с нетронутыми точками — плоская: гнуть нечего, полоса рисуется
    // напрямую. Ответ запомнен, чтобы не пересчитывать кривые на каждый кадр;
    // сбрасывают его смена темы, реестра обложек и размера полосы.
    if (warpFlat_) return false;

    if (!warpMap_) {
        // Чьи кривые гнут полосу: предпросмотр мастера или выбранная обложка.
        // Формулы здесь больше нет. Купол синуса, стоявший за «Антиквариат»,
        // был одинаков для любого снимка, а у настоящей фотографии каждая из
        // четырёх границ своя — и «Антиквариат» теперь описан точками, как
        // всякая другая обложка, только описание лежит в коде (skins.h).
        const Skin* skin = preview_ ? &*preview_ : activeSkin();

        // Выбрана тема, а не обложка: бумаги на экране нет, гнуть нечего.
        if (!skin) {
            warpFlat_ = true;
            return false;
        }

        // Отклонения краёв от их прямых начальных линий, в долях высоты
        // полосы, по значению на столбец карты. У каждой границы каждого листа
        // изгиб свой; какой лист под столбцом, кривая решает сама — по своей
        // точке корешка.
        const int columns = static_cast<int>(pixels.width);
        std::vector<float> topEdge(static_cast<size_t>(columns));
        std::vector<float> bottomEdge(static_cast<size_t>(columns));

        // Касательные считаются один раз на обе кромки, а не на каждый столбец.
        const EdgeSpline upper{skin->top};
        const EdgeSpline lower{skin->bottom};
        for (int x = 0; x < columns; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(columns);
            topEdge[static_cast<size_t>(x)] = upper.at(u) - kEdgeInset;
            bottomEdge[static_cast<size_t>(x)] = lower.at(u) - (1.0f - kEdgeInset);
        }

        float amplitude = 0.0f;
        for (int x = 0; x < columns; ++x) {
            amplitude = std::max({amplitude, std::abs(topEdge[static_cast<size_t>(x)]),
                                  std::abs(bottomEdge[static_cast<size_t>(x)])});
        }
        if (amplitude <= 0.0f) {
            warpFlat_ = true;
            return false;
        }
        warpAmplitude_ = amplitude;

        // Карта хранит чистую форму изгиба: канал G — доля смещения по Y от
        // размаха эффекта, 128 — «не смещать». Смещение обратное (эффект
        // читает «откуда взять», а не «куда сдвинуть»), поэтому у строк,
        // уезжающих вверх, в карте стоит плюс — взять снизу. Между краями
        // отклонение интерполируется линейно по высоте.
        std::vector<uint8_t> bytes(size_t{pixels.width} * pixels.height * 4);
        for (UINT32 y = 0; y < pixels.height; ++y) {
            const float down = (static_cast<float>(y) + 0.5f) / static_cast<float>(pixels.height);
            uint8_t* row = &bytes[size_t{y} * pixels.width * 4];
            for (UINT32 x = 0; x < pixels.width; ++x) {
                const float shift = topEdge[x] + (bottomEdge[x] - topEdge[x]) * down;
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
        if (FAILED(warpContext_->CreateBitmap(pixels, bytes.data(), pixels.width * 4, &mapProps,
                                              &warpMap_)))
            return false;
    }

    warpDisplace_->SetInput(0, warpLayer_.Get());
    warpDisplace_->SetInput(1, warpMap_.Get());
    // Размах — в пикселях слоя: наибольшее отклонение краёв в долях высоты —
    // это warpAmplitude_·высота·scale_ пикселей поверхности и вдвое больше в
    // слое двойной высоты; ещё двойка — потому что карта отклоняется от
    // середины не дальше половины размаха.
    warpDisplace_->SetValue(D2D1_DISPLACEMENTMAP_PROP_SCALE,
                            4.0f * warpAmplitude_ * flow_.height() * scale_);
    return true;
}

bool BookView::ensureBlur() {
    if (warpBlur_) return true;
    if (!warpContext_) return false;

    warpContext_->CreateEffect(CLSID_D2D1GaussianBlur, &warpBlur_);
    if (!warpBlur_) return false;

    // HARD вместо мягкой кромки по умолчанию: у границ полосы размытие не
    // должно подмешивать прозрачность из-за края слоя.
    warpBlur_->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD);
    return true;
}

void BookView::drawThemeBackdrop(ID2D1DeviceContext* context, float width, float height) {
    if (!backdropSource_) return;

    // Битмап устройства заводится раз на снимок и держится: CreateBitmapFromWicBitmap
    // на каждый кадр стоил бы дорого, а перерисовка бывает лишь на листании и
    // растяжке. Устройство поверхности стабильно, пока не потеряно.
    if (!photoBitmap_) {
        if (FAILED(context->CreateBitmapFromWicBitmap(backdropSource_.Get(), nullptr, &photoBitmap_)))
            return;
    }

    // На всю полосу, без сохранения пропорций (как прежняя кисть Fill): снимок
    // скомпонован под полосу — книга по центру, стол по краям.
    context->DrawBitmap(photoBitmap_.Get(), D2D1::RectF(0.0f, 0.0f, width, height), 1.0f,
                        D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
}

void BookView::drawPage(ID2D1DeviceContext* context, float width, float height) {
    // Ровная тема кроет лист бумагой; тема с подложкой начинает лист с самой
    // фотоподложки, а текст ложится поверх неё — всё в одну поверхность, одним
    // битмапом, без отдельного визуала под страницей.
    if (!hasBackdrop()) {
        context->Clear(paper().background);
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
        context->Clear(paper().background);
    }

    const bool warped = ensureWarp(context);

    // Под конфигуратором изгиба набор слегка размывается: текст отступает на
    // второй план, и направляющие мастера читаются чётче. Ещё не согнутая —
    // плоская — обложка идёт тем же слоем, но мимо смещения: размытию нужен
    // образ страницы, а прямому рисунку в поверхность его не дать.
    const bool blurred = preview_ && (warped || warpFlat_) && ensureBlur();

    if (!warped && !blurred) {
        drawPageContent(context, width, height);
        return;
    }

    warpContext_->SetTarget(warpLayer_.Get());
    warpContext_->BeginDraw();
    warpContext_->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
    // Вертикальная двойка растит слой, горизонтальный масштаб — тот же, что
    // redraw() даёт поверхности: содержимое рисуется в DIP, слой — в пикселях.
    warpContext_->SetTransform(D2D1::Matrix3x2F::Scale(scale_, scale_ * 2.0f));
    drawPageContent(warpContext_.Get(), width, height);
    if (FAILED(warpContext_->EndDraw())) return;   // фон с фотографией уже есть

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
        warpBlur_->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, kPreviewBlur * scale_);
        tail = warpBlur_.Get();
    }

    // Выход эффектов — в пикселях поверхности, поэтому масштаб DIP→пиксели из
    // трансформа на время вынимается: остаётся только смещение атласа.
    D2D1_MATRIX_3X2_F outer{};
    context->GetTransform(&outer);
    context->SetTransform(D2D1::Matrix3x2F::Scale(1.0f / scale_, 1.0f / scale_) *
                          *D2D1::Matrix3x2F::ReinterpretBaseType(&outer));
    Microsoft::WRL::ComPtr<ID2D1Image> page;
    tail->GetOutput(&page);
    context->DrawImage(page.Get());
    context->SetTransform(*D2D1::Matrix3x2F::ReinterpretBaseType(&outer));
}

void BookView::drawPageContent(ID2D1DeviceContext* context, float width, float height) {
    const Theme& shade = paper();
    const float margin = flow_.margin();
    const float top = flow_.verticalMargin();

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> textBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> dimBrush;
    context->CreateSolidColorBrush(shade.text, &textBrush);
    context->CreateSolidColorBrush(shade.dim, &dimBrush);
    if (!textBrush || !dimBrush) return;

    // Колонки разворота — готовая лента потока страниц: каждая своя
    // страница, идущие подряд, при нужде со стыка из следующей главы. Разворот
    // стоит по середине окна, остаток ширины уходит в поля поровну.
    const std::vector<const typography::Page*>& columns = flow_.spread().pages;
    for (size_t column = 0; column < columns.size(); ++column) {
        const float left = flow_.columnLeft(column);
        const typography::Page& page = *columns[column];

        for (const typography::PlacedImage& image : page.images) {
            if (ID2D1Bitmap1* bitmap = flow_.book()->bitmap(image.imageIndex, context)) {
                const D2D1_RECT_F target =
                    D2D1::RectF(left + image.x, top + image.y, left + image.x + image.width,
                                top + image.y + image.height);
                context->DrawBitmap(bitmap, target, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            }
        }

        for (const typography::PlacedLine& placed : page.lines) {
            typography::drawLine(context, *placed.line, left + placed.x, top + placed.baseline,
                                 textBrush.Get());
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
    if (flow_.columns() == 2) {
        const float centre = flow_.spine();
        const float band = width * kBendOfWindow;
        const D2D1_COLOR_F& hue = paper().shadow;
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
        context->CreateGradientStopCollection(ramp, static_cast<UINT32>(std::size(ramp)), &stops);
        if (stops) {
            Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> gutter;
            context->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(D2D1::Point2F(centre - band, 0.0f),
                                                    D2D1::Point2F(centre + band, 0.0f)),
                stops.Get(), &gutter);
            if (gutter) {
                context->FillRectangle(D2D1::RectF(centre - band, 0.0f, centre + band, height),
                                       gutter.Get());
            }
        }
    }

    if (statusFormat_) {
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
        const PageFlow::Status status = flow_.status();
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
}

void BookView::drawInvitation(ID2D1DeviceContext* context, float width, float height) {
    const Theme& shade = paper();
    context->Clear(!hasBackdrop() ? shade.background : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

    IDWriteFactory* const dwrite = dwriteFactory();
    if (!dwrite) return;

    if (!invitationFormat_) {
        dwrite->CreateTextFormat(L"Georgia", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                 DWRITE_FONT_STYLE_ITALIC, DWRITE_FONT_STRETCH_NORMAL, 22.0f,
                                 L"ru-RU", &invitationFormat_);
        if (!invitationFormat_) return;
        invitationFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        invitationFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    context->CreateSolidColorBrush(shade.dim, &brush);
    if (!brush) return;

    static constexpr wchar_t kInvitation[] = L"Перетащите книгу FB3 в это окно";
    context->DrawText(kInvitation, static_cast<UINT32>(sizeof(kInvitation) / sizeof(wchar_t) - 1),
                      invitationFormat_.Get(), D2D1::RectF(0, 0, width, height), brush.Get());
}

}  // namespace bukvitsa::reader
