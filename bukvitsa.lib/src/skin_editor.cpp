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

}  // namespace

void SkinEditor::openNew(std::filesystem::path image) {
    image_ = std::move(image);
    skin_ = defaultSkin();
    pendingTicket_ = 0;
    name.set({});
    naming_.set(false);
    shownPreview_.set(SkinPreview{skin_, image_});
}

void SkinEditor::openEdit(const Skin& skin, std::filesystem::path image) {
    image_ = std::move(image);
    skin_ = skin;
    pendingTicket_ = 0;

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
    name.set(skin_.name);
    naming_.set(false);
    shownPreview_.set(SkinPreview{skin_, image_});
}

void SkinEditor::drop() {
    // Закрытой правке отпускать нечего: точку тянут только в открытом мастере.
    // Принять отпущенную значило бы открыть правку снова — полоса показала бы
    // предпросмотр, которого никто не открывал.
    if (!shownPreview_.get()) wxl::core::abort("SkinEditor::drop: правка закрыта");
    shownPreview_.set(SkinPreview{skin_, image_});
}

void SkinEditor::close() {
    pendingTicket_ = 0;
    naming_.set(false);
    shownPreview_.set(std::nullopt);
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
    const float wasX = edited.x[at];
    const float wasY = edited.y[at];

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

    // Перенос, после которого кромки где-то между точками разошлись на высоту
    // полосы и вывернули столбец, не принимается вовсе: точка остаётся, где
    // была. Прижать её некуда — выворачивает пара кромок, а не одна точка, —
    // а такой столбец карта смещения не нарисует, и перевести в нём точку
    // указателя обратно в вёрстку нельзя.
    if (!PageWarp{skin_}.keepsColumns()) {
        edited.x[at] = wasX;
        edited.y[at] = wasY;
    }
}

bool SkinEditor::nameOk() const {
    // Пустое и из одних пробелов и табуляций — не имя. Проверять сам текст не
    // нужно: поле держит `u16_text`, и привязка кладёт в него текст контрола
    // уже исправленным.
    const u16_view typed = name.get();
    return !trim(typed.plain(), u" \t").empty();
}

std::optional<Skin> SkinEditor::result() const {
    if (!nameOk()) return std::nullopt;

    Skin saved = skin_;
    saved.name = name.get();
    return saved;
}

void SkinEditor::askName() {
    // Каждое — сбой того, кто просит: закрытый мастер имени не спрашивает,
    // «Сохранить» при идущем сохранении молчит (saving()), а при открытом
    // диалоге значит его «ОК» (naming()).
    if (!shownPreview_.get()) wxl::core::abort("SkinEditor::askName: правка закрыта");
    if (saving()) wxl::core::abort("SkinEditor::askName: сохранение уже идёт");
    if (naming_.get()) wxl::core::abort("SkinEditor::askName: диалог имени уже открыт");

    name.set(skin_.name);
    naming_.set(true);
}

void SkinEditor::cancelName() {
    naming_.set(false);
}

SkinSave SkinEditor::beginSave() {
    // Каждое из трёх — сбой того, кто просит: мастер спрашивает saving() и
    // nameOk() раньше, а закрытый мастер сохранить не просит. Начать всё равно
    // значило бы записать вторую копию или безымянную обложку.
    if (!shownPreview_.get()) wxl::core::abort("SkinEditor::beginSave: правка закрыта");
    if (saving()) wxl::core::abort("SkinEditor::beginSave: сохранение уже идёт");
    std::optional<Skin> savedSkin = needsName() ? result() : std::optional<Skin>{skin_};
    if (!savedSkin) wxl::core::abort("SkinEditor::beginSave: имя не годится");

    pendingTicket_ = ++lastTicket_;
    naming_.set(false);
    return SkinSave{std::move(*savedSkin), image_, pendingTicket_};
}

bool SkinEditor::saveEnded(uint64_t ticket) {
    if (ticket != pendingTicket_) return false;
    pendingTicket_ = 0;
    return true;
}

}  // namespace bukvitsa::reader
