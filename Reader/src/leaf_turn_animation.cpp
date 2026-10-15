// Свой заголовок первым: он ведёт к импорту модуля книги, после которого
// стандартный заголовок MSVC уже не принимает. Ожидание конца пакета под
// токеном (dsl::onCompleted) — из event_awaitable.h.
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
/// копим — старейший переворот кончается рывком. Каждый лист в полёте держит
/// растр разворота в размер окна, то есть память (десяток с лишним листов —
/// сотня-другая мегабайт), поэтому число хоть и щедрое, но не бесконечное.
constexpr size_t kMaxLeaves = 16;

/// Отпускание пула листов в простое. Первый лишний лист роняем через долгую
/// паузу — вдруг читатель тут же листнёт снова и пул понадобится сразу; дальше
/// по одному в секунду, пока не останется один (его держим на следующее
/// листание, чтобы не заводить поверхность заново). Так пик памяти держится
/// лишь на время листания, а в покое от пула остаются один лист и один
/// свободный растр. Паузы две, и таймера два: интервал у WaitableTimer свой на
/// всю его жизнь.
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

/// Ставит числа трапеции одной стороны листа: у `hinge` она остаётся в своих
/// размерах, у `freeEdge` вырастает в kLiftPeak раз — во столько, сколько
/// скажет Amount в `liftAmount`.
///
/// Обе стороны листа — снимаемая бумага и приходящий лист — берут общий
/// корешок и свои, противоположные, свободные края. Оттого они и сходятся на
/// сгибе в одной высоте: точка на расстоянии s от корешка поднята одинаково, с
/// какой бы стороны бумаги она ни была, а сгиб — это одна точка, видимая с
/// обеих. Ничего согласовывать для этого не нужно, так выходит само.
void setLiftGeometry(ExpressionAnimation const& liftExpression,
                     CompositionPropertySet const& liftAmount, float hinge, float freeEdge,
                     float height) {
    const float slant = 1.0f / kLiftPeak - 1.0f;
    liftExpression.setReferenceParameter(L"lift", liftAmount);
    liftExpression.setScalarParameter(L"Slant", slant);
    liftExpression.setScalarParameter(L"Fall", slant / (freeEdge - hinge));
    liftExpression.setScalarParameter(L"Half", height * 0.5f);
    liftExpression.setScalarParameter(L"Hinge", hinge);
}

}  // namespace

/* ---------------- лист ---------------- */

