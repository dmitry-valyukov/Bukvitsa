#include <windows.h>

#include <algorithm>

// Свой заголовок последним: он импортирует wxl.xml (см. store.h).
#include "store.h"

import wxl.core;

namespace bukvitsa::reader {

std::string xmlValue(const u16_view value) {
    return xml_escaped(value.to_utf8().chars());
}

std::string xmlValue(const std::wstring_view value) {
    // Здесь чинят по-настоящему, а не для порядка: через этот вызов уходят в
    // файл пути и имена файлов, а имя файла в Windows — просто последовательность
    // 16-битных чисел, и непарный суррогат в ней возможен. Взятый на веру, он
    // превратился бы в три байта, которых UTF-8 не знает, и при следующем
    // запуске wxl.xml отвергла бы весь файл — то есть реестр книг или настройки
    // пропали бы целиком из-за одного дурного имени.
    return xmlValue(u16_view(repaired(value)));
}

u16_text attributeOf(const wxl::xml::node& element, std::string_view name) {
    const auto value = element.attribute(name);
    return value ? value->to_utf16() : u16_text{};
}

uint64_t numberOf(const wxl::xml::node& element, std::string_view name,
                  uint64_t fallback) {
    const auto value = element.attribute(name);
    if (!value) return fallback;

    // Числом должно быть всё значение: разобралось не до конца — значит там не
    // число, и лучше умолчание, чем половина прочитанного. try_parse не трогает
    // результат, пока не разберёт весь текст, — умолчание и остаётся.
    uint64_t number = fallback;
    try_parse(value->chars(), number);
    return number;
}

double realOf(const wxl::xml::node& element, std::string_view name, double fallback) {
    const auto value = element.attribute(name);
    if (!value) return fallback;

    double number = fallback;
    try_parse(value->chars(), number);
    return number;
}


}  // namespace bukvitsa::reader
