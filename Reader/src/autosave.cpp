#include "autosave.h"

// Импорт — последним: записи — корутины `detached_task`.
import wxl.async;
import wxl.core;

namespace bukvitsa::reader {

using namespace wxl;
using namespace std::chrono_literals;

using wxl::async::detached_task;

namespace {

// Пауза, после которой перемещение окна и настройки вида попадают на диск.
// Окно таскают и ползунок двигают непрерывно, а писать на каждый пиксель
// незачем.
constexpr auto kSaveQuiet = 800ms;

// То же для места чтения: страницы листают подряд, а файл на книгу один.
constexpr auto kPositionQuiet = 1500ms;

// Записи — корутины без владельца: текст рабочее место собирает в момент
// вызова, до ожидания, а кадр живёт, пока файл пишется. Первым параметром —
// якорь жизни владельца рабочего места: промис `detached_task` держит его до
// конца кадра, и рабочее место не уходит из-под ждущей записи. В теле он не
// нужен, потому без имени.

/// Пишет настройки.
detached_task writeSettings(refcounted const& /*owner*/, Workspace& workspace) {
    co_await workspace.saveSettings();
}

/// Пишет состояние книги: место чтения и закладки.
detached_task writeState(refcounted const& /*owner*/, Workspace& workspace, u16_text guid,
                         BookState state) {
    co_await workspace.saveState(std::move(guid), std::move(state));
}

/// Таймер паузы: не повторяется — каждая перемена заводит его заново.
DispatcherQueueTimer quietTimer(const CompositionWindow& window, std::chrono::milliseconds pause) {
    DispatcherQueueTimer timer = window.dispatcherQueue().createTimer();
    timer.interval(pause);
    timer.isRepeating(false);
    return timer;
}

}  // namespace

// Таймеры — очереди интерфейсного потока, а не сон: поток, на котором стоит
// окно, обязан оставаться свободным. Тики — методы: таймеры свои и умирают
// вместе с Autosave.
Autosave::Autosave(refcounted& owner, const CompositionWindow& window, Workspace& workspace,
                   const ThemeList& themes, BookView& view, observable<BookState const>& state)
    : owner_(owner),
      window_(window),
      ws_(workspace),
      themes_(themes),
      view_(view),
      state_(state),
      windowTimer_(quietTimer(window, kSaveQuiet)),
      positionTimer_(quietTimer(window, kPositionQuiet)),
      settingsTimer_(quietTimer(window, kSaveQuiet)),
      // Геометрия сменилась — двигали, растягивали, максимизировали или
      // восстановили: всё, что запоминается. Окно сводит это в одно событие.
      geometryWatch_(window.add_onGeometryChanged(method(this, &Autosave::windowMoved))),
      // Место чтения — отложенно, как и место окна: перелистывание — самое
      // частое действие в читалке, а файл состояния книги переписывается
      // целиком.
      positionWatch_(view.position().on_change(method(this, &Autosave::positionChanged))),
      // Тема — с самого начала: её слушатель пишет, только если имена в
      // настройках изменились, а до чтения настроек тему никто не меняет.
      themeWatch_(view.theme.on_change(method(this, &Autosave::themeChanged))) {
    windowTimer_.add_onTick(method(this, &Autosave::saveWindow));
    positionTimer_.add_onTick(method(this, &Autosave::savePosition));
    settingsTimer_.add_onTick(method(this, &Autosave::saveSettings));
}

Autosave::~Autosave() {
    for (auto& [field, cookie] : flagWatches_) field->remove_change(cookie);
    for (auto& [field, cookie] : styleWatches_) field->remove_change(cookie);
    view_.theme.remove_change(themeWatch_);
    view_.position().remove_change(positionWatch_);
    window_.remove_onGeometryChanged(geometryWatch_);
}

/// Настройки вида — наблюдаемые поля: ползунки панели привязаны к ним (Bind),
/// полоса пишет в них с колеса и клавиш и сама их слушает, галочка полки
/// привязана к continueReading. Здесь остаётся одна запись на диск — с
/// паузой: ползунок шлёт перемену на каждый сдвиг, файл переписывается
/// целиком, а две записи одного файла не должны идти одновременно —
/// `write_all` пишет через один временный файл.
void Autosave::watchSettings() {
    Settings& settings = ws_.settings;
    for (observable<double>* field : {&settings.fontSize, &settings.lineHeight, &settings.margin}) {
        styleWatches_.emplace_back(field, field->on_change(method(this, &Autosave::styleChanged)));
    }
    flagWatches_.emplace_back(
        &settings.continueReading,
        settings.continueReading.on_change(method(this, &Autosave::continueReadingChanged)));
}

void Autosave::later(const DispatcherQueueTimer& timer) {
    timer.stop();   // каждая перемена отодвигает запись
    timer.start();
}

void Autosave::windowMoved() {
    later(windowTimer_);
}

void Autosave::positionChanged(uint32_t) noexcept {
    later(positionTimer_);
}

void Autosave::styleChanged(double) noexcept {
    later(settingsTimer_);
}

void Autosave::continueReadingChanged(bool) noexcept {
    later(settingsTimer_);
}

void Autosave::saveWindow() {
    windowTimer_.stop();

    // Место отдаётся строкой WinRT; к нам она приходит чужим текстом, и
    // проверенным становится так же, как любой другой чужой. Пишутся
    // настройки целиком — с ними и настройки вида, если их запись ещё ждёт.
    ws_.settings.windowPlacement = unicode::repaired(window_.placement());
    writeSettings(owner_, ws_);
}

void Autosave::savePosition() {
    positionTimer_.stop();
    saveState();
}

void Autosave::saveSettings() {
    settingsTimer_.stop();
    writeSettings(owner_, ws_);
}

void Autosave::saveState() {
    if (ws_.settings.lastBookGuid.empty() || !view_.isOpen()) return;

    // Место чтения — полосы, закладки — состояния книги: в файле они одно.
    BookState state = state_.get();
    state.charOffset = view_.position().get();
    writeState(owner_, ws_, ws_.settings.lastBookGuid, std::move(state));
}

void Autosave::flush() {
    // Окно уходит с полными настройками: запись места окна пишет их все.
    windowTimer_.stop();
    positionTimer_.stop();
    settingsTimer_.stop();
    saveWindow();
    saveState();
}

void Autosave::themeChanged(int index) noexcept {
    // Прежний ключ при обложке остаётся как то, куда вернуться, если реестр
    // обложек пропадёт. Запуск ставит ту же тему, что в файле, — её писать
    // незачем.
    ThemeList::Persisted names = themes_.persist(index, ws_.settings.theme);
    if (names.skin == ws_.settings.skin && names.theme == ws_.settings.theme) return;
    ws_.settings.skin = std::move(names.skin);
    ws_.settings.theme = std::move(names.theme);
    later(settingsTimer_);
}

}  // namespace bukvitsa::reader