LeafTurnAnimation::Leaf::Leaf(const Compositor& compositor, const ContainerVisual& leafLayer,
                              const CompositionBrush& restBrush)
    : outgoingPage(compositor.createSpriteVisual()),
      outgoingPageClip(compositor.createInsetClip()),
      flatLeafShadow(compositor.createDropShadow()),
      incomingPage(compositor.createSpriteVisual()),
      incomingPageClip(compositor.createInsetClip()),
      bendHalftone(compositor.createSpriteVisual()),
      bendHalftoneBrush(compositor.createLinearGradientBrush()),
      foldShadow(compositor.createSpriteVisual()),
      foldShadowBrush(compositor.createLinearGradientBrush()),
      foldShadowMidpoint(compositor.createColorGradientStop(kFoldMidStop, colors.transparent)),
      outerEdgeShadow(compositor.createSpriteVisual()),
      outerEdgeShadowBrush(compositor.createLinearGradientBrush()),
      // Доля подъёма — скаляр в собственных свойствах уходящей страницы, а две
      // трапеции читают её выражениями. Числа геометрии ставит начало каждого
      // книжного переворота (lift): корешок и свободный край зависят от
      // стороны листания, а размеры — от окна.
      liftAmount(outgoingPage.properties()),
      outgoingLiftExpression(compositor.createExpressionAnimation(kLiftFormula)),
      incomingLiftExpression(compositor.createExpressionAnimation(kLiftFormula)),
      restBrush_(restBrush) {
    outgoingPage.clip(outgoingPageClip);

    // Тень уезжающего плоского листа повторяет лист, а не описанный
    // прямоугольник: маска ей — содержимое самой страницы, а не своя кисть.
    // Так у листа одна ссылка на растр — кисть страницы, — и тень всегда
    // повторяет ровно то, что лист носит; покой, сняв кисть, отпускает растр
    // целиком. Своя кисть маски держала бы растр мимо пула и мимо resize.
    // Гасится и зажигается прозрачностью.
    flatLeafShadow.blurRadius(kShadowBlur);
    flatLeafShadow.offset({kShadowShift, 0.0f, 0.0f});
    flatLeafShadow.sourcePolicy(CompositionDropShadowSourcePolicy::InheritFromVisualContent);
    outgoingPage.shadow(flatLeafShadow);

    // Тень сгиба (книжное листание): горизонтальный градиент в долях своей
    // ширины, поэтому полоске достаточно ездить — перекрашивать не приходится.
    foldShadowBrush.startPoint({0.0f, 0.0f});
    foldShadowBrush.endPoint({1.0f, 0.0f});
    foldShadowBrush.colorStops().append(
        compositor.createColorGradientStop(0.0f, colors.transparent));
    foldShadowBrush.colorStops().append(foldShadowMidpoint);
    foldShadowBrush.colorStops().append(
        compositor.createColorGradientStop(1.0f, colors.transparent));
    foldShadow.brush(foldShadowBrush);

    // Тень наружного края приходящей страницы: узкая и неизменная, градиент
    // развёрнут — густо у листа, прозрачно прочь.
    outerEdgeShadowBrush.colorStops().append(
        compositor.createColorGradientStop(0.0f, colors.transparent));
    outerEdgeShadowBrush.colorStops().append(
        compositor.createColorGradientStop(kEdgeMidStop, colors.transparent));
    outerEdgeShadowBrush.colorStops().append(
        compositor.createColorGradientStop(1.0f, colors.transparent));
    outerEdgeShadow.brush(outerEdgeShadowBrush);

    // Приходящая страница (кисть ставит переворот — растр нового разворота) и
    // полутон изгиба у сгиба: её ребёнок, кроится вместе с ней.
    incomingPage.clip(incomingPageClip);
    bendHalftoneBrush.colorStops().append(
        compositor.createColorGradientStop(0.0f, colors.transparent));
    bendHalftoneBrush.colorStops().append(
        compositor.createColorGradientStop(kBendMidStop, colors.transparent));
    bendHalftoneBrush.colorStops().append(
        compositor.createColorGradientStop(1.0f, colors.transparent));
    bendHalftone.brush(bendHalftoneBrush);
    incomingPage.children().insertAtTop(bendHalftone);

    // Все визуалы листа — в слой; их Z на каждый переворот уточняет анимация
    // (плоское — под старые, книжное — над старыми).
    VisualCollection const layerChildren = leafLayer.children();
    layerChildren.insertAtTop(outgoingPage);
    layerChildren.insertAtTop(foldShadow);
    layerChildren.insertAtTop(outerEdgeShadow);
    layerChildren.insertAtTop(incomingPage);

    rest();
}

void LeafTurnAnimation::Leaf::fit(float width, float height) const {
    outgoingPage.size({width, height});
    // Полоска тени сгиба меряется целым разворотом (её ужимает Scale по ходу),
    // приходящая страница — тоже; тени края и полутон изгиба — постоянной
    // ширины в долях окна.
    foldShadow.size({width, height});
    incomingPage.size({width, height});
    outerEdgeShadow.size({width * kEdgeOfWindow, height});
    bendHalftone.size({width * kBendOfWindow, height});
}

void LeafTurnAnimation::Leaf::tint(const D2D1_COLOR_F& hue) const {
    // Все кисти теней устроены одинаково: гуще всего у того, кто тень
    // отбрасывает, потом середина спада, потом ничего. Цвет во всех трёх
    // остановках один, разная только прозрачность — в том числе и в последней,
    // где она нулевая: см. tinted().
    auto const paintRamp = [&hue](Collection<CompositionColorGradientStop> const& stops,
                                  uint32_t dense, uint32_t middle) {
        if (stops.size() < 3) return;
        stops[0].color(tinted(hue, alphaOf(dense)));
        stops[1].color(tinted(hue, alphaOf(middle)));
        stops[2].color(tinted(hue, 0.0f));
    };
    paintRamp(foldShadowBrush.colorStops(), kFoldNear, kFoldMid);
    paintRamp(outerEdgeShadowBrush.colorStops(), kEdgeNear, kEdgeMid);
    paintRamp(bendHalftoneBrush.colorStops(), kBendNear, kBendMid);

    // Тень уезжающего листа не градиент, а размытие: тон непрозрачный,
    // прозрачностью правит сама тень.
    flatLeafShadow.color(tinted(hue, 1.0f));
}

