#include <algorithm>
#include <cmath>
#include <utility>

// Заголовки проекта после стандартных: store.h несёт импорт, после которого
// стандартный заголовок MSVC уже не принимает. Свой первым.
#include "skins.h"

#include "imaging.h"   // exeDirectory(): у системной обложки снимок в Assets
#include "settings.h"
#include "store.h"

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

/// Читает до `count` дочерних `<point>` в точки кривой, начиная с `first`.
/// Чего в файле нет или что вылезло из долей — остаётся как было: файл могли
/// поправить руками, и это не повод ронять обложку.
void readPoints(const wxl::xml::node& element, EdgeCurve& curve, size_t first, size_t count) {
    size_t index = first;
    for (const wxl::xml::node& point : element.children_named("point")) {
        if (index >= first + count) break;
        curve.x[index] =
            std::clamp(static_cast<float>(realOf(point, "x", curve.x[index])), 0.0f, 1.0f);
        curve.y[index] =
            std::clamp(static_cast<float>(realOf(point, "y", curve.y[index])), 0.0f, 1.0f);
        ++index;
    }
}

/// Кривая из записи второй версии, где листы описывались порознь: пять точек
/// левого и пять правого. Последняя точка левого и первая правого стояли у
/// корешка обе — теперь там одна точка: на середине, высотой посередине
/// между ними.
EdgeCurve curveOfHalves(const wxl::xml::node* leftHalf, const wxl::xml::node* rightHalf,
                        EdgeCurve fallback) {
    EdgeCurve curve = fallback;
    if (leftHalf) readPoints(*leftHalf, curve, 0, kSpine + 1);

    if (rightHalf) {
        // Точки правого листа ложатся с корешка, поэтому читаются в копию —
        // иначе первая из них затёрла бы точку левого до усреднения.
        EdgeCurve right = curve;
        readPoints(*rightHalf, right, kSpine, kSpine + 1);

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
        antique.name = L"Антиквариат";
        antique.image = L"book-1.png";
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
        tome.name = L"Томик";
        tome.image = L"tom-1.png";
        tome.system = true;

        Skin booklet;
        booklet.name = L"Брошюра";
        booklet.image = L"Брошюра.png";
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

std::filesystem::path skinImagePath(const Skin& skin) {
    if (skin.image.empty()) return {};

    return skin.system ? exeDirectory() / L"Assets" / skin.image : skinDirectory() / skin.image;
}

float edgeAt(const EdgeCurve& curve, float u) {
    constexpr size_t last = kPoints - 1;

    if (u <= curve.x[0]) return curve.y[0];
    if (u >= curve.x[last]) return curve.y[last];

    // Какой лист: левый описывают точки до корешка включительно, правый — от
    // корешка; сам корешок — середина разворота. Обе кривые проходят через
    // его точку, так что край в нём непрерывен.
    const size_t first = u < curve.x[kSpine] ? 0 : kSpine;
    const float* const px = &curve.x[first];
    const float* const py = &curve.y[first];

    // Кривая Безье четвёртой степени: пять точек листа — её управляющая
    // ломаная. Кривая проходит только через крайние точки, средние тянут её к
    // себе — зато она не выскакивает за свою ломаную, как это делал между
    // точками Катмулл-Ром, и край выходит спокойным при любой расстановке.
    const auto at = [px, py](float t) {
        const float s = 1.0f - t;
        const float w0 = s * s * s * s;
        const float w1 = 4.0f * s * s * s * t;
        const float w2 = 6.0f * s * s * t * t;
        const float w3 = 4.0f * s * t * t * t;
        const float w4 = t * t * t * t;
        return std::pair{w0 * px[0] + w1 * px[1] + w2 * px[2] + w3 * px[3] + w4 * px[4],
                         w0 * py[0] + w1 * py[1] + w2 * py[2] + w3 * py[3] + w4 * py[4]};
    };

    // Кривая параметрическая, а спрашивают её по столбцу u, поэтому t ищется
    // по x бисекцией: иксы точек идут по порядку (клампы перетаскивания это
    // держат), значит x(t) монотонен. Двадцать делений — миллионная доля
    // ширины, карте хватает с запасом; замкнутой формулы у корня четвёртой
    // степени всё равно нет.
    float low = 0.0f;
    float high = 1.0f;
    for (int step = 0; step < 20; ++step) {
        const float mid = 0.5f * (low + high);
        if (at(mid).first < u) {
            low = mid;
        } else {
            high = mid;
        }
    }
    return at(0.5f * (low + high)).second;
}

void Skins::loadFrom(std::string xml) {
    skins_.clear();

    if (xml.empty()) return;

    try {
        wxl::xml::document document;
        const wxl::xml::node& root = document.load(std::move(xml));

        for (const wxl::xml::node& element : root.children_named("skin")) {
            Skin skin = defaultSkin();
            skin.name = attributeOf(element, "name").wchars();
            skin.image = attributeOf(element, "image").wchars();

            // Вторая версия писала листы порознь — topLeft и topRight; её
            // кривая собирается из обеих половин. Различаются они по именам
            // элементов, а не по версии файла: элемент сам говорит, что в нём.
            if (const wxl::xml::node* curve = element.child("top")) {
                readPoints(*curve, skin.top, 0, kPoints);
            } else {
                skin.top = curveOfHalves(element.child("topLeft"), element.child("topRight"),
                                         skin.top);
            }
            if (const wxl::xml::node* curve = element.child("bottom")) {
                readPoints(*curve, skin.bottom, 0, kPoints);
            } else {
                skin.bottom = curveOfHalves(element.child("bottomLeft"),
                                            element.child("bottomRight"), skin.bottom);
            }

            // Корешок стоит на середине, что бы ни было написано в файле: его
            // могли поправить руками, а страницы режутся по середине всё равно.
            skin.top.x[kSpine] = EdgeCurve::kSpineX;
            skin.bottom.x[kSpine] = EdgeCurve::kSpineX;

            // Обложка без имени не выбирается, без снимка не рисуется; такого
            // в файле, который писали мы, не бывает — но файл могли и
            // поправить руками.
            if (!skin.name.empty() && !skin.image.empty()) skins_.push_back(std::move(skin));
        }
    } catch (...) {
        // Битый реестр — пустой список обложек, а не отказ запуститься:
        // встроенные темы никуда не деваются.
        skins_.clear();
    }
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

const Skin* Skins::find(std::wstring_view name) const {
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

bool Skins::remove(std::wstring_view name) {
    for (auto it = skins_.begin(); it != skins_.end(); ++it) {
        if (it->name == name) {
            skins_.erase(it);
            return true;
        }
    }
    return false;
}

std::filesystem::path skinsPath() {
    const std::filesystem::path directory = dataDirectory();

    return directory.empty() ? std::filesystem::path{} : directory / L"skins.xml";
}

std::filesystem::path skinDirectory() {
    const std::filesystem::path directory = dataDirectory();

    return directory.empty() ? std::filesystem::path{} : directory / L"skins";
}

}  // namespace bukvitsa::reader
