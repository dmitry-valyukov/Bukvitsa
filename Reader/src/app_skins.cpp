// Приложение: обложки — добавить, править, сохранить, удалить, проверить
// снимок; предпросмотр мастера и снимок подложки полосы.

// Свои заголовки со стандартными внутри — до всего, что тянет import
// wxl.core: заголовок, включённый после импорта, MSVC уже не принимает.
#include "file_dialog.h"

#include "app.h"

// Импорт последним, после всех обычных заголовков.
import wxl.async;
import wxl.core;
import wxl.fmt;

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
// мастера, а мастеру достаётся путь уже проверенного. Предпросмотр мастер
// включает сам — открытием правки (preview).
//
// Каждая дорога начинает сеанс мастера (beginWizardSession) и ждёт снимка под
// его токеном: следующая дорога, выход из мастера и уход с полосы сеанс
// отменяют, и снимок, который ещё читается, мастера уже не откроет. Снимок,
// прочитанный раньше отмены, — ответ, как у вопроса об удалении (deleteSkin),
// поэтому после ожидания сеанс спрашивается ещё раз. Иначе другой снимок
// открыл бы правку под скрытым мастером — полоса осталась бы в предпросмотре,
// и скрытие его уже не сняло бы, — а новая обложка встала бы поверх полки или
// заставки, где с открытым мастером клавиши экранов молчат.

detached_task App::addSkin() {
    const cancellation_token session = beginWizardSession();

    const std::filesystem::path path = askForImage(window_.handle());
    if (path.empty()) co_return;

    if (!co_await checkImage(path, session)) co_return;
    if (session.is_canceled()) co_return;

    wizard_.openNew(path);
    panel_.close();
    wizard_.show();
    reportWarpRefusal();
}

/// По полному списку тем, а не по реестру: системные обложки живут только в
/// нём, а шестерёнка есть и у них — правка «на основе».
detached_task App::editSkin(u16_text name) {
    const cancellation_token session = beginWizardSession();

    const std::optional<int> index = themes_.indexOfSkin(name);
    const Skin* known = index ? themes_.skinAt(*index) : nullptr;
    if (!known) co_return;   // список успел перемениться под руками

    // Копия, а не указатель: пока снимок читают, список тем может
    // перемениться, и указатель в него протухнет.
    const Skin skin = *known;
    const std::filesystem::path image = ws_.skinImagePath(skin);

    if (!co_await checkImage(image, session)) co_return;
    if (session.is_canceled()) co_return;

    wizard_.openEdit(skin, image);
    panel_.close();
    wizard_.show();
    reportWarpRefusal();
}

detached_task App::chooseAnotherImage() {
    const cancellation_token session = beginWizardSession();

    const std::filesystem::path path = askForImage(window_.handle());

    // Отказался — остаёмся на прежнем снимке: читатель ничего не терял.
    if (path.empty()) co_return;

    if (!co_await checkImage(path, session)) co_return;
    if (session.is_canceled()) co_return;

    wizard_.openNew(path);
}

cancellation_token App::beginWizardSession() {
    wizardSession_.cancel();
    wizardSession_ = cancellation_source{stop_.token()};
    return wizardSession_.token();
}

// Предпросмотр снимает и подложку темы приносит слушатель (preview): скрытие
// мастера опустошает то, что тот показывает полосе.
void App::endWizardSession() {
    wizardSession_.cancel();
    wizard_.hide();
}

/// Читает файл и пробует раскодировать его как картинку. Нет — читателю
/// говорится, что именно не так: не картинка или картинка больше бюджета
/// памяти — с её размером из заголовка, чтобы было видно, во что её
/// уменьшить. Байты для показа полосе приносит потом loadBackdrop: второе
/// чтение того же файла дешевле, чем нести байты через мастер в полосу.
wxl::async::task<bool> App::checkImage(std::filesystem::path image, cancellation_token stop) {
    try {
        const PictureVerdict verdict = co_await ws_.probeImage(image, std::move(stop));
        switch (verdict.refusal) {
        case PictureRefusal::None:
            co_return true;
        case PictureRefusal::NotAnImage:
            notices_.post({L"Это не изображение", image.wstring()});
            co_return false;
        case PictureRefusal::TooLarge: {
            const u16_text headline = core::format(u"Снимок слишком велик: {}×{}", verdict.declaredSize.width,
                                                   verdict.declaredSize.height);
            notices_.post({std::wstring{headline.wchars()}, image.wstring()});
            co_return false;
        }
        }
        // Новая причина отказа без своих слов молча отказала бы в снимке.
        core::abort("checkImage: отказ раскодирования вне перечисления");
    } catch (const last_error& failure) {
        notices_.post(noticeOf(L"Не удалось открыть изображение", image.wstring(), failure));
    }
    co_return false;
}

