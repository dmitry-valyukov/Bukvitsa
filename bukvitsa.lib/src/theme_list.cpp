#include <iterator>
#include <span>

// Свой заголовок после стандартных: он несёт импорт wxl.core.
#include "bukvitsa/reader/theme_list.h"

import wxl.core;

namespace bukvitsa::reader {

void ThemeList::setSkins(std::vector<Skin> fromRegistry) {
    const std::span<const Skin> system = systemSkins();

    skins_.assign(system.begin(), system.end());
    skins_.insert(skins_.end(), std::make_move_iterator(fromRegistry.begin()),
                  std::make_move_iterator(fromRegistry.end()));
}

int ThemeList::count() const {
    return kThemeCount + static_cast<int>(skins_.size());
}

int ThemeList::normalize(int index) const {
    const int all = count();
    return ((index % all) + all) % all;
}

const Theme& ThemeList::paperAt(int index) const {
    const int at = normalize(index);
    return at < kThemeCount ? kThemes[at] : kSkinTheme;
}

const Skin* ThemeList::skinAt(int index) const {
    if (index < kThemeCount || index >= count()) return nullptr;
    return &skins_[static_cast<size_t>(index - kThemeCount)];
}

std::optional<int> ThemeList::indexOfSkin(u16_view name) const {
    for (size_t at = 0; at < skins_.size(); ++at) {
        if (skins_[at].name == name) return kThemeCount + static_cast<int>(at);
    }
    return std::nullopt;
}

int ThemeList::indexOfTheme(u16_view themeId) const {
    return themeById(themeId);
}

u16_view ThemeList::themeIdAt(int index) const {
    if (index < 0 || index >= kThemeCount) return {};
    return kThemes[index].id;
}

ThemeList::Persisted ThemeList::persist(int index, u16_view previousThemeId) const {
    if (const Skin* skin = skinAt(index)) return Persisted{u16_text{previousThemeId}, skin->name};

    // Свободная функция темы, а не член: у неё номер вне встроенных отвечает
    // первой темой, и в настройки не уходит пустой ключ.
    return Persisted{u16_text{reader::themeIdAt(index)}, u16_text{}};
}

int ThemeList::indexFromSettings(u16_view theme, u16_view skin) const {
    if (!skin.empty()) {
        if (const std::optional<int> found = indexOfSkin(skin)) return *found;
    }
    return indexOfTheme(theme);
}

int ThemeList::afterRemoval(u16_view removedName, int current, u16_view fallbackThemeId) const {
    const std::optional<size_t> removed = readerSkin(removedName);
    const Skin* active = skinAt(current);

    // Встроенная тема удалением не двигается, а удаляемой может уже и не
    // быть: реестр успел перемениться под руками.
    if (!active || !removed) return current;

    const size_t activeAt = static_cast<size_t>(current - kThemeCount);
    if (activeAt == *removed) return indexOfTheme(fallbackThemeId);

    return activeAt > *removed ? current - 1 : current;
}

int ThemeList::afterSave(u16_view savedName, int current) const {
    const std::optional<size_t> saved = readerSkin(savedName);
    return saved ? kThemeCount + static_cast<int>(*saved) : current;
}

std::optional<size_t> ThemeList::readerSkin(u16_view name) const {
    for (size_t at = 0; at < skins_.size(); ++at) {
        if (!skins_[at].system && skins_[at].name == name) return at;
    }
    return std::nullopt;
}

}  // namespace bukvitsa::reader
