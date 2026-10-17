module;

// Packaging API — это C и COM, стандартной библиотеки они с собой не несут,
// поэтому им место в глобальном фрагменте модуля: `import std;` ниже они не
// трогают, и порядок включений больше ничего не значит.
#include "platform.h"

#include <msopc.h>
#include <objbase.h>

module bukvitsa.fb3;

import std;
import wxl.core;

import :opc;

namespace bukvitsa::fb3 {
namespace {

/// Строка, которую вернул COM: освобождать её должен вызывающий, и забыть
/// об этом легко, поэтому она не хранится голым указателем нигде.
class CoString {
public:
    ~CoString() { if (value_) CoTaskMemFree(value_); }
    CoString() = default;
    CoString(const CoString&) = delete;
    CoString& operator=(const CoString&) = delete;

    LPWSTR* put() { return &value_; }
    std::wstring_view view() const { return value_ ? std::wstring_view(value_) : std::wstring_view(); }
    std::wstring str() const { return value_ ? std::wstring(value_) : std::wstring(); }

private:
    LPWSTR value_ = nullptr;
};

class BStr {
public:
    ~BStr() { if (value_) SysFreeString(value_); }
    BSTR* put() { return &value_; }
    std::wstring str() const { return value_ ? std::wstring(value_, SysStringLen(value_)) : std::wstring(); }

private:
    BSTR value_ = nullptr;
};

/// Файл книги, уже прочитанный кем-то другим, — как поток для Packaging API.
///
/// Нужен потому, что читать файл здесь нельзя: чтение с диска в этой читалке
/// живёт на рабочем потоке и приезжает сюда байтами, а разбор идёт в
/// интерфейсном. `CreateStreamOnFile` открыл бы файл сам и прочитал бы его
/// синхронно — то есть ровно то, чего мы избегаем.
///
/// Только чтение и только то, что спрашивает `ReadPackageFromStream`: `Read`,
/// `Seek` и размер из `Stat`. Всё остальное честно отвечает `E_NOTIMPL` —
/// поток, который делает вид, что умеет писать, хуже того, который не умеет.
///
/// Байтами он не владеет: они принадлежат книге и живут дольше пакета,
/// потому что пакет читает части по требованию — так Packaging API читает и
/// по умолчанию (`OPC_READ_DEFAULT`).
class MemoryStream final : public IStream {
public:
    explicit MemoryStream(std::span<const std::byte> bytes) noexcept : bytes_(bytes) {}

    // ---- IUnknown ----

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;

        if (iid == __uuidof(IUnknown) || iid == __uuidof(ISequentialStream) ||
            iid == __uuidof(IStream)) {
            *out = static_cast<IStream*>(this);
            AddRef();
            return S_OK;
        }

        *out = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = --references_;
        if (left == 0) delete this;
        return left;
    }

    // ---- ISequentialStream ----

    HRESULT STDMETHODCALLTYPE Read(void* into, ULONG wanted, ULONG* got) override {
        const std::size_t left = bytes_.size() - position_;
        const ULONG taken = static_cast<ULONG>(std::min<std::size_t>(wanted, left));

        if (taken != 0) std::memcpy(into, bytes_.data() + position_, taken);

        position_ += taken;

        if (got) *got = taken;

        return taken == wanted ? S_OK : S_FALSE;
    }

    HRESULT STDMETHODCALLTYPE Write(const void*, ULONG, ULONG*) override { return STG_E_ACCESSDENIED; }