void LeafTurnAnimation::Leaf::lift(float hinge, float outgoingFreeEdge, float incomingFreeEdge,
                                   float height) {
    // Центр преобразования обнуляется: трапеция задана от начала координат
    // листа, а центр нужен только плоскому листанию, где вокруг него идёт
    // поворот.
    outgoingPage.centerPoint({0.0f, 0.0f, 0.0f});

    setLiftGeometry(outgoingLiftExpression, liftAmount, hinge, outgoingFreeEdge, height);
    start(outgoingPage, L"TransformMatrix", outgoingLiftExpression);
    setLiftGeometry(incomingLiftExpression, liftAmount, hinge, incomingFreeEdge, height);
    start(incomingPage, L"TransformMatrix", incomingLiftExpression);
}

void LeafTurnAnimation::Leaf::start(const CompositionObject& target, const wchar_t* property,
                                    const CompositionAnimation& animation) {
    target.startAnimation(property, animation);
    started_.push_back({target, property});
}

void LeafTurnAnimation::Leaf::stop() {
    // Сначала снять анимации, потом писать: пока анимация на свойстве жива,
    // прямая запись до него не доходит, и недоехавший лист доехал бы позже уже
    // не к месту. Сняв анимацию, композитор оставляет свойству последнее
    // значение, а не прежнее: то, что видно или стоит между переворотами,
    // возвращает объявленный покой (rest); чего он не возвращает и почему —
    // там же, одним списком. Выражения подъёма снимаются здесь же: оставленное
    // на свойстве, композитор считал бы его каждый кадр — за все свободные
    // листы пула и без всякой нужды.
    for (const StartedAnimation& startedAnimation : started_)
        startedAnimation.target.stopAnimation(startedAnimation.property);
    started_.clear();
    rest();
}

void LeafTurnAnimation::Leaf::rest() const {
    // Покой объявляет то, что видно или стоит между переворотами: скрытость
    // визуалов, кисти страниц, прозрачность тени листа и полутона изгиба,
    // место и поворот плоского листа, крой уходящей страницы, подъём и
    // матрицы трапеций. Ничего не видно и ничего не носится: растр, который
    // лист нёс, ему больше не нужен — кисть покоя его отпускает, а другой
    // ссылки на растр у листа нет (тень берёт маску из содержимого страницы).
    //
    // Остальное, что трогает переворот, покой не возвращает: оно скрыто вместе
    // с визуалами, а следующий переворот ставит его сам до пуска или начинает
    // с кадра 0 своих анимаций. Это размеры визуалов (fit), центры
    // преобразования уходящей страницы и тени сгиба, порядок визуалов в слое,
    // Offset тени сгиба, тени наружного края, приходящей страницы и полутона
    // изгиба, Scale и Opacity тени сгиба, середина спада (Offset
    // foldShadowMidpoint), отступы кроя приходящей страницы и точки градиентов
    // трёх теней.
    outgoingPage.isVisible(false);
    outgoingPage.brush(restBrush_);
    foldShadow.isVisible(false);
    outerEdgeShadow.isVisible(false);
    incomingPage.isVisible(false);
    incomingPage.brush(restBrush_);
    flatLeafShadow.opacity(0.0f);

    // Плоский лист — на месте и ровно.
    outgoingPage.offset({0.0f, 0.0f, 0.0f});
    outgoingPage.rotationAngleInDegrees(0.0f);

    // Крой в покое отпущен на вылет тени: нулевые отступы — ровно лист, а тени
    // положено лежать за его краем.
    outgoingPageClip.leftInset(-kShadowReach);
    outgoingPageClip.rightInset(-kShadowReach);
    outgoingPageClip.topInset(-kShadowReach);
    outgoingPageClip.bottomInset(-kShadowReach);

    // Подъёма нет — трапеции тождественны, полутон изгиба на своей плотности.
    liftAmount.insertScalar(L"Amount", 0.0f);
    outgoingPage.transformMatrix(identity_matrix());
    incomingPage.transformMatrix(identity_matrix());
    bendHalftone.opacity(1.0f);
}

