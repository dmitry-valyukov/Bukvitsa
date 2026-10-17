#include <algorithm>

#include "bukvitsa/reader/note_bands.h"

// Импорт последним: остановка — у wxl.core.
import wxl.core;

namespace bukvitsa::reader {

void NoteBands::reset(const NoteBandsGeometry& geometry) {
    if (geometry.surfaceWidth < 0 || geometry.surfaceHeight < 0 || geometry.viewportHeight < 0 ||
        geometry.bandHeight <= 0 || geometry.maxUpdateSide <= 0) {
        wxl::core::abort(
            "NoteBands: мера решётки отрицательна, а полоса или предел устройства — не больше нуля");
    }
    geometry_ = geometry;
    geometry_.bandHeight = std::min(geometry.bandHeight, geometry.maxUpdateSide);

    const int64_t height = geometry_.surfaceHeight;
    const int64_t band = geometry_.bandHeight;
    drawnBands_.assign(static_cast<size_t>((height + band - 1) / band), false);
}

NoteBands::BandRange NoteBands::bandsAcross(int64_t top, int64_t bottom) const {
    top = std::max<int64_t>(top, 0);
    bottom = std::min<int64_t>(bottom, geometry_.surfaceHeight);
    if (top >= bottom) return {};
    const int64_t band = geometry_.bandHeight;
    return {static_cast<int32_t>(top / band), static_cast<int32_t>((bottom + band - 1) / band)};
}

void NoteBands::appendPieces(int32_t newBand, std::vector<NotePixelRect>& newPieces) const {
    // Полоса начинается внутри сноски, так что её верх — в int32_t; шаг по
    // ширине — в 64 битах: x + предел может выйти за int32_t.
    const int32_t top = newBand * geometry_.bandHeight;
    const int32_t height = std::min(geometry_.bandHeight, geometry_.surfaceHeight - top);
    for (int64_t x = 0; x < geometry_.surfaceWidth; x += geometry_.maxUpdateSide) {
        const auto width =
            static_cast<int32_t>(std::min<int64_t>(geometry_.maxUpdateSide, geometry_.surfaceWidth - x));
        newPieces.push_back({static_cast<int32_t>(x), top, width, height});
    }
}

NoteBandsStep NoteBands::advance(std::initializer_list<int32_t> viewTops) {
    NoteBandsStep step;
    const int64_t viewport = geometry_.viewportHeight;
    std::vector<bool> keptBands(drawnBands_.size(), false);

    // Окна в 64 битах: верх вида и три окна над ним в int32_t могут не уместиться.
    for (const int32_t viewTop : viewTops) {
        const BandRange wantedBands = bandsAcross(viewTop - viewport, viewTop + 2 * viewport);
        for (int32_t wantedBand = wantedBands.first; wantedBand < wantedBands.last; ++wantedBand) {
            if (drawnBands_[static_cast<size_t>(wantedBand)]) continue;
            drawnBands_[static_cast<size_t>(wantedBand)] = true;
            appendPieces(wantedBand, step.drawRects);
        }
        const BandRange heldBands = bandsAcross(viewTop - 2 * viewport, viewTop + 3 * viewport);
        for (int32_t heldBand = heldBands.first; heldBand < heldBands.last; ++heldBand)
            keptBands[static_cast<size_t>(heldBand)] = true;
    }

    for (size_t drawnBand = 0; drawnBand < drawnBands_.size(); ++drawnBand) {
        if (drawnBands_[drawnBand] && !keptBands[drawnBand]) {
            drawnBands_[drawnBand] = false;
            step.trimNeeded = true;
        }
    }

    for (int32_t runFirst = 0; runFirst < bandCount();) {
        if (!drawnBands_[static_cast<size_t>(runFirst)]) {
            ++runFirst;
            continue;
        }
        int32_t runLast = runFirst + 1;
        while (runLast < bandCount() && drawnBands_[static_cast<size_t>(runLast)]) ++runLast;
        const int32_t top = runFirst * geometry_.bandHeight;
        const auto bottom = static_cast<int32_t>(
            std::min<int64_t>(int64_t{runLast} * geometry_.bandHeight, geometry_.surfaceHeight));
        step.keepRects.push_back({0, top, geometry_.surfaceWidth, bottom - top});
        runFirst = runLast;
    }
    return step;
}

}  // namespace bukvitsa::reader