    // ---- IStream ----

    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move, DWORD from, ULARGE_INTEGER* out) override {
        std::int64_t base = 0;

        switch (from) {
            case STREAM_SEEK_SET: base = 0; break;
            case STREAM_SEEK_CUR: base = static_cast<std::int64_t>(position_); break;
            case STREAM_SEEK_END: base = static_cast<std::int64_t>(bytes_.size()); break;
            default: return STG_E_INVALIDFUNCTION;
        }

        const std::int64_t wanted = base + move.QuadPart;

        // Позади начала — ошибка; за концом — законно и означает пустое
        // чтение, как у всякого потока.
        if (wanted < 0) return STG_E_INVALIDFUNCTION;

        position_ = static_cast<std::size_t>(
            std::min<std::int64_t>(wanted, static_cast<std::int64_t>(bytes_.size())));

        if (out) out->QuadPart = position_;

        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Stat(STATSTG* out, DWORD flags) override {
        if (!out) return E_POINTER;

        *out = STATSTG{};
        out->type = STGTY_STREAM;
        out->cbSize.QuadPart = bytes_.size();

        if (flags != STATFLAG_NONAME) out->pwcsName = nullptr;

        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override { return STG_E_ACCESSDENIED; }
    HRESULT STDMETHODCALLTYPE CopyTo(IStream*, ULARGE_INTEGER, ULARGE_INTEGER*,
                                     ULARGE_INTEGER*) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Revert() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override {
        return STG_E_INVALIDFUNCTION;
    }
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override {
        return STG_E_INVALIDFUNCTION;
    }
    HRESULT STDMETHODCALLTYPE Clone(IStream** out) override {
        if (!out) return E_POINTER;

        MemoryStream* copy = new MemoryStream(bytes_);
        copy->position_ = position_;

        *out = copy;
        return S_OK;
    }

private:
    std::span<const std::byte> bytes_;
    std::size_t position_ = 0;
    ULONG references_ = 1;
};

/// Простой владеющий указатель на COM-интерфейс. Своего хватает: тащить сюда
/// C++/WinRT ради одного шаблона незачем, а FB3 не должен зависеть от WinRT.
template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { if (p_) p_->Release(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ComPtr(ComPtr&& other) noexcept : p_(other.p_) { other.p_ = nullptr; }

    T** put() { return &p_; }
    void** putVoid() { return reinterpret_cast<void**>(&p_); }
    T* get() const { return p_; }
    T* operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }

private:
    T* p_ = nullptr;
};

/// Отказ Packaging API — исключение с причиной: разбор книги несёт его наверх,
/// до «Не удалось открыть книгу». Испорченный пакет и сбой системы (памяти нет,
/// фабрики) идут одним путём, как и в конструкторах: пакет, открытый без
/// OPC_VALIDATE_ON_LOAD, проверяется при обращении, ошибку в связях или имени
/// части отдаёт тот вызов, что первым к ним прикоснулся, и разбор, прерванный
/// тем или другим, продолжить нечем.
///
/// HRESULT — в самом тексте, как у остановок вёрстки: для кодов Packaging API
/// (FACILITY_OPC) слов у системы нет — за кодом в what() идёт «unknown error»,
/// — и без кода не узнать, что отказало. Слова эти — честный ответ системы, и
/// показываются они, как у прочих сбоев без слов (reasonOf у bukvitsa.lib).
void check(HRESULT hr, const char* what) {
    if (FAILED(hr))
        throw std::system_error(hr, std::system_category(),
                                std::format("{}, HRESULT 0x{:08X}", what, static_cast<std::uint32_t>(hr)));
}

/// Отказ, который значит «такой части или связи в пакете нет», — ответ о
/// файле, а не сбой: связь вправе вести к части, которой нет, а Id — не
/// найтись. Тогда false и пустой ответ вызывающему; всякий иной отказ — check.
bool present(HRESULT hr, const char* what) {
    if (hr == OPC_E_NO_SUCH_PART || hr == OPC_E_NO_SUCH_RELATIONSHIP) return false;
    check(hr, what);
    return true;
}

/// COM нужен потоку, а не пакету: приложение обычно уже его подняло
/// (CompositionHost делает это первым делом), но консольный тест — нет.
/// Повторная инициализация в том же режиме безвредна, чужой режим — не наша
/// забота, лишь бы COM работал.
void ensureComInitialized() {
    static thread_local bool done = false;
    if (done) return;
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
        check(hr, "CoInitializeEx");
    done = true;
}

}  // namespace

struct OpcPackage::Impl {
    ComPtr<IOpcFactory> factory;
    ComPtr<IOpcPackage> package;
    ComPtr<IOpcPartSet> parts;

    /// Часть по её адресу: общий хвост всех разрешений связей. Пусто — части
    /// с таким именем в пакете нет.
    std::optional<PackagePart> partAt(IOpcPartUri* uri) const {
        BOOL exists = FALSE;
        check(parts->PartExists(uri, &exists), "OPC: не узнать, есть ли часть");
        if (!exists) return std::nullopt;

        // Часть есть — так ответил PartExists, и отказ GetPart после этого уже
        // не «нет», а сбой, даже с кодом OPC_E_NO_SUCH_PART.
        ComPtr<IOpcPart> part;
        check(parts->GetPart(uri, part.put()), "OPC: часть не открывается");

        CoString contentType;
        check(part->GetContentType(contentType.put()), "OPC: у части не узнать тип содержимого");

        BStr name;
        check(uri->GetRawUri(name.put()), "OPC: у части не узнать имя");

        // Пакет пишет Windows, а не мы, и обещать за неё правильный UTF-16
        // нечего: repaired() ставит U+FFFD там, где обещать было бы нельзя.
        // По спецификации это в любом случае ASCII.
        return PackagePart{name.str(), wxl::core::unicode::repaired(contentType.view()).to_utf8()};
    }

