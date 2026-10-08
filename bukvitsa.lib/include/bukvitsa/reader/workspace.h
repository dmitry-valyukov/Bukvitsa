#pragma once

// Рабочее место читалки — каталог данных и всё, что в нём живёт: настройки,
// реестр книг, обложки читателя, состояния книг, кэш картинок.
//
// Одно рабочее место — один каталог. Reader даёт ему настоящий
// (`standardDataDirectory`), тест — временный; где лежат файлы, не знает
// больше никто: пути считаются здесь и только здесь.
//
// Диск трогают только операции wxl (`read_all`, `write_all`, `exists`,
// `list`) и только из корутин этого класса; разбор идёт на потоке вызова, в
// STA-пуле. Корутины — `task<T>`: вызывающий ждёт их `co_await` и получает
// значение, а сбой операции — исключением `system_exception`. Окон модель не
// знает: что сказать читателю, она отдаёт словами (`Notice`), показывает их
// Reader.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// Заголовки модели — последними: они несут импорт.
#include "book.h"
#include "library.h"
#include "settings.h"
#include "skins.h"
#include "store.h"

import wxl.async;

namespace bukvitsa::reader {

/// Слова для читателя: заголовок и подробности. Модель окон не показывает.
struct Notice {
    std::wstring headline;
    std::wstring details;
};

/// Чем кончился запуск: настройки, обложки и реестр уже в полях рабочего
/// места, а здесь — что при этом пошло не так, и книга, которую можно
/// продолжить читать (пусто — нечего).
struct Started {
    std::vector<Notice> notices;
    std::filesystem::path lastBook;
};

/// Книга, прочитанная и разобранная: прогрев или первый шаг открытия.
struct Warmed {
    std::shared_ptr<Book> book;
    uint64_t fileSize = 0;   ///< размер файла: его записывает реестр
};

/// Что вернула регистрация книги: запись реестра и была ли книга новой.
struct Registered {
    BookEntry entry;
    bool isNew = false;   ///< в реестре её не было — полке есть что добавить
};

/// Открытая книга со стороны модели: запись реестра и состояние — место
/// чтения и закладки (чистый лист, если книгу ещё не открывали).
struct Opened {
    BookEntry entry;
    BookState state;
};

/// Чем кончился обход каталога.
struct FolderAdded {
    bool added = false;                 ///< хоть одна книга зарегистрирована — реестр записан
    std::vector<std::wstring> unread;   ///< файлы, которые не прочитались: имя и причина
};

/// Причина сбоя операции словами системы, для сообщения читателю. Текст
/// приходит в кодировке потока (`FormatMessageA`), потому переводится через
/// CP_ACP.
std::wstring reasonOf(const wxl::async::system_exception& failure);

/// Каталог данных по умолчанию: `%LOCALAPPDATA%\Bukvitsa\Reader`. Корень общий
/// для семейства — рядом однажды встанет Writer; не Roaming: в реестре лежат
/// локальные пути и кэш обложек, и роуминг перенёс бы на другую машину битые
/// ссылки и лишние мегабайты. Переменная окружения `BUKVITSA_DATA` подменяет
/// каталог целиком: прогон драйвером (`tools\drive.ps1 -DataDir`) идёт на
/// подопытных данных, не трогая настоящих.
std::filesystem::path standardDataDirectory();

class Workspace {
public:
    explicit Workspace(std::filesystem::path directory);

    Workspace(const Workspace&) = delete;
    Workspace& operator=(const Workspace&) = delete;

    // ---- где что лежит ----

    const std::filesystem::path& directory() const noexcept { return directory_; }
    std::filesystem::path settingsPath() const;    ///< settings.xml
    std::filesystem::path libraryPath() const;     ///< library.xml
    std::filesystem::path skinsPath() const;       ///< skins.xml
    std::filesystem::path skinDirectory() const;   ///< skins\ — снимки обложек читателя
    std::filesystem::path coverDirectory() const;  ///< cache\ — обложки книг из реестра
    /// books\{guid}.xml. Имя файла — guid и ничего больше: он наш, выдан
    /// `CoCreateGuid`, и в нём не может оказаться ни разделителя пути, ни
    /// двоеточия. Названия книги здесь нет намеренно — оно чужое.
    std::filesystem::path statePath(u16_view guid) const;
    /// Снимок обложки: встроенная — из Assets рядом с исполняемым, читателя —
    /// из skins\.
    std::filesystem::path skinImagePath(const Skin& skin) const;

