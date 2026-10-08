// Тесты модели правки обложки: попадание в кружочек, перенос точки между
// соседками, точка корешка, начало правки и имя. Всё, что мастер решает, не
// глядя на экран, — без окна и указателя.

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string_view>

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
    check(editor.imagePath() == std::filesystem::path(L"skins/autumn.png"), "снимок — её");

    const Skin& antique = systemSkins()[0];
    editor.openEdit(antique, std::filesystem::path(L"Assets/book-1.png"));
    check(editor.skin().name.empty() && editor.skin().image.empty() && !editor.skin().system,
          "правка системной: новая обложка — без имени и без копии");
    check(editor.skin().top.x == antique.top.x && editor.skin().bottom.y == antique.bottom.y,
          "правка системной: на её точках");
    check(editor.needsName(), "правленой системной имя спрашивается");

    editor.drag(Grip{Edge::Top, 1}, {300.0f, 100.0f}, kWide);
    editor.openNew(std::filesystem::path(L"photo.jpg"));
    check(editor.skin().top.x == fresh.top.x && editor.skin().top.y == fresh.top.y &&
              editor.skin().bottom.x == fresh.bottom.x && editor.skin().bottom.y == fresh.bottom.y,
          "новая: начальные прямые, прежняя правка забыта");
    check(editor.skin().name.empty() && editor.needsName(), "новая: имя пустое и спрашивается");
    check(editor.imagePath() == std::filesystem::path(L"photo.jpg"), "новая: выбранный снимок");
}

/// Имя проверяется как чужой текст: пустое, из пробелов и битое не годятся.
void testName() {
    std::printf("\n=== правка обложки: имя ===\n");

    const char16_t lone[] = {0xD800, 0};

    check(!SkinEditor::nameOk(u""), "пустое — не имя");
    check(!SkinEditor::nameOk(u"   ") && !SkinEditor::nameOk(u" \t "),
          "из пробелов и табуляций — не имя");
    check(!SkinEditor::nameOk(std::u16string_view(lone, 1)), "одинокий суррогат — не имя");
    check(SkinEditor::nameOk(u"Осень") && SkinEditor::nameOk(u" Осень "), "слово — имя");

    SkinEditor editor;
    editor.openNew(std::filesystem::path(L"photo.jpg"));
    editor.drag(Grip{Edge::Bottom, 3}, {350.0f, 700.0f}, kWide);

    check(!editor.result(u"  "), "без имени сохранять нечего");

    const std::optional<Skin> saved = editor.result(u"Осень");
    check(saved && saved->name.plain() == u"Осень", "сохраняется под введённым именем");
    check(saved && saved->bottom.x == editor.skin().bottom.x &&
              saved->bottom.y == editor.skin().bottom.y,
          "с поправленными точками");
    check(editor.skin().name.empty(), "сама правка имени не получает: «Отмена» спросит снова");
}

}  // namespace

void runSkinEditorTests() {
    testGripByReach();
    testDragKeepsOrder();
    testSpineMovesVertically();
    testOpen();
    testName();
}