    /// Часть, на которую ведёт связь: цель связи задана относительно её
    /// источника, поэтому адрес всегда собирается от источника, а не от корня.
    /// Пусто — связь ведёт за пределы пакета или к части, которой в нём нет.
    std::optional<PackagePart> resolve(IOpcRelationship* relationship) const {
        // Внешняя цель (адрес в сети, файл рядом) — законная связь OPC, но не
        // часть пакета. Отличить её надо до CombinePartUri: на абсолютном
        // адресе тот отказывает так же, как на испорченной цели.
        OPC_URI_TARGET_MODE mode = OPC_URI_TARGET_MODE_INTERNAL;
        check(relationship->GetTargetMode(&mode), "OPC: у связи не узнать, внутрь ли пакета она ведёт");
        if (mode != OPC_URI_TARGET_MODE_INTERNAL) return std::nullopt;

        ComPtr<IOpcUri> source;
        check(relationship->GetSourceUri(source.put()), "OPC: у связи не узнать источник");

        ComPtr<IUri> target;
        check(relationship->GetTargetUri(target.put()), "OPC: у связи не узнать цель");

        // Имя части из цели собирает сам Packaging API. Отказ — испорченная
        // цель: у абсолютного адреса внутренней связи имени части нет
        // (OPC_E_RELATIVE_URI_REQUIRED), и это отказ разбора. Цель с косой
        // чертой в конце он не отвергает, а собирает без неё («img/» от
        // /fb3/body.xml — /fb3/img): такого имени среди частей нет, и это
        // «части нет» у partAt, как у всякой связи к отсутствующей части.
        ComPtr<IOpcPartUri> combined;
        check(source->CombinePartUri(target.get(), combined.put()), "OPC: цель связи — не имя части");

        return partAt(combined.get());
    }

