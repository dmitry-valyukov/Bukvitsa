// Тесты модели правки обложки: попадание в кружочек, перенос точки между
// соседками, точка корешка, начало правки, имя и диалог имени, то, что полоса
// показывает под мастером, и сохранение одно за раз. Всё, что мастер решает, не
// глядя на экран, — без окна и указателя.

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

// Заголовки читалки после всех стандартных: модель правки импортирует wxl.core.
#include "check.h"

#include "bukvitsa/reader/skin_editor.h"

import wxl.core;

using namespace bukvitsa::reader;
using bukvitsa::reader::tests::check;

namespace {

using Edge = SkinEditor::Edge;
using Grip = SkinEditor::Grip;

constexpr size_t kSpinePoint = static_cast<size_t>(EdgeCurve::kSpine);
constexpr size_t kLastPoint = static_cast<size_t>(EdgeCurve::kPoints) - 1;

/// Сетка мастера во весь экран: точки начальной обложки стоят друг от друга
/// на 119 DIP, и зоны захвата не перекрываются.
constexpr SkinEditor::Size kWide{1000.0f, 800.0f};

/// Доли снимка: точнее тысячной доли мастер всё равно не ставит.
bool aboutEqual(float a, float b) {
    return std::abs(a - b) < 1e-3f;
}

const EdgeCurve& curveOf(const SkinEditor& editor, Edge edge) {
    return edge == Edge::Top ? editor.skin().top : editor.skin().bottom;
}

/// Центр кружочка на сетке размером `area`.
SkinEditor::Point centerOf(const SkinEditor& editor, Edge edge, size_t at, SkinEditor::Size area) {
    const EdgeCurve& curve = curveOf(editor, edge);
    return {curve.x[at] * area.width, curve.y[at] * area.height};
}

/// Всё, что модель сказала слушателю предпросмотра, по порядку — так, как это
/// слышит приложение.
using PreviewLog = std::vector<std::optional<SkinPreview>>;

/// Слушает предпросмотр `editor` с этих пор; слушатель уходит вместе с полем.
void listen(SkinEditor& editor, PreviewLog& told) {
    static_cast<void>(editor.shownPreview().on_change(
        [&told](const std::optional<SkinPreview>& wizardPreview) noexcept { told.push_back(wizardPreview); }));
}

bool ascending(const EdgeCurve& curve) {
    for (size_t at = 1; at <= kLastPoint; ++at) {
        if (!(curve.x[at] > curve.x[at - 1])) return false;
    }
    return true;
}

/// Кружочек берётся в круге `kGripReach` вокруг точки, а не в квадрате; из
/// двух, чьи зоны перекрылись, — ближайший.
void testGripByReach() {
    std::printf("\n=== правка обложки: попадание в кружочек ===\n");

    SkinEditor editor;
    const SkinEditor::Point second = centerOf(editor, Edge::Top, 2, kWide);
    const float reach = SkinEditor::kGripReach;

    check(editor.gripAt(second, kWide) == Grip{Edge::Top, 2}, "в точку — она");
    check(editor.gripAt({second.x + reach - 1.0f, second.y}, kWide) == Grip{Edge::Top, 2},
          "рядом, в зоне захвата — она же");
    check(!editor.gripAt({second.x + reach + 1.0f, second.y}, kWide),
          "за зоной захвата — мимо");
    check(!editor.gripAt({second.x + 0.75f * reach, second.y + 0.75f * reach}, kWide),
          "зона — круг: в углу квадрата мимо");
    check(editor.gripAt(centerOf(editor, Edge::Bottom, 6, kWide), kWide) == Grip{Edge::Bottom, 6},
          "нижняя кривая — своя точка");
    check(!editor.gripAt(second, SkinEditor::Size{}), "сетки ещё нет — мимо");

    // Узкое окно: третью точку прижимают к второй — до зазора в 10 DIP, и
    // зоны захвата перекрываются. Щелчок прямо в кружочек берёт его, а не
    // соседку, которая раньше в списке.
    const SkinEditor::Size narrow{500.0f, 400.0f};
    editor.drag(Grip{Edge::Top, 3}, {0.0f, centerOf(editor, Edge::Top, 3, narrow).y}, narrow);

    const SkinEditor::Point left = centerOf(editor, Edge::Top, 2, narrow);
    const SkinEditor::Point right = centerOf(editor, Edge::Top, 3, narrow);
    check(right.x - left.x < reach, "соседки ближе зоны захвата");
    check(editor.gripAt(right, narrow) == Grip{Edge::Top, 3}, "ближайшая из двух — правая");
    check(editor.gripAt(left, narrow) == Grip{Edge::Top, 2}, "ближайшая из двух — левая");
}

/// Точка ходит между соседками с зазором `kMinGap` и не проходит их; крайние —
/// до кромок; по вертикали — от своей кромки до `kEdgeReach`.
void testDragKeepsOrder() {
    std::printf("\n=== правка обложки: точка не проходит соседку ===\n");

    SkinEditor editor;
    const EdgeCurve& top = editor.skin().top;
    const EdgeCurve& bottom = editor.skin().bottom;
    const float gap = SkinEditor::kMinGap;
    const float sixth = top.x[6];

    editor.drag(Grip{Edge::Top, 5}, {900.0f, 20.0f}, kWide);
    check(aboutEqual(top.x[5], sixth - gap) && top.x[6] == sixth,
          "за правую соседку — упирается перед ней");

    editor.drag(Grip{Edge::Top, 5}, {0.0f, 20.0f}, kWide);
    check(aboutEqual(top.x[5], EdgeCurve::kSpineX + gap), "за корешок влево — упирается в него");

    editor.drag(Grip{Edge::Top, 3}, {1000.0f, 20.0f}, kWide);
    check(aboutEqual(top.x[3], EdgeCurve::kSpineX - gap), "к корешку справа — тоже");

    editor.drag(Grip{Edge::Top, 0}, {-50.0f, 20.0f}, kWide);
    editor.drag(Grip{Edge::Bottom, kLastPoint}, {5000.0f, 780.0f}, kWide);
    check(top.x[0] == 0.0f && bottom.x[kLastPoint] == 1.0f, "крайние — до кромок снимка");

    editor.drag(Grip{Edge::Top, 2}, {top.x[2] * kWide.width, 80.0f}, kWide);
    check(aboutEqual(top.y[2], 0.1f), "по вертикали — под указатель");
    editor.drag(Grip{Edge::Top, 2}, {top.x[2] * kWide.width, 800.0f}, kWide);
    editor.drag(Grip{Edge::Bottom, 2}, {bottom.x[2] * kWide.width, 0.0f}, kWide);
    check(top.y[2] == kEdgeReach && bottom.y[2] == 1.0f - kEdgeReach,
          "по вертикали — не дальше четверти высоты от своей кромки");

    check(ascending(top) && ascending(bottom), "иксы по-прежнему строго возрастают");

    const Skin before = editor.skin();
    editor.drag(Grip{Edge::Top, 1}, {500.0f, 400.0f}, SkinEditor::Size{});
    check(editor.skin().top.x == before.top.x && editor.skin().top.y == before.top.y,
          "сетки нет — точка стоит");
}

/// Перенос, после которого кромки между точками разошлись бы на высоту полосы
/// и вывернули столбец, не принимается: кривая остаётся прежней. Соседний
/// допустимый перенос той же точки принимается.
void testDragKeepsColumns() {
    std::printf("\n=== правка обложки: столбцы не выворачиваются ===\n");

    // Верхняя кромка с точками мастера, сжатыми у правого края: доли снимка,
    // зазор не меньше 0,02. Подними здесь шестую точку на всю четверть высоты
    // — и сплайн перелетит её у края выше верха почти на высоту полосы.
    Skin start = defaultSkin();
    start.top.x = {0.156f, 0.194f, 0.371f, 0.465f, 0.5f, 0.877f, 0.905f, 0.927f, 0.948f};
    start.top.y = {0.0f, 0.0f, 0.0f, 0.0f, 0.039f, 0.0f, 0.0f, 0.0f, 0.0f};
    check(PageWarp{start}.keepsColumns(), "начальная кромка столбцов не выворачивает");

    SkinEditor editor;
    editor.openEdit(start, std::filesystem::path{L"start.png"});
    const float sixthX = start.top.x[6] * kWide.width;

    editor.drag(Grip{Edge::Top, 6}, {sixthX, kEdgeReach * kWide.height}, kWide);
    check(editor.skin().top.x == start.top.x && editor.skin().top.y == start.top.y,
          "перенос, выворачивающий столбец, отвергнут — кривая прежняя");

    editor.drag(Grip{Edge::Top, 6}, {sixthX, 0.1f * kWide.height}, kWide);
    const EdgeCurve& moved = editor.skin().top;
    check(aboutEqual(moved.y[6], 0.1f) && aboutEqual(moved.x[6], start.top.x[6]) &&
              PageWarp{editor.skin()}.keepsColumns(),
          "соседний допустимый перенос той же точки принят");
}

/// Точка корешка ходит только по вертикали: её X — середина разворота, где
/// режутся страницы. Она одна у левого и правого листа своей кромки, и кромка
/// идёт к ней с обеих сторон.
void testSpineMovesVertically() {
    std::printf("\n=== правка обложки: точка корешка ===\n");

    SkinEditor editor;
    const EdgeCurve& top = editor.skin().top;
    const EdgeCurve& bottom = editor.skin().bottom;
    const EdgeCurve fresh = defaultSkin().top;

    check(editor.gripAt(centerOf(editor, Edge::Top, kSpinePoint, kWide), kWide) ==
              Grip{Edge::Top, kSpinePoint},
          "кружочек корешка берётся, как любой");

    editor.drag(Grip{Edge::Top, kSpinePoint}, {100.0f, 80.0f}, kWide);
    check(top.x[kSpinePoint] == EdgeCurve::kSpineX, "X корешка стоит на середине");
    check(aboutEqual(top.y[kSpinePoint], 0.1f), "Y корешка — под указатель");
    check(top.x[kSpinePoint - 1] == fresh.x[kSpinePoint - 1] &&
              top.x[kSpinePoint + 1] == fresh.x[kSpinePoint + 1] &&
              top.y[kSpinePoint - 1] == fresh.y[kSpinePoint - 1] &&
              top.y[kSpinePoint + 1] == fresh.y[kSpinePoint + 1],
          "соседки с обоих листов не тронуты");
    check(bottom.y[kSpinePoint] == defaultSkin().bottom.y[kSpinePoint],
          "нижняя кромка своя: её корешок не двинулся");

    const EdgeSpline edge{top};
    check(aboutEqual(edge.at(EdgeCurve::kSpineX - 0.0001f), 0.1f) &&
              aboutEqual(edge.at(EdgeCurve::kSpineX + 0.0001f), 0.1f),
          "оба листа подходят к поднятому корешку");
    check(edge.at(0.45f) > kEdgeInset && edge.at(0.55f) > kEdgeInset,
          "корешок тянет за собой оба листа");

    editor.drag(Grip{Edge::Bottom, kSpinePoint}, {900.0f, 0.0f}, kWide);
    check(bottom.x[kSpinePoint] == EdgeCurve::kSpineX &&
              bottom.y[kSpinePoint] == 1.0f - kEdgeReach,
          "у нижней — так же: только по вертикали");
}

/// Новая — начальные прямые и пустое имя; правка своей — копия с именем;
/// правка системной — новая обложка на её точках.
void testOpen() {
    std::printf("\n=== правка обложки: начало ===\n");

    const Skin fresh = defaultSkin();

    Skin own = defaultSkin();
    own.name = u16_text{u"Осень"};
    own.image = u16_text{u"autumn.png"};
    own.top.y[2] = 0.1f;

    SkinEditor editor;
    editor.openEdit(own, std::filesystem::path(L"skins/autumn.png"));
    check(editor.skin().name.plain() == u"Осень" && editor.skin().image.plain() == u"autumn.png",
          "правка своей: имя и копия снимка — её");
    check(editor.skin().top.y == own.top.y && editor.skin().bottom.x == own.bottom.x,
          "правка своей: её точки");
    check(!editor.needsName(), "своя сохраняется под своим именем, не спрашивая");
    check(editor.name.get().plain() == u"Осень", "правка своей: в поле имени — её имя");
    check(editor.imagePath() == std::filesystem::path(L"skins/autumn.png"), "снимок — её");

    const Skin& antique = systemSkins()[0];
    editor.openEdit(antique, std::filesystem::path(L"Assets/book-1.png"));
    check(editor.skin().name.empty() && editor.skin().image.empty() && !editor.skin().system,
          "правка системной: новая обложка — без имени и без копии");
    check(editor.skin().top.x == antique.top.x && editor.skin().bottom.y == antique.bottom.y,
          "правка системной: на её точках");
    check(editor.needsName(), "правленой системной имя спрашивается");
    check(editor.name.get().empty(), "правка системной: поле имени пустое, как у новой");

    editor.drag(Grip{Edge::Top, 1}, {300.0f, 100.0f}, kWide);
    editor.name.set(u16_text{u"Набранное"});
    editor.openNew(std::filesystem::path(L"photo.jpg"));
    check(editor.skin().top.x == fresh.top.x && editor.skin().top.y == fresh.top.y &&
              editor.skin().bottom.x == fresh.bottom.x && editor.skin().bottom.y == fresh.bottom.y,
          "новая: начальные прямые, прежняя правка забыта");
    check(editor.skin().name.empty() && editor.needsName(), "новая: имя пустое и спрашивается");
    check(editor.name.get().empty(), "новая: поле имени пустое, набранное прежде забыто");
    check(editor.imagePath() == std::filesystem::path(L"photo.jpg"), "новая: выбранный снимок");
}

/// Имя берётся из поля модели, к которому привязано поле мастера: пустое и из
/// пробелов не годится, годное уходит в обложку как набрано.
void testName() {
    std::printf("\n=== правка обложки: имя ===\n");

    SkinEditor editor;
    editor.openNew(std::filesystem::path(L"photo.jpg"));
    editor.drag(Grip{Edge::Bottom, 3}, {350.0f, 700.0f}, kWide);

    check(!editor.nameOk() && !editor.result(), "пустое поле — не имя, сохранять нечего");

    editor.name.set(u16_text{u"   "});
    check(!editor.nameOk(), "из пробелов — не имя");
    editor.name.set(u16_text{u" \t "});
    check(!editor.nameOk() && !editor.result(), "из пробелов и табуляций — не имя");

    editor.name.set(u16_text{u" Осень "});
    check(editor.nameOk(), "слово с пробелами по краям — имя");

    editor.name.set(u16_text{u"Осень"});
    const std::optional<Skin> saved = editor.result();
    check(saved && saved->name.plain() == u"Осень", "сохраняется под именем из поля");
    check(saved && saved->bottom.x == editor.skin().bottom.x &&
              saved->bottom.y == editor.skin().bottom.y,
          "с поправленными точками");
    check(editor.skin().name.empty(), "сама правка имени не получает: «Отмена» спросит снова");
}

/// Открытие и закрытие — каждое перемена того, что видит полоса, и слушатель
/// узнаёт о каждом, даже когда открывают ту же новую обложку на том же снимке:
/// обложка у неё всякий раз начальная, и без закрытия между открытиями
/// значение не сменилось бы.
void testPreviewFollowsOpenAndClose() {
    std::printf("\n=== правка обложки: полоса узнаёт об открытии и закрытии ===\n");

    SkinEditor editor;
    PreviewLog told;
    listen(editor, told);
    auto const photo = std::filesystem::path{L"a.jpg"};

    check(!editor.shownPreview().get(), "до открытия полосе показывать нечего");

    editor.openNew(photo);
    check(told.size() == 1 && told[0] && told[0]->skin == defaultSkin() && told[0]->imagePath == photo,
          "новая: сказано раз — начальная обложка на выбранном снимке");
    check(told.size() == 1 && editor.shownPreview().get() == told[0],
          "сказанное и есть то, что лежит в поле");

    editor.close();
    check(told.size() == 2 && !told[1], "закрытие: сказано, что показывать нечего");

    editor.openNew(photo);
    check(told.size() == 3 && told[2] == SkinPreview{defaultSkin(), photo},
          "та же новая на том же снимке после закрытия — сказано снова");

    editor.close();
    editor.close();
    check(told.size() == 4 && !told[3], "закрытое закрыть ещё раз — не перемена");
}

/// «Другой снимок» открывает новую на другом файле. При нетронутых точках
/// обложка та же — меняется только снимок, и о нём сказано. После сдвинутых
/// точек и после правки своей полоса получает начальную обложку, а не прежние
/// кривые: сказанное — то, что открыто, а не то, что было до открытия.
void testPreviewTellsAnotherImage() {
    std::printf("\n=== правка обложки: другой снимок ===\n");

    SkinEditor editor;
    PreviewLog told;
    listen(editor, told);
    auto const firstPhoto = std::filesystem::path{L"a.jpg"};
    auto const secondPhoto = std::filesystem::path{L"b.jpg"};

    editor.openNew(firstPhoto);
    editor.openNew(secondPhoto);
    check(told.size() == 2 && told[1] && told[1]->imagePath == secondPhoto && told[1]->skin == told[0]->skin,
          "при нетронутых точках — сказано: обложка та же, снимок новый");

    editor.openNew(secondPhoto);
    check(told.size() == 2, "тот же снимок снова — не перемена: полоса его уже показывает");

    editor.drag(Grip{Edge::Top, 2}, {editor.skin().top.x[2] * kWide.width, 80.0f}, kWide);
    editor.drop();
    editor.openNew(firstPhoto);
    check(told.size() == 4 && told.back() && told.back()->skin == defaultSkin() &&
              told.back()->imagePath == firstPhoto,
          "после сдвинутых точек — сказано с начальной обложкой на новом снимке");

    Skin alpha = defaultSkin();
    alpha.name = u16_text{u"Alpha"};
    alpha.image = u16_text{u"alpha.png"};
    alpha.top.y[2] = 0.1f;
    editor.openEdit(alpha, std::filesystem::path{L"skins/alpha.png"});
    editor.openNew(secondPhoto);
    check(told.size() == 6 && told.back() && told.back()->skin == defaultSkin() &&
              told.back()->imagePath == secondPhoto,
          "после правки своей — сказано с начальной обложкой, без её имени и точек");
}

/// Правка одной обложки, выход и та же правка снова — три перемены: открытие,
/// закрытие, открытие. Так и у системной, которую правят «на основе».
void testPreviewReopensEdit() {
    std::printf("\n=== правка обложки: та же обложка после выхода ===\n");

    Skin alpha = defaultSkin();
    alpha.name = u16_text{u"Alpha"};
    alpha.image = u16_text{u"alpha.png"};
    alpha.top.y[2] = 0.1f;
    auto const alphaPhoto = std::filesystem::path{L"skins/alpha.png"};

    SkinEditor editor;
    PreviewLog told;
    listen(editor, told);

    editor.openEdit(alpha, alphaPhoto);
    editor.close();
    editor.openEdit(alpha, alphaPhoto);
    check(told.size() == 3 && told[0] && told[0]->skin == alpha && told[0]->imagePath == alphaPhoto &&
              !told[1] && told[2] == told[0],
          "своя: открытие, закрытие, открытие — сказано трижды");

    const Skin& antique = systemSkins()[0];
    auto const antiquePhoto = std::filesystem::path{L"Assets/book-1.png"};
    editor.close();
    editor.openEdit(antique, antiquePhoto);
    editor.close();
    editor.openEdit(antique, antiquePhoto);
    check(told.size() == 7 && told[4] && told[4]->skin.top == antique.top && told[4]->skin.name.empty() &&
              !told[5] && told[6] == told[4],
          "системная: то же — на её точках, без имени");
}

/// Полоса получает кривые отпущенными, а не на каждом движении. Отпущенная
/// точка, которая ничего не сдвинула, — то же значение, и слушатель не
/// зовётся: так обещает поле (`observable` не объявляет равного), и карта
/// изгиба не пересобирается зря — проход по всем пикселям слоя.
void testPreviewTellsDrop() {
    std::printf("\n=== правка обложки: отпущенная точка ===\n");

    SkinEditor editor;
    PreviewLog told;
    listen(editor, told);
    auto const photo = std::filesystem::path{L"a.jpg"};

    editor.openNew(photo);
    const EdgeCurve& top = editor.skin().top;
    editor.drag(Grip{Edge::Top, 2}, {top.x[2] * kWide.width, 40.0f}, kWide);
    editor.drag(Grip{Edge::Top, 2}, {top.x[2] * kWide.width, 80.0f}, kWide);
    check(told.size() == 1, "пока тянут — полосе не сказано");

    editor.drop();
    check(told.size() == 2 && told[1] && aboutEqual(told[1]->skin.top.y[2], 0.1f) &&
              told[1]->skin == editor.skin() && told[1]->imagePath == photo,
          "отпустили — сказано: новые кривые на том же снимке");

    editor.drop();
    check(told.size() == 2, "нажали и отпустили, не сдвинув, — кривые те же, не сказано");

    // Точки сжаты у правого края, как в testDragKeepsColumns: подъём шестой на
    // всю четверть высоты вывернул бы столбец, модель его отвергает, точка
    // стоит — и отпускать её значит снова отпустить те же кривые.
    Skin crowdedSkin = defaultSkin();
    crowdedSkin.top.x = {0.156f, 0.194f, 0.371f, 0.465f, 0.5f, 0.877f, 0.905f, 0.927f, 0.948f};
    crowdedSkin.top.y = {0.0f, 0.0f, 0.0f, 0.0f, 0.039f, 0.0f, 0.0f, 0.0f, 0.0f};
    editor.openEdit(crowdedSkin, photo);
    const size_t toldAtOpen = told.size();
    editor.drag(Grip{Edge::Top, 6}, {crowdedSkin.top.x[6] * kWide.width, kEdgeReach * kWide.height}, kWide);
    editor.drop();
    check(toldAtOpen == 3 && told.size() == toldAtOpen, "отвергнутый перенос отпустили — не сказано");
}

/// Сохранение — одно за раз: пока отданное не кончилось, второе не начинается.
/// Конец — по номеру: конец сохранения прежнего открытия, пришедший после
/// нового открытия или закрытия, нынешнего не трогает.
void testSaveOneAtATime() {
    std::printf("\n=== правка обложки: сохранение одно за раз ===\n");

    SkinEditor editor;
    auto const photo = std::filesystem::path{L"a.jpg"};

    editor.openNew(photo);
    editor.name.set(u16_text{u"Осень"});
    check(!editor.saving(), "открыта — сохранение не идёт");

    const SkinSave firstSave = editor.beginSave();
    check(firstSave.skin.name.plain() == u"Осень" && firstSave.photo == photo && editor.saving(),
          "новая: под набранным именем и с выбранного снимка, и оно идёт");
    check(editor.skin().name.empty() && editor.needsName(),
          "сама правка имени не получает: отказ записи спросит его снова");

    check(editor.saveEnded(firstSave.ticket) && !editor.saving(),
          "кончилось нынешнее — сказано, что нынешнее, и можно снова");

    const SkinSave secondSave = editor.beginSave();
    check(secondSave.ticket != firstSave.ticket && editor.saving(), "второе — со своим номером");
    check(!editor.saveEnded(firstSave.ticket) && editor.saving(),
          "конец первого — не нынешнего: сказано, и второе идёт");
    check(editor.saveEnded(secondSave.ticket) && !editor.saving(),
          "конец второго — нынешнего — кончает");

    Skin own = defaultSkin();
    own.name = u16_text{u"Alpha"};
    own.image = u16_text{u"alpha.png"};
    own.top.y[2] = 0.1f;
    auto const ownPhoto = std::filesystem::path{L"skins/alpha.png"};
    const SkinSave thirdSave = editor.beginSave();
    editor.openEdit(own, ownPhoto);
    check(!editor.saving(), "правка другой забывает идущее сохранение");
    const SkinSave ownSave = editor.beginSave();
    check(ownSave.skin == own && ownSave.photo == ownPhoto,
          "своя: как есть, под своим именем, без вопроса");
    check(!editor.saveEnded(thirdSave.ticket) && editor.saving(),
          "конец сохранения новой — не нынешнего — сохранение своей не кончает");

    // Сохранение своей идёт, а читатель взял другой снимок: новая — другая
    // правка, её можно сохранять, а конец прежнего её не касается.
    editor.openNew(photo);
    check(!editor.saving(), "новое открытие забывает идущее сохранение");
    editor.name.set(u16_text{u"Зима"});
    const SkinSave newSave = editor.beginSave();
    check(!editor.saveEnded(ownSave.ticket) && editor.saving(),
          "конец сохранения прежнего открытия — не нынешней правки, и нынешнее идёт");
    check(editor.saveEnded(newSave.ticket) && !editor.saving(), "конец своего — кончает");

    const SkinSave closedSave = editor.beginSave();
    editor.close();
    check(!editor.saving(), "закрытие забывает идущее сохранение");
    editor.openNew(photo);
    editor.name.set(u16_text{u"Весна"});
    const SkinSave reopenedSave = editor.beginSave();
    check(!editor.saveEnded(closedSave.ticket) && editor.saving() &&
              reopenedSave.ticket != closedSave.ticket,
          "конец сохранения до закрытия новое после открытия не кончает");

    // Сохранение своей идёт, другой снимок открыл новую, и тогда запись
    // кончилась: правка уже другая, и закрывать мастер по этому концу нельзя.
    editor.openEdit(own, ownPhoto);
    const SkinSave lateSave = editor.beginSave();
    editor.openNew(photo);
    check(!editor.saveEnded(lateSave.ticket) && !editor.saving(),
          "позднее сохранение своей после другого снимка — не нынешней правки");
}

/// Диалог имени — часть правки: другой снимок, правка другой обложки и
/// закрытие закрывают диалог прежней правки, и набранное в нём уходит с ней;
/// «Отмена» и начатое сохранение закрывают его тоже. Так диалог прежней правки
/// поверх новой не остаётся, а слушатель видимости — панель диалога — узнаёт о
/// каждом закрытии.
void testNameDialogBelongsToEdit() {
    std::printf("\n=== правка обложки: диалог имени — у правки ===\n");

    SkinEditor editor;
    std::vector<bool> shown;
    static_cast<void>(
        editor.naming().on_change([&shown](bool naming) noexcept { shown.push_back(naming); }));
    auto const firstPhoto = std::filesystem::path{L"a.jpg"};
    auto const secondPhoto = std::filesystem::path{L"b.jpg"};

    editor.openNew(firstPhoto);
    check(!editor.naming().get(), "открыта — диалога нет");
    editor.askName();
    check(editor.naming().get() && editor.name.get().empty(), "новая: диалог открыт, поле пустое");
    editor.name.set(u16_text{u"Осень"});

    editor.openNew(secondPhoto);
    check(!editor.naming().get() && editor.name.get().empty() &&
              shown == std::vector<bool>{true, false},
          "другой снимок закрывает диалог прежней правки, и набранное уходит с ней");
    editor.askName();
    check(editor.naming().get() && editor.name.get().empty(),
          "у новой правки — свой диалог, с пустым полем");

    Skin own = defaultSkin();
    own.name = u16_text{u"Alpha"};
    own.image = u16_text{u"alpha.png"};
    editor.openEdit(own, std::filesystem::path{L"skins/alpha.png"});
    check(!editor.naming().get() && editor.name.get().plain() == u"Alpha",
          "правка своей закрывает диалог; в поле — её имя");

    editor.openEdit(systemSkins()[0], std::filesystem::path{L"Assets/book-1.png"});
    editor.askName();
    editor.close();
    check(!editor.naming().get(), "закрытие закрывает диалог");

    editor.openNew(firstPhoto);
    editor.askName();
    editor.name.set(u16_text{u"Зима"});
    editor.cancelName();
    check(!editor.naming().get(), "«Отмена» закрывает диалог");
    editor.askName();
    check(editor.naming().get() && editor.name.get().empty(),
          "открытый снова — с именем правки, набранное в отменённом забыто");

    editor.name.set(u16_text{u"Весна"});
    const SkinSave save = editor.beginSave();
    check(!editor.naming().get() && save.skin.name.plain() == u"Весна",
          "начатое сохранение закрывает диалог: имя взято");
    check(editor.saveEnded(save.ticket), "и кончается своим номером");
}

}  // namespace

void runSkinEditorTests() {
    testGripByReach();
    testDragKeepsOrder();
    testDragKeepsColumns();
    testSpineMovesVertically();
    testOpen();
    testName();
    testPreviewFollowsOpenAndClose();
    testPreviewTellsAnotherImage();
    testPreviewReopensEdit();
    testPreviewTellsDrop();
    testSaveOneAtATime();
    testNameDialogBelongsToEdit();
}
