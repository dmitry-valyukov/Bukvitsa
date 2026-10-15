// Перевод presentation-MathML в TeX: обход дерева wxl.xml с выпиской TeX в
// строку. Правила перевода — по элементам, см. таблицу в element(); всё
// незнакомое прозрачно, переводится его содержимое.
//
// Обход рекурсивный с явным потолком глубины — в отличие от парсеров wxl.html
// и wxl.xml, которым нельзя падать ни на каком входе, переводчик вправе
// ответить nullopt: формулы глубже сотни уровней в книгах не бывает, это
// враждебный вход, и честный отказ лучше, чем усложнять каждый случай ради
// него.

module bukvitsa.mathml;

import std;
import wxl.core;
import wxl.xml;

namespace bukvitsa::mathml {

namespace {

namespace xml = wxl::xml;

constexpr int kMaxDepth = 100;

// Одиночный символ токена -> макрос TeX. Греческие буквы и знаки, которых
// нет в ASCII; пробел после макроса отделяет его от следующей буквы.
struct mapped_char {
    char32_t code;
    const wchar_t* tex;
};

constexpr mapped_char kCharMap[] = {
    // греческие строчные
    {U'α', L"\\alpha "}, {U'β', L"\\beta "}, {U'γ', L"\\gamma "}, {U'δ', L"\\delta "},
    {U'ε', L"\\varepsilon "}, {U'ϵ', L"\\epsilon "}, {U'ζ', L"\\zeta "}, {U'η', L"\\eta "},
    {U'θ', L"\\theta "}, {U'ϑ', L"\\vartheta "}, {U'ι', L"\\iota "}, {U'κ', L"\\kappa "},
    {U'λ', L"\\lambda "}, {U'μ', L"\\mu "}, {U'ν', L"\\nu "}, {U'ξ', L"\\xi "},
    {U'π', L"\\pi "}, {U'ρ', L"\\rho "}, {U'σ', L"\\sigma "}, {U'ς', L"\\varsigma "},
    {U'τ', L"\\tau "}, {U'υ', L"\\upsilon "}, {U'φ', L"\\varphi "}, {U'ϕ', L"\\phi "},
    {U'χ', L"\\chi "}, {U'ψ', L"\\psi "}, {U'ω', L"\\omega "},
    // греческие прописные (те, что не совпадают с латиницей)
    {U'Γ', L"\\Gamma "}, {U'Δ', L"\\Delta "}, {U'Θ', L"\\Theta "}, {U'Λ', L"\\Lambda "},
    {U'Ξ', L"\\Xi "}, {U'Π', L"\\Pi "}, {U'Σ', L"\\Sigma "}, {U'Υ', L"\\Upsilon "},
    {U'Φ', L"\\Phi "}, {U'Ψ', L"\\Psi "}, {U'Ω', L"\\Omega "},
    // операции и отношения
    {U'−', L"-"}, {U'±', L"\\pm "}, {U'∓', L"\\mp "}, {U'×', L"\\times "},
    {U'⋅', L"\\cdot "}, {U'∗', L"\\ast "}, {U'÷', L"\\div "}, {U'≤', L"\\leq "},
    {U'≥', L"\\geq "}, {U'≠', L"\\neq "}, {U'≈', L"\\approx "}, {U'≡', L"\\equiv "},
    {U'∼', L"\\sim "}, {U'≃', L"\\simeq "}, {U'∝', L"\\propto "}, {U'≪', L"\\ll "},
    {U'≫', L"\\gg "},
    // множества и логика
    {U'∈', L"\\in "}, {U'∉', L"\\notin "}, {U'⊂', L"\\subset "}, {U'⊆', L"\\subseteq "},
    {U'⊃', L"\\supset "}, {U'⊇', L"\\supseteq "}, {U'∪', L"\\cup "}, {U'∩', L"\\cap "},
    {U'∅', L"\\emptyset "}, {U'∀', L"\\forall "}, {U'∃', L"\\exists "}, {U'¬', L"\\neg "},
    {U'∧', L"\\wedge "}, {U'∨', L"\\vee "}, {U'⊕', L"\\oplus "}, {U'⊗', L"\\otimes "},
    // стрелки
    {U'→', L"\\rightarrow "}, {U'←', L"\\leftarrow "}, {U'↔', L"\\leftrightarrow "},
    {U'⇒', L"\\Rightarrow "}, {U'⇐', L"\\Leftarrow "}, {U'⇔', L"\\Leftrightarrow "},
    {U'↦', L"\\mapsto "},
    // большие операторы и анализ
    {U'∑', L"\\sum "}, {U'∏', L"\\prod "}, {U'∫', L"\\int "}, {U'∬', L"\\iint "},
    {U'∭', L"\\iiint "}, {U'∮', L"\\oint "}, {U'∂', L"\\partial "}, {U'∇', L"\\nabla "},
    {U'∞', L"\\infty "}, {U'′', L"'"}, {U'″', L"''"},
    // прочие символы
    {U'⋯', L"\\cdots "}, {U'…', L"\\ldots "}, {U'⟨', L"\\langle "}, {U'⟩', L"\\rangle "},
    {U'ℏ', L"\\hbar "}, {U'ℓ', L"\\ell "}, {U'ℜ', L"\\Re "}, {U'ℑ', L"\\Im "},
    {U'∘', L"\\circ "}, {U'√', L"\\surd "},
};

const wchar_t* mappedChar(char32_t code) {
    for (const mapped_char& entry : kCharMap) {
        if (entry.code == code) return entry.tex;
    }
    return nullptr;
}

// Невидимые операторы MathML (U+2061..U+2064): применение функции, невидимое
// умножение и прочие подсказки читалкам экрана. В TeX им соответствует
// пустота; числами, потому что в редакторе эти символы не видны.
constexpr bool isInvisible(char32_t code) { return code >= 0x2061 && code <= 0x2064; }

constexpr bool isWhitespace(char16_t c) {
    return c == u' ' || c == u'\t' || c == u'\r' || c == u'\n';
}

// Больше одной кодовой точки — слово, а не знак. Единицы UTF-16 считать
// нельзя: буква вне BMP — две единицы, но всё ещё одна буква.
bool isWord(wxl::core::u16_view text) {
    const wxl::core::unicode::code_points points(text);
    auto point = points.begin();
    return point != points.end() && ++point != points.end();
}

// Имена функций, у которых в TeX есть свой макрос: \sin набирается прямым,
// как положено функции, а не курсивом перемноженных переменных.
constexpr std::wstring_view kFunctions[] = {
    L"sin", L"cos", L"tan", L"cot", L"sec", L"csc", L"arcsin", L"arccos", L"arctan",
    L"sinh", L"cosh", L"tanh", L"coth", L"log", L"ln", L"lg", L"lim", L"exp",
    L"min", L"max", L"sup", L"inf", L"det", L"gcd", L"deg", L"dim", L"arg", L"mod",
};

bool isFunctionName(std::wstring_view name) {
    for (const std::wstring_view known : kFunctions) {
        if (name == known) return true;
    }
    return false;
}

// Основания, у которых munder/munderover — это пределы, а не украшение.
bool isBigOperator(std::wstring_view base) {
    if (base.size() == 1) {
        switch (base.front()) {
        case L'∑': case L'∏': case L'∫': case L'∮': case L'⋀': case L'⋁':
        case L'⋂': case L'⋃':
            return true;
        default:
            return false;
        }
    }
    return base == L"lim" || base == L"max" || base == L"min" || base == L"sup" ||
           base == L"inf" || base == L"∬" || base == L"∭";
}

class translator {
public:
    std::wstring take() && { return std::move(out_); }