void LeafTurnAnimation::Leaf::detach(const ContainerVisual& leafLayer) const {
    VisualCollection const layerChildren = leafLayer.children();
    layerChildren.remove(outgoingPage);
    layerChildren.remove(foldShadow);
    layerChildren.remove(outerEdgeShadow);
    // Полутон изгиба — ребёнок приходящей страницы, уходит с ней.
    layerChildren.remove(incomingPage);
}

/* ---------------- перевороты ---------------- */

LeafTurnAnimation::LeafTurnAnimation(const Compositor& compositor, const ContainerVisual& leafLayer,
                                     const DispatcherQueue& queue, SpreadView& spread,
                                     const PageFlow& flow,
                                     const async::cancellation_source& readingScope,
                                     async::cancellation_token stripStop)
    : compositor_(compositor),
      leafLayer_(leafLayer),
      spread_(spread),
      flow_(flow),
      readingScope_(readingScope),
      restBrush_(compositor.createColorBrush(rgba(0, 0, 0, 0))),
      turnSeries_(readingScope.token()),
      releaseStop_(std::move(stripStop)),
      releasePauses_(make_refcounted<ReleasePauses>(queue, releaseStop_.token())) {
    // Отпускание пула — на всю жизнь стратегии: цикл тут же встаёт на долгую
    // паузу и до первой посадки ничего не трогает.
    releaseIdleLeaves(releasePauses_, releaseStop_.token());
}

LeafTurnAnimation::~LeafTurnAnimation() {
    // Ожидания этой стратегии — концы переворотов и паузы отпускания пула —
    // больше никому не нужны: её сменили или полоса умирает. Отменяет их
    // владелец — она сама; корутины кончатся отменой ходом очереди и стратегии
    // уже не тронут, а таймеры, на которых стоит цикл, доживут до конца его
    // кадра (ReleasePauses).
    turnSeries_.cancel();
    releaseStop_.cancel();
}

void LeafTurnAnimation::turnStarting(bool forward) {
    // Встречное листание сперва доводит до конца всё, что летит, — в обоих
    // режимах: серия — перевороты в одну сторону. В книге бумага снимается с
    // осевшего разворота, а у летящего листа перелистываемая страница ещё в
    // воздухе: снимать с неё нечего, и лист, пущенный навстречу поверх
    // летящих, показывал бы под собой то, чего в книге уже нет. В газете копия
    // одного и того же листа летела бы в разные стороны разом. Осевшим
    // становится разворот самого нового (finishAll); листы в ту же сторону
    // остаются лететь внахлёст.
    if (!liveTurns_.empty() && liveTurns_.back().forward != forward) finishAll();

    // Первый переворот серии заводит её источник — под нынешним источником
    // чтения: тот полоса заводит заново при каждом входе.
    if (liveTurns_.empty()) turnSeries_ = async::cancellation_source{readingScope_.token()};

    // Отпускание пула таймеров не трогает: цикл, проснувшись, сам увидит, что
    // листают, и не отпустит ничего, пока все не сядут.

    // Лист возьмётся свободный, новый или у старейшего переворота — см.
    // acquireLeaf. Режим решаем сразу: в книге (две колонки) есть корешок и
    // переворот листа у него, в газете (одна колонка, три с лишним) уезжает
    // целая страница, и растры лист носит разные.
    const size_t leafIndex = acquireLeaf();
    const bool bookMode = flow_.columns() == 2;
    const size_t flippingSide = forward ? kRightPage : kLeftPage;

    // Уходящая страница — старая, и она уже растеризована: в книге её
    // показывает перелистываемая страница, в газете — обе, осевшим растром.
    // Лист её заимствует, а не рисует, — пока книга ещё стоит на ней.
    liveTurns_.push_back(Turn{
        .leafIndex = leafIndex,
        .forward = forward,
        .bookMode = bookMode,
        .stop = async::cancellation_source{turnSeries_.token()},
        .outgoingRaster = bookMode ? spread_.pageRaster(flippingSide) : spread_.settledRaster(),
    });

    if (bookMode) {
        // Книга: неперелистываемая страница (при листании вперёд левая)
        // остаётся на старом развороте, пока приходящий лист её не накроет.
        // Новых страниц у переворота две — оборот снимаемой бумаги и та, что
        // открывается под ней, — то есть ровно один новый разворот: он ляжет в
        // свободный растр листа единственной отрисовкой на переворот. С этой
        // минуты нынешний разворот несёт самый новый лист (drawTarget).
        liveTurns_.back().incomingRaster = acquireRaster();
    } else {
        // Газета: уезжающий лист несёт бывший осевший растр, а осевшим
        // становится свободный — в него полоса нарисует новый разворот после
        // шага книги и оденет в него страницы.
        spread_.settleOn(acquireRaster());
    }
}

