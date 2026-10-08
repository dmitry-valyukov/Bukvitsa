#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>
#include <span>

// Заголовки модели после всех стандартных: они ведут к импорту модулей книги и
// wxl.core, а стандартный заголовок после импорта MSVC уже не принимает. Свой
// первым: он тянет за собой книгу и вёрстку; пределы вида — из настроек.
#include "bukvitsa/reader/page_flow.h"
#include "bukvitsa/reader/settings.h"

namespace bukvitsa::reader {

namespace {

/// Мера полосы — длина строки в знаках, а не доля окна: слишком длинная строка
/// перестаёт читаться, глаз теряет начало следующей. Сколько знаков в строке —
/// известно точно, потому что известны шрифт и кегль; правило «шире трёх пятых
/// окна» кегля не знает и ошибается там, где важнее всего: на кегле 28 широкое
/// окно прекрасно читается в одну колонку, на кегле 14 то же окно — уже нет.
///
/// Восемьдесят пять, а не классические семьдесят: колонка на экране не то же
/// самое, что колонка в книге. Экран шире разворота, строка на нём длиннее
/// естественным образом, и разбивать её на колонки раньше времени — значит
/// дробить страницу там, где читателю удобнее целая.
constexpr float kMaxLineChars = 85.0f;

/// Нижняя граница новой колонки. Сорок три — половина верхней, округлённая
/// вверх (85 / 2 = 42,5): мера остаётся одна, и строка, переросшая восемьдесят
/// пять знаков, делится на колонки, которые попадают в ту же меру. Стоявшие
/// здесь прежде шестьдесят оставляли провал — окно между 85 и 130 знаками было
/// широко для одной колонки и узко для двух, и читатель получал строку в
/// полтораста знаков там, где просил разворот. Цена известна: пока нет
/// переносов, выключенная по формату строка в сорок с небольшим знаков идёт
/// с широкими пробелами, — но провал в мере обходился дороже.
///
/// Точно сомкнуть границы константой нельзя: средник вычитается до деления, и
/// половина строки выходит короче половины — при полях 7,5 % это 0,46 целой
/// строки, при 25 % четверть. Провал поэтому не исчезает, а сжимается: при
/// обычных полях вторая колонка приходит на 94 знаках вместо 85.
constexpr float kMinLineChars = 43.0f;

/// Гистерезис в знаках. Один знак и только для окна, которое тянут мышью
/// (relayout с windowResize): граница меры проходит под курсором, и без него
/// колонки защёлкают. Знака хватает — дрожь бывает в доли знака, а не в
/// четыре. Смена кегля и полей идёт мимо: читатель просит новый вид и
/// получает ровно его.
constexpr float kColumnHysteresis = 1.0f;

/// Средник — расстояние между колонками, в долях поля. Равен полю: просветы
/// у краёв окна и посередине разворота — одной ширины. Слиться колонкам это
/// не даёт, потому что по среднику лежит тень корешка — граница у них есть,
/// и шире зазора для неё не нужно.
constexpr float kGutterOfMargin = 1.0f;

/// Отступы сверху и снизу, в DIP. Регулировка «Поля» правит только
/// горизонтальные поля: ими читатель выбирает ширину строки, а высоте полосы
/// выбирать нечего — она и так вся, что осталось от окна.
constexpr float kVerticalMargin = 50.0f;

/// Книга ещё ни разу не наводилась на главу.
constexpr size_t kNoChapter = static_cast<size_t>(-1);

}  // namespace

/* ---------------- книга ---------------- */

void PageFlow::open(std::shared_ptr<Book> book, uint32_t charOffset) {
    // Разворот указывает в страницы прежней книги — отпускается раньше неё.
    spread_ = Spread{};
    book_ = std::move(book);
    position_ = charOffset;
    page_ = 0;
}

void PageFlow::close() {
    spread_ = Spread{};
    book_.reset();
    position_ = 0;
    page_ = 0;
}

std::span<const typography::Block> PageFlow::blocks() const {
    if (!book_) return {};
    return book_->blocks();
}

bool PageFlow::readable() const {
    // Книга без единой главы (пустое тело) верстать нечего: пагинатор такой
    // книги взял бы начало главы, которой нет.
    return book_ && book_->chapterCount() != 0;
}

/* ---------------- мера и стиль ---------------- */

void PageFlow::resize(float width, float height) {
    width_ = width;
    height_ = height;
}

bool PageFlow::setStyle(float fontSize, float lineHeightMultiplier, float marginFraction) {
    // Пределы — те же, что у ползунков: поле настроек могли сдвинуть и мимо
    // них (колесо, клавиши, файл), а полоса обязана остаться читаемой.
    const float size =
        std::clamp(fontSize, static_cast<float>(kFontSizeMin), static_cast<float>(kFontSizeMax));
    const float leading =
        std::clamp(lineHeightMultiplier, static_cast<float>(kLineHeightMin) / 100.0f,
                   static_cast<float>(kLineHeightMax) / 100.0f);
    const float side = std::clamp(marginFraction, static_cast<float>(kMarginMin) / 100.0f,
                                  static_cast<float>(kMarginMax) / 100.0f);

    if (size == fontSize_ && leading == lineHeight_ && side == marginFraction_) return false;

    fontSize_ = size;
    lineHeight_ = leading;
    marginFraction_ = side;
    return true;
}

float PageFlow::characterWidth() const {
    if (!book_) return fontSize_ * 0.5f;
    return book_->engine().averageCharWidth(fontSize_);
}

float PageFlow::lineChars(int columnCount) const {
    const float side = margin();
    const float available = width_ - side * 2.0f;
    const float gutters = side * kGutterOfMargin * static_cast<float>(columnCount - 1);
    return (available - gutters) / static_cast<float>(columnCount) / characterWidth();
}

int PageFlow::chooseColumns(bool sticky) const {
    const float side = margin();
    if (width_ - side * 2.0f <= 0.0f) return 1;

    // Колонки заполняют место между полями целиком, поэтому мера решает
    // единственный вопрос — сколько их. Добавляем колонку, пока строка длиннее
    // меры и пока следующая колонка не выйдет слишком узкой: на мелком кегле
    // широкое окно — это не одна строка в двести знаков, а три по семьдесят.
    int wanted = 1;
    while (lineChars(wanted) > kMaxLineChars && lineChars(wanted + 1) >= kMinLineChars) {
        ++wanted;
    }

    if (sticky) {
        // Держимся за нынешнее число, пока оно не стало откровенно плохим.
        if (wanted > columns_ && lineChars(columns_) <= kMaxLineChars + kColumnHysteresis) {
            return columns_;
        }
        if (wanted < columns_ && lineChars(columns_) >= kMinLineChars - kColumnHysteresis) {
            return columns_;
        }
    }
    return wanted;
}

bool PageFlow::relayout(bool windowResize) {
    if (!readable() || width_ <= 0.0f || height_ <= 0.0f) {
        spread_ = Spread{};
        return false;
    }

    // Пагинатор верстает по одной главе: наводим его на ту, где стоит читатель,
    // прежде чем считать. Та же глава — вызов ничего не делает, и тяга кегля не
    // пере-шейпит; другая (открыли книгу на запомненном месте) — глава
    // посчитается заново.
    book_->setCurrentChapter(position_);

    const float side = margin();
    const float statusHeight = fontSize_ * 1.6f;

    columns_ = chooseColumns(windowResize);

    const float gutters = side * kGutterOfMargin * static_cast<float>(columns_ - 1);
    const float share = (width_ - side * 2.0f - gutters) / static_cast<float>(columns_);

    typography::PageStyle style;
    // Ширину полосы задают поля, и ничто больше: место между ними делится
    // между колонками поровну. Свой предел здесь стоял бы поперёк ползунка
    // «Поля» — читатель просит колонку уже, а она не слушается.
    style.width = std::max(share, fontSize_ * 8.0f);
    style.height = std::max(height_ - kVerticalMargin * 2.0f - statusHeight, fontSize_ * 4.0f);
    style.fontSize = fontSize_;
    style.lineHeight = lineHeight_;

    pageStyle_ = style;

    // Вот ради чего книга режется на главы: полоса стала другой, а перевёрстка
    // считает не всю книгу, а одну текущую главу — единицы миллисекунд, — и
    // потому идёт начисто прямо в обработчике события. Черновика нет: с
    // разбивкой по главам чистовой набор сам достаточно дёшев.
    //
    // Досчитываем ровно до видимого разворота — этого хватает, чтобы показать
    // страницу; остаток главы добирается порциями в простое (advanceTail), и
    // с него становится известно общее число страниц («из M»).
    typography::Chapter& paginator = book_->paginator();
    paginator.beginLayout(pageStyle_);
    paginator.advanceTo(position_);

    // Колонка, где лежит буква места чтения, становится левой колонкой
    // разворота — разворот начинается ровно с неё, а не с округлённого вниз
    // края. Так на стыке глав не пропадает колонка: лента идёт от места чтения
    // подряд, и правую сторону разворота при нужде занимает начало следующей
    // главы. Саму букву перевёрстка не трогает: место чтения ставят открытие
    // книги, листание и прыжок — действия читателя, — а перевёрстка лишь
    // находит, где эта буква лежит теперь. Прижать место к началу колонки
    // здесь нельзя: при новой ширине начало колонки с буквой лежит не позже
    // самой буквы, и каждая перевёрстка уводила бы место назад — растяжка
    // мышью рождает десятки перевёрсток, каждая со своей шириной, и за одну
    // растяжку так терялось по нескольку разворотов. С неподвижной буквой
    // кегль туда и обратно возвращает на ту же колонку.
    page_ = paginator.pageCount() == 0 ? 0 : paginator.pageForCharOffset(position_);
    paginator.advanceToPage(page_ + static_cast<size_t>(columns_));

    buildSpread();
    return true;
}

/* ---------------- разворот и место чтения ---------------- */

PageFlow::Column PageFlow::anchor() const {
    if (!book_) return Column{};
    const size_t chapter =
        book_->currentChapter() == kNoChapter ? 0 : book_->currentChapter();
    return Column{chapter, page_};
}

size_t PageFlow::pageCount() const {
    return readable() ? book_->paginator().pageCount() : 0;
}

bool PageFlow::isComplete() const {
    return readable() && book_->paginator().isComplete();
}

float PageFlow::progress() const {
    if (!book_ || book_->characterCount() == 0) return 0.0f;
    return std::clamp(
        static_cast<float>(position_) / static_cast<float>(book_->characterCount()), 0.0f, 1.0f);
}

PageFlow::Status PageFlow::status() const {
    Status result;
    result.progress = progress();
    if (!readable()) return result;

    // Номер левой колонки известен сразу — видимый разворот посчитан начисто
    // в тот же кадр. Общее же число страниц главы становится известно, только
    // когда её набор кончился. Процент при этом верен всегда: он считается по
    // символам книги, а место чтения перевёрстка не двигает.
    result.chapter = anchor().chapter;
    result.first = page_;
    result.own = std::max<size_t>(spread_.ownColumns, 1);
    if (book_->paginator().isComplete())
        result.total = std::max<size_t>(book_->paginator().pageCount(), 1);
    return result;
}

typography::Chapter& PageFlow::chapterLaidTo(size_t index, size_t pages) {
    typography::Chapter& chapter = book_->chapterAt(index);

    // Разложена ли она под нынешнюю полосу? Свежая (ни одной страницы) или
    // соседняя, оставшаяся в кэше от прежней полосы, — переложить под текущую.
    // Текущую главу это не трогает: её стиль уже совпадает.
    if (chapter.pageCount() == 0 || !(chapter.style() == pageStyle_))
        chapter.beginLayout(pageStyle_);
    chapter.advanceToPage(pages);
    return chapter;
}

bool PageFlow::ribbonStep(Column& pos, bool forward) {
    if (forward) {
        // В пределах главы — следующая колонка, если она есть. advanceToPage до
        // pos.index+2 доводит счёт настолько, чтобы знать: либо колонка есть,
        // либо глава на ней и кончилась (тогда она уже complete).
        typography::Chapter& chapter = chapterLaidTo(pos.chapter, pos.index + 2);
        if (pos.index + 1 < chapter.pageCount()) {
            ++pos.index;
            return true;
        }
        // Глава кончилась — на начало первой непустой следующей.
        for (size_t next = pos.chapter + 1; next < book_->chapterCount(); ++next) {
            if (chapterLaidTo(next, 1).pageCount() > 0) {
                pos.chapter = next;
                pos.index = 0;
                return true;
            }
        }
        return false;   // последняя колонка книги
    }

    if (pos.index > 0) {
        --pos.index;
        return true;
    }
    // Начало главы — в конец предыдущей непустой. Её нужно знать целиком, чтобы
    // взять последнюю колонку, — верстаем до конца (глава мала).
    for (size_t prev = pos.chapter; prev-- > 0;) {
        typography::Chapter& chapter = chapterLaidTo(prev, static_cast<size_t>(-1));
        if (chapter.pageCount() > 0) {
            pos.chapter = prev;
            pos.index = chapter.pageCount() - 1;
            return true;
        }
    }
    return false;   // первая колонка книги
}

bool PageFlow::ribbonSpread(Column& pos, bool forward) {
    Column probe = pos;
    for (int i = 0; i < columns_; ++i) {
        if (!ribbonStep(probe, forward)) {
            if (forward)
                return false;         // конец книги — разворот не сдвинуть
            probe = Column{};         // начало книги — на самый первый разворот
            break;
        }
    }
    if (probe == pos)
        return false;
    pos = probe;
    return true;
}

void PageFlow::buildSpread() {
    spread_.pages.clear();
    spread_.ownColumns = 0;
    if (!readable() || width_ <= 0.0f)
        return;

    // Кэш держим ровно вокруг текущей главы. Радиус обязан покрыть весь
    // показанный разворот: он тянется на несколько глав вперёд, если они короче
    // него. Чистим до сборки — иначе трим уронил бы главу, чью страницу лента
    // уже держит.
    book_->trimChapters(static_cast<size_t>(columns_) + 1);

    const Column left = anchor();
    Column pos = left;

    for (int slot = 0; slot < columns_; ++slot) {
        // Довести колонку до реальной страницы, перешагивая исчерпанные главы:
        // короткая глава бывает уже разворота, и на неё приходится не одна его
        // колонка.
        const typography::Page* found = nullptr;
        while (pos.chapter < book_->chapterCount()) {
            typography::Chapter& chapter = chapterLaidTo(pos.chapter, pos.index + 1);
            if (pos.index < chapter.pageCount()) {
                found = &chapter.page(pos.index);
                break;
            }
            ++pos.chapter;   // в этой главе такой колонки нет — на начало следующей
            pos.index = 0;
        }
        if (!found)
            break;   // конец книги — дальше пусто

        spread_.pages.push_back(found);
        if (pos.chapter == left.chapter)
            ++spread_.ownColumns;
        ++pos.index;
    }
}

/* ---------------- переходы ---------------- */

std::optional<PageFlow::Column> PageFlow::turnTarget(bool forward) {
    if (!readable() || pageCount() == 0) return std::nullopt;

    // Следующий разворот ленты от нынешнего места. Лента непрерывна, так что
    // это обычный шаг: границу главы он проходит сам, не прыжком.
    Column target = anchor();
    if (!ribbonSpread(target, forward)) return std::nullopt;   // край книги
    return target;
}

PageFlow::Column PageFlow::columnOf(uint32_t charOffset) {
    if (!readable()) return Column{};

    // Место может лежать в другой главе — наводим на неё и верстаем начисто.
    // Сменилась глава — её раскладка сброшена, а разворот мог держать её
    // страницы (на стыке она была соседней): отпускаем его до show().
    if (book_->setCurrentChapter(charOffset)) spread_ = Spread{};

    typography::Chapter& paginator = book_->paginator();
    if (paginator.pageCount() == 0)
        paginator.beginLayout(pageStyle_);

    // Энергично, без срока: читатель прыгнул по закладке и ждёт ответа. Остаток
    // главы по-прежнему добирается порциями — тот, кто покажет колонку, заведёт
    // досчёт, и он продолжит с того, на чём мы кончили.
    paginator.advanceTo(charOffset);
    if (paginator.pageCount() == 0)
        return Column{book_->currentChapter(), 0};

    // Колонка, в которой лежит символ, — левая колонка разворота. К числу
    // колонок не прижимаем: разворот начинается ровно с места чтения, а не с
    // округлённого вниз края, — иначе на стыке глав пропадала бы колонка.
    return Column{book_->currentChapter(), paginator.pageForCharOffset(charOffset)};
}

void PageFlow::show(const Column& target) {
    if (!readable() || target.chapter >= book_->chapterCount()) return;

    // Целевая колонка может лежать в соседней главе: делаем её текущей, не
    // теряя вёрстки — лента разложила её как соседнюю на стыке.
    if (target.chapter != book_->currentChapter())
        book_->makeCurrentChapter(target.chapter);
    page_ = target.index;

    // Место чтения — начало колонки, на которую встали. У главы без единой
    // страницы колонки нет — место встаёт на её начало: разворот с неё
    // перешагнёт на следующую главу, и перевёрстка найдёт его там же.
    const typography::Chapter& chapter = book_->paginator();
    position_ = chapter.pageCount() != 0 ? chapter.page(page_).firstCharOffset
                                         : book_->chapterFirstChar(target.chapter);

    buildSpread();
}

/* ---------------- хвост главы ---------------- */

bool PageFlow::needsTail() const {
    return readable() && !book_->paginator().isComplete();
}

bool PageFlow::advanceTail(std::chrono::steady_clock::duration slice) {
    // Порции продолжают счёт с курсора текущей главы, не начиная заново:
    // посчитанные страницы остаются на месте, добирается лишь хвост. «Текущей»
    // — на момент порции: листание через границу меняет главу под идущим
    // досчётом, и он просто считает дальше ту, что стала текущей.
    if (!readable()) return false;
    return book_->paginator().advance(slice);
}

/* ---------------- геометрия полосы ---------------- */

float PageFlow::margin() const {
    return width_ * marginFraction_;
}

float PageFlow::verticalMargin() const {
    return kVerticalMargin;
}

float PageFlow::columnLeft(size_t index) const {
    if (!book_) return 0.0f;

    const float side = margin();
    const float gutter = side * kGutterOfMargin;

    // Первая колонка начинается ровно от поля: колонки занимают всё место
    // между полями, и центрировать тут нечего.
    return side + static_cast<float>(index) * (pageStyle_.width + gutter);
}

float PageFlow::spine() const {
    // Считается от колонок, а не как половина полосы. Поля симметричны, и
    // ответ тот же, но зависеть от этого незачем: корешок — это середина
    // средника, и сказано это должно быть про средник.
    const float gutter = margin() * kGutterOfMargin;
    return columnLeft(1) - gutter * 0.5f;
}

PageFlow::NoteHit PageFlow::noteAt(Point point) const {
    if (!book_) return NoteHit{};

    for (size_t column = 0; column < spread_.pages.size(); ++column) {
        const float left = columnLeft(column);

        for (const typography::PlacedLine& placed : spread_.pages[column]->lines) {
            const typography::Line& line = *placed.line;
            const float baseline = kVerticalMargin + placed.baseline;

            // По вертикали засчитываем всю строку, а не только надстрочный
            // знак: попасть мышью в шесть пикселей высотой нельзя.
            if (point.y < baseline - line.ascent || point.y > baseline + line.descent) continue;

            // Зона щелчка шире самого знака на треть кегля с каждой стороны —
            // по той же причине.
            const float slack = fontSize_ * 0.33f;

            for (const typography::PlacedNote& note : line.notes) {
                const float x = left + placed.x + note.x;
                if (point.x < x - slack || point.x > x + note.width + slack) continue;

                return NoteHit{note.target, Point{x + note.width * 0.5f, baseline + line.descent}};
            }
        }
    }

    return NoteHit{};
}

}  // namespace bukvitsa::reader
