// Приложение: книги — открыть, продолжить, прогреть, добавить книгу или
// каталог, полка с прогрессом.

// Свои заголовки со стандартными внутри — до всего, что тянет import
// wxl.core: заголовок, включённый после импорта, MSVC уже не принимает.
#include "file_dialog.h"

#include "app.h"

// Импорт последним, после всех обычных заголовков.
import wxl.async;
import wxl.core;

namespace bukvitsa::reader {

using namespace wxl;

using wxl::async::detached_task;
using wxl::async::system_exception;

/// Путь в настройках — копия того, что в реестре, и она там ради быстрого
/// пути. Протух — спрашиваем реестр по guid; нет и там — читателю нечего
/// продолжать, и он хотел открыть книгу.
detached_task App::continueReading() {
    std::filesystem::path path = ws_.settings.lastBookPath;

    // Прогретую книгу открываем не спрашивая диск вовсе: она уже в памяти
    // целиком, и файл ей больше не нужен — даже если его успели унести. Тем
    // самым у нажатия не остаётся ни одного ожидания: книга встаёт на экран в
    // том же обороте очереди.
    if (!path.empty() && warm_.holds(path)) {
        open(std::move(path));
        co_return;
    }

    path = co_await ws_.lastBookPath();

    if (path.empty()) {
        chooseBook();
        co_return;
    }

    open(std::move(path));
}

/// Системный диалог модален, и пока он открыт, читалке всё равно делать
/// нечего; отказался читатель — ничего и не было.
detached_task App::chooseBook() {
    std::filesystem::path path = askForBook(window_.handle());
    if (!path.empty()) open(std::move(path));
    co_return;
}

detached_task App::openBook(u16_text guid) {
    if (const BookEntry* known = ws_.library.find(guid)) open(known->path);
    co_return;
}

/// Обходит каталог и добавляет из него книги — по одной, на глазах у читателя.
///
/// Каталог перечисляется на рабочем потоке; каждая книга читается там же;
/// разбирается она здесь, между двумя `co_await`, и сразу встаёт на полку. Ни
/// один шаг не ждёт остальных: первая книга появляется на полке, пока десятая
/// ещё не прочитана, — а окно всё это время отвечает, потому что
/// интерфейсный поток каждый раз возвращается в свой цикл сообщений.
///
/// Разбирается при этом документ, а не `Book`: реестру нужны метаданные и
/// обложка, а движок вёрстки с пагинатором книге, которую никто не открывал,
/// ни к чему (`Workspace::addFolder`).
detached_task App::chooseFolder() {
    const std::filesystem::path folder = askForFolder(window_.handle());
    if (folder.empty()) co_return;

    // Полка — прежде обхода: читатель, добавивший каталог, должен видеть, как
    // тот наполняется, а не пустой стартовый экран, за которым что-то
    // происходит.
    show(Screen::Library);

    FolderAdded result;

    try {
        // Карточка — на каждую новую книгу сразу, пока обход идёт: одной
        // карточкой, а не пересборкой всей полки — та стоила бы квадрата от
        // числа книг и стирала бы прогресс, который уже проступил на соседях.
        // Колбэк живёт в кадре задачи обхода, а этот кадр держит приложение,
        // так что голый `this` в нём жив.
        result = co_await ws_.addFolder(folder, [this](const BookEntry& entry) {
            if (shown_.get() == Screen::Library) shelf_.appendBook(entry);
        });
    } catch (const system_exception& failure) {
        notices_.post(noticeOf(L"Не удалось добавить каталог", folder.wstring(), failure));
        co_return;
    }

    if (!result.unread.empty()) {
        std::wstring text;
        for (const std::wstring& line : result.unread) {
            if (!text.empty()) text += L"\n";
            text += line;
        }
        notices_.post({L"Не удалось прочитать", std::move(text)});
    }
}

/// Читает файл и разбирает его, пока читатель смотрит на заставку и решает,
/// чего он хочет. Нажмёт — книга уже в памяти, и открытие обходится без диска
/// и без разбора, то есть без самой долгой своей части; не нажмёт — потеряна
/// одна фоновая загрузка, которая шла в те секунды, когда приложению всё
/// равно нечего делать.
///
/// Вёрстки здесь нет и быть не может: она зависит от размера окна и настроек
/// вида. Греется ровно то, что от вида не зависит: байты файла и разобранный
/// документ с блоками. Разбор идёт в интерфейсном потоке, как и при открытии:
/// память разбора берётся из STA-пула, а он чужого потока не терпит.
detached_task App::warmBook(std::filesystem::path path) {
    // Чью книгу ждём, записывается здесь и сразу — до того, как чтение уйдёт к
    // диску. По этой записи прогрев потом и узнаёт, нужен ли ещё его результат.
    warm_.expect(path);

    Warmed warmed;

    try {
        warmed = co_await ws_.readBook(path);
    } catch (std::exception const&) {
        // Молча: читатель ни о чём не просил, и жаловаться ему пока не на что.
        // Файл не прочитался или книга испорчена — он узнает об этом, когда
        // нажмёт, из открытия, которое прочитает её само и скажет поимённо.
        warm_.cancel();
        co_return;
    }

    // Пока шло чтение, читатель мог открыть эту книгу и сам — тогда слот её не
    // примет: вторая копия в памяти никому не нужна.
    warm_.fill(path, std::move(warmed.book), warmed.fileSize);
}

/// Порядок здесь — это порядок обязательств. Сначала книга разбирается (и
/// только если разобралась, старая уступает ей место), потом на диск уходит
/// место чтения предыдущей, и лишь затем реестр, обложка, настройки и
/// состояние новой. Каждый `co_await` — это выход в цикл сообщений: окно всё
/// это время живо, отвечает и перерисовывается.
///
/// Две короткие дороги в начале: книга уже открыта (читатель вернулся к ней) —
/// показать; книга прогрета (warmBook) — взять её из памяти и не трогать диск.
detached_task App::open(std::filesystem::path path) {
    // Эта книга уже открыта — читатель просто вернулся к ней со стартового
    // экрана или с полки. Ни читать, ни разбирать заново нечего: полоса держит
    // её со всей вёрсткой и местом чтения, а реестр и настройки давно на неё
    // указывают. Остаётся показать.
    if (view_.isOpen() && view_.book()->path() == path) {
        show(Screen::Book);
        co_return;
    }

    // Прогретая книга — та, что прочиталась и разобралась, пока читатель
    // смотрел на заставку. Забираем её из слота целиком: слот держит одну
    // книгу, и держать в нём ту, что сейчас откроется, незачем.
    std::optional<Warmed> warmed = warm_.take(path);

    std::shared_ptr<Book> book = warmed ? std::move(warmed->book) : nullptr;
    uint64_t fileSize = warmed ? warmed->fileSize : 0;

    // Прогрев для этой же книги мог ещё идти — пусть, вернувшись, выбросит
    // своё: книга открывается и без него, а вторая её копия в памяти не нужна.
    // Прогретая для другой книги — тоже прочь: открывается эта.
    warm_.cancel();

    if (!book) {
        Warmed read;

        try {
            read = co_await ws_.readBook(path);
        } catch (const system_exception& failure) {
            notices_.post(noticeOf(L"Не удалось прочитать файл книги", path.wstring(), failure));
            co_return;
        } catch (std::exception const& failure) {
            // Разговор с читателем, а не запись в лог: он только что выбрал этот
            // файл и вправе узнать, что с ним не так.
            u16_text const reason = unicode::assume_valid(failure.what()).to_utf16();
            notices_.post({L"Не удалось открыть книгу",
                           path.wstring() + L"\n\n" + std::wstring(reason.wchars())});
            co_return;
        }

        book = std::move(read.book);
        fileSize = read.fileSize;
    }

    // Место чтения предыдущей книги — на диск сразу: сейчас настройки укажут
    // на другую, и записывать станет некуда. Место — у полосы, закладки — у
    // мест книги.
    if (!ws_.settings.lastBookGuid.empty() && view_.isOpen()) {
        co_await ws_.saveState(ws_.settings.lastBookGuid, places_.stateAt(view_.position().get()));
    }

    // Реестр, обложка, настройки, состояние — у рабочего места; здесь только
    // то, что видно: книга на полосе, её оглавление и закладки в панели
    // (панель идёт за списками сама).
    Opened opened = co_await ws_.openBook(*book, fileSize);

    // Оглавление и закладки — новой книги, пока прежняя ещё на полосе: строки
    // прежнего оглавления смотрят в её блоки, и панель должна отпустить их
    // раньше, чем книга уйдёт. Находки прежнего поиска указывают в прежнюю
    // книгу.
    places_.open(book->blocks(), std::move(opened.state.bookmarks));
    search_.clear();

    view_.open(std::move(book), opened.state.charOffset);

    show(Screen::Book);
}

/// Поставить или снять и где ей лежать — решают места книги
/// (`BookPlaces::toggleBookmark`); панель идёт за списком закладок сама, а на
/// диск закладка уходит сразу — вместе с местом чтения, паузы не ждёт.
detached_task App::toggleBookmark() {
    if (!view_.isOpen()) co_return;

    const uint32_t here = view_.position().get();
    places_.toggleBookmark(here, hintAt(view_.blocks(), here));

    autosave_.saveState();
}

/// Достраивает полку: у каждой книги свой файл состояния, и читаются они по
/// одному, уже после того, как полка показана.
///
/// Это и есть «библиотека наполняется по мере чтения»: карточки встают сразу,
/// а «прочитано 42%» проступает на каждой, как только её файл прочитан. Полка
/// с сотней книг не ждёт сотни обращений к диску, чтобы показать первую.
/// Строка — поле карточки (`ShelfCard::showState`): полка не пересобирается.
detached_task App::fillProgress() {
    // Карточки — копией указателей: пока файлы читаются, обход каталога может
    // дополнить полку, а карточку, которую читают, держит сама копия.
    std::vector<intrusive_ptr<ShelfCard>> const cards(ws_.library.cards().begin(), ws_.library.cards().end());

    for (const intrusive_ptr<ShelfCard>& card : cards) {
        if (card->entry.characterCount == 0) continue;   // не открывалась — и читать нечего

        const BookState state = co_await ws_.readState(card->entry.guid);
        card->showState(state);
        shelf_.setProgress(*card);
    }
}

}  // namespace bukvitsa::reader
