#include <algorithm>
#include <utility>

// Свой первым после стандартных: он несёт импорт, после которого стандартный
// заголовок MSVC уже не принимает.
#include "bukvitsa/reader/skin_editor.h"

import wxl.core;

namespace bukvitsa::reader {

namespace {

constexpr size_t kSpine = static_cast<size_t>(EdgeCurve::kSpine);
constexpr size_t kLast = static_cast<size_t>(EdgeCurve::kPoints) - 1;

/// Имя, которое можно сохранить: текст поля чужой — он пришёл из контрола, —
/// и в обложку попадает проверенным, а не принятым на веру. Пустое и из одних
/// пробелов и табуляций — не имя.
std::optional<u16_view> nameOf(std::u16string_view typed) {
    const std::optional<u16_view> name = unicode::checked(typed);
    if (!name || trim(name->plain(), u" \t").empty()) return std::nullopt;
    return name;
}

}  // namespace

void SkinEditor::openNew(std::filesystem::path image) {
    image_ = std::move(image);
    skin_ = defaultSkin();
}

void SkinEditor::openEdit(const Skin& skin, std::filesystem::path image) {
    image_ = std::move(image);
    skin_ = skin;

    // Системную правят «на основе», а не поверх: записать поверх нечего — в
    // реестре её нет, она часть программы, и удалить такую запись потом было
    // бы нечем. Поэтому здесь она становится новой обложкой: пустое `image`
    // заставит сохранение скопировать снимок в skinDirectory(), а пустое имя —
    // спросить его, как у новой. Своя обложка правится молча, под своим именем.
    if (skin.system) {
        skin_.system = false;
        skin_.image = {};
        skin_.name = {};
    }
}

std::optional<SkinEditor::Grip> SkinEditor::gripAt(Point point, Size area) const {
    if (area.width <= 0.0f || area.height <= 0.0f) return std::nullopt;

    // Ближайший, а не первый попавшийся: в узком окне или у сдвинутых вплотную
    // соседок зоны захвата перекрываются, и щелчок прямо в кружочек должен
    // брать его, а не соседку.
    std::optional<Grip> nearest;
    float best = kGripReach * kGripReach;

    for (const Edge edge : {Edge::Top, Edge::Bottom}) {
        const EdgeCurve& edited = edge == Edge::Top ? skin_.top : skin_.bottom;
        for (size_t at = 0; at <= kLast; ++at) {
            const float dx = point.x - edited.x[at] * area.width;
            const float dy = point.y - edited.y[at] * area.height;
            const float distance = dx * dx + dy * dy;
            if (distance <= best && (!nearest || distance < best)) {
                best = distance;
                nearest = Grip{edge, at};
            }
        }
    }
    return nearest;
}

void SkinEditor::drag(Grip grip, Point to, Size area) {
    // Сетки нет — переводить указатель в доли не во что: деление на ноль
    // записало бы в точку бесконечность.
    if (area.width <= 0.0f || area.height <= 0.0f || grip.point > kLast) return;

    EdgeCurve& edited = grip.edge == Edge::Top ? skin_.top : skin_.bottom;
    const size_t at = grip.point;

    // Точка ходит в обе оси. По вертикали — от кромки до четверти высоты; по
    // горизонтали — между соседками, крайние — до кромок. Точка корешка по
    // горизонтали не ходит: корешок — середина разворота, там режутся
    // страницы и лежит тень шва, и излом кромки обязан стоять там же.
    edited.y[at] = grip.edge == Edge::Top
                       ? std::clamp(to.y / area.height, 0.0f, kEdgeReach)
                       : std::clamp(to.y / area.height, 1.0f - kEdgeReach, 1.0f);

    if (at != kSpine) {
        const float low = at == 0 ? 0.0f : edited.x[at - 1] + kMinGap;
        const float high = at == kLast ? 1.0f : edited.x[at + 1] - kMinGap;

        // Соседки ближе двух зазоров — только из поправленного руками реестра
        // (он требует лишь возрастания): точке между ними места нет, и по
        // горизонтали она стоит, а не получает перевёрнутые границы clamp.
        if (low <= high) edited.x[at] = std::clamp(to.x / area.width, low, high);
    }
}

bool SkinEditor::nameOk(std::u16string_view typed) {
    return nameOf(typed).has_value();
}

std::optional<Skin> SkinEditor::result(std::u16string_view typed) const {
    const std::optional<u16_view> name = nameOf(typed);
    if (!name) return std::nullopt;

    Skin saved = skin_;
    saved.name = u16_text{*name};
    return saved;
}

}  // namespace bukvitsa::reader
