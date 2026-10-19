#include <algorithm>
#include <cstddef>
#include <span>

#include "bukvitsa/reader/note_frame.h"

// Последними: заголовок ведёт к импорту модулей вёрстки, остановка — из
// wxl.core.
#include "bukvitsa/reader/note_scroll.h"

import wxl.core;

namespace bukvitsa::reader {

namespace {

/// Допуск сравнения края окна с краем строки, DIP. Окно прокрутки встаёт в
/// целые пиксели экрана, и верх, поставленный ровно на край строки,
/// возвращается из пикселей на долю пикселя выше или ниже; без допуска шаг
/// на строку с края строки остался бы на месте или перескочил бы через неё.
constexpr double kEdgeSlack = 0.5;

/// Половина интерлиньяжа строки — то, что лежит над подъёмом и под выносом.
/// Межстрочный множитель меньше единицы даёт его отрицательным; тогда края
/// строки — по подъёму и выносу, иначе край окна прошёл бы по её чернилам.
double halfLeadingOf(const typography::NoteLine& noteLine) {
    const typography::Line& line = noteLine.line;
    return std::max(0.0, (static_cast<double>(line.height) - line.ascent - line.descent) * 0.5);
}

/// Края строки на подложке: подъём и вынос с половиной интерлиньяжа.
double topOf(const typography::NoteLine& noteLine) {
    return kNotePadding + static_cast<double>(noteLine.baseline) - noteLine.line.ascent -
           halfLeadingOf(noteLine);
}

double bottomOf(const typography::NoteLine& noteLine) {
    return kNotePadding + static_cast<double>(noteLine.baseline) + noteLine.line.descent +
           halfLeadingOf(noteLine);
}

double heightOf(const typography::NoteLine& noteLine) {
    return bottomOf(noteLine) - topOf(noteLine);
}

/// Верх строки для шага на строку: у первой — край подложки, иначе шаг вниз с
/// начала ушёл бы на одно поле над ней.
double stepTopOf(std::span<const typography::NoteLine> bodyLines, size_t index) {
    return index == 0 ? 0.0 : topOf(bodyLines[index]);
}

/// Шаг на окно, когда опереться не на что: строки на краю нет или она выше
/// окна. Окно без строки у края — она остаётся на виду и связывает старый вид
/// с новым; но не меньше половины окна: строка выше окна съела бы шаг целиком.
double pageStep(double edgeLineHeight, double windowHeight) {
    return std::max(windowHeight - edgeLineHeight, windowHeight * 0.5);
}

double pageDown(std::span<const typography::NoteLine> bodyLines, double viewTop,
                double windowHeight) {
    if (bodyLines.empty()) return viewTop + windowHeight;
    const double viewBottom = viewTop + windowHeight;
    for (const typography::NoteLine& noteLine : bodyLines) {
        if (bottomOf(noteLine) <= viewBottom + kEdgeSlack) continue;   // видна целиком

        // Первая строка, которую окно не показало целиком, — новая верхняя,
        // если от этого есть ход: строка выше окна начинается не ниже его верха.
        const double lineTop = topOf(noteLine);
        if (lineTop > viewTop + kEdgeSlack) return lineTop;
        return viewTop + pageStep(heightOf(noteLine), windowHeight);
    }
    // Ниже окна — только поле: у края последняя строка.
    return viewTop + pageStep(heightOf(bodyLines.back()), windowHeight);
}

double pageUp(std::span<const typography::NoteLine> bodyLines, double viewTop,
              double windowHeight) {
    for (auto noteLine = bodyLines.rbegin(); noteLine != bodyLines.rend(); ++noteLine) {
        if (topOf(*noteLine) >= viewTop - kEdgeSlack) continue;   // видна целиком или ниже

        // Последняя строка, которую окно не показало сверху целиком, — новая
        // нижняя, если от этого есть ход.
        const double target = bottomOf(*noteLine) - windowHeight;
        if (target < viewTop - kEdgeSlack) return std::max(target, 0.0);
        return std::max(viewTop - pageStep(heightOf(*noteLine), windowHeight), 0.0);
    }
    // Выше окна строк нет — над ним одно поле.
    return 0.0;
}

double lineDown(std::span<const typography::NoteLine> bodyLines, double viewTop,
                double windowHeight, double paperHeight) {
    size_t nextIndex = 0;
    while (nextIndex < bodyLines.size() && stepTopOf(bodyLines, nextIndex) <= viewTop + kEdgeSlack)
        ++nextIndex;
    // Ниже нет начала ни одной строки — остался низ последней и поле под ней.
    const double nextTop =
        nextIndex < bodyLines.size() ? stepTopOf(bodyLines, nextIndex) : paperHeight;
    if (nextIndex == 0) return nextTop;

    // Строка перед следующей уходит за низ окна: прыжок к следующей пропустил
    // бы её низ. Помещается — встаёт низом к низу окна, иначе — по полуокну.
    const typography::NoteLine& cutLine = bodyLines[nextIndex - 1];
    if (bottomOf(cutLine) > viewTop + windowHeight + kEdgeSlack) {
        if (heightOf(cutLine) <= windowHeight) return bottomOf(cutLine) - windowHeight;
        return viewTop + windowHeight * 0.5;
    }
    return nextTop;
}

double lineUp(std::span<const typography::NoteLine> bodyLines, double viewTop,
              double windowHeight) {
    size_t index = bodyLines.size();
    while (index > 0 && stepTopOf(bodyLines, index - 1) >= viewTop - kEdgeSlack) --index;
    if (index == 0) return 0.0;   // выше — одно поле

    // Окно, вставшее на верх строки, может срезать её низ: у первой строки
    // верх — край подложки с полем над ней, а иная строка выше окна. Строка,
    // которая помещается, тогда встаёт низом к низу окна; та, что выше окна,
    // — по полуокну, если иначе между видами остался бы её кусок.
    const typography::NoteLine& previousLine = bodyLines[index - 1];
    const double previousTop = stepTopOf(bodyLines, index - 1);
    if (bottomOf(previousLine) > previousTop + windowHeight + kEdgeSlack) {
        const double fitted = bottomOf(previousLine) - windowHeight;
        if (heightOf(previousLine) <= windowHeight && fitted < viewTop - kEdgeSlack)
            return std::max(fitted, 0.0);
        if (previousTop + windowHeight < viewTop - kEdgeSlack)
            return std::max(viewTop - windowHeight * 0.5, 0.0);
    }
    return previousTop;
}

}  // namespace

double noteScrollTarget(std::span<const typography::NoteLine> bodyLines, double viewTop,
                        double windowHeight, double paperHeight, NoteScroll scroll) {
    // Помещается — прокручивать нечего, и верх у окна один.
    if (paperHeight <= windowHeight) return 0.0;

    switch (scroll) {
        case NoteScroll::LineDown:
            return lineDown(bodyLines, viewTop, windowHeight, paperHeight);
        case NoteScroll::LineUp:
            return lineUp(bodyLines, viewTop, windowHeight);
        case NoteScroll::PageDown:
            return pageDown(bodyLines, viewTop, windowHeight);
        case NoteScroll::PageUp:
            return pageUp(bodyLines, viewTop, windowHeight);
        case NoteScroll::ToStart:
            return 0.0;
        case NoteScroll::ToEnd:
            return paperHeight;
    }
    wxl::core::abort("noteScrollTarget: род прокрутки вне перечисления");
}

}  // namespace bukvitsa::reader