    bool failed() const { return failed_; }

    // Содержимое элемента: дети по очереди. Значимый текст прямо в
    // контейнере (кривой MathML, но встречается) не пропадает.
    void children(const xml::node& parent) {
        if (enter()) return;
        for (const xml::node& child : parent.children()) {
            if (child.is_element()) element(child);
            else if (child.is_text()) looseText(child);
        }
        leave();
    }

    void element(const xml::node& e) {
        if (failed_) return;
        const std::string_view name = e.name().chars();

        if (name == "mi") identifier(e);
        else if (name == "mn") token(e);
        else if (name == "mo") operatorToken(e);
        else if (name == "mtext" || name == "ms") textToken(e);
        else if (name == "mspace") out_ += L"\\; ";
        else if (name == "mfrac") fraction(e);
        else if (name == "msqrt") sqrt(e);
        else if (name == "mroot") root(e);
        else if (name == "msub") scripts(e, true, false);
        else if (name == "msup") scripts(e, false, true);
        else if (name == "msubsup") scripts(e, true, true);
        else if (name == "mover") over(e);
        else if (name == "munder") under(e);
        else if (name == "munderover") underOver(e);
        else if (name == "mfenced") fenced(e);
        else if (name == "mtable") table(e);
        else if (name == "mphantom") wrap(e, L"\\phantom");
        else if (name == "semantics") presentation(e);
        else if (name == "annotation" || name == "annotation-xml") { /* не текст формулы */ }
        else if (name == "none") out_ += L"{}";
        else if (name == "mmultiscripts") multiscripts(e);
        else children(e);  // mrow, mstyle, math и всё незнакомое — прозрачны
    }

private:
    // ---- служебное ----