    /// Набор связей части либо самого пакета, когда part пуст. Пусто — такой
    /// части в пакете нет, а значит, нет и её связей.
    ComPtr<IOpcRelationshipSet> relationshipsOf(const PackagePart* part) const {
        ComPtr<IOpcRelationshipSet> set;

        // Файл связей разбирается при первом обращении, и испорченный — не
        // XML, повтор Id, негодная цель — отказывает здесь, в перечислении или
        // в разрешении связи, а не при открытии пакета. Пустой набор вместо
        // отказа выдал бы такую книгу за книгу без картинок или без связи Book.
        if (part == nullptr) {
            check(package->GetRelationshipSet(set.put()), "OPC: связи пакета не читаются");
            return set;
        }

        // Имя части — то, что отдал сам Packaging API (partAt), и отказ
        // принять его обратно — не ответ о файле.
        ComPtr<IOpcPartUri> uri;
        check(factory->CreatePartUri(part->name.c_str(), uri.put()), "OPC: имя части не принято");

        ComPtr<IOpcPart> opcPart;
        if (!present(parts->GetPart(uri.get(), opcPart.put()), "OPC: часть не открывается"))
            return set;

        check(opcPart->GetRelationshipSet(set.put()), "OPC: связи части не читаются");
        return set;
    }
};

OpcPackage::OpcPackage(const std::filesystem::path& path) : impl_(std::make_unique<Impl>()) {
    ensureComInitialized();

    check(CoCreateInstance(__uuidof(OpcFactory), nullptr, CLSCTX_INPROC_SERVER,
                           __uuidof(IOpcFactory), impl_->factory.putVoid()),
          "CoCreateInstance(OpcFactory)");

    ComPtr<IStream> stream;
    check(impl_->factory->CreateStreamOnFile(path.c_str(), OPC_STREAM_IO_READ, nullptr, 0, stream.put()),
          "OPC: файл не открывается");

    // Умолчание, а не OPC_CACHE_ON_ACCESS: тот при первом чтении части пишет
    // её разжатой во временный файл, а разбор идёт в потоке окна — диск там,
    // где его быть не должно, — и выгоды нет: часть читается один раз.
    check(impl_->factory->ReadPackageFromStream(stream.get(), OPC_READ_DEFAULT, impl_->package.put()),
          "OPC: это не пакет OPC");

    check(impl_->package->GetPartSet(impl_->parts.put()), "OPC: у пакета нет частей");
}

OpcPackage::OpcPackage(std::span<const std::byte> bytes) : impl_(std::make_unique<Impl>()) {
    ensureComInitialized();

    check(CoCreateInstance(__uuidof(OpcFactory), nullptr, CLSCTX_INPROC_SERVER,
                           __uuidof(IOpcFactory), impl_->factory.putVoid()),
          "CoCreateInstance(OpcFactory)");

    // Считаем от единицы, и первым владельцем становится ComPtr: дальше
    // пакет добавит свою ссылку и будет держать поток столько, сколько
    // читает части.
    ComPtr<IStream> stream;
    *stream.put() = new MemoryStream(bytes);

    // Умолчание, а не OPC_CACHE_ON_ACCESS, — как у конструктора из пути.
    check(impl_->factory->ReadPackageFromStream(stream.get(), OPC_READ_DEFAULT, impl_->package.put()),
          "OPC: это не пакет OPC");

    check(impl_->package->GetPartSet(impl_->parts.put()), "OPC: у пакета нет частей");
}

OpcPackage::~OpcPackage() = default;

OpcPackage::OpcPackage(OpcPackage&&) noexcept = default;

OpcPackage& OpcPackage::operator=(OpcPackage&&) noexcept = default;

std::optional<PackagePart> OpcPackage::partByPackageRelationship(std::wstring_view relationshipType) const {
    return partByRelationship(PackagePart{}, relationshipType);
}

std::optional<PackagePart> OpcPackage::partByRelationship(const PackagePart& source,
                                                          std::wstring_view relationshipType) const {
    ComPtr<IOpcRelationshipSet> set = impl_->relationshipsOf(source.name.empty() ? nullptr : &source);
    if (!set) return std::nullopt;

    ComPtr<IOpcRelationshipEnumerator> enumerator;
    check(set->GetEnumeratorForType(std::wstring(relationshipType).c_str(), enumerator.put()),
          "OPC: связи не перечисляются");

    for (;;) {
        BOOL has = FALSE;
        check(enumerator->MoveNext(&has), "OPC: связи не перечисляются");
        if (!has) break;

        ComPtr<IOpcRelationship> relationship;
        check(enumerator->GetCurrent(relationship.put()), "OPC: связь не читается");

        if (auto part = impl_->resolve(relationship.get()))
            return part;
    }

    return std::nullopt;
}

std::optional<PackagePart> OpcPackage::partByRelationshipId(const PackagePart& source,
                                                            wxl::core::u8_view relationshipId) const {
    ComPtr<IOpcRelationshipSet> set = impl_->relationshipsOf(source.name.empty() ? nullptr : &source);
    if (!set) return std::nullopt;

    ComPtr<IOpcRelationship> relationship;
    if (!present(set->GetRelationship(relationshipId.to_utf16().c_str(), relationship.put()),
               "OPC: связь не читается"))
        return std::nullopt;

    return impl_->resolve(relationship.get());
}

bool OpcPackage::hasRelationship(const PackagePart& source, std::wstring_view relationshipType) const {
    ComPtr<IOpcRelationshipSet> set = impl_->relationshipsOf(source.name.empty() ? nullptr : &source);
    if (!set) return false;

    ComPtr<IOpcRelationshipEnumerator> enumerator;
    check(set->GetEnumeratorForType(std::wstring(relationshipType).c_str(), enumerator.put()),
          "OPC: связи не перечисляются");

    BOOL has = FALSE;
    check(enumerator->MoveNext(&has), "OPC: связи не перечисляются");
    return has != FALSE;
}

std::vector<std::pair<wxl::core::u8_text, PackagePart>> OpcPackage::relationshipsOfType(
    const PackagePart& source, std::wstring_view relationshipType) const {
    std::vector<std::pair<wxl::core::u8_text, PackagePart>> found;

    ComPtr<IOpcRelationshipSet> set = impl_->relationshipsOf(source.name.empty() ? nullptr : &source);
    if (!set) return found;

    ComPtr<IOpcRelationshipEnumerator> enumerator;
    check(set->GetEnumeratorForType(std::wstring(relationshipType).c_str(), enumerator.put()),
          "OPC: связи не перечисляются");

    for (;;) {
        BOOL has = FALSE;
        check(enumerator->MoveNext(&has), "OPC: связи не перечисляются");
        if (!has) break;

        ComPtr<IOpcRelationship> relationship;
        check(enumerator->GetCurrent(relationship.put()), "OPC: связь не читается");

        CoString id;
        check(relationship->GetId(id.put()), "OPC: у связи не узнать Id");

        if (auto part = impl_->resolve(relationship.get()))
            found.emplace_back(wxl::core::unicode::repaired(id.view()).to_utf8(), std::move(*part));
    }

    return found;
}

bool OpcPackage::samePart(const PackagePart& first, const PackagePart& second) const {
    // Имена пришли из GetRawUri адресов, которые собрал сам Packaging API, —
    // отказ принять их обратно не ответ о файле, а сбой, и он не прячется.
    ComPtr<IOpcPartUri> firstUri;
    check(impl_->factory->CreatePartUri(first.name.c_str(), firstUri.put()), "OPC: имя части не принято");
    ComPtr<IOpcPartUri> secondUri;
    check(impl_->factory->CreatePartUri(second.name.c_str(), secondUri.put()), "OPC: имя части не принято");

    INT32 order = 0;
    check(firstUri->ComparePartUri(secondUri.get(), &order), "OPC: имена частей не сравнились");
    return order == 0;
}

namespace {

/// Больше этого часть не может быть книгой. Позиции символов книги
/// (Node::charOffset, Measures::codePoints) — uint32_t, а кодовых точек в части
/// не больше, чем в ней байтов UTF-8. Длин того, что из части развернут потом,
/// предел не обещает: их стережёт тот, кто разворачивает. Тот же предел и
/// картинке: длину её байтов декодеры берут 32-битной.
constexpr std::uint64_t kMaxPartBytes = std::numeric_limits<std::uint32_t>::max();

/// Сколько памяти часть получает сразу, по слову заголовка: тело большого
/// романа — единицы мегабайт и в резерв ложится целиком, а больше — растёт
/// удвоением, за те же байты, что разжаты на деле.
constexpr std::uint64_t kMaxPartReserve = 64ull * 1024 * 1024;

/// Отказ тем же путём, что и прочие отказы разбора: исключение с причиной.
[[noreturn]] void refuseTooLarge(const PackagePart& part) {
    throw std::runtime_error(
        std::format("FB3: часть {} разжимается в 4 ГиБ и больше — книгой она быть не может",
                    wxl::core::unicode::repaired(part.name).to_utf8().chars()));
}

}  // namespace

std::string OpcPackage::readPart(const PackagePart& part) const {
    ComPtr<IOpcPartUri> uri;
    check(impl_->factory->CreatePartUri(part.name.c_str(), uri.put()), "OPC: неверное имя части");

    ComPtr<IOpcPart> opcPart;
    check(impl_->parts->GetPart(uri.get(), opcPart.put()), "OPC: часть не найдена");

    ComPtr<IStream> stream;
    check(opcPart->GetContentStream(stream.put()), "OPC: часть не читается");

    std::string content;

    // Размер известен заранее, но полагаться на него нельзя: часть в пакете
    // сжата, и Read всё равно отдаёт столько, сколько распаковал за раз. Да и
    // пишет его заголовок архива — тот же файл: больше предела — отказ сразу,
    // но предел стережёт то, что разжато на деле, а не то, что обещано. Ради
    // памяти заголовку тоже не верим: резерв — не больше kMaxPartReserve, а
    // дальше строка растёт сама, удвоением, — и часть, что обещает 4 ГиБ, а
    // разжимается в десять байт, 4 ГиБ не стоит.
    STATSTG stat{};
    check(stream->Stat(&stat, STATFLAG_NONAME), "OPC: у части не узнать размер");
    if (stat.cbSize.QuadPart > kMaxPartBytes) refuseTooLarge(part);
    content.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(stat.cbSize.QuadPart, kMaxPartReserve)));

    char buffer[64 * 1024];
    for (;;) {
        ULONG read = 0;
        const HRESULT hr = stream->Read(buffer, sizeof(buffer), &read);
        if (FAILED(hr))
            check(hr, "OPC: часть не дочитана");
        if (read == 0)
            break;
        if (read > kMaxPartBytes - content.size()) refuseTooLarge(part);
        content.append(buffer, read);
    }

    return content;
}

}  // namespace bukvitsa::fb3
