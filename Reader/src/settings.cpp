#include "settings.h"

#include <windows.h>
#include <shlobj.h>



// Свои заголовки со стандартными внутри — до импорта: он несёт с собой
// модульный std, а стандартный заголовок после него MSVC уже не принимает.
#include "store.h"

import wxl.core;
import wxl.fmt;
import wxl.xml;

namespace bukvitsa::reader {

std::filesystem::path dataDirectory() {
    PWSTR folder = nullptr;
    if (FAILED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &folder))) {
        return {};
    }
    std::filesystem::path path{folder};
    ::CoTaskMemFree(folder);
    return path / L"Bukvitsa" / L"Reader";
}

std::filesystem::path settingsPath() {
    const std::filesystem::path directory = dataDirectory();

    return directory.empty() ? std::filesystem::path{} : directory / L"settings.xml";
}

Settings parseSettings(std::string xml) {
    Settings settings;

    if (xml.empty()) return settings;   // первого запуска ещё не было

    try {
        wxl::xml::document document;
        const wxl::xml::node& root = document.load(std::move(xml));

        if (const wxl::xml::node* window = root.child("window")) {
            settings.windowPlacement = attributeOf(*window, "placement");
        }
        if (const wxl::xml::node* reading = root.child("reading")) {
            settings.continueReading = reading->attribute("continue") == "true";
        }
        // Файл читается как есть, какую бы версию он о себе ни объявил:
        // прежние написания не переводятся. Настройки вида — кегль,
        // интерлиньяж, поля, тема — это то, что читатель выставил за минуту
        // ползунками, и восстанавливать их из позапрошлого формата дороже,
        // чем выставить заново. Что не прочиталось, берётся умолчанием, и
        // первое же закрытие окна перепишет файл начисто.
        if (const wxl::xml::node* text = root.child("text")) {
            settings.theme = attributeOf(*text, "theme");
            settings.fontSize = static_cast<float>(realOf(*text, "fontSize", 20.0));
            settings.lineHeight = static_cast<float>(realOf(*text, "lineHeight", 1.45));
            settings.margin = static_cast<float>(realOf(*text, "margin", 0.075));
        }
        if (const wxl::xml::node* skin = root.child("skin")) {
            settings.skin = attributeOf(*skin, "name");
        }
        if (const wxl::xml::node* book = root.child("lastBook")) {
            settings.lastBookGuid = attributeOf(*book, "guid");
            settings.lastBookPath = attributeOf(*book, "path");
        }
    } catch (...) {
        // Битый файл, файл от будущей версии, файл, который правили руками, —
        // всё это повод открыться со значениями по умолчанию, а не повод не
        // открыться. Настройки не стоят отказа запускаться.
        return Settings{};
    }

    return settings;
}

std::string settingsXml(const Settings& settings) {
    // text_builder, а не поток с нейтральной локалью: локали у него нет вовсе,
    // и дробное число пишется точкой, какие бы настройки ни стояли в Windows.
    // Аллокатор -- STA-пул: формирователь зовётся из корутины между двумя
    // `co_await`, а этот код у читалки исполняется в интерфейсном потоке (см.
    // io.h), на рабочий поток уходят только байты.
    wxl::core::text_builder<wxl::core::sta_allocator> out;

    out.append("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n");
    out.format("<settings version=\"{}\">\n", Settings::kVersion);
    out.format("  <window placement=\"{}\"/>\n", xmlValue(settings.windowPlacement));
    out.format("  <reading continue=\"{}\"/>\n", settings.continueReading ? "true" : "false");
    out.format("  <text theme=\"{}\" fontSize=\"{}\" lineHeight=\"{}\" margin=\"{}\"/>\n",
               xmlValue(settings.theme), settings.fontSize, settings.lineHeight, settings.margin);

    if (!settings.skin.empty()) {
        out.format("  <skin name=\"{}\"/>\n", xmlValue(settings.skin));
    }

    if (!settings.lastBookGuid.empty()) {
        out.format("  <lastBook guid=\"{}\" path=\"{}\"/>\n", xmlValue(settings.lastBookGuid),
                   xmlValue(settings.lastBookPath));
    }

    out.append("</settings>\n");

    return std::string(out.view());
}

}  // namespace bukvitsa::reader