    bool enter() {
        if (++depth_ > kMaxDepth) failed_ = true;
        return failed_;
    }

    void leave() { --depth_; }

    static std::vector<const xml::node*> elementChildren(const xml::node& parent) {
        std::vector<const xml::node*> elements;
        for (const xml::node& child : parent.children()) {
            if (child.is_element()) elements.push_back(&child);
        }
        return elements;
    }

    // Весь текст токена, одной строкой UTF-16, пробельные края обрезаны.
    // Куски — проверенный UTF-8: wxl.xml проверяет документ целиком до
    // разбора. Склейка проверенного проверена, а края режутся по пробелам
    // ASCII, то есть между кодовыми точками, — гарантия идёт до конца без
    // assume_valid.
    static wxl::core::u16_text tokenText(const xml::node& e) {
        wxl::core::u16_text text;
        for (const wxl::core::u8_view piece : e.text_pieces()) text += piece.to_utf16();
        const std::u16string_view units = text.plain();
        std::size_t begin = 0;
        std::size_t end = units.size();
        while (begin < end && isWhitespace(units[begin])) ++begin;
        while (end > begin && isWhitespace(units[end - 1])) --end;
        text.erase(end);
        text.erase(0, begin);
        return text;
    }

    // Символ как есть, но со спецсимволами TeX под защитой.
    void appendEscaped(char32_t code) {
        switch (code) {
        case U'{': out_ += L"\\{"; break;
        case U'}': out_ += L"\\}"; break;
        case U'$': out_ += L"\\$"; break;
        case U'&': out_ += L"\\&"; break;
        case U'#': out_ += L"\\#"; break;
        case U'%': out_ += L"\\%"; break;
        case U'_': out_ += L"\\_"; break;
        case U'\\': out_ += L"\\backslash "; break;
        case U' ': out_ += L' '; break;
        default: wxl::core::unicode::append_utf16(out_, code);
        }
    }

    // Текст токена: каждая кодовая точка — через таблицу или экранированной.
    void appendMapped(wxl::core::u16_view text) {
        for (const char32_t code : wxl::core::unicode::code_points(text)) {
            if (isInvisible(code)) continue;
            if (const wchar_t* tex = mappedChar(code)) out_ += tex;
            else appendEscaped(code);
        }
    }

    // Один ребёнок в группе {…}; отсутствующий — пустая группа: перевод
    // терпит и обрезанную формулу.
    void group(const xml::node* e) {
        out_ += L'{';
        if (e) element(*e);
        out_ += L'}';
    }

    void groupChildren(const xml::node& e) {
        out_ += L'{';
        children(e);
        out_ += L'}';
    }

    void wrap(const xml::node& e, std::wstring_view macro) {
        out_ += macro;
        groupChildren(e);
    }

    // ---- токены ----

    void identifier(const xml::node& e) {
        const wxl::core::u16_text text = tokenText(e);
        if (text.empty()) return;
        const char32_t first = *wxl::core::unicode::code_points(text).begin();
        const bool multi = isWord(text) && mappedChar(first) == nullptr;
        if (multi && isFunctionName(text)) {
            out_ += L'\\';
            out_ += text.wchars();
            out_ += L' ';
            return;
        }
        const std::optional<wxl::core::u8_view> variant = e.attribute("mathvariant");
        const bool upright = variant && variant->chars() == "normal";
        if (multi || upright) {
            out_ += L"\\mathrm{";
            appendMapped(text);
            out_ += L'}';
            return;
        }
        appendMapped(text);
    }