detached_task App::leaveWizard() {
    endWizardSession();
    view_.root().focus(FocusState::Programmatic);
    co_return;
}

/* ---------------- сохранить и удалить ---------------- */

/// Копия снимка, запись реестра, немедленное применение — сохранённая обложка
/// тут же становится текущей темой. Отмена кончает сценарий, только пока
/// рабочее место не начало писать: тогда не записано ничего и применять
/// нечего; начатое сохранение доходит до конца и применяется.
///
/// Запись — под своим источником от общего, а не под сеансом мастера: выход
/// сразу после «Сохранить» кончает сеанс, но молча не записать то, что
/// читатель попросил записать, было бы маскировкой. От сеанса зависит только
/// конец: список тем меняется всегда — реестр уже другой, — а мастер
/// закрывается, только пока открыта та же правка и жив сеанс, в котором
/// сохранить попросили. Правка другая — пока писалось, другой снимок открыл
/// новую, — или сеанс кончился — читатель вышел, ушёл с полосы или пошёл в
/// мастер другой дорогой, — и закрывать мастер этому сценарию уже не по праву.
detached_task App::saveSkin(SkinSave save) {
    auto const stop = cancellation_source{stop_.token()};
    const cancellation_token askedSession = wizardSession_.token();

    const u16_text skinName = save.skin.name;

    // Конец мастеру говорится всегда — удачный, отказ и всякое другое
    // исключение, уходящее дальше (отмена кончает сценарий, сбой идёт в
    // обработчик сбоев сценариев): по номеру мастер сам решит, его ли это
    // сохранение, а конец снова разрешает сохранять.
    try {
        co_await ws_.saveSkin(std::move(save.skin), save.photo, stop.token());
    } catch (const last_error& failure) {
        static_cast<void>(wizard_.saveEnded(save.ticket));
        notices_.post(noticeOf(L"Не удалось сохранить обложку", save.photo.wstring(), failure));
        co_return;
    } catch (...) {
        static_cast<void>(wizard_.saveEnded(save.ticket));
        throw;
    }
    const bool sameEdit = wizard_.saveEnded(save.ticket);

    themes_.setSkins(ws_.skins.list());

    // Номер — по полному списку, а не по реестру: номера считаются вместе с
    // системными обложками, которые стоят впереди реестровых.
    //
    // Имя обложки в настройки и их запись — дело слушателя темы
    // (themeChanged): смена темы здесь ничем не отличается от смены клавишей T.
    applySkins(themes_.afterSave(skinName, presenter_.theme().get()));

    if (sameEdit && !askedSession.is_canceled()) leaveWizard();
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
    // Дорога в мастер, начатая раньше вопроса, уже не откроет, что бы читатель
    // ни ответил: открылась бы правка обложки, которую удаляют, и её
    // сохранение вернуло бы удалённую в реестр.
    wizardSession_.cancel();

    auto const stop = cancellation_source{stop_.token()};

    if (!ws_.skins.find(name)) co_return;   // реестр успел перемениться под руками

    // Удаление спрашивает: точки по снимку читатель расставлял руками, вернуть
    // их нечем, а корзина стоит вплотную к шестерёнке. Ответ по умолчанию —
    // «Отмена»: промах по соседней кнопке не должен ничего стоить. Вопрос
    // говорит ровно то, что произойдёт: снимок остаётся в `skins\`, и обещать
    // его пропажу значило бы соврать про собственное поведение.
    //
    // Диалог — над показанным экраном (XamlRoot) и в его теме (screenTheme_).
    // Открытое сообщение сюда не пустит — оно заслоняет остров; а сообщение,
    // пришедшее, пока вопрос открыт, ждёт в Notices и выходит, когда вопрос
    // закрыт.
    //
    // Ответ ждётся под токеном сценария: закрытие окна отменяет общий
    // источник, а с ним и этот, — вопрос закрывается сам (платформу просят
    // остановить показ: диалог уходит без Closing, которому было бы чем
    // отказать), и ожидание кончается отменой. Без токена выход ждал бы
    // ответа, который уже некому дать: окно скрыто, а процесс стоит.
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
        if (co_await dialog.showAsync(stop.token()) != ContentDialogResult::Primary) co_return;
    }

    // Ответ, успевший раньше отмены, — ответ, но удаление после отмены не
    // начинается, каким бы он ни был.
    if (stop.is_canceled()) co_return;

    if (!ws_.skins.find(name)) co_return;   // реестр мог перемениться, пока спрашивали

    // Реестр не записался — рабочее место вернуло обложку в реестр в памяти
    // на прежнее место, а список тем её и не терял: читатель остаётся на
    // своей теме и узнаёт, что удаления не было и почему.
    try {
        co_await ws_.deleteSkin(name, stop.token());
    } catch (const last_error& failure) {
        notices_.post(noticeOf(L"Не удалось удалить обложку", ws_.skinsPath().wstring(), failure));
        co_return;
    }

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

