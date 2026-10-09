#pragma once
// Всплывающая сноска.
//
// Сноски внизу полосы отложены не из лени: у них высота полосы зависит от
// того, что на неё попало, а что на неё попало — от высоты полосы, и пагинация
// перестаёт быть одним проходом. Всплывающая сноска этой связи не создаёт
// вовсе: страница уже сверстана, а сноска показывается поверх неё по щелчку и
// исчезает по Escape. Читателю она к тому же удобнее — не нужно искать глазами
// низ полосы и возвращаться обратно.
//
// Набирает сноску вёрстка (`typography::layoutNote`) тем же движком, что и
// книгу, только уже и мельче; здесь — место всплывашки, подложка и рисунок.
// Не поместилась — прокручивается.

// Свои заголовки со стандартными внутри — до всего, что тянет import
// wxl.core.
#include <optional>

#include "DrawingSurface.h"
#include "Object.h"
#include "pch.h"

// Последними: они импортируют wxl.core (тема — сама, книга — через модель,
// сноска — через заголовки вёрстки), после чего стандартный заголовок MSVC уже
// не принимает.
#include "bukvitsa/reader/book.h"
#include "bukvitsa/reader/theme.h"
#include "bukvitsa/typography/note.h"

namespace bukvitsa::reader {

class NotePopup {
public:
    explicit NotePopup(const wxl::Compositor& compositor);

    /// Элемент, который кладут поверх полосы набора.
    const wxl::UIElement& root() const { return root_; }

    /// Показывает тело сноски, стараясь встать рядом с её знаком.
    ///
    /// @param anchor низ знака сноски в координатах полосы, DIP.
    /// @param area   размер полосы: в него всплывашка и вписывается.
    /// @param scale  масштаб экрана: поверхность живёт в пикселях.
    void show(Book& book, const fb3::Node* note, wxl::Point anchor, wxl::Size area,
              const Theme& theme, float fontSize, float scale);

    void hide();
    bool visible() const { return visible_; }

private:
    void draw(const Theme& theme, float width, float height, float scale);

    wxl::Compositor compositor_;

    // Контролы — поля, построенные вместе со всплывашкой: дети выше корня.
    wxl::Grid paper_;   ///< подложка ростом с текст сноски
    wxl::SpriteVisual sprite_;
    wxl::ScrollViewer scroll_;
    wxl::Border root_;
    std::optional<wxl::DrawingSurface> surface_;

    typography::NoteLayout body_;   ///< тело показанной сноски; шрифты — у движка книги
    bool visible_ = false;
};

}  // namespace bukvitsa::reader