    void token(const xml::node& e) { appendMapped(tokenText(e)); }

    void operatorToken(const xml::node& e) {
        const wxl::core::u16_text text = tokenText(e);
        appendMapped(text);
        // После словесного оператора (mod и подобных) TeX-имя требует зазора;
        // односимвольные и так отделены своей природой.
        if (isWord(text) && text.plain().find(u'\\') == std::u16string::npos) out_ += L' ';
    }

    void textToken(const xml::node& e) {
        out_ += L"\\text{";
        appendMapped(tokenText(e));
        out_ += L'}';
    }

    // Края текста обрезаны, так что непустой остаток — уже не одни пробелы.
    void looseText(const xml::node& text) { appendMapped(tokenText(text)); }

    // ---- структуры ----

    void fraction(const xml::node& e) {
        const std::vector<const xml::node*> parts = elementChildren(e);
        out_ += L"\\frac";
        group(parts.size() > 0 ? parts[0] : nullptr);
        group(parts.size() > 1 ? parts[1] : nullptr);
    }

    void sqrt(const xml::node& e) {
        out_ += L"\\sqrt";
        groupChildren(e);
    }

    void root(const xml::node& e) {
        const std::vector<const xml::node*> parts = elementChildren(e);
        out_ += L"\\sqrt[";
        if (parts.size() > 1) element(*parts[1]);
        out_ += L']';
        group(parts.size() > 0 ? parts[0] : nullptr);
    }

    void scripts(const xml::node& e, bool sub, bool sup) {
        const std::vector<const xml::node*> parts = elementChildren(e);
        group(parts.size() > 0 ? parts[0] : nullptr);
        std::size_t next = 1;
        if (sub) {
            out_ += L'_';
            group(parts.size() > next ? parts[next] : nullptr);
            ++next;
        }
        if (sup) {
            out_ += L'^';
            group(parts.size() > next ? parts[next] : nullptr);
        }
    }

    // Акцент над основанием, когда второй ребёнок — известный значок.
    const wchar_t* accentMacro(const xml::node* accent) {
        if (!accent) return nullptr;
        const wxl::core::u16_text text = tokenText(*accent);
        if (text == L"¯" || text == L"‾" || text == L"_") return L"\\bar";
        if (text == L"^" || text == L"ˆ") return L"\\hat";
        if (text == L"~" || text == L"˜") return L"\\tilde";
        if (text == L"˙") return L"\\dot";
        if (text == L"¨") return L"\\ddot";
        if (text == L"→" || text == L"⃗") return L"\\vec";
        return nullptr;
    }

    void over(const xml::node& e) {
        const std::vector<const xml::node*> parts = elementChildren(e);
        const xml::node* base = parts.size() > 0 ? parts[0] : nullptr;
        const xml::node* top = parts.size() > 1 ? parts[1] : nullptr;

        if (base && isBigOperator(tokenText(*base))) {
            element(*base);
            out_ += L'^';
            group(top);
            return;
        }
        if (const wchar_t* accent = accentMacro(top)) {
            out_ += accent;
            group(base);
            return;
        }
        out_ += L"\\overset";
        group(top);
        group(base);
    }

    void under(const xml::node& e) {
        const std::vector<const xml::node*> parts = elementChildren(e);
        const xml::node* base = parts.size() > 0 ? parts[0] : nullptr;
        const xml::node* bottom = parts.size() > 1 ? parts[1] : nullptr;

        if (base && isBigOperator(tokenText(*base))) {
            element(*base);
            out_ += L'_';
            group(bottom);
            return;
        }
        out_ += L"\\underset";
        group(bottom);
        group(base);
    }

    void underOver(const xml::node& e) {
        const std::vector<const xml::node*> parts = elementChildren(e);
        const xml::node* base = parts.size() > 0 ? parts[0] : nullptr;

        // И у больших операторов, и у чего угодно ещё запись одна — пределы
        // снизу и сверху; \sum сам поднимет их над собой в display-стиле.
        if (base) element(*base);
        out_ += L'_';
        group(parts.size() > 1 ? parts[1] : nullptr);
        out_ += L'^';
        group(parts.size() > 2 ? parts[2] : nullptr);
    }

