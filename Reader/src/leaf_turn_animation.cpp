// Свой заголовок первым: он ведёт к импорту модуля книги, после которого
// стандартный заголовок MSVC уже не принимает.
#include "leaf_turn_animation.h"
#include "event_awaitable.h"

namespace bukvitsa::reader {

using namespace wxl;
using namespace std::chrono_literals;

namespace {

/// Переворот листа. Полсекунды: движение руки укладывается и в половину этого,
/// но лист — не курсор, а страница книги, и глазу нужно успеть увидеть, что
/// именно перевернулось. Дольше читатель начнёт ждать.
constexpr auto kTurn = 480ms;

/// На сколько лист поворачивается, уходя. Плоский лист, повёрнутый на десять
/// градусов вокруг своего левого края, читается как поднятая бумага — большего
/// объёма тут и не нужно, весь остальной делает тень.
constexpr float kTurnAngle = 10.0f;

/// Его же косинус и синус — числами, а не вызовом. Угол известен на этапе
/// компиляции, а `<cmath>` сюда уже не включить: заголовок, включённый после
/// import std;, MSVC не принимает.
constexpr float kTurnCos = 0.98481f;
constexpr float kTurnSin = 0.17365f;

/// Тень уезжающего листа: размытие и сдвиг вправо. Она не украшение — лист и
/// страница под ним одного цвета, и без тени глаз не видит, что один поднят
/// над другим.
constexpr float kShadowBlur = 24.0f;
constexpr float kShadowShift = 6.0f;
constexpr float kShadowOpacity = 0.45f;

/// Насколько тень выходит за лист — и, стало быть, насколько крой листа
/// отпущен в покое. Клип режет не только содержимое визуала, но и то, что
/// визуал отбрасывает, а тень уезжающего листа лежит как раз за его правым
/// краем: нулевые отступы срезали бы её начисто (проверено — от неё остаётся
/// линия в один пиксель). Отрицательный отступ клип расширяет.
constexpr float kShadowReach = kShadowShift + kShadowBlur;

/// Книжное листание: сколько едет кромка от края разворота до корешка. Пути
/// вдвое меньше, чем у обычного переворота, — отсюда и время меньше kTurn.
/// Кривая хода та же, что у всего листания, — пологая S-кривая (kEase*).
///
/// ВРЕМЕННО замедлено почти вдвое против рабочих 280 мс: так переворот видно
/// глазом и по кадрам. Вернуть перед тем, как читать книгу.
constexpr auto kLeafSlide = 500ms;

/// Стороны разворота — номера страниц-визуалов разворота.
constexpr size_t kLeftPage = SpreadView::kLeftPage;
constexpr size_t kRightPage = SpreadView::kRightPage;

/// Сколько листов переворота держим в воздухе разом. При быстром листании
/// каждый нажим пускает свой лист, и они летят внахлёст; больше этого числа не
/// копим — самый старый добивается рывком. Каждый лист в полёте держит растр
/// разворота в размер окна, то есть память (десяток с лишним листов —
/// сотня-другая мегабайт), поэтому число хоть и щедрое, но не бесконечное.
constexpr size_t kMaxLeaves = 16;

/// Отпускание пула листов в простое. Первый лишний лист роняем через долгую
/// паузу — вдруг читатель тут же листнёт снова и пул понадобится сразу; дальше
/// по одному в секунду, пока не останется один (его держим на следующее
/// листание, чтобы не заводить поверхность заново). Так пик памяти держится
/// лишь на время листания, а в покое от пула остаются один лист и один
/// свободный растр.
constexpr auto kLeafReleaseFirst = 10s;
constexpr auto kLeafReleaseStep = 1s;

/// Закон листания — пологая S-кривая (кубическая безье): небольшой разгон в
/// начале и торможение в конце, чтобы движение читалось живым, а не
/// равномерным. Контрольные точки симметричны относительно середины разворота;
/// концы мягкие, но скорость на них не гаснет в ноль — оттого «небольшой», а не
/// полная остановка на краях. Правится этими четырьмя числами.
constexpr float kEaseX1 = 0.30f, kEaseY1 = 0.10f;
constexpr float kEaseX2 = 0.70f, kEaseY2 = 0.90f;

/// Насколько широко тень расходится к концу переворота — в долях страницы, а
/// не в точках. Тень меряется окном: на широком экране полсотни точек
/// теряются, на узком закрывают текст. Целая страница в конце означает ровно
/// половину страницы на середине переворота, а дальше только шире.
constexpr float kFoldOfPage = 1.0f;

/// Плотность тени в начале и в конце. Расходясь, полутень светлеет: иначе
/// впятеро более широкая полоса читалась бы не мягче, а тяжелее — а мягкость
/// тут и есть признак того, что бумага поднялась.
constexpr float kFoldDense = 1.0f;
constexpr float kFoldFaint = 0.5f;

// Плотность у самой кромки и в середине спада (kFoldNear, kFoldMid) — у
// разворота, spread_view.h: той же парой меряется нарисованный в него средник.

/// Куда по ходу переворота уезжает середина спада. В начале он сосредоточен у
/// кромки — тень читается краем; к концу выравнивается и края у тени не
/// остаётся вовсе. Это и есть «мягче», всё остальное делает ширина.
constexpr float kFoldMidStop = 0.30f;
constexpr float kFoldMidSoft = 0.62f;

/// Тень, которую приходящий лист кладёт наружным краем на страницу под ним.
/// Она узкая и по ходу переворота не меняется: этот край всё время лежит на
/// бумаге, в отличие от корешкового, который поднят и оттого мажет широко.
/// Без неё лист не читается отдельной бумагой поверх страницы — а на неё вся
/// вторая половина переворота и опирается. Тоже в долях окна.
constexpr float kEdgeOfWindow = 0.039f;
constexpr uint32_t kEdgeNear = 0x70000000;
constexpr uint32_t kEdgeMid = 0x26000000;
constexpr float kEdgeMidStop = 0.42f;

// Полутон изгиба (kBendOfWindow, kBendNear, kBendMid, kBendMidStop) и
// прозрачность из записанного цвета (alphaOf) — тоже у разворота: средник
// обязан совпасть с полутоном, и оба берут их из одного места.

/// Когда движущиеся тени передают средник нарисованному. Не в самом конце:
/// подмена должна успеть пройти незаметно, пока лист доходит последние
/// проценты пути.
constexpr float kHandover = 0.78f;

/// Подъём листа к глазу: во сколько раз свободный край выше собственной высоты
/// в верхней точке переворота. У корешка лист остаётся в своих размерах — там
/// он держится страницы и не поднимается вовсе, — а к свободному краю растёт, и
/// прямоугольник становится трапецией. Это и есть «ближе»: край, который
/// перелистывают, идёт к глазу, и глаз читает больший размер как меньшее
/// расстояние.
constexpr float kLiftPeak = 1.25f;

/// Выключка подъёма — своя, с погасшей скоростью на концах. Подъём состоит из
/// двух отрезков, вверх и вниз, и общая кривая листания (kEase*), у которой
/// концы намеренно не гаснут, дала бы на вершине излом — глаз читает такой
/// излом щелчком. Здесь лист трогается плавно, замирает наверху и так же
/// плавно ложится.
constexpr float kLiftEaseX1 = 0.50f, kLiftEaseY1 = 0.0f;
constexpr float kLiftEaseX2 = 0.50f, kLiftEaseY2 = 1.0f;

/// Трапеция листа — формула, которую композитор считает каждый кадр.
///
/// Аффинной матрицей трапеции не выйдет: сдвиг, поворот и растяжение сохраняют
/// параллельность сторон, и прямоугольник ими становится параллелограммом.
/// Нужен проективный переход — четвёртый столбец Matrix4x4, тот самый, которым
/// композиция делает перспективу: композитор делит на W после умножения, и
/// линейный по x знаменатель поднимает высоту тем сильнее, чем дальше от
/// корешка.
///
/// Вывод. Пусть корешок стоит в Hinge, свободный край отстоит от него на d, и
/// высота у края должна вырасти в k раз. В долях u = (x - Hinge)/d переход
///     W = 1 + p·u,   X = u·(1 + p),   Y = y - Half
/// при p = 1/k - 1 даёт ровно требуемое: у корешка (u = 0) единица, у края
/// (u = 1) высота в k раз. Раскрыв u обратно в x, получаем числа ниже: Slant —
/// это p, Fall — p/d, Half — середина листа по высоте, от которой он растёт в
/// обе стороны. Ширина при этом поджимается сама, как в перспективе: дальняя
/// от глаза половина листа занимает меньше места, чем ближняя, — этого не
/// избежать и не надо, ровно так выглядит наклонённая бумага.
///
/// Amount — доля подъёма, 0..1. Все шестнадцать чисел линейны по ней, поэтому
/// её одной довольно, чтобы вести трапецию во времени обычной скалярной
/// анимацией: матричной покадровой анимации в композиции нет.
constexpr wchar_t kLiftFormula[] =
    L"Matrix4x4("
    L"1 + lift.Amount * (Slant + Hinge * Fall), lift.Amount * Half * Fall, 0, lift.Amount * Fall,"
    L"0, 1, 0, 0,"
    L"0, 0, 1, 0,"
    L"-Hinge * lift.Amount * (Slant + Hinge * Fall), -Hinge * lift.Amount * Half * Fall, 0,"
    L"1 - Hinge * lift.Amount * Fall)";

/// Цвет тени темы с нужной прозрачностью — в том виде, в каком его берёт
/// композитор. Прозрачный конец градиента тоже красится тоном, а не сводится
/// к прозрачному чёрному: интерполируй композитор без предумножения, чистая
/// прозрачность увела бы спад в серое.
Color tinted(const D2D1_COLOR_F& color, float alpha) {
    auto channel = [](float value) {
        return static_cast<uint8_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    return Color{channel(alpha), channel(color.r), channel(color.g), channel(color.b)};
}

/// Ставит трапецию на одну сторону листа: у `hinge` она остаётся в своих
/// размерах, у `freeEdge` вырастает в kLiftPeak раз — во столько, сколько
/// скажет Amount в `liftAmount`.
///
/// Обе стороны листа — снимаемая бумага и приходящий лист — берут общий
/// корешок и свои, противоположные, свободные края. Оттого они и сходятся на
/// сгибе в одной высоте: точка на расстоянии s от корешка поднята одинаково, с
/// какой бы стороны бумаги она ни была, а сгиб — это одна точка, видимая с
/// обеих. Ничего согласовывать для этого не нужно, так выходит само.
void liftPage(ExpressionAnimation const& liftExpression, CompositionPropertySet const& liftAmount,
              Visual const& page, float hinge, float freeEdge, float height) {
    const float slant = 1.0f / kLiftPeak - 1.0f;
    liftExpression.setReferenceParameter(L"lift", liftAmount);
    liftExpression.setScalarParameter(L"Slant", slant);
    liftExpression.setScalarParameter(L"Fall", slant / (freeEdge - hinge));
    liftExpression.setScalarParameter(L"Half", height * 0.5f);
    liftExpression.setScalarParameter(L"Hinge", hinge);
    page.startAnimation(L"TransformMatrix", liftExpression);
}

}  // namespace

LeafTurnAnimation::LeafTurnAnimation(const Compositor& compositor, const ContainerVisual& leafLayer,
                                     const DispatcherQueue& queue, SpreadView& spread,
                                     const PageFlow& flow, async::cancellation_token parent)
    : compositor_(compositor),
      leafLayer_(leafLayer),
      spread_(spread),
      flow_(flow),
      restBrush_(compositor.createColorBrush(rgba(0, 0, 0, 0))),
      releaseTimer_(queue.createTimer()),
      stop_(std::move(parent)) {
    // Пул листов зарезервирован под предел: дальше push_back не переселяет
    // вектор, и указатели на листы, что держат ожидания конца переворота
    // (landWhenDone), остаются годными.
    leafPool_.reserve(kMaxLeaves);

    // Таймер отпускания пула листов не повторяется — перезаводится сам с новой
    // паузой (armRelease/onReleaseTick).
    releaseTimer_.isRepeating(false);
    releaseTick_ = releaseTimer_.add_onTick(method(this, &LeafTurnAnimation::onReleaseTick));
}

LeafTurnAnimation::~LeafTurnAnimation() {
    // Ожидания концов переворотов стратегии больше не нужны: отменяет их
    // владелец, то есть она сама. Корутины кончатся отменой на своём ожидании
    // и стратегии уже не тронут. Таймер пула — тоже её, и тик после неё звал
    // бы мёртвое.
    stop_.cancel();
    releaseTimer_.stop();
    releaseTimer_.remove_onTick(releaseTick_);
}

void LeafTurnAnimation::turnStarting(bool forward) {
    // Режим книги — разворот в две колонки: там есть корешок и переворот листа
    // у него. Одна колонка и три с лишним — режим газеты: уезжает целая
    // страница. Страницы ведут себя в них по-разному, поэтому режим решаем
    // сразу.
    const bool book = flow_.columns() == 2;

    // Встречное листание сперва сажает всё, что летит, — в обоих режимах. В
    // книге бумага снимается с осевшего разворота, а у летящего листа
    // перелистываемая страница ещё в воздухе: снимать с неё нечего, и лист,
    // пущенный навстречу поверх летящих, показывал бы под собой то, чего в
    // книге уже нет. В газете копия одного и того же листа летела бы в разные
    // стороны разом. Садятся они рывком, в покой, осевший разворот — самого
    // нового из них (cancelTurn); листы в ту же сторону остаются лететь
    // внахлёст.
    for (const Leaf& pooledLeaf : leafPool_)
        if (pooledLeaf.active && pooledLeaf.forward != forward) {
            cancelTurn();
            break;
        }

    // Лист возьмётся свободный, новый или посаженный рывком — см. acquireLeaf.
    Leaf& flyingLeaf = acquireLeaf();
    flyingLeaf.forward = forward;
    flyingLeaf.book = book;
    flyingLeaf.epoch = flyingLeaf.started = ++leafClock_;
    flyingLeaf.outgoingRaster = flyingLeaf.incomingRaster = SpreadView::kNoRaster;

    if (book) {
        // Режим книги: неперелистываемая страница (при листании вперёд левая)
        // остаётся на старом развороте, пока приходящий лист её не накроет.
        // Новых страниц у переворота две — оборот снимаемой бумаги и та, что
        // открывается под ней, — то есть ровно один новый разворот: он ляжет
        // в свободный растр листа единственной отрисовкой на переворот (redraw
        // полосы после шага: нынешний разворот с этой минуты несёт самый новый
        // лист, drawTarget). Из него animateBookTurn оденет перелистываемую
        // страницу и выведет приходящий лист; старое лист заимствует у
        // страницы, на которой оно уже показано. Осевшим разворотом лист, сев,
        // станет указателем, без отрисовки (landLeaf).
        flyingLeaf.incomingRaster = acquireRaster();
    } else {
        // Режим газеты: уезжающий лист несёт уходящую страницу, а из-под него
        // открываются страницы — новый разворот. Уходящее уже растеризовано —
        // это осевший растр, который страницы носят сейчас: лист берёт его как
        // есть, а осевшим становится свободный растр, и в него redraw полосы
        // нарисует новый разворот после шага книги. Так в газете, как и в
        // книге, на переворот приходится одна отрисовка.
        flyingLeaf.outgoingRaster = spread_.settledRaster();
        spread_.settleOn(acquireRaster());
    }
}

void LeafTurnAnimation::turnStarted(bool forward) {
    Leaf& flyingLeaf = *newestInFlight();   // его только что завёл turnStarting
    fitLeaf(flyingLeaf);

    leafLayer_.isVisible(true);   // листы — на время переворота

    if (flyingLeaf.book)
        animateBookTurn(flyingLeaf, forward);
    else
        animateFlatTurn(flyingLeaf, forward);
}

void LeafTurnAnimation::finishAll() {
    cancelTurn();
}

SpreadView::RasterId LeafTurnAnimation::drawTarget() const {
    if (const Leaf* const newestLeaf = newestInFlight(); newestLeaf && newestLeaf->book)
        return newestLeaf->incomingRaster;
    return spread_.settledRaster();
}

void LeafTurnAnimation::tintShadows(const D2D1_COLOR_F& hue) {
    shadowHue_ = hue;
    applyShadowTint();
}

void LeafTurnAnimation::fitLeaf(Leaf& flyingLeaf) {
    const float width = flow_.width();
    const float height = flow_.height();
    flyingLeaf.outgoingPage.size({width, height});
    // Полоска тени сгиба меряется целым разворотом (её ужимает Scale по ходу),
    // приходящий лист — тоже; тени края и полутон изгиба — постоянной ширины
    // в долях окна.
    flyingLeaf.foldShadow.size({width, height});
    flyingLeaf.incomingPage.size({width, height});
    flyingLeaf.outerEdgeShadow.size({width * kEdgeOfWindow, height});
    flyingLeaf.bendHalftone.size({width * kBendOfWindow, height});
}

LeafTurnAnimation::Leaf& LeafTurnAnimation::acquireLeaf() {
    releaseTimer_.stop();   // снова листают — отпускание пула отменяется

    // Свободный лист в пуле?
    for (Leaf& pooledLeaf : leafPool_)
        if (!pooledLeaf.active) {
            pooledLeaf.active = true;
            return pooledLeaf;
        }

    // Ещё не набрали лимит — заводим новый. Пул зарезервирован под kMaxLeaves
    // (конструктор), так что push_back не переселяет вектор: указатели на листы,
    // что держат ожидания конца, остаются годными. Растр новому листу не нужен:
    // растры — у разворота, лист берёт их на переворот.
    if (leafPool_.size() < kMaxLeaves) {
        leafPool_.push_back(makeLeaf());
        Leaf& freshLeaf = leafPool_.back();
        freshLeaf.active = true;
        applyShadowTint();   // покрасить его тени под нынешнюю тему
        return freshLeaf;
    }

    // Все в воздухе, а листать просят ещё — сажаем самый старый рывком. Сев,
    // он отдаёт свой разворот неперелистываемой странице как доигравший
    // (landLeaf): иначе из-под ещё летящих на миг выглянуло бы то, что было
    // до него.
    Leaf* oldestLeaf = &leafPool_.front();
    for (Leaf& pooledLeaf : leafPool_)
        if (pooledLeaf.started < oldestLeaf->started)
            oldestLeaf = &pooledLeaf;
    oldestLeaf->epoch = ++leafClock_;   // его ожидание конца, дождавшись, увидит чужой номер
    finishLeaf(*oldestLeaf);
    landLeaf(*oldestLeaf);
    oldestLeaf->active = true;
    return *oldestLeaf;
}

const LeafTurnAnimation::Leaf* LeafTurnAnimation::newestInFlight() const {
    const Leaf* newestLeaf = nullptr;
    for (const Leaf& pooledLeaf : leafPool_)
        if (pooledLeaf.active && (!newestLeaf || pooledLeaf.started > newestLeaf->started))
            newestLeaf = &pooledLeaf;
    return newestLeaf;
}

LeafTurnAnimation::Leaf* LeafTurnAnimation::newestInFlight() {
    return const_cast<Leaf*>(std::as_const(*this).newestInFlight());
}

SpreadView::RasterId LeafTurnAnimation::acquireRaster() {
    return spread_.acquireRaster(
        [this](SpreadView::RasterId raster) { return rasterInFlight(raster); });
}

bool LeafTurnAnimation::rasterInFlight(SpreadView::RasterId raster) const {
    for (const Leaf& pooledLeaf : leafPool_)
        if (pooledLeaf.active &&
            (pooledLeaf.outgoingRaster == raster || pooledLeaf.incomingRaster == raster))
            return true;
    return false;
}

LeafTurnAnimation::Leaf LeafTurnAnimation::makeLeaf() {
    // Лист в покое ничего не носит (restBrush_): растры — у разворота, лист
    // берёт их на время переворота. Размеры ставит каждый переворот (fitLeaf):
    // мера могла смениться, пока лист лежал в пуле.
    SpriteVisual outgoingPage = compositor_.createSpriteVisual();
    outgoingPage.brush(restBrush_);
    // Крой в покое отпущен на вылет тени: нулевые отступы — ровно лист, а тени
    // положено лежать за его краем.
    InsetClip outgoingPageClip =
        compositor_.createInsetClip(-kShadowReach, -kShadowReach, -kShadowReach, -kShadowReach);
    outgoingPage.clip(outgoingPageClip);
    outgoingPage.isVisible(false);

    // Тень уезжающего листа — маской ей кисть листа, поэтому тень повторяет
    // лист, а не описанный прямоугольник. Гасится и зажигается прозрачностью.
    DropShadow flatLeafShadow = compositor_.createDropShadow();
    flatLeafShadow.blurRadius(kShadowBlur);
    flatLeafShadow.offset({kShadowShift, 0.0f, 0.0f});
    flatLeafShadow.mask(outgoingPage.brush());
    flatLeafShadow.opacity(0.0f);
    outgoingPage.shadow(flatLeafShadow);

    // Тень сгиба (книжное листание): горизонтальный градиент в долях своей
    // ширины, поэтому полоске достаточно ездить — перекрашивать не приходится.
    CompositionLinearGradientBrush foldShadowBrush = compositor_.createLinearGradientBrush();
    foldShadowBrush.startPoint({0.0f, 0.0f});
    foldShadowBrush.endPoint({1.0f, 0.0f});
    CompositionColorGradientStop foldShadowMidpoint =
        compositor_.createColorGradientStop(kFoldMidStop, colors.transparent);
    foldShadowBrush.colorStops().append(compositor_.createColorGradientStop(0.0f, colors.transparent));
    foldShadowBrush.colorStops().append(foldShadowMidpoint);
    foldShadowBrush.colorStops().append(compositor_.createColorGradientStop(1.0f, colors.transparent));
    SpriteVisual foldShadow = compositor_.createSpriteVisual();
    foldShadow.brush(foldShadowBrush);
    foldShadow.isVisible(false);

    // Тень наружного края приходящего листа: узкая и неизменная, градиент
    // развёрнут — густо у листа, прозрачно прочь.
    CompositionLinearGradientBrush outerEdgeShadowBrush = compositor_.createLinearGradientBrush();
    outerEdgeShadowBrush.colorStops().append(compositor_.createColorGradientStop(0.0f, colors.transparent));
    outerEdgeShadowBrush.colorStops().append(
        compositor_.createColorGradientStop(kEdgeMidStop, colors.transparent));
    outerEdgeShadowBrush.colorStops().append(compositor_.createColorGradientStop(1.0f, colors.transparent));
    SpriteVisual outerEdgeShadow = compositor_.createSpriteVisual();
    outerEdgeShadow.brush(outerEdgeShadowBrush);
    outerEdgeShadow.isVisible(false);

    // Приходящий лист (кисть выдаётся на каждый переворот — растр нового
    // разворота) и полутон изгиба у сгиба: ребёнок листа, кроится вместе с
    // ним.
    SpriteVisual incomingPage = compositor_.createSpriteVisual();
    incomingPage.brush(restBrush_);
    InsetClip incomingPageClip = compositor_.createInsetClip();
    incomingPage.clip(incomingPageClip);
    incomingPage.isVisible(false);
    CompositionLinearGradientBrush bendHalftoneBrush = compositor_.createLinearGradientBrush();
    bendHalftoneBrush.colorStops().append(compositor_.createColorGradientStop(0.0f, colors.transparent));
    bendHalftoneBrush.colorStops().append(
        compositor_.createColorGradientStop(kBendMidStop, colors.transparent));
    bendHalftoneBrush.colorStops().append(compositor_.createColorGradientStop(1.0f, colors.transparent));
    SpriteVisual bendHalftone = compositor_.createSpriteVisual();
    bendHalftone.brush(bendHalftoneBrush);
    incomingPage.children().insertAtTop(bendHalftone);

    // Подъём листа к глазу (книжное листание): доля подъёма — скаляр в
    // собственных свойствах листа, а две трапеции читают её выражениями. Числа
    // геометрии ставит начало каждого переворота: корешок и свободный край
    // зависят от стороны листания, а размеры — от окна.
    CompositionPropertySet liftAmount = outgoingPage.properties();
    liftAmount.insertScalar(L"Amount", 0.0f);
    ExpressionAnimation outgoingLiftExpression = compositor_.createExpressionAnimation(kLiftFormula);
    ExpressionAnimation incomingLiftExpression = compositor_.createExpressionAnimation(kLiftFormula);

    // Все листовые визуалы — в контейнер; их Z на каждый переворот уточняет
    // анимация (плоское — под старые, книжное — над старыми).
    VisualCollection const layerChildren = leafLayer_.children();
    layerChildren.insertAtTop(outgoingPage);
    layerChildren.insertAtTop(foldShadow);
    layerChildren.insertAtTop(outerEdgeShadow);
    layerChildren.insertAtTop(incomingPage);

    return Leaf{SpreadView::kNoRaster,  SpreadView::kNoRaster, outgoingPage,
                outgoingPageClip,       flatLeafShadow,        incomingPage,
                incomingPageClip,       bendHalftone,          bendHalftoneBrush,
                foldShadow,             foldShadowBrush,       foldShadowMidpoint,
                outerEdgeShadow,        outerEdgeShadowBrush,  liftAmount,
                outgoingLiftExpression, incomingLiftExpression};
}

void LeafTurnAnimation::finishLeaf(Leaf& flyingLeaf) {
    // Сначала снять анимации, потом писать: пока анимация на свойстве жива,
    // прямая запись до него не доходит, и недоехавший лист доехал бы позже уже
    // не к месту.
    flyingLeaf.outgoingPage.stopAnimation(L"Offset");
    flyingLeaf.outgoingPage.stopAnimation(L"RotationAngleInDegrees");
    flyingLeaf.outgoingPage.offset({0.0f, 0.0f, 0.0f});
    flyingLeaf.outgoingPage.rotationAngleInDegrees(0.0f);
    flyingLeaf.outgoingPage.isVisible(false);
    flyingLeaf.outgoingPage.brush(restBrush_);   // растр, который лист нёс, ему больше не нужен

    flyingLeaf.outgoingPageClip.stopAnimation(L"LeftInset");
    flyingLeaf.outgoingPageClip.stopAnimation(L"RightInset");
    flyingLeaf.outgoingPageClip.leftInset(-kShadowReach);
    flyingLeaf.outgoingPageClip.rightInset(-kShadowReach);
    flyingLeaf.outgoingPageClip.topInset(-kShadowReach);
    flyingLeaf.outgoingPageClip.bottomInset(-kShadowReach);

    flyingLeaf.flatLeafShadow.opacity(0.0f);
    flyingLeaf.flatLeafShadow.mask(restBrush_);

    flyingLeaf.foldShadow.stopAnimation(L"Offset");
    flyingLeaf.foldShadow.stopAnimation(L"Scale");
    flyingLeaf.foldShadow.stopAnimation(L"Opacity");
    flyingLeaf.foldShadow.isVisible(false);
    flyingLeaf.foldShadowMidpoint.stopAnimation(L"Offset");

    flyingLeaf.outerEdgeShadow.stopAnimation(L"Offset");
    flyingLeaf.outerEdgeShadow.isVisible(false);

    flyingLeaf.incomingPage.stopAnimation(L"Offset");
    flyingLeaf.incomingPage.isVisible(false);
    flyingLeaf.incomingPage.brush(restBrush_);
    flyingLeaf.bendHalftone.stopAnimation(L"Offset");
    flyingLeaf.bendHalftone.stopAnimation(L"Opacity");
    flyingLeaf.bendHalftone.opacity(1.0f);
    flyingLeaf.incomingPageClip.stopAnimation(L"LeftInset");
    flyingLeaf.incomingPageClip.stopAnimation(L"RightInset");

    // Подъём снимается с обеих сторон листа, и трапеция сходит в тождественную.
    // Не только ради вида застывшего листа: выражение, оставленное на свойстве,
    // композитор считает каждый кадр — за все свободные листы пула и без
    // всякой нужды.
    flyingLeaf.liftAmount.stopAnimation(L"Amount");
    flyingLeaf.liftAmount.insertScalar(L"Amount", 0.0f);
    flyingLeaf.outgoingPage.stopAnimation(L"TransformMatrix");
    flyingLeaf.outgoingPage.transformMatrix(identity_matrix());
    flyingLeaf.incomingPage.stopAnimation(L"TransformMatrix");
    flyingLeaf.incomingPage.transformMatrix(identity_matrix());

    flyingLeaf.active = false;
}

void LeafTurnAnimation::landLeaf(Leaf& flyingLeaf) {
    // Режим газеты: лист нёс уходящий разворот, и он больше никому не нужен —
    // осевший сменился ещё в turnStarting.
    if (!flyingLeaf.book) return;

    // Режим книги: лист нёс новый разворот, и теперь тот — осевший. Не рисуя
    // и ничем не меняясь: осевшим становится растр листа, а прежний осевший
    // освобождается, как только его перестанут носить страницы и летящие
    // листы. Так на пачку листов приходится ровно по одной отрисовке на лист и
    // ни одной сверх.
    //
    // Неперелистываемая страница переезжает на него сейчас, а не когда сядут
    // все: приходящий лист только что лёг на неё ровно этими точками (подмены
    // не видно — тот же кадр, что и сокрытие листа), а следующий лист, ещё
    // летящий, ложится уже на них. Ждать последнего значило бы показать из-под
    // него то, что было до первого. Перелистываемая страница носит растр
    // самого нового листа и здесь не трогается: когда сядет и он, осевшим
    // станет тот же растр.
    spread_.settleOn(flyingLeaf.incomingRaster);
    if (spread_.shown())
        spread_.dressPage(flyingLeaf.forward ? kLeftPage : kRightPage, flyingLeaf.incomingRaster);
}

void LeafTurnAnimation::settleLeaves() {
    for (Leaf& pooledLeaf : leafPool_)
        if (pooledLeaf.active) return;

    // Все листы сели. В режиме книги осевший разворот — разворот последнего
    // севшего, то есть самого нового (landLeaf), и обе страницы уже на нём:
    // неперелистываемую одел landLeaf, а перелистываемая носит растр самого
    // нового листа — тот самый, что стал осевшим. Одевать заново нечего:
    // осевший разворот — нынешний.
    leafLayer_.isVisible(false);
    armRelease();   // все листы свободны — отпускать пул по таймеру
}

void LeafTurnAnimation::cancelTurn() {
    // Все идущие перевороты — в покой рывком. Пакеты их концов ещё придут, но
    // каждому листу здесь меняется epoch, и ожидание конца, дождавшись, увидит
    // чужой номер и промолчит: остановленная анимация закрывает пакет так же,
    // как доигравшая. Садится при этом один самый новый: его разворот — нынешний,
    // а развороты остальных пропущены — в книге их уже нет.
    Leaf* newestLeaf = nullptr;
    for (Leaf& pooledLeaf : leafPool_)
        if (pooledLeaf.active) {
            pooledLeaf.epoch = ++leafClock_;
            finishLeaf(pooledLeaf);
            if (!newestLeaf || pooledLeaf.started > newestLeaf->started) newestLeaf = &pooledLeaf;
        }
    if (newestLeaf) landLeaf(*newestLeaf);
    settleLeaves();   // все сели: листы скрыть, пул — на отпускание
}

bool LeafTurnAnimation::hasSpareLeaves() const {
    auto const inFlight = [this](SpreadView::RasterId raster) { return rasterInFlight(raster); };
    return leafPool_.size() > 1 || spread_.spareRasters(inFlight) > 1;
}

void LeafTurnAnimation::armRelease() {
    // Один лист и один свободный растр держим всегда — на следующее листание,
    // чтобы не заводить поверхность заново. Отпускать нечего, пока их не
    // больше.
    if (!hasSpareLeaves()) return;
    releaseTimer_.stop();
    releaseTimer_.interval(kLeafReleaseFirst);
    releaseTimer_.start();
}

void LeafTurnAnimation::onReleaseTick() {
    releaseTimer_.stop();

    // Снова листают — пул нужен, ничего не трогаем. acquireLeaf таймер уже
    // остановил, но тик мог уйти в очередь раньше остановки.
    for (const Leaf& pooledLeaf : leafPool_)
        if (pooledLeaf.active) return;

    // Отпускаем один лишний лист: снимаем его визуалы со сцены и роняем. Берём
    // последний: все свободны, порядок не важен, а pop_back остальных не
    // двигает. Последний, один, лист оставляем — на следующее листание.
    if (leafPool_.size() > 1) {
        Leaf& pooledLeaf = leafPool_.back();
        VisualCollection const layerChildren = leafLayer_.children();
        layerChildren.remove(pooledLeaf.outgoingPage);
        layerChildren.remove(pooledLeaf.foldShadow);
        layerChildren.remove(pooledLeaf.outerEdgeShadow);
        layerChildren.remove(pooledLeaf.incomingPage);   // полутон изгиба — ребёнок листа, уходит с ним
        leafPool_.pop_back();
    }

    // И один лишний растр разворота — с ним уходит поверхность в размер окна.
    // Один свободный держим, как и лист.
    auto const inFlight = [this](SpreadView::RasterId raster) { return rasterInFlight(raster); };
    if (spread_.spareRasters(inFlight) > 1) spread_.dropSpareRaster(inFlight);

    // Ещё есть лишние — следующий через секунду.
    if (hasSpareLeaves()) {
        releaseTimer_.interval(kLeafReleaseStep);
        releaseTimer_.start();
    }
}

void LeafTurnAnimation::applyShadowTint() {
    const D2D1_COLOR_F& hue = shadowHue_;

    // Все кисти теней устроены одинаково: гуще всего у того, кто тень
    // отбрасывает, потом середина спада, потом ничего. Цвет во всех трёх
    // остановках один, разная только прозрачность — в том числе и в последней,
    // где она нулевая: см. tinted().
    auto paintRamp = [&hue](Collection<CompositionColorGradientStop> const& stops,
                            uint32_t dense, uint32_t middle) {
        if (stops.size() < 3) return;
        stops[0].color(tinted(hue, alphaOf(dense)));
        stops[1].color(tinted(hue, alphaOf(middle)));
        stops[2].color(tinted(hue, 0.0f));
    };

    // Тени у каждого листа пула свои — красим все. Тень уезжающего листа не
    // градиент, а размытие: тон непрозрачный, прозрачностью правит сама тень.
    for (Leaf& pooledLeaf : leafPool_) {
        paintRamp(pooledLeaf.foldShadowBrush.colorStops(), kFoldNear, kFoldMid);
        paintRamp(pooledLeaf.outerEdgeShadowBrush.colorStops(), kEdgeNear, kEdgeMid);
        paintRamp(pooledLeaf.bendHalftoneBrush.colorStops(), kBendNear, kBendMid);
        pooledLeaf.flatLeafShadow.color(tinted(hue, 1.0f));
    }
}

void LeafTurnAnimation::animateFlatTurn(Leaf& flyingLeaf, bool forward) {
    // Уезжающий лист несёт уходящую страницу — бывший осевший растр, который
    // turnStarting отдал ему, — и им же маскирует тень: в газете лист открывает
    // страницы под собой, а не повторяет их.
    flyingLeaf.outgoingPage.brush(spread_.raster(flyingLeaf.outgoingRaster).brush());
    flyingLeaf.flatLeafShadow.mask(flyingLeaf.outgoingPage.brush());

    // Поворот идёт вокруг левого края: там корешок, оттуда лист и поднимается.
    // Ставится здесь, а не раз на лист: центр преобразования принадлежит
    // плоскому листанию, а книжное его обнуляет — там лист не поворачивается, а
    // гнётся трапецией от собственного начала координат.
    flyingLeaf.outgoingPage.centerPoint({0.0f, flow_.height() * 0.5f, 0.0f});

    // Плоское листание: новые листы — под старыми. Только что заведённый лист
    // кладём в самый низ контейнера, над страницами; уже летящие остаются выше
    // и уезжают первыми, открывая тех, что под ними.
    VisualCollection const layerChildren = leafLayer_.children();
    layerChildren.remove(flyingLeaf.outgoingPage);
    layerChildren.insertAtBottom(flyingLeaf.outgoingPage);

    // Уезжает копия уходящего разворота (уходящая страница листа), открывая
    // задник — новый разворот. Уехать надо дальше собственной ширины: лист
    // поворачивается
    // вокруг левого края, и его дальний нижний угол отходит не на ширину, а на
    // гипотенузу (ширина·cos + полвысоты·sin); плюс тень, сдвинутую вправо и
    // размытую, иначе она осталась бы у края серой полоской. Вперёд лист уходит
    // влево, назад — вправо.
    const float reach =
        flow_.width() * kTurnCos + flow_.height() * 0.5f * kTurnSin + kShadowShift + kShadowBlur;
    const float to = forward ? -reach : reach;
    const float angleTo = forward ? -kTurnAngle : kTurnAngle;

    auto const turnEasing =
        compositor_.createCubicBezierEasingFunction({kEaseX1, kEaseY1}, {kEaseX2, kEaseY2});

    auto outgoingSlide = compositor_.createVector3KeyFrameAnimation();
    outgoingSlide.duration(kTurn);
    outgoingSlide.insertKeyFrame(0.0f, Vector3{0.0f, 0.0f, 0.0f}, turnEasing);
    outgoingSlide.insertKeyFrame(1.0f, Vector3{to, 0.0f, 0.0f}, turnEasing);

    auto outgoingTurn = compositor_.createScalarKeyFrameAnimation();
    outgoingTurn.duration(kTurn);
    outgoingTurn.insertKeyFrame(0.0f, 0.0f, turnEasing);
    outgoingTurn.insertKeyFrame(1.0f, angleTo, turnEasing);

    // Тень — не украшение: лист и страница под ним одного цвета, и без тени
    // глаз не видит, что один поднят над другим.
    flyingLeaf.flatLeafShadow.opacity(kShadowOpacity);
    flyingLeaf.outgoingPage.isVisible(true);

    // Конец переворота отслеживается пакетом: по нему лист освобождается в пул
    // (landWhenDone). Ждать начинаем до закрытия пакета: корутина начинается в
    // вызове, и к end() её подписка на конец уже стоит.
    auto batch = compositor_.createScopedBatch(CompositionBatchTypes::Animation);
    flyingLeaf.outgoingPage.startAnimation(L"Offset", outgoingSlide);
    flyingLeaf.outgoingPage.startAnimation(L"RotationAngleInDegrees", outgoingTurn);
    landWhenDone(batch, &flyingLeaf, flyingLeaf.epoch);
    batch.end();
}

void LeafTurnAnimation::animateBookTurn(Leaf& flyingLeaf, bool forward) {
    // Уходит старый разворот, и в обе стороны он остаётся сверху: книжное
    // листание снимает верхнюю бумагу с неподвижной стопки, а не увозит
    // страницу за край. У переворота две стороны разной давности: под снимаемой
    // бумагой с первого же кадра открывается НОВАЯ страница, а неперелистываемая
    // остаётся СТАРОЙ, пока приходящий лист её не накроет. Для того страницы и
    // два визуала: перелистываемую переставляем на новый разворот сейчас,
    // другую — когда лист сядет (landLeaf).
    SpriteVisual const& outgoingPage = flyingLeaf.outgoingPage;
    InsetClip const& outgoingPageClip = flyingLeaf.outgoingPageClip;
    SpriteVisual const& incomingPage = flyingLeaf.incomingPage;
    InsetClip const& incomingPageClip = flyingLeaf.incomingPageClip;
    SpriteVisual const& foldShadow = flyingLeaf.foldShadow;
    SpriteVisual const& outerEdgeShadow = flyingLeaf.outerEdgeShadow;
    const size_t flippingSide = forward ? kRightPage : kLeftPage;
    SpriteVisual const& flippingPage = spread_.pageVisual(flippingSide);

    // Уходящий лист несёт СТАРУЮ перелистываемую страницу — ту, что показана
    // прямо сейчас: её кисть и забираем у страницы-визуала, прежде чем одеть ту
    // в новое. Своей отрисовки у уходящего нет, и какой из летящих листов или
    // осевший разворот эту кисть дал — неважно: страница уже носит верную. Её
    // растр лист помнит: пока он летит, растр не свободен.
    outgoingPage.brush(flippingPage.brush());
    flyingLeaf.outgoingRaster = spread_.pageRaster(flippingSide);

    // Обе НОВЫЕ страницы — из растра листа, куда redraw нарисовал новый
    // разворот: перелистываемая страница-визуал одевается в него с первого
    // кадра (из-под снимаемой бумаги открывается новое), приходящий лист несёт
    // ту, что ляжет на неперелистываемую. Кисть DrawingSurface делает новую на
    // каждый вызов, а поверхность за ней та же — второй отрисовки не возникает.
    spread_.dressPage(flippingSide, flyingLeaf.incomingRaster);
    incomingPage.brush(spread_.raster(flyingLeaf.incomingRaster).brush());

    // Стопка листов: снимаемая бумага нового листа — ПОД всеми летящими, а
    // приходящий лист — НАД ними. Летящие подняты раньше и висят выше, так что
    // в правой половине сверху лежит самый старый — и его тень сгиба падает на
    // бумагу, которую снимают следом; а снятый последним ляжет на левую стопку
    // последним, сверху. Первая версия клала весь новый лист поверх старых, и
    // читатель видел это как «нижняя страница вылезла на передний план»: его
    // бумага — те же точки, что только что показывала правая страница, — с
    // первого кадра накрывала всё, что летело. Друг друга по развороту листы
    // не затирают, потому что каждый несёт лишь свою перелистываемую страницу
    // (см. крой ниже), а не весь разворот. Тень сгиба — под бумагой, тень
    // наружного края — под приходящим листом.
    VisualCollection const layerChildren = leafLayer_.children();
    layerChildren.remove(outgoingPage);
    layerChildren.insertAtBottom(outgoingPage);
    layerChildren.remove(foldShadow);
    layerChildren.insertAtBottom(foldShadow);
    layerChildren.remove(outerEdgeShadow);
    layerChildren.insertAtTop(outerEdgeShadow);
    layerChildren.remove(incomingPage);
    layerChildren.insertAtTop(incomingPage);

    const float leftPage = flow_.spine();
    const float rightPage = flow_.width() - leftPage;

    // Кромка уходящего листа доходит до корешка: вперёд едет правая, назад —
    // левая.
    const float travel = forward ? rightPage : leftPage;
    const wchar_t* const crawlingInset = forward ? L"RightInset" : L"LeftInset";

    // Лист несёт только перелистываемую страницу: вперёд — правую (левую половину
    // разворота отрезаем к корешку неподвижным отступом), назад — левую. Иначе
    // неперелистываемая половина каждого листа затирала бы соседние листы при
    // быстром листании внахлёст; за неё отвечает своя страница-визуал.
    if (forward)
        outgoingPageClip.leftInset(leftPage);
    else
        outgoingPageClip.rightInset(rightPage);

    // Лист поднимается к глазу: у корешка он в своих размерах, к свободному
    // краю растёт трапецией. Корешок у обеих сторон общий, а свободные края
    // противоположны — это один и тот же край бумаги, только у снимаемой
    // стороны он ещё снаружи перелистываемой страницы, а у приходящей уже
    // перевёрнут на другую сторону разворота. Центр преобразования при этом
    // обнуляется: трапеция задана от начала координат листа, а центр нужен
    // только плоскому листанию, где вокруг него идёт поворот.
    outgoingPage.centerPoint({0.0f, 0.0f, 0.0f});
    liftPage(flyingLeaf.outgoingLiftExpression, flyingLeaf.liftAmount, outgoingPage, leftPage,
             forward ? flow_.width() : 0.0f, flow_.height());
    liftPage(flyingLeaf.incomingLiftExpression, flyingLeaf.liftAmount, incomingPage, leftPage,
             forward ? 0.0f : flow_.width(), flow_.height());

    // Пологая S-кривая: рука, тянущая бумагу, слегка разгоняется в начале и
    // тормозит к корешку — не роняет тяжесть, но и не тянет мёртво-равномерно
    // (kEase*).
    auto const turnEasing =
        compositor_.createCubicBezierEasingFunction({kEaseX1, kEaseY1}, {kEaseX2, kEaseY2});

    auto outgoingClipCrawl = compositor_.createScalarKeyFrameAnimation();
    outgoingClipCrawl.duration(kLeafSlide);
    outgoingClipCrawl.insertKeyFrame(0.0f, 0.0f, turnEasing);
    outgoingClipCrawl.insertKeyFrame(1.0f, travel, turnEasing);

    // Тень едет за кромкой той же прямой и той же длительности. Обе анимации
    // уходят в один коммит, обе — функции нормализованного времени от общего
    // начала, поэтому разойтись им не на чем.
    //
    // Полоска лежит за кромкой: вперёд — справа от неё, назад — слева, и
    // тогда же разворачивается градиент, чтобы гуще было у бумаги.
    const float spreadEdge = forward ? flow_.width() : 0.0f;
    const float lead = forward ? 0.0f : -flow_.width();
    const float direction = forward ? 1.0f : -1.0f;

    flyingLeaf.foldShadowBrush.startPoint({forward ? 0.0f : 1.0f, 0.0f});
    flyingLeaf.foldShadowBrush.endPoint({forward ? 1.0f : 0.0f, 0.0f});

    auto foldFollow = compositor_.createVector3KeyFrameAnimation();
    foldFollow.duration(kLeafSlide);
    foldFollow.insertKeyFrame(0.0f, Vector3{spreadEdge + lead, 0.0f, 0.0f}, turnEasing);
    foldFollow.insertKeyFrame(1.0f, Vector3{spreadEdge + lead - direction * travel, 0.0f, 0.0f},
                              turnEasing);

    // По ходу переворота тень расходится и светлеет. Растёт она от той своей
    // стороны, что прижата к бумаге: центр преобразования стоит на кромке, и
    // полоска раздаётся от неё наружу, никуда не съезжая. Градиент растянут в
    // долях полоски, так что вместе с ней растягивается и спад — половину
    // мягкости даёт уже одно это.
    foldShadow.centerPoint({forward ? 0.0f : flow_.width(), 0.0f, 0.0f});

    // Полоска нарезана целым разворотом, а ширину тени задаёт доля от неё:
    // от нуля в начале до целой страницы в конце. Прямая через эти две точки
    // проходит серединой ровно по половине страницы — то, чем переворот и
    // меряется на глаз.
    const float span = flow_.width() > 0.0f ? leftPage * kFoldOfPage / flow_.width() : 0.0f;

    auto foldWiden = compositor_.createVector3KeyFrameAnimation();
    foldWiden.duration(kLeafSlide);
    foldWiden.insertKeyFrame(0.0f, Vector3{0.0f, 1.0f, 1.0f}, turnEasing);
    foldWiden.insertKeyFrame(1.0f, Vector3{span, 1.0f, 1.0f}, turnEasing);

    // Последним движением тень сходит на нет — и открывает нарисованный
    // средник, который всё это время лежал на самой странице. Это и есть
    // подмена: к концу переворота справа от корешка остаётся ровно то же, что
    // и слева, а движущейся тени, которой там больше нечего делать, не
    // остаётся вовсе.
    auto foldLighten = compositor_.createScalarKeyFrameAnimation();
    foldLighten.duration(kLeafSlide);
    foldLighten.insertKeyFrame(0.0f, kFoldDense, turnEasing);
    foldLighten.insertKeyFrame(kHandover, kFoldFaint, turnEasing);
    foldLighten.insertKeyFrame(1.0f, 0.0f, turnEasing);

    // Вторую половину мягкости даёт середина спада: пока она у кромки, тень
    // читается краем, а уехав к дальнему концу — размывается целиком.
    auto foldMidFlatten = compositor_.createScalarKeyFrameAnimation();
    foldMidFlatten.duration(kLeafSlide);
    foldMidFlatten.insertKeyFrame(0.0f, kFoldMidStop, turnEasing);
    foldMidFlatten.insertKeyFrame(1.0f, kFoldMidSoft, turnEasing);

    // Приходящий лист выезжает из-под уходящего, держась правым краем за его
    // кромку (назад — левым за левую), и растёт от нулевой ширины до целой
    // страницы. К последнему кадру он встаёт ровно на её место, и разворот в
    // конце анимации целиком новый — ни одного чужого столбца.
    //
    // Устроен он двумя прямыми. Лист лежит целым разворотом, сдвинутым к
    // кромке, а видно его через окно, у которого внешний край страницы открыт
    // с самого начала, а сторона у корешка закрыта и открывается по ходу: лист
    // выезжает наружным краем вперёд, а не разворачивается от корешка.
    // На середине окно доходит до половины страницы ровно тогда, когда кромка
    // проходит половину своего пути, — потому старая правая страница там и
    // скрывается целиком, ни раньше ни позже.
    const float shift = forward ? flow_.width() : -flow_.width();
    const wchar_t* const openingInset = forward ? L"RightInset" : L"LeftInset";
    const float openTo = forward ? rightPage : leftPage;

    // Внешняя сторона окна стоит на краю разворота и не двигается; та, что у
    // корешка, открывает страницу от края к нему.
    incomingPageClip.leftInset(forward ? 0.0f : flow_.width());
    incomingPageClip.rightInset(forward ? flow_.width() : 0.0f);

    auto incomingSlide = compositor_.createVector3KeyFrameAnimation();
    incomingSlide.duration(kLeafSlide);
    incomingSlide.insertKeyFrame(0.0f, Vector3{shift, 0.0f, 0.0f}, turnEasing);
    incomingSlide.insertKeyFrame(1.0f, Vector3{0.0f, 0.0f, 0.0f}, turnEasing);

    auto incomingClipOpen = compositor_.createScalarKeyFrameAnimation();
    incomingClipOpen.duration(kLeafSlide);
    incomingClipOpen.insertKeyFrame(0.0f, flow_.width(), turnEasing);
    incomingClipOpen.insertKeyFrame(1.0f, openTo, turnEasing);

    // Тень наружного края идёт за самим краем. Край — это ведущая сторона
    // листа, и едет она через всё окно: от дальнего края разворота до
    // ближнего, то есть ровно вдвое быстрее кромки.
    const float band = flow_.width() * kEdgeOfWindow;
    const float behind = forward ? -band : 0.0f;

    flyingLeaf.outerEdgeShadowBrush.startPoint({forward ? 1.0f : 0.0f, 0.0f});
    flyingLeaf.outerEdgeShadowBrush.endPoint({forward ? 0.0f : 1.0f, 0.0f});

    auto edgeTrail = compositor_.createVector3KeyFrameAnimation();
    edgeTrail.duration(kLeafSlide);
    edgeTrail.insertKeyFrame(0.0f, Vector3{spreadEdge + behind, 0.0f, 0.0f}, turnEasing);
    edgeTrail.insertKeyFrame(
        1.0f, Vector3{spreadEdge - direction * flow_.width() + behind, 0.0f, 0.0f},
        turnEasing);

    // Полутон изгиба живёт в координатах самого листа, а не полосы: лист
    // едет, и вместе с ним едет всё, что на нём нарисовано. Двигаться ему
    // остаётся ровно настолько, насколько открывается окно кроя, — тем он и
    // держится у той стороны листа, что уходит в сгиб.
    const float bendWidth = flow_.width() * kBendOfWindow;
    const float bendFrom = forward ? -bendWidth : flow_.width();
    const float bendTo = forward ? leftPage - bendWidth : leftPage;

    flyingLeaf.bendHalftoneBrush.startPoint({forward ? 1.0f : 0.0f, 0.0f});
    flyingLeaf.bendHalftoneBrush.endPoint({forward ? 0.0f : 1.0f, 0.0f});

    auto bendCurve = compositor_.createVector3KeyFrameAnimation();
    bendCurve.duration(kLeafSlide);
    bendCurve.insertKeyFrame(0.0f, Vector3{bendFrom, 0.0f, 0.0f}, turnEasing);
    bendCurve.insertKeyFrame(1.0f, Vector3{bendTo, 0.0f, 0.0f}, turnEasing);

    // Полутон изгиба подменяется тем же нарисованным средником и тем же
    // движением: к последнему кадру приходящий лист встаёт на место, и его
    // собственная половина средника оказывается ровно там, где полутон только
    // что был. Не гасить его — значит удвоить тень вдвое против левой.
    auto bendSettle = compositor_.createScalarKeyFrameAnimation();
    bendSettle.duration(kLeafSlide);
    bendSettle.insertKeyFrame(0.0f, 1.0f, turnEasing);
    bendSettle.insertKeyFrame(kHandover, 1.0f, turnEasing);
    bendSettle.insertKeyFrame(1.0f, 0.0f, turnEasing);

    // Доля подъёма — вверх к середине переворота и обратно вниз. Ею одной
    // ведутся обе трапеции: все числа матрицы линейны по этой доле, и
    // композитор пересчитывает их сам (kLiftFormula).
    auto const liftEasing = compositor_.createCubicBezierEasingFunction(
        {kLiftEaseX1, kLiftEaseY1}, {kLiftEaseX2, kLiftEaseY2});

    auto liftRise = compositor_.createScalarKeyFrameAnimation();
    liftRise.duration(kLeafSlide);
    liftRise.insertKeyFrame(0.0f, 0.0f, liftEasing);
    liftRise.insertKeyFrame(0.5f, 1.0f, liftEasing);
    liftRise.insertKeyFrame(1.0f, 0.0f, liftEasing);

    outgoingPage.isVisible(true);
    foldShadow.isVisible(true);
    outerEdgeShadow.isVisible(true);
    incomingPage.isVisible(true);

    // Конец отслеживается пакетом: по нему лист освобождается в пул
    // (landWhenDone). Пакет закрывает и остановленную анимацию (лист добили при
    // быстром листании или сбросили при перевёрстке), поэтому ожидание конца
    // проверяет, свой ли это лист, — по epoch: у добитого он уже сменился, и
    // ожидание узнаёт свой номер чужим и молчит.
    auto batch = compositor_.createScopedBatch(CompositionBatchTypes::Animation);

    outgoingPageClip.startAnimation(crawlingInset, outgoingClipCrawl);
    foldShadow.startAnimation(L"Offset", foldFollow);
    foldShadow.startAnimation(L"Scale", foldWiden);
    foldShadow.startAnimation(L"Opacity", foldLighten);
    flyingLeaf.foldShadowMidpoint.startAnimation(L"Offset", foldMidFlatten);
    outerEdgeShadow.startAnimation(L"Offset", edgeTrail);
    incomingPage.startAnimation(L"Offset", incomingSlide);
    incomingPageClip.startAnimation(openingInset, incomingClipOpen);
    flyingLeaf.bendHalftone.startAnimation(L"Offset", bendCurve);
    flyingLeaf.bendHalftone.startAnimation(L"Opacity", bendSettle);
    flyingLeaf.liftAmount.startAnimation(L"Amount", liftRise);

    // Ждать конца — до закрытия пакета, как и в плоском листании.
    landWhenDone(batch, &flyingLeaf, flyingLeaf.epoch);
    batch.end();
}

// Пакет закрывается не только доигравшей анимацией, но и остановленной: лист
// добили при быстром листании или сбросили перевёрсткой, и конец пакета
// приходит к листу, который уже служит другому перевороту. Свой ли это конец,
// говорит epoch — у добитого листа он сменился.
//
// Стратегии к концу пакета может уже не быть: её сменили или окно закрыли
// посреди переворота. Корутина её жизни не проверяет — ожидание стоит под
// токеном стратегии, а его отменяет владелец: деструктор стратегии, а при
// закрытии окна ещё раньше — общий источник приложения. Отменённое ожидание
// кончается operation_canceled_exception, обычным концом detached_task, даже
// если конец пакета пришёл уже после отмены, — и до `this` и `landingLeaf` дело не
// доходит. Кадр стратегию не держит (она не самосчитающая), и знак жизни ему
// не нужен.
async::detached_task LeafTurnAnimation::landWhenDone(CompositionScopedBatch batch, Leaf* landingLeaf,
                                                     uint64_t epoch) {
    co_await dsl::onCompleted(batch, stop_.token());

    if (landingLeaf->epoch != epoch) co_return;

    finishLeaf(*landingLeaf);
    landLeaf(*landingLeaf);
    settleLeaves();
}

}  // namespace bukvitsa::reader
