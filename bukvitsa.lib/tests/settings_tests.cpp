// Тесты файла настроек: разбор и запись — чистые функции над текстом, и то,
// что читатель выставил, проверяется дорогой «записали → прочитали» без
// диска. Старые файлы — тоже здесь: чего в них нет, то читается умолчанием,
// а соседние поля читаются, как были.

#include <cstdio>
#include <string>

#include "check.h"

// Заголовок модели — последним: он несёт импорт wxl.core.
#include "bukvitsa/reader/settings.h"

import wxl.core;

using namespace bukvitsa::reader;
using bukvitsa::reader::tests::check;

namespace {

/// Файл четвёртой версии — такой писала читалка до анимации листания.
constexpr const char* kOlderFile =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<settings version=\"4\">\n"
    "  <reading continue=\"true\"/>\n"
    "  <text theme=\"Night\" fontSize=\"22\" lineHeight=\"1.5\" margin=\"0.08\"/>\n"
    "</settings>\n";

/// Анимация листания: умолчание — включена, как читалка листала всегда;
/// снятая и включённая переживают запись и чтение, и чтение кладёт
/// прочитанное поверх того, что было в поле.
void testTurnAnimationRoundTrip() {
    std::printf("\n=== настройки: анимация листания ===\n");

    Settings defaultSettings;
    check(defaultSettings.animateTurns.get(), "умолчание — анимация включена");

    Settings savedSettings;
    savedSettings.animateTurns.set(false);
    const std::string savedXml = settingsXml(savedSettings);
    check(savedXml.find("<turning animated=\"false\"/>") != std::string::npos,
          "в файле — своим элементом и словом");

    Settings parsedSettings;
    check(readSettings(savedXml, parsedSettings) && !parsedSettings.animateTurns.get(),
          "снятая: запись → чтение — снята");

    savedSettings.animateTurns.set(true);
    check(readSettings(settingsXml(savedSettings), parsedSettings) && parsedSettings.animateTurns.get(),
          "включённая: запись → чтение — включена поверх снятой");
}

/// Файл, где о листании ни слова, читается включённым, а прочее в нём — как
/// было: новое поле не мешает читать старый файл.
void testTurnAnimationFromOlderFile() {
    std::printf("\n=== настройки: файл четвёртой версии ===\n");

    Settings parsedSettings;
    parsedSettings.animateTurns.set(false);
    check(readSettings(kOlderFile, parsedSettings), "файл четвёртой версии разобран");
    check(parsedSettings.animateTurns.get(), "без поля — анимация включена");
    check(parsedSettings.continueReading.get() && parsedSettings.theme == u"Night" &&
              parsedSettings.fontSize.get() == 22.0,
          "прочее прочитано, как было записано");
}

/// Значение, которого читалка не пишет, читается умолчанием, как и
/// пропущенное.
void testTurnAnimationUnknownValue() {
    std::printf("\n=== настройки: незнакомое значение ===\n");

    Settings parsedSettings;
    parsedSettings.animateTurns.set(false);
    check(readSettings("<settings version=\"5\"><turning animated=\"yes\"/></settings>", parsedSettings) &&
              parsedSettings.animateTurns.get(),
          "animated=\"yes\" — умолчание, включена");
}

}  // namespace

void runSettingsTests() {
    testTurnAnimationRoundTrip();
    testTurnAnimationFromOlderFile();
    testTurnAnimationUnknownValue();
}