    void fenced(const xml::node& e) {
        // Значения атрибутов — проверенный UTF-8 из того же документа, что и
        // текст токенов; у отсутствующего — умолчание спецификации mfenced.
        const auto attributeOr = [&](std::string_view name, wxl::core::u8_view fallback) {
            return e.attribute(name).value_or(fallback);
        };
        const wxl::core::u8_view open = attributeOr("open", u8"(");
        const wxl::core::u8_view close = attributeOr("close", u8")");

        // Разделитель — кодовая точка, а не единица: знак вне BMP иначе
        // разорвался бы пополам.
        const wxl::core::u8_view listed = attributeOr("separators", u8",");
        std::vector<char32_t> separators;
        for (const char32_t code : wxl::core::unicode::code_points(listed))
            separators.push_back(code);

        const std::vector<const xml::node*> parts = elementChildren(e);

        appendFence(L"\\left", open);
        for (std::size_t i = 0; i < parts.size(); ++i) {
            if (i > 0) {
                // Разделителей может быть меньше, чем зазоров: последний
                // повторяется — так велит спецификация mfenced.
                const std::size_t at = std::min(i - 1, separators.size() - 1);
                if (!separators.empty()) appendEscaped(separators[at]);
            }
            element(*parts[i]);
        }
        appendFence(L"\\right", close);
    }

    // Скобка — первая кодовая точка значения, целиком.
    void appendFence(std::wstring_view side, wxl::core::u8_view fence) {
        out_ += side;
        if (fence.empty()) {
            out_ += L'.';
            return;
        }
        const char32_t c = *wxl::core::unicode::code_points(fence).begin();
        if (c == U'{') out_ += L"\\{";
        else if (c == U'}') out_ += L"\\}";
        else if (c == U'⟨') out_ += L"\\langle ";
        else if (c == U'⟩') out_ += L"\\rangle ";
        else if (c == U'‖') out_ += L"\\Vert ";
        else wxl::core::unicode::append_utf16(out_, c);
    }

    void table(const xml::node& e) {
        out_ += L"\\begin{matrix}";
        bool firstRow = true;
        for (const xml::node& row : e.children()) {
            if (!row.is_element()) continue;
            const std::string_view rowName = row.name().chars();
            if (rowName != "mtr" && rowName != "mlabeledtr") continue;
            if (!firstRow) out_ += L" \\\\ ";
            firstRow = false;
            bool firstCell = true;
            for (const xml::node& cell : row.children()) {
                if (!cell.is_element() || cell.name().chars() != "mtd") continue;
                if (!firstCell) out_ += L" & ";
                firstCell = false;
                children(cell);
            }
        }
        out_ += L"\\end{matrix}";
    }

    // <semantics>: первый элемент — сама формула, остальные — аннотации
    // (среди них часто лежит исходный TeX, но он может звать макросы, которых
    // у MicroTeX нет; свой перевод предсказуем).
    void presentation(const xml::node& e) {
        for (const xml::node& child : e.children()) {
            if (child.is_element()) {
                element(child);
                return;
            }
        }
    }

    void multiscripts(const xml::node& e) {
        // База и пары под/над после неё; преиндексы (после <mprescripts/>)
        // первая очередь не переводит — в книгах они экзотика.
        const std::vector<const xml::node*> parts = elementChildren(e);
        if (parts.empty()) return;
        element(*parts[0]);
        for (std::size_t i = 1; i + 1 < parts.size(); i += 2) {
            if (parts[i]->name().chars() == "mprescripts") break;
            out_ += L'_';
            group(parts[i]);
            out_ += L'^';
            group(parts[i + 1]);
        }
    }

    std::wstring out_;
    bool failed_ = false;
    int depth_ = 0;
};

}  // namespace

std::optional<TexFormula> toTex(wxl::core::u8_view mathml) {
    xml::document document;
    const xml::node* root = nullptr;
    try {
        root = &document.load(std::string{mathml.chars()});
    } catch (const xml::exception&) {
        return std::nullopt;
    }

    const xml::node* math = root->find("math");
    if (!math) return std::nullopt;

    translator walk;
    walk.children(*math);
    if (walk.failed()) return std::nullopt;

    TexFormula formula;
    formula.tex = std::move(walk).take();
    const std::optional<wxl::core::u8_view> display = math->attribute("display");
    formula.display = display && display->chars() == "block";
    return formula;
}

}  // namespace bukvitsa::mathml
