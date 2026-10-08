#include <shlobj.h>

// Свои заголовки — после системных: settings.h и store.h несут импорт, а
// заголовок после импорта MSVC принимает не всякий.
#include "bukvitsa/reader/settings.h"
#include "bukvitsa/reader/store.h"

import wxl.core;
import wxl.fmt;
import wxl.xml;

namespace bukvitsa::reader {

std::filesystem::path dataDirectory() {
    // Подопытный каталог: сценарии прогона портят settings.xml и реестры, и
    // гонять их на настоящих данных читателя нельзя. Переменная подменяет
    // каталог целиком. Длина не ограничена MAX_PATH: путь к рабочей папке
    // сессии длиннее обычного, потому буфер спрашивается у самой Windows.
    if (const DWORD size = ::GetEnvironmentVariableW(L"BUKVITSA_DATA", nullptr, 0); size > 1) {
        std::wstring sandbox(size - 1, L'\0');
        if (::GetEnvironmentVariableW(L"BUKVITSA_DATA", sandbox.data(), size) == size - 1)
            return std::filesystem::path{std::move(sandbox)};
    }

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

bool readSettings(std::string xml, Settings& into) {
    if (xml.empty()) return true;   // первого запуска ещё не было

    // Сперва всё читается в местные, и лишь потом ложится в поля: битый файл
    // не должен оставить настройки прочитанными наполовину.
    u16_text placement;
    bool continueReading = false;
    u16_text theme;
    double fontSize = kFontSizeDefault;
    double lineHeight = 145.0;
    double margin = 7.5;
    u16_text skin;
    u16_text lastBookGuid;
    std::filesystem::path lastBookPath;

    try {
        wxl::xml::document document;
        const wxl::xml::node& root = document.load(std::move(xml));

        if (const wxl::xml::node* window = root.child("window")) {
            placement = attributeOf(*window, "placement");
        }
        if (const wxl::xml::node* reading = root.child("reading")) {
            continueReading = reading->attribute("continue") == "true";
        }
        // Файл читается как есть, какую бы версию он о себе ни объявил:
        // прежние написания не переводятся. Настройки вида — кегль,
        // интерлиньяж, поля, тема — это то, что читатель выставил за минуту
        // ползунками, и восстанавливать их из позапрошлого формата дороже,
        // чем выставить заново. Что не прочиталось, берётся умолчанием, и
        // первое же закрытие окна перепишет файл начисто.
        //
        // Интерлиньяж и поля в файле — множитель и доля, в модели — проценты
        // (см. Settings).
        if (const wxl::xml::node* typeset = root.child("text")) {
            theme = attributeOf(*typeset, "theme");
            fontSize = realOf(*typeset, "fontSize", kFontSizeDefault);
            lineHeight = realOf(*typeset, "lineHeight", 1.45) * 100.0;
            margin = realOf(*typeset, "margin", 0.075) * 100.0;
        }
        if (const wxl::xml::node* look = root.child("skin")) {
            skin = attributeOf(*look, "name");
        }
        if (const wxl::xml::node* book = root.child("lastBook")) {
            lastBookGuid = attributeOf(*book, "guid");
            lastBookPath = std::filesystem::path(attributeOf(*book, "path").wchars());
        }
    } catch (...) {
        // Битый файл, файл от будущей версии, файл, который правили руками, —
        // всё это повод открыться со значениями по умолчанию, а не повод не
        // открыться. Настройки не стоят отказа запускаться; сказать об этом
        // читателю — дело вызывающего.
        return false;
    }

    into.windowPlacement = std::move(placement);
    into.continueReading.set(continueReading);
    into.theme = std::move(theme);
    into.fontSize.set(std::clamp(fontSize, kFontSizeMin, kFontSizeMax));
    into.lineHeight.set(std::clamp(lineHeight, kLineHeightMin, kLineHeightMax));
    into.margin.set(std::clamp(margin, kMarginMin, kMarginMax));
    into.skin = std::move(skin);
    into.lastBookGuid = std::move(lastBookGuid);
    into.lastBookPath = std::move(lastBookPath);
    return true;
}

std::string settingsXml(const Settings& settings) {
    // text_builder, а не поток с нейтральной локалью: локали у него нет вовсе,
    // и дробное число пишется точкой, какие бы настройки ни стояли в Windows.
    // Аллокатор -- STA-пул: формирователь зовётся из корутины между двумя
    // `co_await`, а этот код у читалки исполняется в интерфейсном потоке (см.
    // main.cpp), на рабочий поток уходят только байты.
    text_builder<sta_allocator> out;

    out.append("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n");
    out.format("<settings version=\"{}\">\n", Settings::kVersion);
    out.format("  <window placement=\"{}\"/>\n", xmlValue(settings.windowPlacement));
    out.format("  <reading continue=\"{}\"/>\n", settings.continueReading.get() ? "true" : "false");
    // Интерлиньяж и поля — множителем и долей, как их писали прежние версии; в
    // модели они в процентах (см. Settings).
    out.format("  <text theme=\"{}\" fontSize=\"{}\" lineHeight=\"{}\" margin=\"{}\"/>\n",
               xmlValue(settings.theme), settings.fontSize.get(), settings.lineHeight.get() / 100.0,
               settings.margin.get() / 100.0);

    if (!settings.skin.empty()) {
        out.format("  <skin name=\"{}\"/>\n", xmlValue(settings.skin));
    }

    if (!settings.lastBookGuid.empty()) {
        out.format("  <lastBook guid=\"{}\" path=\"{}\"/>\n", xmlValue(settings.lastBookGuid),
                   xmlValue(settings.lastBookPath.native()));
    }

    out.append("</settings>\n");

    return std::string(out.view());
}

}  // namespace bukvitsa::reader
