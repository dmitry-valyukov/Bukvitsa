#include "spread_view.h"

namespace bukvitsa::reader {

using namespace wxl;

namespace {

/// Кисть страницы-визуала для растра: тянет его на весь визуал
/// (UniformToFill, как прежде задник окна). В просвете быстрой растяжки страница
/// уже нового размера, а растр ещё старого, и без растяжения его край сквозил
/// бы на рабочий стол.
CompositionSurfaceBrush pageBrush(DrawingSurface const& surface) {
    CompositionSurfaceBrush stretchedBrush = surface.brush();
    stretchedBrush.stretch(CompositionStretch::UniformToFill);
    return stretchedBrush;
}

}  // namespace

SpreadView::SpreadView(const Compositor& compositor, const ContainerVisual& scene, Painter painter)
    : compositor_(compositor),
      painter_(std::move(painter)),
      leftPageVisual_(compositor.createSpriteVisual()),
      rightPageVisual_(compositor.createSpriteVisual()) {
    // Две страницы разворота — на сцене под листами, каждая в своей половине
    // окна. Размер и шов не наши: сцену (задний спрайт окна) wxl ресайзит
    // синхронно в WM_SIZE, а страницы берут у неё размер и половину ширины
    // выражениями композитора — те считаются на потоке DWM в том же кадре, и в
    // просвете быстрой растяжки окно остаётся покрытым без единого нашего
    // вызова (resize их не трогает, своего Size у них нет). Половина ширины
    // — это и есть корешок: поля симметричны (см. PageFlow::spine()). Задник
    // окна под ними без кисти (BookView::setActive): красить под страницами
    // нечего.
    ExpressionAnimation const sizeOfScene = compositor_.createExpressionAnimation(L"scene.Size");
    sizeOfScene.setReferenceParameter(L"scene", scene);

    // Шов страниц — на целом пикселе. При нечётной ширине половина дробная, и
    // клип по дробной кромке кроет средний столбец наполовину с каждой
    // стороны: два полупрозрачных края складываются в просвет насквозь, до
    // рабочего стола, — красить под страницами нечего. Левая страница — до
    // целого, правая — от него, и при нечётной ширине она на пиксель шире.
    ExpressionAnimation const leftPageInset =
        compositor_.createExpressionAnimation(L"scene.Size.X - Floor(scene.Size.X * 0.5)");
    leftPageInset.setReferenceParameter(L"scene", scene);
    ExpressionAnimation const rightPageInset =
        compositor_.createExpressionAnimation(L"Floor(scene.Size.X * 0.5)");
    rightPageInset.setReferenceParameter(L"scene", scene);
    for (size_t side = 0; side < 2; ++side) {
        SpriteVisual const& sideVisual = pageVisual(side);
        InsetClip sideCrop = compositor_.createInsetClip();
        if (side == kLeftPage) {
            sideCrop.startAnimation(L"RightInset", leftPageInset);
        } else {
            sideCrop.startAnimation(L"LeftInset", rightPageInset);
        }
        sideVisual.clip(sideCrop);
        sideVisual.startAnimation(L"Size", sizeOfScene);
        sideVisual.isVisible(false);
        scene.children().insertAtBottom(sideVisual);
    }
}

void SpreadView::resize(SizeInt32 pixels) {
    pixels_ = pixels;
    if (rasters_.empty()) {
        settledRaster_ = addRaster();
        return;
    }
    for (std::optional<DrawingSurface>& raster : rasters_)
        if (raster) raster->resize(pixels);
}

void SpreadView::show(bool shown) {
    shown_ = shown;
    leftPageVisual_.isVisible(shown);
    rightPageVisual_.isVisible(shown);
}

void SpreadView::redraw(RasterId target) {
    raster(target).draw(painter_);

    // Обе страницы — на осевший разворот. В чтении: до входа страницы скрыты,
    // а задником окна владеет заставка стартового экрана. Одна кисть на обе
    // страницы: растр один, крой у каждой свой. Кисть DrawingSurface на каждый
    // вызов новая, поверхность за ней та же.
    if (target != settledRaster_ || !shown_) return;
    CompositionSurfaceBrush const settledBrush = pageBrush(raster(settledRaster_));
    leftPageVisual_.brush(settledBrush);
    rightPageVisual_.brush(settledBrush);
    pageRasters_ = {settledRaster_, settledRaster_};
}

const DrawingSurface& SpreadView::raster(RasterId raster) const {
    // Номер без растра — ошибка проводки: рисовать или одевать было бы не во
    // что, а молча взять другой растр значило бы показать не тот разворот.
    if (raster >= rasters_.size() || !rasters_[raster])
        core::abort("SpreadView: растра с этим номером нет");
    return *rasters_[raster];
}

const SpriteVisual& SpreadView::pageVisual(size_t side) const {
    return side == kLeftPage ? leftPageVisual_ : rightPageVisual_;
}

void SpreadView::dressPage(size_t side, RasterId raster) {
    pageVisual(side).brush(pageBrush(this->raster(raster)));
    pageRasters_[side] = raster;
}

void SpreadView::dropSpareRasters() {
    auto const none = [](RasterId) { return false; };
    while (spareRasters(none) > 0) dropSpareRaster(none);
}

bool SpreadView::worn(RasterId raster) const {
    return raster == settledRaster_ || raster == pageRasters_[kLeftPage] ||
           raster == pageRasters_[kRightPage];
}

SpreadView::RasterId SpreadView::addRaster() {
    for (RasterId raster = 0; raster < rasters_.size(); ++raster)
        if (!rasters_[raster]) {
            rasters_[raster].emplace(compositor_, pixels_);
            return raster;
        }
    rasters_.emplace_back(std::in_place, compositor_, pixels_);
    return static_cast<RasterId>(rasters_.size() - 1);
}

}  // namespace bukvitsa::reader