void LeafTurnAnimation::turnStarted(bool forward) {
    // Переворот, который только что завёл turnStarting.
    Turn& startingTurn = liveTurns_.back();
    Leaf& startingLeaf = leafPool_[startingTurn.leafIndex];
    startingLeaf.fit(flow_.width(), flow_.height());

    if (startingTurn.bookMode) {
        // Уходящая страница листа несёт СТАРУЮ перелистываемую — ту, что
        // показана прямо сейчас: её кисть и забираем у страницы-визуала, прежде
        // чем одеть ту в новое. Своей отрисовки у уходящей нет, и какой из
        // летящих листов или осевший разворот эту кисть дал — неважно: страница
        // уже носит верную.
        const size_t flippingSide = forward ? kRightPage : kLeftPage;
        startingLeaf.outgoingPage.brush(spread_.pageVisual(flippingSide).brush());

        // Обе НОВЫЕ страницы — из растра листа, куда полоса только что
        // нарисовала новый разворот: перелистываемая страница-визуал одевается
        // в него с первого кадра (из-под снимаемой бумаги открывается новое),
        // приходящая страница листа несёт ту, что ляжет на неперелистываемую.
        // Кисть DrawingSurface делает новую на каждый вызов, а поверхность за
        // ней та же — второй отрисовки не возникает.
        spread_.dressPage(flippingSide, startingTurn.incomingRaster);
        startingLeaf.incomingPage.brush(spread_.raster(startingTurn.incomingRaster).brush());
    } else {
        // Уезжающий лист несёт уходящую страницу — бывший осевший растр: в
        // газете лист открывает страницы под собой, а не повторяет их. Тень
        // берёт форму из неё же сама.
        startingLeaf.outgoingPage.brush(spread_.raster(startingTurn.outgoingRaster).brush());
    }

    leafLayer_.isVisible(true);   // листы — на время переворота
    flyTurn(startingTurn.leafIndex, forward, startingTurn.bookMode, startingTurn.stop.token());
}

void LeafTurnAnimation::finishAll() {
    // Серия кончается вся: её источник отменён и больше не нужен — следующая
    // заведёт свой. Отмена кончает ожидания корутин ходом очереди, а покой и
    // посадка нужны сейчас, в этой строке: на них стоит следующая.
    turnSeries_.cancel();
    if (liveTurns_.empty()) {
        settle();
        return;
    }
    finishTurn(liveTurns_.size() - 1);
}

SpreadView::RasterId LeafTurnAnimation::drawTarget() const {
    // Посреди книжного переворота нынешний разворот несёт растр самого нового
    // листа: перелистываемая страница и приходящая страница листа смотрят на
    // него и обновятся сами, а неперелистываемой до посадки положено старое.
    if (!liveTurns_.empty() && liveTurns_.back().bookMode) return liveTurns_.back().incomingRaster;
    return spread_.settledRaster();
}

void LeafTurnAnimation::tintShadows(const D2D1_COLOR_F& hue) {
    // Тени у каждого листа пула свои — красим все, и летящие тоже.
    shadowHue_ = hue;
    for (const Leaf& pooledLeaf : leafPool_) pooledLeaf.tint(hue);
}

