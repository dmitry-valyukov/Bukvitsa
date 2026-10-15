// Проверка загрузки книги на официальных примерах из репозитория
// спецификации. Без фреймворка: тест ценен тем, что читает настоящие
// файлы, а не тем, как он об этом сообщает.
//
// wxl.xml выделяет память из STA-пула wxl.core — он должен существовать
// до первого разбора и пережить последнюю книгу.

#include <cstdio>
#include <filesystem>
#include <string>

import wxl.core;

import bukvitsa.fb3;

using namespace bukvitsa::fb3;

namespace {

int failures = 0;

void check(bool condition, std::string_view what) {
    std::printf("%s %.*s\n", condition ? "  ok  " : "FAILED", static_cast<int>(what.size()),
                what.data());
    if (!condition) ++failures;
}

/// Сколько узлов каждого рода в книге — самый быстрый способ увидеть,
/// что разбор не потерял и не выдумал структуру.
void countNodes(const Node& node, int counts[64]) {
    counts[static_cast<int>(node.kind())]++;
    for (const Node& child : node.children())
        countNodes(child, counts);
}

/// Первые символы книги как их увидит вёрстка: текст в порядке документа.
std::string firstText(const Node& node, size_t limit) {
    std::string text;

    auto walk = [&](const Node& n, auto& self) -> void {
        if (text.size() >= limit) return;
        if (n.kind() == NodeKind::Text) text += n.text().chars();
        for (const Node& child : n.children()) self(child, self);
    };

    walk(node, walk);
    return text.substr(0, limit);
}

void testBook(const std::filesystem::path& path) {
    std::printf("\n=== %s ===\n", path.filename().string().c_str());

    Document book(path);
    const Description& d = book.description();

    std::printf("  название: %s\n", d.title.c_str());
    std::printf("  авторы:   %s\n", d.authorsLine().c_str());
    std::printf("  язык:     %s, символов: %u, картинок: %zu\n",
                d.language.c_str(), book.characterCount(), book.images().size());

    check(!d.title.empty(), "название прочитано");
    check(book.characterCount() > 0, "в книге есть текст");
    check(book.body().hasChildren(), "у тела есть дети");

    int counts[64] = {};
    countNodes(book.body(), counts);
    std::printf("  секций: %d, абзацев: %d, текстовых узлов: %d, картинок в тексте: %d\n",
                counts[static_cast<int>(NodeKind::Section)], counts[static_cast<int>(NodeKind::Paragraph)],
                counts[static_cast<int>(NodeKind::Text)], counts[static_cast<int>(NodeKind::Image)]);

    check(counts[static_cast<int>(NodeKind::Section)] > 0, "секции разобраны");
    check(counts[static_cast<int>(NodeKind::Paragraph)] > 0, "абзацы разобраны");

    std::printf("  начало:   %s...\n", firstText(book.body(), 120).c_str());

    // Позиция чтения: по смещению символа находится узел, и он не позже него.
    if (const Node* at = book.nodeAtCharOffset(book.characterCount() / 2))
        check(at->charOffset() <= book.characterCount() / 2, "позиция чтения находит узел");

    // Сноски: каждая ссылка нашла своё тело — иначе при вёрстке некуда
    // будет отправить читателя.
    int refs = 0, resolved = 0;
    auto walkNotes = [&](const Node& n, auto& self) -> void {
        if (const NoteRefData* ref = n.noteRef()) {
            ++refs;
            if (ref->target) ++resolved;
        }
        for (const Node& child : n.children()) self(child, self);
    };
    walkNotes(book.body(), walkNotes);

    if (refs > 0) {
        std::printf("  сносок: %d, разрешено: %d\n", refs, resolved);
        check(refs == resolved, "все ссылки на сноски разрешены");
    }
}

// Числа ниже сверены с body.xml этих книг обходом, который повторяет разбор:
// роман без картинок и официальный пример, где есть всё.
constexpr const char* kNovel = "Turgenev_I._Spisokshkolnoy._Otcyi_I_Deti.fb3";
constexpr const char* kNightmare = "nightmare_example.fb3";

void printMeasures(const Measures& measures) {
    std::printf("  кодовых точек: %u, абзацев: %u, картинок: %u, разделителей: %u, "
                "стилей: %u, ссылок на сноски: %u\n",
                measures.codePoints, measures.paragraphs, measures.images, measures.separators,
                measures.inlineStyles, measures.noteRefs);
}

/// Меры прохода разбора: по ним вёрстка резервирует массивы, и меры, которые
/// разошлись с книгой, — это разбор, который что-то потерял или выдумал.
void testMeasures(const std::filesystem::path& testdata) {
    std::printf("\n=== меры: %s ===\n", kNovel);
    {
        Document book(testdata / kNovel);
        const Measures measures = book.measures();
        printMeasures(measures);

        check(measures.codePoints == book.characterCount(), "кодовые точки — это characterCount()");
        check(measures.images == 0, "картинок в тексте нет");
        check(measures.separators == 0, "разделителей нет");
        check(measures.noteRefs == 155, "155 ссылок на сноски");
        check(measures.paragraphs > 2000, "больше двух тысяч абзацев");
    }

    std::printf("\n=== меры: %s ===\n", kNightmare);
    {
        Document book(testdata / kNightmare);
        const Measures measures = book.measures();
        printMeasures(measures);

        check(measures.images == 124, "124 картинки в тексте");
        check(measures.noteRefs == 40, "40 ссылок на сноски");
        // Разделитель — вид узла, а не блок вёрстки: 38 `<br>` (ни одного с
        // clear) и 8 границ бумажной страницы, хоть семь из них и стоят внутри
        // абзаца или подзаголовка.
        check(measures.separators == 46, "46 разделителей: 38 <br> и 8 границ страницы");
    }
}

/// Тела сносок по порядку: номер в ссылке указывает на то же тело, что и
/// узел, — иначе вёрстка, перейдя на номера, развернула бы чужую сноску.
void testNoteBodies(const std::filesystem::path& testdata) {
    std::printf("\n=== тела сносок: %s ===\n", kNovel);

    Document book(testdata / kNovel);
    const auto bodies = book.noteBodies();
    std::printf("  тел: %zu\n", bodies.size());

    check(bodies.size() == 155, "155 тел сносок");

    bool allNoteBodies = true;
    bool inDocumentOrder = true;
    for (size_t i = 0; i < bodies.size(); ++i) {
        allNoteBodies = allNoteBodies && bodies[i]->kind() == NodeKind::NoteBody;
        if (i > 0 && bodies[i - 1]->charOffset() > bodies[i]->charOffset())
            inDocumentOrder = false;
    }
    check(allNoteBodies, "в списке только узлы NoteBody");
    check(inDocumentOrder, "тела идут в порядке документа");

    int refs = 0, numbered = 0, same = 0;
    auto walk = [&](const Node& n, auto& self) -> void {
        if (const NoteRefData* ref = n.noteRef()) {
            ++refs;
            if (ref->body) {
                ++numbered;
                if (*ref->body < bodies.size() && bodies[*ref->body] == ref->target &&
                    bodies[*ref->body]->kind() == NodeKind::NoteBody)
                    ++same;
            }
        }
        for (const Node& child : n.children()) self(child, self);
    };
    walk(book.body(), walk);

    std::printf("  ссылок: %d, с номером тела: %d, номер и узел совпали: %d\n", refs, numbered, same);
    check(refs == 155 && numbered == refs, "у каждой ссылки стоит номер тела");
    check(same == refs, "номер указывает на тот же NoteBody, что и target");
}

/// Байты картинок уходят книге: у части остаются связь и тип, а байты
/// пустеют — теперь они у того, кто их взял.
void testTakeImageBytes(const std::filesystem::path& testdata) {
    std::printf("\n=== байты картинок отдаются: %s ===\n", kNightmare);

    Document book(testdata / kNightmare);
    const size_t count = book.images().size();
    std::printf("  частей-картинок: %zu\n", count);
    check(count > 0, "у книги есть части-картинки");

    bool allBytes = true, allEmptied = true, allKept = true;
    for (uint32_t index = 0; index < count; ++index) {
        const std::string relationshipId(book.images()[index].relationshipId.chars());
        const std::string contentType(book.images()[index].contentType.chars());

        const std::string bytes = book.takeImageBytes(index);

        // Смотрим images(), а не image(): тот прочёл бы байты из пакета заново.
        const ImagePart& part = book.images()[index];
        allBytes = allBytes && !bytes.empty();
        allEmptied = allEmptied && part.bytes.empty();
        allKept = allKept && part.relationshipId.chars() == relationshipId &&
                  part.contentType.chars() == contentType;
    }

    check(allBytes, "takeImageBytes отдаёт непустые байты каждой части");
    check(allEmptied, "после взятия байты части пусты");
    check(allKept && book.images().size() == count, "связь, тип и число частей прежние");
}

/// Описание уходит книге. Второго вызова здесь нет: он — нарушение, на
/// котором процесс останавливается.
void testTakeDescription(const std::filesystem::path& testdata) {
    std::printf("\n=== описание отдаётся: %s ===\n", kNovel);

    Document book(testdata / kNovel);
    const std::string title(book.description().title.chars());

    const Description description = book.takeDescription();
    std::printf("  название: %s\n", description.title.c_str());

    check(!description.title.empty() && description.title.chars() == title,
          "takeDescription отдаёт название");
}

/// Проверка на одной книге: исключение разбора — её провал, а не конец теста.
template <class Test>
void guarded(const char* what, const Test& test) {
    try {
        test();
    } catch (const std::exception& error) {
        std::printf("FAILED %s: %s\n", what, error.what());
        ++failures;
    }
}

}  // namespace