    // ---- модели ----

    Settings settings;   ///< настройки вида и запуска: наблюдаемые поля, к ним привязаны контролы
    Library library;     ///< реестр книг
    Skins skins;         ///< обложки читателя

    // ---- запуск ----

    /// Читает настройки, обложки и реестр. Нет файла — первый запуск, молча;
    /// файл не читается или не разбирается — слово читателю в `notices`;
    /// испорченный остаётся рядом копией `.bad`, а первая же запись заменит
    /// его. Заодно ищет книгу, которую можно продолжить (`lastBookPath`).
    wxl::async::task<Started> start();

    // ---- книги ----

    /// Читает и разбирает книгу: байты — операцией, разбор — здесь. Файл не
    /// прочитался — `system_exception`; не разобрался — исключение разбора.
    wxl::async::task<Warmed> readBook(std::filesystem::path path);

    /// Регистрирует книгу: запись в реестре и обложка в кэше. Сам реестр не
    /// пишет — это решает вызывающий: открытие пишет сразу, обход каталога
    /// один раз после цикла. Новизна считается до ожидания: пока пишется
    /// обложка, соседний сценарий может дополнить реестр. Документ — ссылкой,
    /// а не копией: его держит кадр ждущего, а тот стоит на `co_await` этой
    /// корутины до её конца.
    wxl::async::task<Registered> registerBook(const fb3::Document& document, std::filesystem::path path,
                                              uint64_t fileSize);

    /// Открывает книгу со стороны модели: регистрация, реестр, последняя книга
    /// в настройках, состояние. Место чтения предыдущей книги вызывающий пишет
    /// до этого (`saveState`): после здесь настройки укажут на другую.
    wxl::async::task<Opened> openBook(const Book& book, uint64_t fileSize);

    /// Обходит каталог: каждую книгу `*.fb3` читает, разбирает и регистрирует;
    /// `onNew` зовётся на каждую новую сразу — полка растёт по ходу; реестр
    /// пишется один раз в конце, если было что регистрировать. Нечитаемый файл
    /// попадает в `unread`, не-книга молча пропускается: обход идёт дальше.
    wxl::async::task<FolderAdded> addFolder(std::filesystem::path folder,
                                            std::function<void(const BookEntry&)> onNew);

    /// Книга, которую можно продолжить читать: путь из настроек, если файл на
    /// месте, иначе по guid из реестра. Пусто — нечего.
    wxl::async::task<std::filesystem::path> lastBookPath();

    /// Состояние книги; нет файла — чистый лист, это не ошибка.
    wxl::async::task<BookState> readState(u16_view guid);
    wxl::async::task<> saveState(u16_text guid, BookState state);

    /// Текст настроек и реестра собирается в момент вызова, до ожидания: на
    /// диск уходит решённое, а не то, что читатель поменяет за время записи.
    wxl::async::task<> saveSettings();
    wxl::async::task<> saveLibrary();

    // ---- обложки читателя ----

    /// Сохраняет обложку: у новой — копия снимка в skins\ под новым guid с
    /// родным расширением (имена обложек выбирает читатель, они повторяются;
    /// guid — нет), у правки копия уже лежит; затем запись реестра.
    wxl::async::task<> saveSkin(Skin skin, std::filesystem::path photo);

    /// Убирает обложку из реестра и пишет его; не было — ничего. Снимок
    /// остаётся в skins\: копия не принадлежит одной обложке — мастер, открыв
    /// обложку и сохранив её под другим именем, заводит вторую с тем же файлом,
    /// а лишний файл на диске дешевле сломанной обложки.
    wxl::async::task<> deleteSkin(u16_view name);

    /// Читается ли файл как картинка: байты операцией, проба декодером.
    wxl::async::task<bool> isImage(std::filesystem::path image);

    /// Байты файла целиком — для снимка подложки, который раскодирует полоса
    /// у себя: второе чтение того же файла дешевле, чем нести байты через
    /// мастер в полосу.
    wxl::async::task<std::string> readBytes(std::filesystem::path file);

    // ---- общее ----

    /// Испорченный файл — копией рядом, под тем же именем с «.bad»: байты те,
    /// что прочли и не разобрали; копия одна, прежняя перезаписывается —
    /// истории порч хранить незачем.
    wxl::async::task<> keepBrokenCopy(std::filesystem::path file, std::string bytes);

private:
    std::filesystem::path directory_;
};

}  // namespace bukvitsa::reader