size_t LeafTurnAnimation::acquireLeaf() {
    // Свободный лист в пуле?
    for (size_t leafIndex = 0; leafIndex < leafPool_.size(); ++leafIndex)
        if (!leafInFlight(leafIndex)) return leafIndex;

    // Ещё не набрали предел — заводим новый, тенями под нынешнюю тему. Растра
    // ему не нужно: растры — у разворота, лист берёт их на переворот.
    if (leafPool_.size() < kMaxLeaves) {
        leafPool_.emplace_back(compositor_, leafLayer_, restBrush_);
        leafPool_.back().tint(shadowHue_);
        return leafPool_.size() - 1;
    }

    // Все в воздухе, а листать просят ещё — старейший переворот кончается
    // рывком, как доигравший: его разворот становится осевшим, и
    // неперелистываемая страница одевается в него, иначе из-под ещё летящих на
    // миг выглянуло бы то, что было до него. Его лист — новому перевороту.
    const size_t oldestLeaf = liveTurns_.front().leafIndex;
    finishTurn(0);
    return oldestLeaf;
}

bool LeafTurnAnimation::leafInFlight(size_t leafIndex) const {
    return std::ranges::any_of(
        liveTurns_, [leafIndex](const Turn& liveTurn) { return liveTurn.leafIndex == leafIndex; });
}

SpreadView::RasterId LeafTurnAnimation::acquireRaster() {
    return spread_.acquireRaster(
        [this](SpreadView::RasterId raster) { return rasterInFlight(raster); });
}

bool LeafTurnAnimation::rasterInFlight(SpreadView::RasterId raster) const {
    return std::ranges::any_of(liveTurns_, [raster](const Turn& liveTurn) {
        return liveTurn.outgoingRaster == raster || liveTurn.incomingRaster == raster;
    });
}

size_t LeafTurnAnimation::turnOfLeaf(size_t leafIndex) const {
    for (size_t turnIndex = 0; turnIndex < liveTurns_.size(); ++turnIndex)
        if (liveTurns_[turnIndex].leafIndex == leafIndex) return turnIndex;
    core::abort("LeafTurnAnimation: переворот кончился, а его листа нет среди летящих");
}

// Корутина переворота. Пакет закрывает и остановленная анимация — переворот
// отменили (встречное, перевёрстка, прыжок, добивание) или стратегию сменили,
// и конец пакета приходит к листу, который, может быть, уже служит другому
// перевороту. Своего ли переворота это конец, говорит токен: отменяющий
// отменил источник раньше, чем остановил анимации, и ожидание кончается
// operation_canceled_exception, кто бы его ни разбудил, — обычным концом
// detached_task. Так же оно кончается, когда отменён источник чтения, полосы
// или приложения: до `this` дело не доходит, кадр стратегию не держит (она не
// самосчитающая), и знак жизни ему не нужен.
//
// Нормальный конец — внутри обработчика Completed: переворот доиграл, и его
// конечное состояние наступает тем же ходом.
async::detached_task LeafTurnAnimation::flyTurn(size_t leafIndex, bool forward, bool bookMode,
                                                async::cancellation_token turnStop) {
    CompositionScopedBatch const flightBatch = startFlight(leafIndex, forward, bookMode);

    // Подписка — до закрытия пакета: Completed без ждущего пропадает.
    auto flightEnd = dsl::onCompleted(flightBatch, std::move(turnStop));
    flightBatch.end();
    co_await flightEnd;

    finishTurn(turnOfLeaf(leafIndex));
}

CompositionScopedBatch LeafTurnAnimation::startFlight(size_t leafIndex, bool forward,
                                                      bool bookMode) {
    Leaf& flyingLeaf = leafPool_[leafIndex];

    // Подъём книжного листа — выражения, а у выражения нет конца: его снимают.
    // Пакет же кончается, когда кончились все его анимации, — в пакете
    // выражение не дало бы ему кончиться никогда. Поэтому — до пакета.
    if (bookMode) {
        const float hinge = flow_.spine();
        flyingLeaf.lift(hinge, forward ? flow_.width() : 0.0f, forward ? 0.0f : flow_.width(),
                        flow_.height());
    }

    CompositionScopedBatch flightBatch =
        compositor_.createScopedBatch(CompositionBatchTypes::Animation);
    if (bookMode)
        animateBookTurn(flyingLeaf, forward);
    else
        animateFlatTurn(flyingLeaf, forward);
    return flightBatch;
}

