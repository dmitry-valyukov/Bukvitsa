#include <algorithm>

// Свой заголовок после всех стандартных: он ведёт к импорту модуля книги, а
// стандартный заголовок после импорта MSVC уже не принимает.
#include "bukvitsa/typography/glyph_painter.h"

namespace bukvitsa::typography {
namespace {

/// Положение и толщина линейки — подчёркивания или зачёркивания. Их нет в
/// наборе глифов: это чертёж по метрикам шрифта, и только шрифт знает, на какой
/// высоте и какой толщины его проводить.
struct Rule {
    float offset;      ///< от базовой линии вниз
    float thickness;
};

Rule ruleOf(IDWriteFontFace* face, float fontSize, bool strikethrough) {
    DWRITE_FONT_METRICS metrics{};
    face->GetMetrics(&metrics);

    const float scale = fontSize / static_cast<float>(metrics.designUnitsPerEm);
    const float position = strikethrough ? metrics.strikethroughPosition : metrics.underlinePosition;
    const float thickness =
        strikethrough ? metrics.strikethroughThickness : metrics.underlineThickness;

    return {-position * scale, std::max(thickness * scale, 1.0f)};
}

}  // namespace

not_null<ID2D1Brush> TextBrushes::brushFor(TextRole role) const {
    switch (role) {
    case TextRole::Body:
        return body;
    case TextRole::NoteMark:
        return noteMark;
    }

    // Роль вне перечисления — испорченный стиль прогона, а не повод набрать
    // его чем придётся.
    wxl::core::abort("TextBrushes::brushFor: роль прогона вне перечисления");
}

void drawGlyphRun(ID2D1DeviceContext* context, const GlyphRun& run, float x,
                  float baseline, const TextBrushes& brushes) {
    if (!run.fontFace || run.glyphIndices.empty())
        return;

    ID2D1Brush* const brush = brushes.brushFor(run.style.role).get();

    DWRITE_GLYPH_RUN glyphRun{};
    glyphRun.fontFace = run.fontFace;
    glyphRun.fontEmSize = run.fontSize;
    glyphRun.glyphCount = static_cast<UINT32>(run.glyphIndices.size());
    glyphRun.glyphIndices = run.glyphIndices.data();
    glyphRun.glyphAdvances = run.advances.data();
    glyphRun.glyphOffsets = run.offsets.data();
    glyphRun.isSideways = FALSE;
    glyphRun.bidiLevel = run.bidiLevel;

    context->DrawGlyphRun(D2D1::Point2F(x, baseline), &glyphRun, brush,
                          DWRITE_MEASURING_MODE_NATURAL);

    for (const bool strikethrough : {false, true}) {
        if (!(strikethrough ? run.style.strikethrough : run.style.underline))
            continue;

        const Rule rule = ruleOf(run.fontFace, run.fontSize, strikethrough);
        context->FillRectangle(D2D1::RectF(x, baseline + rule.offset, x + run.width,
                                           baseline + rule.offset + rule.thickness),
                               brush);
    }
}

void drawLine(ID2D1DeviceContext* context, const Line& line, float x, float baseline,
              const TextBrushes& brushes) {
    for (const GlyphRun& run : line.runs)
        drawGlyphRun(context, run, x + run.originX, baseline, brushes);
}

}  // namespace bukvitsa::typography
