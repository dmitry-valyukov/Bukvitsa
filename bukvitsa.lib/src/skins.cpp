#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

// Заголовки проекта после стандартных: store.h несёт импорт, после которого
// стандартный заголовок MSVC уже не принимает. Свой первым.
#include "bukvitsa/reader/skins.h"

#include "bukvitsa/reader/settings.h"
#include "bukvitsa/reader/store.h"

import wxl.fmt;

namespace bukvitsa::reader {

namespace {

constexpr size_t kSpine = static_cast<size_t>(EdgeCurve::kSpine);
constexpr size_t kPoints = static_cast<size_t>(EdgeCurve::kPoints);

/// Кривая с точками на прямой: равномерно от `from` до `to` по X, все на
/// одной высоте `level`.
EdgeCurve straightCurve(float from, float to, float level) {
    EdgeCurve curve;

    const float step = (to - from) / (EdgeCurve::kPoints - 1);
    for (int index = 0; index < EdgeCurve::kPoints; ++index) {
        curve.x[static_cast<size_t>(index)] = from + step * static_cast<float>(index);
        curve.y[static_cast<size_t>(index)] = level;
    }
    // Корешок — ровно на середине, а не в сумме четырёх шагов с плавающей
    // точкой.
    curve.x[kSpine] = EdgeCurve::kSpineX;
    return curve;
}

/// Что чтение точек кромки нашло в файле не таким, как пишет мастер.
struct PointFaults {
    bool outsidePage = false;         ///< координата есть, но не доля страницы
    bool missingCoordinate = false;   ///< у точки нет x или y
    bool extraPoints = false;         ///< точек больше, чем у кромки бывает
};

/// Доля из атрибута, который есть: вне [0, 1], NaN (from_chars его
/// разбирает), не число — пусто. Сравнение написано так, чтобы и NaN ушёл в
/// пусто.
std::optional<float> shareOf(const wxl::xml::node& point, const char* name) {
    const auto value = static_cast<float>(realOf(point, name, std::numeric_limits<double>::quiet_NaN()));
    if (value >= 0.0f && value <= 1.0f) return value;
    return std::nullopt;
}

/// Читает до `count` дочерних `<point>` в точки кривой, начиная с `first`.
/// Точка, у которой нет координаты или координата не доля страницы, остаётся
/// прежней целиком (у чтения реестра — точкой начальной прямой), а не смесью
/// координат из файла и прямой; точки сверх `count` отбрасываются. Файл могли
/// поправить руками, и это не повод ронять обложку, но и не повод молчать: всё
/// это отмечается в `faults`. Недостающие точки в конце — прежние, молча.
void readPoints(const wxl::xml::node& element, EdgeCurve& curve, size_t first, size_t count, PointFaults& faults) {
    size_t index = first;
    for (const wxl::xml::node& point : element.children_named("point")) {
        if (index >= first + count) {
            faults.extraPoints = true;
            break;
        }
        if (!point.attribute("x") || !point.attribute("y")) {
            faults.missingCoordinate = true;
        } else if (const std::optional<float> x = shareOf(point, "x"), y = shareOf(point, "y"); x && y) {
            curve.x[index] = *x;
            curve.y[index] = *y;
        } else {
            faults.outsidePage = true;
        }
        ++index;
    }
}

/// Иксы строго возрастают: сплайну нужен отрезок ненулевой длины между
/// соседками, иначе деление на ноль. Мастер это держит клампами, а файл
/// могли поправить руками.
bool ascending(const EdgeCurve& curve) {
    for (size_t index = 1; index < kPoints; ++index) {
        if (!(curve.x[index] > curve.x[index - 1])) return false;
    }
    return true;
}

/// Кривая из записи второй версии, где листы описывались порознь: пять точек
/// левого и пять правого. Последняя точка левого и первая правого стояли у
/// корешка обе — теперь там одна точка: на середине, высотой посередине
/// между ними. Высота корешка поэтому — полусумма двух точек, корешка левого
/// листа и первой точки правого, и точка, которой в записи нет или которая
/// исправлена, входит в неё точкой начальной прямой на своём месте: правая
/// без высоты тянет корешок к прямой наполовину. Правого листа нет вовсе —
/// высота корешка левого как есть.
EdgeCurve curveOfHalves(const wxl::xml::node* leftHalf, const wxl::xml::node* rightHalf,
                        EdgeCurve fallback, PointFaults& faults) {
    EdgeCurve curve = fallback;
    if (leftHalf) readPoints(*leftHalf, curve, 0, kSpine + 1, faults);

    if (rightHalf) {
        // Точки правого листа ложатся с корешка, поэтому читаются в свою
        // кривую — иначе первая из них затёрла бы точку левого до усреднения.
        // Кривая — начальная прямая, а не уже прочитанная левая: первая точка
        // правого без координаты должна остаться точкой прямой, как обещает
        // чтение, а не точкой корешка левого листа.
        EdgeCurve right = fallback;
        readPoints(*rightHalf, right, kSpine, kSpine + 1, faults);

        curve.y[kSpine] = 0.5f * (curve.y[kSpine] + right.y[kSpine]);
        for (size_t index = kSpine + 1; index < kPoints; ++index) {
            curve.x[index] = right.x[index];
            curve.y[index] = right.y[index];
        }
    }
    curve.x[kSpine] = EdgeCurve::kSpineX;
    return curve;
}

void writeCurve(text_builder<sta_allocator>& out, const char* name, const EdgeCurve& curve) {
    out.format("    <{}>\n", name);
    for (size_t index = 0; index < kPoints; ++index) {
        out.format("      <point x=\"{}\" y=\"{}\"/>\n", curve.x[index], curve.y[index]);
    }
    out.format("    </{}>\n", name);
}

}  // namespace

Skin defaultSkin() {
    Skin skin;

    skin.top = straightCurve(kEdgeInset, 1.0f - kEdgeInset, kEdgeInset);
    skin.bottom = straightCurve(kEdgeInset, 1.0f - kEdgeInset, 1.0f - kEdgeInset);
    return skin;
}

std::span<const Skin> systemSkins() {
    // Точки ровно те, что мастер записал в skins.xml для этих фотографий, —
    // перенесённые оттуда как есть, вместе с их неровностью. Округлять их
    // «покрасивее» нельзя: они описывают конкретный снимок, а не идею изгиба,
    // и подогнанное число увело бы строку мимо края бумаги. Точка корешка
    // (пятая) — на середине, а её высота — среднее двух точек прежней записи,
    // где листы описывались порознь и у корешка стояли две: последняя левого
    // и первая правого.
    static const std::vector<Skin> list = [] {
        Skin antique;
        antique.name = u16_text{u"Антиквариат"};
        antique.image = u16_text{u"book-1.png"};
        antique.system = true;
        antique.top = {{0.0453125f, 0.13645834f, 0.25677082f, 0.43489584f, 0.5f, 0.5494792f,
                        0.7375f, 0.8567708f, 0.95677084f},
                       {0.0074074073f, 0.013888889f, 0.011111111f, 0.0074074073f, 0.023148147f,
                        0.014814815f, 0.0129629625f, 0.016666668f, 0.016666668f}};
        antique.bottom = {{0.041145835f, 0.29947916f, 0.38489583f, 0.45625f, 0.5f, 0.5541667f,
                           0.6015625f, 0.69947916f, 0.95677084f},
                          {0.97037035f, 0.9759259f, 0.9861111f, 0.98333335f, 0.97175925f,
                           0.9851852f, 0.98796296f, 0.9861111f, 0.9759259f}};

        // Томик — снимок с прямыми краями: точки стоят на начальных местах, и
        // это не недоделка, а ответ. Гнуть тут нечего, и вёрстка это увидит
        // сама — размах выйдет нулевым, полоса нарисуется без изгиба вовсе.
        Skin tome = defaultSkin();
        tome.name = u16_text{u"Томик"};
        tome.image = u16_text{u"tom-1.png"};
        tome.system = true;

        Skin booklet;
        booklet.name = u16_text{u"Брошюра"};
        booklet.image = u16_text{u"Брошюра.png"};
        booklet.system = true;
        booklet.top = {{0.043335162f, 0.14920461f, 0.33790454f, 0.43938562f, 0.5f, 0.56939113f,
                        0.63960505f, 0.81130004f, 0.9511794f},
                       {0.07692308f, 0.054626532f, 0.005574136f, 0.0011148272f, 0.070234113f,
                        0.0f, 0.0011148272f, 0.04793757f, 0.0780379f}};
        booklet.bottom = {{0.036752604f, 0.15469007f, 0.29072955f, 0.3609435f, 0.5f, 0.5485464f,
                           0.6544158f, 0.7778387f, 0.9478881f},
                          {0.9531773f, 0.9319955f, 0.9632107f, 0.8361204f, 0.94147158f,
                           0.89966553f, 0.8573021f, 0.9509476f, 0.9587514f}};

        return std::vector<Skin>{std::move(antique), std::move(tome), std::move(booklet)};
    }();

    return list;
}

EdgeSpline::EdgeSpline(const EdgeCurve& curve) : curve_(curve) {
    // У каждого листа свои пять точек и четыре хорды; корешок входит в оба
    // листа и получает две касательные — по одной с каждой стороны. Так
    // кромка в нём непрерывна, но вольна ломаться: бумага там сгибается.
    // Иксы строго возрастают — это держат мастер и чтение реестра.
    for (size_t leaf = 0; leaf < 2; ++leaf) {
        const float* const x = &curve.x[leaf * (kLeaf - 1)];
        const float* const y = &curve.y[leaf * (kLeaf - 1)];
        float* const slope = &slope_[leaf * kLeaf];

        float step[kLeaf - 1];
        float chord[kLeaf - 1];
        for (size_t k = 0; k + 1 < kLeaf; ++k) {
            step[k] = x[k + 1] - x[k];
            chord[k] = (y[k + 1] - y[k]) / step[k];
        }

        // Вторые производные естественного сплайна: на концах листа ноль,
        // внутренние — из трёхдиагональной системы
        //   h[k-1]·M[k-1] + 2(h[k-1]+h[k])·M[k] + h[k]·M[k+1] = 6(d[k]-d[k-1]),
        // прогонкой: у системы диагональное преобладание, прогонка устойчива.
        float curvature[kLeaf] = {};
        float diagonal[kLeaf - 1];
        float right[kLeaf - 1];
        for (size_t k = 1; k + 1 < kLeaf; ++k) {
            diagonal[k] = 2.0f * (step[k - 1] + step[k]);
            right[k] = 6.0f * (chord[k] - chord[k - 1]);
            if (k > 1) {
                const float factor = step[k - 1] / diagonal[k - 1];
                diagonal[k] -= factor * step[k - 1];
                right[k] -= factor * right[k - 1];
            }
        }
        for (size_t k = kLeaf - 2; k > 0; --k) {
            curvature[k] = (right[k] - step[k] * curvature[k + 1]) / diagonal[k];
        }

        // Касательные — производные сплайна в точках: у каждого отрезка в
        // левом конце, у последней точки — в правом конце последнего отрезка.
        for (size_t k = 0; k + 1 < kLeaf; ++k) {
            slope[k] = chord[k] - step[k] * (2.0f * curvature[k] + curvature[k + 1]) / 6.0f;
        }
        slope[kLeaf - 1] = chord[kLeaf - 2] + step[kLeaf - 2] *
                                                  (curvature[kLeaf - 2] + 2.0f * curvature[kLeaf - 1]) /
                                                  6.0f;
    }
}

float EdgeSpline::at(float u) const {
    constexpr size_t last = kPoints - 1;

    if (u <= curve_.x[0]) return curve_.y[0];
    if (u >= curve_.x[last]) return curve_.y[last];

    // Лист — по корешку, отрезок — по точкам листа. `u` уже внутри кривой,
    // так что отрезок найдётся не позже последнего в листе.
    const size_t leaf = u < curve_.x[kSpine] ? 0 : 1;
    size_t k = leaf * (kLeaf - 1);
    while (u >= curve_.x[k + 1]) ++k;
    const size_t s = leaf * kLeaf + (k - leaf * (kLeaf - 1));

    // Кубический Эрмит на отрезке: базисные многочлены от доли t, касательные
    // приведены к длине отрезка. Записан как «начало плюс приращения», а не
    // как сумма h00·y0 + h01·y1: так прямая остаётся прямой без ошибки
    // округления, а в точке кривая равна точке.
    const float step = curve_.x[k + 1] - curve_.x[k];
    const float t = (u - curve_.x[k]) / step;
    const float t2 = t * t;
    const float t3 = t2 * t;
    const float h10 = t3 - 2.0f * t2 + t;
    const float h01 = -2.0f * t3 + 3.0f * t2;
    const float h11 = t3 - t2;
    return curve_.y[k] + (curve_.y[k + 1] - curve_.y[k]) * h01 +
           step * (h10 * slope_[s] + h11 * slope_[s + 1]);
}

namespace {

/// Лежат ли все точки кривой на одной высоте. Тогда кромка — ровно эта
/// высота всюду, без ошибки округления: хорды нулевые, касательные тоже, и
/// отрезок Эрмита отдаёт значение своей первой точки.
bool liesAt(const EdgeCurve& curve, float level) {
    return std::all_of(curve.y.begin(), curve.y.end(), [level](float y) { return y == level; });
}

/// Мера полосы, в которой переводится точка: на пустую доли не делятся.
void requireArea(PageWarp::Size area) {
    if (!(area.width > 0.0f && area.height > 0.0f))
        wxl::core::abort("PageWarp: точка переводится в пустой мере полосы");
}

}  // namespace

PageWarp::PageWarp(const Skin& skin)
    : upper_(skin.top),
      lower_(skin.bottom),
      flat_(liesAt(skin.top, kEdgeInset) && liesAt(skin.bottom, 1.0f - kEdgeInset)) {}

PageWarp::Column PageWarp::column(float u) const {
    return Column{upper_.at(u) - kEdgeInset, lower_.at(u) - (1.0f - kEdgeInset)};
}

PageWarp::Point PageWarp::toScreen(Point layout, Size area) const {
    requireArea(area);

    // Точка экрана y показывает вёрстку на y − (a + (b − a)·y/H)·H, где a и b —
    // отклонения верхней и нижней кромок в столбце. Отсюда y·(1 + a − b) =
    // y_вёрстки + a·H. Множитель — во сколько раз изгиб сжимает столбец по
    // высоте: на экране вся высота вёрстки занимает H/(1 + a − b), у прямых
    // кромок он единица. Ноль и ниже значили бы, что кромки разошлись против
    // прямых на целую высоту полосы и столбец растянулся бы без конца или
    // вывернулся. Таких кромок не ставит мастер — перенос точки, выворачивающий
    // столбец, он отвергает (SkinEditor::drag), — а правленный руками реестр
    // чтение делает ровным и называет (keepsColumns, Skins::loadFrom); сюда
    // они не доходят, и дошедшие — нарушенный инвариант.
    const Column edges = column(layout.x / area.width);
    const float stretch = 1.0f + edges.top - edges.bottom;
    if (!(stretch > 0.0f))
        wxl::core::abort("PageWarp::toScreen: кромки в столбце разошлись на высоту полосы");

    return Point{layout.x, (layout.y + edges.top * area.height) / stretch};
}

bool PageWarp::keepsColumns() const {
    // Между соседними точками обеих кривых каждая кромка — один кубический
    // отрезок (за крайними точками — постоянная), и b − a там — кубика.
    // Наибольшее её значение — на концах отрезка или в корне производной, а
    // сама кубика восстанавливается по четырём значениям на равных шагах:
    // касательные сплайна для этого не нужны.
    //
    // Узлы — края полосы и точки обеих кривых: их не больше, чем точек, и
    // память на каждый перенос точки в мастере не выделяется.
    constexpr size_t kKnots = 2 + 2 * static_cast<size_t>(EdgeCurve::kPoints);
    auto knots = std::array<float, kKnots>{0.0f, 1.0f};
    std::copy(upper_.curve().x.begin(), upper_.curve().x.end(), knots.begin() + 2);
    std::copy(lower_.curve().x.begin(), lower_.curve().x.end(),
              knots.begin() + 2 + EdgeCurve::kPoints);
    std::sort(knots.begin(), knots.end());
    const auto knotCount =
        static_cast<size_t>(std::unique(knots.begin(), knots.end()) - knots.begin());

    // Неконечное отклонение — не изгиб, а сломанная кромка: точки из
    // правленного руками файла встают и на субнормальном шаге, сплайн даёт на
    // нём бесконечность и NaN, а std::max с NaN пропустил бы его молча. Такая
    // кромка считается выворотом — файлу не верят.
    bool finite = true;
    const auto gapAt = [this, &finite](double u) {
        const Column edges = column(static_cast<float>(u));
        const double gap = static_cast<double>(edges.bottom) - static_cast<double>(edges.top);
        finite = finite && std::isfinite(gap);
        return gap;
    };

    double widest = gapAt(0.0);
    for (size_t index = 0; index + 1 < knotCount; ++index) {
        const double from = knots[index];
        const double span = static_cast<double>(knots[index + 1]) - from;
        if (from < 0.0 || from + span > 1.0 || !(span > 0.0)) continue;

        // Кубика по значениям в s = 0, 1/3, 2/3, 1 — коэффициенты Лагранжа.
        const double f0 = gapAt(from);
        const double f1 = gapAt(from + span / 3.0);
        const double f2 = gapAt(from + span * 2.0 / 3.0);
        const double f3 = gapAt(from + span);
        const double c1 = (-11.0 * f0 + 18.0 * f1 - 9.0 * f2 + 2.0 * f3) / 2.0;
        const double c2 = 9.0 * (2.0 * f0 - 5.0 * f1 + 4.0 * f2 - f3) / 2.0;
        const double c3 = 9.0 * (-f0 + 3.0 * f1 - 3.0 * f2 + f3) / 2.0;

        widest = std::max({widest, f0, f3});

        // Корни производной c1 + 2·c2·s + 3·c3·s² внутри отрезка.
        auto roots = std::array<double, 2>{};
        size_t rootCount = 0;
        if (std::abs(c3) > 1e-12) {
            const double discriminant = c2 * c2 - 3.0 * c3 * c1;
            if (discriminant >= 0.0) {
                const double root = std::sqrt(discriminant);
                roots[rootCount++] = (-c2 + root) / (3.0 * c3);
                roots[rootCount++] = (-c2 - root) / (3.0 * c3);
            }
        } else if (std::abs(c2) > 1e-12) {
            roots[rootCount++] = -c1 / (2.0 * c2);
        }
        for (size_t at = 0; at < rootCount; ++at)
            if (roots[at] > 0.0 && roots[at] < 1.0)
                widest = std::max(widest, gapAt(from + span * roots[at]));
    }

    return finite && widest < 1.0;
}

PageWarp::Point PageWarp::toLayout(Point screen, Size area) const {
    requireArea(area);
    const float shiftHere = shift(screen.x / area.width, screen.y / area.height);
    return Point{screen.x, screen.y - shiftHere * area.height};
}

std::optional<std::vector<CorrectedSkin>> Skins::loadFrom(std::string xml) {
    skins_.clear();

    std::vector<CorrectedSkin> correctedSkins;

    if (xml.empty()) return correctedSkins;

    try {
        wxl::xml::document document;
        const wxl::xml::node& root = document.load(std::move(xml));

        const Skin straight = defaultSkin();

        for (const wxl::xml::node& element : root.children_named("skin")) {
            Skin skin = straight;
            skin.name = attributeOf(element, "name");
            skin.image = attributeOf(element, "image");

            // Вторая версия писала листы порознь — topLeft и topRight; её
            // кривая собирается из обеих половин. Различаются они по именам
            // элементов, а не по версии файла: элемент сам говорит, что в нём.
            PointFaults pointFaults;
            if (const wxl::xml::node* curve = element.child("top")) {
                readPoints(*curve, skin.top, 0, kPoints, pointFaults);
            } else {
                skin.top = curveOfHalves(element.child("topLeft"), element.child("topRight"),
                                         skin.top, pointFaults);
            }
            if (const wxl::xml::node* curve = element.child("bottom")) {
                readPoints(*curve, skin.bottom, 0, kPoints, pointFaults);
            } else {
                skin.bottom = curveOfHalves(element.child("bottomLeft"),
                                            element.child("bottomRight"), skin.bottom, pointFaults);
            }

            // Корешок стоит на середине, что бы ни было написано в файле, — и
            // сдвинутый называется: его могли поправить руками, а страницы
            // режутся по середине всё равно. Вторая версия корешка не писала —
            // её кривая собрана с ним на середине, — так что сдвинут он может
            // быть только рукой.
            const bool spineMoved =
                skin.top.x[kSpine] != EdgeCurve::kSpineX || skin.bottom.x[kSpine] != EdgeCurve::kSpineX;
            skin.top.x[kSpine] = EdgeCurve::kSpineX;
            skin.bottom.x[kSpine] = EdgeCurve::kSpineX;

            // Кривая, у которой точки не идут слева направо, — не кривая:
            // такая кромка становится начальной прямой, а не роняет полосу.
            // Порядок — уже с корешком на середине: по нему её и рисуют.
            const bool unordered = !ascending(skin.top) || !ascending(skin.bottom);
            if (!ascending(skin.top)) skin.top = straight.top;
            if (!ascending(skin.bottom)) skin.bottom = straight.bottom;

            // Кромки, которые где-то между точками разошлись против прямых на
            // целую высоту полосы, вывернули бы столбец страницы: точку под
            // таким изгибом не перевести. Порознь кривые бывают и ни при чём —
            // выворачивает их пара, — так что ровной становится обложка.
            const bool folds = !PageWarp{skin}.keepsColumns();
            if (folds) {
                skin.top = straight.top;
                skin.bottom = straight.bottom;
            }

            // Обложка без имени не выбирается, без снимка не рисуется; такого
            // в файле, который писали мы, не бывает — но файл могли и
            // поправить руками. Она пропускается, и тоже не молча; что ещё в
            // ней было не так, неважно — её нет.
            if (skin.name.empty() || skin.image.empty()) {
                std::vector<SkinFault> skipReasons;
                if (skin.name.empty()) skipReasons.push_back(SkinFault::SkippedNoName);
                if (skin.image.empty()) skipReasons.push_back(SkinFault::SkippedNoImage);
                correctedSkins.push_back(CorrectedSkin{skin.name, std::move(skipReasons)});
                continue;
            }

            // Исправленная обложка — не молча: читатель видит страницу не
            // такой, какой правил файл, и должен знать почему. Из двух прямых
            // причина — та, что решила: выворот ровняет всю обложку.
            std::vector<SkinFault> faults;
            if (pointFaults.outsidePage) faults.push_back(SkinFault::OutsidePage);
            if (pointFaults.missingCoordinate) faults.push_back(SkinFault::MissingCoordinate);
            if (pointFaults.extraPoints) faults.push_back(SkinFault::ExtraPoints);
            if (spineMoved) faults.push_back(SkinFault::SpineMoved);
            if (folds) {
                faults.push_back(SkinFault::FoldsColumn);
            } else if (unordered) {
                faults.push_back(SkinFault::Unordered);
            }
            if (!faults.empty()) correctedSkins.push_back(CorrectedSkin{skin.name, std::move(faults)});

            skins_.push_back(std::move(skin));
        }
    } catch (const wxl::xml::exception&) {
        // Битый реестр — пустой список обложек, а не отказ запуститься:
        // встроенные темы никуда не деваются. Ловится только отказ чтения
        // XML: нехватка памяти и прочие беды программы — не «файл испорчен»,
        // они идут наверх.
        skins_.clear();
        return std::nullopt;
    }
    return correctedSkins;
}

std::string Skins::toXml() const {
    text_builder<sta_allocator> out;

    out.append("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n");
    out.format("<skins version=\"{}\">\n", kVersion);

    for (const Skin& skin : skins_) {
        out.format("  <skin name=\"{}\" image=\"{}\">\n", xmlValue(skin.name),
                   xmlValue(skin.image));
        writeCurve(out, "top", skin.top);
        writeCurve(out, "bottom", skin.bottom);
        out.append("  </skin>\n");
    }

    out.append("</skins>\n");
    return std::string(out.view());
}

const Skin* Skins::find(u16_view name) const {
    for (const Skin& skin : skins_) {
        if (skin.name == name) return &skin;
    }
    return nullptr;
}

void Skins::put(Skin skin) {
    for (Skin& known : skins_) {
        if (known.name == skin.name) {
            known = std::move(skin);
            return;
        }
    }
    skins_.push_back(std::move(skin));
}

bool Skins::remove(u16_view name) {
    for (auto it = skins_.begin(); it != skins_.end(); ++it) {
        if (it->name == name) {
            skins_.erase(it);
            return true;
        }
    }
    return false;
}

}  // namespace bukvitsa::reader