void LeafTurnAnimation::animateFlatTurn(Leaf& flyingLeaf, bool forward) {
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
    // поворачивается вокруг левого края, и его дальний нижний угол отходит не
    // на ширину, а на гипотенузу (ширина·cos + полвысоты·sin); плюс тень,
    // сдвинутую вправо и размытую, иначе она осталась бы у края серой
    // полоской. Вперёд лист уходит влево, назад — вправо.
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

    flyingLeaf.start(flyingLeaf.outgoingPage, L"Offset", outgoingSlide);
    flyingLeaf.start(flyingLeaf.outgoingPage, L"RotationAngleInDegrees", outgoingTurn);
}

void LeafTurnAnimation::animateBookTurn(Leaf& flyingLeaf, bool forward) {
    // Уходит старый разворот, и в обе стороны он остаётся сверху: книжное
    // листание снимает верхнюю бумагу с неподвижной стопки, а не увозит
    // страницу за край. У переворота две стороны разной давности: под снимаемой
    // бумагой с первого же кадра открывается НОВАЯ страница, а неперелистываемая
    // остаётся СТАРОЙ, пока приходящий лист её не накроет. Для того страницы и
    // два визуала: перелистываемую turnStarted переставил на новый разворот,
    // другую переставит конец переворота (finishTurn).
    SpriteVisual const& outgoingPage = flyingLeaf.outgoingPage;
    InsetClip const& outgoingPageClip = flyingLeaf.outgoingPageClip;
    SpriteVisual const& incomingPage = flyingLeaf.incomingPage;
    InsetClip const& incomingPageClip = flyingLeaf.incomingPageClip;
    SpriteVisual const& foldShadow = flyingLeaf.foldShadow;
    SpriteVisual const& outerEdgeShadow = flyingLeaf.outerEdgeShadow;

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

    flyingLeaf.start(outgoingPageClip, crawlingInset, outgoingClipCrawl);
    flyingLeaf.start(foldShadow, L"Offset", foldFollow);
    flyingLeaf.start(foldShadow, L"Scale", foldWiden);
    flyingLeaf.start(foldShadow, L"Opacity", foldLighten);
    flyingLeaf.start(flyingLeaf.foldShadowMidpoint, L"Offset", foldMidFlatten);
    flyingLeaf.start(outerEdgeShadow, L"Offset", edgeTrail);
    flyingLeaf.start(incomingPage, L"Offset", incomingSlide);
    flyingLeaf.start(incomingPageClip, openingInset, incomingClipOpen);
    flyingLeaf.start(flyingLeaf.bendHalftone, L"Offset", bendCurve);
    flyingLeaf.start(flyingLeaf.bendHalftone, L"Opacity", bendSettle);
    flyingLeaf.start(flyingLeaf.liftAmount, L"Amount", liftRise);
}

void LeafTurnAnimation::finishTurn(size_t newestFinished) {
    // Кончается переворот — кончаются и все, пущенные раньше него: их
    // конечные состояния он покрывает. Источник — первым: пакет закрывает и
    // остановленная анимация, и конец, пришедший к остановленному листу, должен
    // застать ожидание уже отменённым. Потом анимации снимаются, и лист ложится
    // в покой — тем же ходом, что сокрытие листа, без промежуточного кадра.
    for (size_t turnIndex = 0; turnIndex <= newestFinished; ++turnIndex) {
        Turn& finishedTurn = liveTurns_[turnIndex];
        finishedTurn.stop.cancel();
        leafPool_[finishedTurn.leafIndex].stop();
    }

    // Садится один — самый новый из кончившихся: его разворот нынешний, а
    // развороты прежних пропущены — в книге их уже нет. В газете осевший
    // разворот сменился ещё в turnStarting, и садиться нечему. В книге осевшим
    // становится растр листа — без отрисовки и ничем не меняясь, — и
    // неперелистываемая страница переезжает на него сейчас, а не когда сядут
    // все: приходящая страница только что легла на неё ровно этими точками
    // (подмены не видно — тот же кадр, что и сокрытие листа), а следующий лист,
    // ещё летящий, ложится уже на них. Перелистываемая страница носит растр
    // самого нового листа и здесь не трогается: когда сядет и он, осевшим
    // станет тот же растр.
    const Turn& landingTurn = liveTurns_[newestFinished];
    if (landingTurn.bookMode) {
        spread_.settleOn(landingTurn.incomingRaster);
        if (spread_.shown())
            spread_.dressPage(landingTurn.forward ? kLeftPage : kRightPage,
                              landingTurn.incomingRaster);
    }

    liveTurns_.erase(liveTurns_.begin(),
                     liveTurns_.begin() + static_cast<std::ptrdiff_t>(newestFinished) + 1);
    if (liveTurns_.empty()) settle();
}