/// Полоса рисуется с правимыми кривыми на снимке мастера, а после мастера —
/// по теме. Он же — пересчёт после отпускания точки: мастер объявляет правку,
/// когда она устоялась.
void App::preview(std::optional<SkinPreview> const& wizardPreview) noexcept {
    presenter_.setPreview(wizardPreview ? &wizardPreview->skin : nullptr);
    showBackdrop();

    // Ответ о слое изгиба у полосы уже есть: setPreview перерисовал
    // предпросмотр сразу. Под показанным мастером — отпущенная точка, другой
    // снимок — говорим здесь же; открытие мастера зовёт сюда до show(), и
    // скажет открывший — после него. Скрытие — не о чем: вне мастера о
    // пределе не говорится.
    if (wizardPreview && wizard_.isOpen()) reportWarpRefusal();
}

void App::reportWarpRefusal() {
    // Слой изгиба больше предела битмапа устройства — страница плоская, какие
    // точки ни ставь, и читатель, тянущий их, должен знать почему. Один раз
    // за запуск, а не на каждой отпущенной точке: предел — свойство
    // устройства, и окно на каждую было бы шумом. Сообщение, а не подпись в
    // мастере: у мастера слов для читателя нет — только кнопки и имя, — а
    // сообщения уже говорят о том, что не удалось, и тоже по разу
    // (loadBackdrop).
    const std::optional<BookView::WarpRefusal>& refusal = view_.warpRefusal();
    if (!refusal || warpRefusalReported_) return;
    warpRefusalReported_ = true;
    const u16_text details =
        core::format(u"Изгиб рисуется в слое вдвое выше полосы — {}×{} пикселей, а битмап на этом устройстве "
                     u"не больше {} пикселей по стороне. Страница показана без изгиба, какие точки ни ставь; "
                     u"точки сохранятся вместе с обложкой, а в окне поменьше слой уложится в предел, и изгиб "
                     u"появится.",
                     refusal->layerWidth, refusal->layerHeight, refusal->deviceLimit);
    notices_.post({L"Изгиб страницы недоступен на этом устройстве", std::wstring{details.wchars()}});
}

/// Путь снимка знает мастер или рабочее место, байты читает loadBackdrop. Тот
/// же файл — ничего: снимок у полосы уже есть (или в дороге).
void App::showBackdrop() {
    std::filesystem::path wanted;
    if (const std::optional<SkinPreview>& wizardPreview = wizard_.shownPreview().get()) {
        wanted = wizardPreview->imagePath;
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
/// прочитался, не раскодировался или больше бюджета памяти — полоса остаётся
/// при бумаге темы, а читатель узнаёт почему — один раз на путь за запуск:
/// тему с этим снимком выбирают снова и снова (T, панель), и окно на каждый
/// раз было бы шумом, а молчание прятало бы сбой. Пока читали, могли захотеть
/// другой — тогда этот не нужен.
detached_task App::loadBackdrop(std::filesystem::path file) {
    auto const stop = cancellation_source{stop_.token()};

    try {
        std::string bytes = co_await ws_.readBytes(file, stop.token());
        if (file != backdrop_) co_return;

        const PictureRefusal refusal = view_.setBackdrop(std::move(bytes));
        if (refusal == PictureRefusal::None || !reportedBackdrops_.insert(file).second) co_return;
        notices_.post({refusal == PictureRefusal::TooLarge ? L"Снимок обложки слишком велик"
                                                           : L"Снимок обложки не раскодировался",
                       file.wstring()});
    } catch (const last_error& failure) {
        if (reportedBackdrops_.insert(file).second)
            notices_.post(noticeOf(L"Не удалось прочитать снимок обложки", file.wstring(), failure));
    }
}

}  // namespace bukvitsa::reader
