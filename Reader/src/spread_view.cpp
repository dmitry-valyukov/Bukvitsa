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

SpreadView::SpreadView(const Compositor& compositor, const ContainerVisual& scene,
                       Painter spreadPainter)
    : compositor_(compositor),
      spreadPainter_(std::move(spreadPainter)),
      leftPageVisual_(compositor.createSpriteVisual()),
      rightPageVisual_(compositor.createSpriteVisual()) {
    // Две страницы разворота — на сцене под листами, каждая в своей половине
    // окна. Размер и шов не наши: сцену (задний спрайт окна) wxl ресайзит
    // синхронно в WM_SIZE, а страницы берут у неё размер и половину ширины
    // выражениями композитора — те считаются на потоке DWM в том же кадре, и в
    // просвете быстрой растяжки окно остаётся покрытым без единого нашего
    // вызова (resize их не трогает, своего Size у них нет). Половина ширины
    // — это и есть корешок: поля симметричны (см. PageFlow::spine()). Задник
    // окна под ними без кисти (BookView::activate): красить под страницами
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
    for (size_t pageSide = 0; pageSide < 2; ++pageSide) {
        SpriteVisual const& sideVisual = pageVisual(pageSide);
        InsetClip sideCrop = compositor_.createInsetClip();
        if (pageSide == kLeftPage) {
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
    rasterPixels_ = pixels;
    if (rasterPool_.empty()) {
        settledRaster_ = addRaster();
        return;
    }
    for (std::optional<DrawingSurface>& poolRaster : rasterPool_)
        if (poolRaster) poolRaster->resize(pixels);
}

void SpreadView::show(bool shown) {
    shown_ = shown;
    leftPageVisual_.isVisible(shown);
    rightPageVisual_.isVisible(shown);
}

void SpreadView::redraw(RasterId targetRaster) {
    raster(targetRaster).draw(spreadPainter_);

    // Обе страницы — на осевший разворот. В чтении: до входа страницы скрыты,
    // а задником окна владеет заставка стартового экрана. Одна кисть на обе
    // страницы: растр один, крой у каждой свой. Кисть DrawingSurface на каждый
    // вызов новая, поверхность за ней та же.
    if (targetRaster != settledRaster_ || !shown_) return;
    CompositionSurfaceBrush const settledBrush = pageBrush(raster(settledRaster_));
    leftPageVisual_.brush(settledBrush);
    rightPageVisual_.brush(settledBrush);
    pageRasters_ = {settledRaster_, settledRaster_};
}

const DrawingSurface& SpreadView::raster(RasterId poolRaster) const {
    // Номер без растра — ошибка проводки: рисовать или одевать было бы не во
    // что, а молча взять другой растр значило бы показать не тот разворот.
    if (poolRaster >= rasterPool_.size() || !rasterPool_[poolRaster])
        core::abort("SpreadView: растра с этим номером нет");
    return *rasterPool_[poolRaster];
}

const SpriteVisual& SpreadView::pageVisual(size_t pageSide) const {
    return pageSide == kLeftPage ? leftPageVisual_ : rightPageVisual_;
}

void SpreadView::dressPage(size_t pageSide, RasterId wornRaster) {
    pageVisual(pageSide).brush(pageBrush(raster(wornRaster)));
    pageRasters_[pageSide] = wornRaster;
}

void SpreadView::dropSpareRasters() {
    auto const noneInFlight = [](RasterId) { return false; };
    while (spareRasters(noneInFlight) > 0) dropSpareRaster(noneInFlight);
}

bool SpreadView::worn(RasterId poolRaster) const {
    return poolRaster == settledRaster_ || poolRaster == pageRasters_[kLeftPage] ||
           poolRaster == pageRasters_[kRightPage];
}

SpreadView::RasterId SpreadView::addRaster() {
    for (RasterId poolRaster = 0; poolRaster < rasterPool_.size(); ++poolRaster)
        if (!rasterPool_[poolRaster]) {
            rasterPool_[poolRaster].emplace(compositor_, rasterPixels_);
            return poolRaster;
        }
    rasterPool_.emplace_back(std::in_place, compositor_, rasterPixels_);
    return static_cast<RasterId>(rasterPool_.size() - 1);
}

}  // namespace bukvitsa::reader