void LeafTurnAnimation::settle() {
    // Все сели. В книге осевший разворот — самого нового, и обе страницы уже
    // на нём: неперелистываемую одел finishTurn, а перелистываемая носит его
    // растр. Одевать заново нечего; листам в покое на сцене не место, а пулу —
    // в памяти: долгая пауза отпускания начинается заново, а если цикл уже
    // отпускал по секундам — он вернётся к ней.
    leafLayer_.isVisible(false);
    settledDuringRelease_ = true;
    releasePauses_->firstPause.restart();
}

/* ---------------- отпускание пула ---------------- */

LeafTurnAnimation::ReleasePauses::ReleasePauses(const DispatcherQueue& queue,
                                                async::cancellation_token releaseToken)
    : firstPause{queue, kLeafReleaseFirst, releaseToken},
      stepPause{queue, kLeafReleaseStep, releaseToken} {}

// Кадр держит паузы сам (параметр releasePauses): стратегию могут сменить, пока
// он стоит в отменённом ожидании, и таймер, на котором он стоит, должен дожить
// до хода очереди, который его разбудит. Токен — тоже свой, параметром: после
// отмены цикл спрашивает его, не трогая `this`.
async::detached_task LeafTurnAnimation::releaseIdleLeaves(
    intrusive_ptr<ReleasePauses> releasePauses, async::cancellation_token releaseToken) {
    for (;;) {
        // Долгая пауза: её начинает заново каждая посадка всех (settle). Тик,
        // пришедший, пока цикл отпускал, не теряется — следующее ожидание его
        // возьмёт и не встанет. Отмена кончает ожидание без исключения, и
        // токен цикл спрашивает сам.
        co_await releasePauses->firstPause;
        if (releaseToken.is_canceled()) co_return;

        // Пауза отсчитана от последней посадки — посадки до неё уже учтены.
        settledDuringRelease_ = false;

        // Дальше — по листу в секунду, пока есть лишние и никто не летит.
        // Листают снова — цикл увидит это сам, проснувшись, и вернётся ждать
        // долгую паузу: её начнёт посадка. Листнули и уже сели — то же самое:
        // посадка перезапустила долгую паузу и оставила признак.
        while (liveTurns_.empty() && hasSpareLeaves()) {
            releaseSpareLeaf();
            releasePauses->stepPause.restart();
            co_await releasePauses->stepPause;
            if (releaseToken.is_canceled()) co_return;
            if (std::exchange(settledDuringRelease_, false)) break;
        }
    }
}

bool LeafTurnAnimation::hasSpareLeaves() const {
    auto const inFlight = [this](SpreadView::RasterId raster) { return rasterInFlight(raster); };
    return leafPool_.size() > 1 || spread_.spareRasters(inFlight) > 1;
}

void LeafTurnAnimation::releaseSpareLeaf() {
    // Последний лист: все свободны, порядок не важен, а pop_back остальных не
    // двигает. Последний, один, лист оставляем — на следующее листание.
    if (leafPool_.size() > 1) {
        leafPool_.back().detach(leafLayer_);
        leafPool_.pop_back();
    }

    // И один лишний растр разворота — с ним уходит поверхность в размер окна.
    // Один свободный держим, как и лист.
    auto const inFlight = [this](SpreadView::RasterId raster) { return rasterInFlight(raster); };
    if (spread_.spareRasters(inFlight) > 1) spread_.dropSpareRaster(inFlight);
}

}  // namespace bukvitsa::reader
