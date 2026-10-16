// Приложение: обложки — добавить, править, сохранить, удалить, проверить
// снимок; предпросмотр мастера и снимок подложки полосы.

// Свои заголовки со стандартными внутри — до всего, что тянет import
// wxl.core: заголовок, включённый после импорта, MSVC уже не принимает.
#include "file_dialog.h"

#include "app.h"

// Импорт последним, после всех обычных заголовков.
import wxl.async;
import wxl.core;

namespace bukvitsa::reader {

using namespace wxl;

using wxl::async::cancellation_source;
using wxl::async::cancellation_token;
using wxl::async::detached_task;
using wxl::async::last_error;

/* ---------------- три дороги в мастер ---------------- */
//
// Новая обложка, правка существующей и другой снимок под открытым мастером —
// у каждой своя корутина над общим шагом checkImage: снимок проверяется до
// мастера, а мастеру достаётся путь уже проверенного. Шаг ждёт под токеном
// своего сценария. Предпросмотр мастер включает сам — открытием правки
// (preview).

detached_task App::addSkin() {
    auto const stop = cancellation_source{stop_.token()};

    const std::filesystem::path path = askForImage(window_.handle());
    if (path.empty()) co_return;

    if (!co_await checkImage(path, stop.token())) co_return;

    wizard_.openNew(path);
    panel_.close();
    wizard_.show();
}

/// По полному списку тем, а не по реестру: системные обложки живут только в
/// нём, а шестерёнка есть и у них — правка «на основе».
detached_task App::editSkin(u16_text name) {
    auto const stop = cancellation_source{stop_.token()};

    const std::optional<int> index = themes_.indexOfSkin(name);
    const Skin* known = index ? themes_.skinAt(*index) : nullptr;
    if (!known) co_return;   // список успел перемениться под руками

    // Копия, а не указатель: пока снимок читают, список тем может
    // перемениться, и указатель в него протухнет.
    const Skin skin = *known;
    const std::filesystem::path image = ws_.skinImagePath(skin);

    if (!co_await checkImage(image, stop.token())) co_return;

    wizard_.openEdit(skin, image);
    panel_.close();
    wizard_.show();
}

detached_task App::chooseAnotherImage() {
    auto const stop = cancellation_source{stop_.token()};

    const std::filesystem::path path = askForImage(window_.handle());

    // Отказался — остаёмся на прежнем снимке: читатель ничего не терял.
    if (path.empty()) co_return;

    if (!co_await checkImage(path, stop.token())) co_return;

    wizard_.openNew(path);
}

/// Читает файл и пробует раскодировать его как картинку. Нет — читателю
/// говорится, что именно не так. Байты для показа полосе приносит потом
/// loadBackdrop: второе чтение того же файла дешевле, чем нести байты через
/// мастер в полосу.
wxl::async::task<bool> App::checkImage(std::filesystem::path image, cancellation_token stop) {
    try {
        if (co_await ws_.isImage(image, std::move(stop))) co_return true;
        notices_.post({L"Это не изображение", image.wstring()});
    } catch (const last_error& failure) {
        notices_.post(noticeOf(L"Не удалось открыть изображение", image.wstring(), failure));
    }
    co_return false;
}

detached_task App::leaveWizard() {
    wizard_.hide();
    presenter_.setPreview(nullptr);
    showBackdrop();
    view_.root().focus(FocusState::Programmatic);
    co_return;
}

/* ---------------- сохранить и удалить ---------------- */

/// Копия снимка, запись реестра, немедленное применение — сохранённая обложка
/// тут же становится текущей темой. Отмена кончает сценарий, только пока
/// рабочее место не начало писать: тогда не записано ничего и применять
/// нечего; начатое сохранение доходит до конца и применяется.
detached_task App::saveSkin(Skin skin, std::filesystem::path photo) {
    auto const stop = cancellation_source{stop_.token()};

    const u16_text skinName = skin.name;

    try {
        co_await ws_.saveSkin(std::move(skin), photo, stop.token());
    } catch (const last_error& failure) {
        notices_.post(noticeOf(L"Не удалось сохранить обложку", photo.wstring(), failure));
        co_return;
    }

    themes_.setSkins(ws_.skins.list());

    // Номер — по полному списку, а не по реестру: номера считаются вместе с
    // системными обложками, которые стоят впереди реестровых.
    //
    // Имя обложки в настройки и их запись — дело слушателя темы
    // (themeChanged): смена темы здесь ничем не отличается от смены клавишей T.
    applySkins(themes_.afterSave(skinName, presenter_.theme().get()));

    leaveWizard();
}

/// Убрать обложку из реестра и из полосы тем.
///
/// **Снимок при этом остаётся лежать в `skins\`.** Копия картинки не
/// принадлежит той обложке, которая её привела: мастер, открыв обложку и
/// сохранив её под другим именем, заводит вторую с тем же именем файла, — и
/// удаление одной унесло бы снимок из-под другой. Ссылок на файл никто не
/// считает, а цена ошибки несимметрична: лишний файл на диске — мусор,
/// который никого не касается, отсутствующий — сломанная обложка.
///
/// Тема после удаления ищется по имени, а не по номеру: номера всех обложек
/// за удаляемой сдвигаются, и «остаться на своей» значит найти её заново.
/// Удалили ту, что была на экране, — читатель возвращается на встроенную
/// тему, номер которой читалка держит в настройках ровно на этот случай.
detached_task App::deleteSkin(u16_text name) {
    auto const stop = cancellation_source{stop_.token()};

    if (!ws_.skins.find(name)) co_return;   // реестр успел перемениться под руками

    // Удаление спрашивает: точки по снимку читатель расставлял руками, вернуть
    // их нечем, а корзина стоит вплотную к шестерёнке. Ответ по умолчанию —
    // «Отмена»: промах по соседней кнопке не должен ничего стоить. Вопрос
    // говорит ровно то, что произойдёт: снимок остаётся в `skins\`, и обещать
    // его пропажу значило бы соврать про собственное поведение.
    //
    // Диалог — над показанным экраном (XamlRoot) и в его теме (screenTheme_);
    // ответ ждётся здесь же, окно всё это время живо. Открытое сообщение сюда
    // не пустит — оно заслоняет остров; а сообщение, пришедшее, пока вопрос
    // открыт, ждёт в Notices и выходит, когда вопрос закрыт.
    //
    // Отмену сценарий проверяет сам, получив ответ: попросили кончиться, пока
    // вопрос открыт, — удаление после него не начнётся, каким бы ни был ответ.
    // Сам вопрос отмена не закрывает.
    {
        using namespace wxl::dsl;

        UIElement const host = window_.content();
        auto dialog = ContentDialog {
            xamlRoot = host.xamlRoot(),
            requestedTheme = screenTheme_.get(),
            title = L"Удалить обложку «" + std::wstring(name.wchars()) + L"»?",
            content = TextBlock{u"Расставленные по снимку точки пропадут; сам снимок останется.",
                                textWrapping.wrap},
            primaryButtonText = u"Удалить",
            closeButtonText = u"Отмена",
            defaultButton = ContentDialogButton::Close,
            onClosed = method(&notices_, &Notices::flush),
        };
        if (co_await dialog.showAsync() != ContentDialogResult::Primary) co_return;
    }

    if (stop.is_canceled()) co_return;

    if (!ws_.skins.find(name)) co_return;   // реестр мог перемениться, пока спрашивали

    co_await ws_.deleteSkin(name, stop.token());

    // Куда встать — спрашиваем по факту, после удаления: пока писался реестр,
    // читатель мог сменить тему (T), и номер, взятый до ожидания, вернул бы его
    // на прежнюю. Но до смены списка тем: номер текущей темы смотрит в нынешний
    // список, а по укоротившемуся он уже ничего не значит. Сменил список за
    // это время другой сценарий (сохранение обложки) — номер и список оба
    // нынешние, и ответ считается по ним же.
    const int theme = themes_.afterRemoval(name, presenter_.theme().get(), ws_.settings.theme);

    // Настройки — имя обложки или ключ темы — поправит и запишет слушатель
    // темы (themeChanged).
    themes_.setSkins(ws_.skins.list());
    applySkins(theme);
}

/* ---------------- список обложек, предпросмотр, подложка ---------------- */

void App::applySkins(int theme) {
    // Строки тем в панели идут за списком сами, подпиской. Полосе — явно, а не
    // подпиской на те же строки: строка — это имя и кнопки, пересохранённая
    // под тем же именем обложка строк не меняет, а снимок и кривые у неё
    // новые. И номер полоса берёт тем же вызовом: куда встать, она не знает,
    // а поставь она номер сама — хотя бы первую тему, когда прежний выпал за
    // укоротившийся список, — слушатель темы записал бы её ключ в настройки
    // поверх запасного, к которому читатель вернётся, удалив текущую обложку.
    //
    // Снимок — заново: пересохранённая обложка могла сменить его под тем же
    // путём. Забыт он раньше, чем встанет номер: слушатель темы, если номер
    // другой, принесёт нужный сам, и showBackdrop ниже второй раз его не
    // читает.
    backdrop_.clear();
    presenter_.skinsChanged(theme);
    showBackdrop();
}

/// Полоса рисуется с правимыми кривыми. Он же — пересчёт после отпускания
/// точки: мастер объявляет правку, когда она устоялась.
void App::preview(Skin const& skin) noexcept {
    presenter_.setPreview(&skin);
    showBackdrop();
}

/// Путь снимка знает рабочее место, байты читает loadBackdrop. Тот же файл —
/// ничего: снимок у полосы уже есть (или в дороге).
void App::showBackdrop() {
    std::filesystem::path wanted;
    if (presenter_.previewing()) {
        wanted = wizard_.imagePath();
    } else if (const Skin* skin = presenter_.activeSkin()) {
        wanted = ws_.skinImagePath(*skin);
    }

    if (wanted == backdrop_) return;
    backdrop_ = wanted;

    // До прихода снимка страница рисуется бумагой темы: прежний снимок ей не
    // годится.
    view_.setBackdrop({});
    if (!wanted.empty()) loadBackdrop(std::move(wanted));
}

/// Байты файла читает операция wxl, раскодирует их полоса у себя. Не
/// прочитался или не раскодировался — полоса остаётся при бумаге темы, а
/// читатель узнаёт почему — один раз на путь за запуск: тему с этим снимком
/// выбирают снова и снова (T, панель), и окно на каждый раз было бы шумом, а
/// молчание прятало бы сбой. Пока читали, могли захотеть другой — тогда этот
/// не нужен.
detached_task App::loadBackdrop(std::filesystem::path file) {
    auto const stop = cancellation_source{stop_.token()};

    try {
        std::string bytes = co_await ws_.readBytes(file, stop.token());
        if (file == backdrop_ && !view_.setBackdrop(std::move(bytes)) && reportedBackdrops_.insert(file).second)
            notices_.post({L"Снимок обложки не раскодировался", file.wstring()});
    } catch (const last_error& failure) {
        if (reportedBackdrops_.insert(file).second)
            notices_.post(noticeOf(L"Не удалось прочитать снимок обложки", file.wstring(), failure));
    }
}

}  // namespace bukvitsa::reader