int main() {
    const std::filesystem::path testdata{BUKVITSA_TESTDATA_DIR};

    // Три официальных примера из репозитория спецификации плюс, если он лежит
    // рядом, настоящий файл ЛитРес: официальные примеры проверяют формат,
    // а книга из магазина — то, что издатель на самом деле пишет.
    for (const char* name : {"anathomy_tutorial_example.fb3", "nightmare_example.fb3",
                             "hardcore_file_structure.fb3",
                             "Prokofev_R._Stellar9._Stellar_Prometeyi67901532.fb3"}) {
        const std::filesystem::path path = testdata / name;

        if (!std::filesystem::exists(path)) {
            std::printf("\n=== %s: нет файла, пропущен ===\n", name);
            continue;
        }

        try {
            testBook(path);
        } catch (const std::exception& error) {
            std::printf("FAILED %s: %s\n", name, error.what());
            ++failures;
        }
    }

    guarded("меры", [&] { testMeasures(testdata); });
    guarded("тела сносок", [&] { testNoteBodies(testdata); });
    guarded("байты картинок", [&] { testTakeImageBytes(testdata); });
    guarded("описание", [&] { testTakeDescription(testdata); });

    std::printf("\n%s\n", failures == 0 ? "OK" : "ЕСТЬ ОШИБКИ");
    return failures;
}
