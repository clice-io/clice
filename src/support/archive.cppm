module;

#include "modules/prelude.h"

/// Zero-copy archives of reflected structs: a value laid out once in one
/// contiguous buffer and read in place as a C++ struct.
///
/// A struct holding strings or sequences is declared once, as a template
/// over its sequence type:
///
///     template <template <typename> typename Seq>
///     struct BlobOf {
///         std::uint32_t version = 0;
///         Seq<char> name;
///         Seq<std::uint64_t> hashes;
///     };
///     using Blob = BlobOf<archive::Owned>;
///
/// `Blob` owns its contents (std::string, std::vector): a writer fills it
/// and encode archives it. `archive::Archived<Blob>`, which is
/// `BlobOf<archive::Vec>`, is the same struct read in place, each string or
/// sequence an archive::Vec — two u32, the distance from the Vec to its
/// first element and the element count. A struct of scalars only is its own
/// archived form.
///
/// An archive is a header — a magic and the fingerprint of the root's
/// archived layout — then the archived root, then every string's bytes and
/// every sequence's elements, out of line, in pre-order: a record's fields
/// in declaration order, a sequence's elements in order, each part followed
/// by its own out-of-line parts before the next one's. Strings sit
/// unaligned, sequences at `alignment`, so a byte sequence holding another
/// archive is read in place. A scalar (an arithmetic type, an enum with a
/// fixed underlying type) is its native little-endian image.
///
/// The layout is canonical — one value has exactly one archive, padding
/// zeroed — so the bytes' hash identifies the value. verify checks
/// untrusted bytes against exactly that layout: every Vec pointing where
/// the writer puts its part, every bool 0 or 1, nothing past the last part.
/// A verified archive is then read with no check at all. There is no schema
/// evolution: the fingerprint rejects an archive of another layout, and a
/// reader never sees one.

module clice:support.archive;

namespace clice::archive {

static_assert(std::endian::native == std::endian::little, "archives are little-endian images");

/// The alignment an archive's bytes must start at, and every sequence's
/// elements start at inside it.
constexpr inline std::uint32_t alignment = 8;

/// A string (`Vec<char>`) or a sequence read in place. Its offset counts
/// from its own address, so it is never copied out of the archive: bind a
/// reference to it, or read it as an llvm::ArrayRef or llvm::StringRef —
/// naming the element type, since `llvm::ArrayRef(vec)` deduces a
/// one-element array of the Vec itself.
template <typename T>
class Vec {
public:
    Vec() = default;

    Vec(const Vec&) = delete;

    Vec& operator=(const Vec&) = delete;

    const T* data() const {
        return reinterpret_cast<const T*>(reinterpret_cast<const char*>(this) + offset);
    }

    std::uint32_t size() const {
        return length;
    }

    bool empty() const {
        return length == 0;
    }

    const T& operator[](std::size_t i) const {
        return data()[i];
    }

    const T& front() const {
        return data()[0];
    }

    const T& back() const {
        return data()[length - 1];
    }

    const T* begin() const {
        return data();
    }

    const T* end() const {
        return data() + length;
    }

    operator llvm::ArrayRef<T>() const {
        return {data(), length};
    }

    operator llvm::StringRef() const
        requires std::is_same_v<T, char>
    {
        return {data(), length};
    }

    std::string str() const
        requires std::is_same_v<T, char>
    {
        return {data(), length};
    }

private:
    std::uint32_t offset = 0;

    std::uint32_t length = 0;
};

/// The owning sequence of a struct template's writer form: std::string for
/// characters, std::vector otherwise.
template <typename T>
using Owned = std::conditional_t<std::is_same_v<T, char>, std::string, std::vector<T>>;

namespace detail {

template <typename T>
struct Archive {
    using type = T;
};

template <>
struct Archive<std::string> {
    using type = Vec<char>;
};

template <typename E>
struct Archive<std::vector<E>> {
    using type = Vec<typename Archive<E>::type>;
};

template <template <template <typename> typename> typename Of>
struct Archive<Of<Owned>> {
    using type = Of<Vec>;
};

}  // namespace detail

/// The form a T is read in place as: Vec for a string or a sequence, the
/// same struct template over Vec for a record holding them, T itself
/// otherwise.
template <typename T>
using Archived = typename detail::Archive<T>::type;

namespace detail {

constexpr inline std::uint32_t magic = 'C' | ('L' << 8) | ('A' << 16) | ('R' << 24);

constexpr inline std::uint32_t header_size = 8;

template <typename T>
constexpr bool is_vec_v = false;

template <typename E>
constexpr bool is_vec_v<Vec<E>> = true;

template <typename A>
struct Element;

template <typename E>
struct Element<Vec<E>> {
    using type = E;
};

template <typename A>
using element_t = typename Element<A>::type;

template <typename A>
constexpr bool is_string_v = false;

template <>
constexpr bool is_string_v<Vec<char>> = true;

template <typename T>
constexpr bool is_scalar_v = std::is_arithmetic_v<T> ? !std::is_same_v<T, long double>
                                                     : std::is_enum_v<T> && requires { T{0}; };

template <typename T>
constexpr std::size_t field_count_v = kota::meta::field_count<T>();

template <typename T, std::size_t I>
using field_t = std::remove_cv_t<kota::meta::field_type<T, I>>;

template <typename T, typename Visit>
constexpr void for_each_index(Visit&& visit) {
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        (visit(std::integral_constant<std::size_t, I>{}), ...);
    }(std::make_index_sequence<field_count_v<T>>{});
}

enum class Kind : std::uint8_t {
    Scalar,
    Vec,
    Record,
};

/// The kind of an archived type A.
template <typename A>
consteval Kind kind_of() {
    if constexpr(is_scalar_v<A>) {
        static_assert(alignof(A) <= alignment, "a sequence aligns its elements to 8 bytes at most");
        return Kind::Scalar;
    } else if constexpr(is_vec_v<A>) {
        return Kind::Vec;
    } else if constexpr(std::is_class_v<A> && std::is_aggregate_v<A>) {
        static_assert(std::is_empty_v<A> || field_count_v<A> > 0,
                      "reflection sees none of this struct's fields");
        static_assert(std::is_standard_layout_v<A>, "an archived record is standard-layout");
        return Kind::Record;
    } else {
        static_assert(sizeof(A) == 0,
                      "an archive holds arithmetic types, enums with a fixed underlying type, "
                      "Vec and aggregates of those; a record holding strings or sequences is "
                      "a struct template over its sequence type");
    }
}

/// Whether A holds no Vec, so a writer copies it as it is.
template <typename A>
consteval bool is_plain() {
    if constexpr(kind_of<A>() == Kind::Record) {
        bool plain = true;
        for_each_index<A>([&](auto index) { plain = plain && is_plain<field_t<A, index()>>(); });
        return plain;
    } else {
        return kind_of<A>() == Kind::Scalar;
    }
}

/// Whether A is plain and its image has no padding, so a sequence of them
/// copies out whole.
template <typename A>
consteval bool is_packed() {
    if constexpr(!is_plain<A>()) {
        return false;
    } else if constexpr(kind_of<A>() == Kind::Scalar) {
        return true;
    } else {
        std::size_t fields = 0;
        bool packed = true;
        for_each_index<A>([&](auto index) {
            using F = field_t<A, index()>;
            fields += sizeof(F);
            packed = packed && is_packed<F>();
        });
        return packed && fields == sizeof(A);
    }
}

/// Whether reading A in place needs a check beyond its bounds: a bool
/// admits two bytes of 256, and a Vec points somewhere to place.
template <typename A>
consteval bool needs_check() {
    if constexpr(kind_of<A>() == Kind::Scalar) {
        return std::is_same_v<A, bool>;
    } else if constexpr(kind_of<A>() == Kind::Vec) {
        return true;
    } else {
        bool check = false;
        for_each_index<A>([&](auto index) { check = check || needs_check<field_t<A, index()>>(); });
        return check;
    }
}

consteval std::uint32_t mix(std::uint32_t hash, std::string_view text) {
    for(auto c: text) {
        hash = (hash ^ static_cast<std::uint8_t>(c)) * 16777619u;
    }
    return hash;
}

/// A hash of A's layout: its kinds, sizes and field names.
template <typename A>
consteval std::uint32_t fingerprint(std::uint32_t hash = 2166136261u) {
    if constexpr(kind_of<A>() == Kind::Scalar) {
        using U = typename std::
            conditional_t<std::is_enum_v<A>, std::underlying_type<A>, std::type_identity<A>>::type;
        char code[] = {std::is_same_v<U, bool>       ? 'b'
                       : std::is_floating_point_v<U> ? 'f'
                       : std::is_signed_v<U>         ? 'i'
                                                     : 'u',
                       static_cast<char>('0' + sizeof(U))};
        return mix(hash, {code, sizeof(code)});
    } else if constexpr(is_string_v<A>) {
        return mix(hash, "s");
    } else if constexpr(kind_of<A>() == Kind::Vec) {
        return mix(fingerprint<element_t<A>>(mix(hash, "[")), "]");
    } else {
        hash = mix(hash, "{");
        for_each_index<A>([&](auto index) {
            hash = mix(hash, kota::meta::field_name<index(), A>());
            hash = fingerprint<field_t<A, index()>>(mix(hash, ":"));
            hash = mix(hash, ";");
        });
        return mix(hash, "}");
    }
}

constexpr std::uint64_t align_up(std::uint64_t at, std::uint32_t align) {
    return (at + align - 1) / align * align;
}

class Writer {
public:
    /// Room for `size` zeroed bytes at the next multiple of `align`.
    std::uint32_t reserve(std::uint64_t size, std::uint32_t align) {
        auto at = align_up(bytes.size(), align);
        assert(at + size <= std::numeric_limits<std::uint32_t>::max() &&
               "an archive stays below 4 GiB");
        bytes.resize(at + size);
        return static_cast<std::uint32_t>(at);
    }

    void write(std::uint32_t at, const void* data, std::size_t size) {
        std::memcpy(bytes.data() + at, data, size);
    }

    /// Write `value` as the archived A at `at`, inside room already
    /// reserved, and append its out-of-line parts.
    template <typename A, typename T>
    void store(std::uint32_t at, const T& value) {
        if constexpr(kind_of<A>() == Kind::Scalar) {
            static_assert(std::is_same_v<A, T>, "a scalar archives as itself");
            write(at, &value, sizeof(A));
        } else if constexpr(kind_of<A>() == Kind::Record) {
            static_assert(field_count_v<A> == field_count_v<T>, "a record archives field by field");
            for_each_index<A>([&](auto index) {
                store<field_t<A, index()>>(at + kota::meta::field_offset<A>(index()),
                                           kota::meta::field_of<index()>(value));
            });
        } else if constexpr(is_string_v<A>) {
            llvm::StringRef text(value);
            if(text.empty()) {
                return;
            }
            auto data = reserve(text.size(), 1);
            write(data, text.data(), text.size());
            write_vec(at, data, text.size());
        } else {
            using E = element_t<A>;
            auto count = std::ranges::size(value);
            if(count == 0) {
                return;
            }
            auto data = reserve(std::uint64_t(count) * sizeof(E), alignment);
            write_vec(at, data, count);
            if constexpr(is_packed<E>()) {
                static_assert(std::is_same_v<std::ranges::range_value_t<const T>, E>,
                              "a plain element archives as itself");
                write(data, std::ranges::data(value), count * sizeof(E));
            } else {
                auto next = data;
                for(auto& element: value) {
                    store<E>(next, element);
                    next += sizeof(E);
                }
            }
        }
    }

    std::string bytes;

private:
    void write_vec(std::uint32_t at, std::uint32_t data, std::uint64_t length) {
        std::uint32_t pair[] = {data - at, static_cast<std::uint32_t>(length)};
        write(at, pair, sizeof(pair));
    }
};

class Checker {
public:
    explicit Checker(llvm::StringRef bytes) : base(bytes.data()), size(bytes.size()) {}

    /// Check the archived A at `at`, inside bytes already placed, and place
    /// its out-of-line parts.
    template <typename A>
    bool check(std::uint64_t at) {
        if constexpr(!needs_check<A>()) {
            return true;
        } else if constexpr(kind_of<A>() == Kind::Scalar) {
            if(static_cast<std::uint8_t>(base[at]) > 1) {
                error = "a bool is neither 0 nor 1";
                return false;
            }
            return true;
        } else if constexpr(kind_of<A>() == Kind::Record) {
            bool ok = true;
            for_each_index<A>([&](auto index) {
                ok = ok && check<field_t<A, index()>>(at + kota::meta::field_offset<A>(index()));
            });
            return ok;
        } else {
            using E = element_t<A>;
            auto placed = is_string_v<A> ? place(at, 1, 1) : place(at, sizeof(E), alignment);
            if(!placed) {
                return false;
            }
            if constexpr(needs_check<E>()) {
                for(std::uint64_t i = 0; i < placed->length; i += 1) {
                    if(!check<E>(placed->at + i * sizeof(E))) {
                        return false;
                    }
                }
            }
            return true;
        }
    }

    const char* base;

    std::uint64_t size;

    /// Where the next out-of-line part begins, give or take its alignment.
    std::uint64_t cursor = 0;

    llvm::StringRef error;

private:
    struct Placed {
        std::uint64_t at;
        std::uint64_t length;
    };

    /// The part the Vec at `at` points to, of elements of `element` bytes:
    /// where the writer puts it, inside the bytes.
    std::optional<Placed> place(std::uint64_t at, std::uint32_t element, std::uint32_t align) {
        std::uint32_t pair[2];
        std::memcpy(pair, base + at, sizeof(pair));
        auto [offset, length] = pair;
        if(length == 0) {
            if(offset != 0) {
                error = "an empty part has an offset";
                return std::nullopt;
            }
            return Placed{0, 0};
        }
        auto expected = align_up(cursor, align);
        auto end = expected + std::uint64_t(length) * element;
        if(at + offset != expected || end > size) {
            error = "a part is not where the layout puts it";
            return std::nullopt;
        }
        cursor = end;
        return Placed{expected, length};
    }
};

}  // namespace detail

/// Archive `value`, a record.
template <typename T>
std::string encode(const T& value) {
    using A = Archived<T>;
    static_assert(detail::kind_of<A>() == detail::Kind::Record, "an archive's root is a record");
    detail::Writer writer;
    writer.reserve(detail::header_size, alignment);
    constexpr std::uint32_t header[] = {detail::magic, detail::fingerprint<A>()};
    writer.write(0, header, sizeof(header));
    writer.store<A>(writer.reserve(sizeof(A), alignof(A)), value);
    return std::move(writer.bytes);
}

/// The archived root of bytes holding an archive of T in exactly its
/// layout, which then reads in place with no further check; the reason
/// otherwise. The bytes must start at `alignment`.
template <typename T>
std::expected<const Archived<T>*, llvm::StringRef> verify(llvm::StringRef bytes) {
    using A = Archived<T>;
    static_assert(detail::kind_of<A>() == detail::Kind::Record, "an archive's root is a record");
    if(reinterpret_cast<std::uintptr_t>(bytes.data()) % alignment != 0) {
        return std::unexpected("the bytes are not aligned");
    }
    if(bytes.size() < detail::header_size + sizeof(A) ||
       bytes.size() > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected("the bytes cannot hold an archive of this type");
    }
    std::uint32_t header[2];
    std::memcpy(header, bytes.data(), sizeof(header));
    if(header[0] != detail::magic || header[1] != detail::fingerprint<A>()) {
        return std::unexpected("not an archive of this type");
    }
    detail::Checker checker(bytes);
    checker.cursor = detail::header_size + sizeof(A);
    if(!checker.check<A>(detail::header_size)) {
        return std::unexpected(checker.error);
    }
    if(checker.cursor != bytes.size()) {
        return std::unexpected("bytes past the archive's last part");
    }
    return reinterpret_cast<const A*>(bytes.data() + detail::header_size);
}

/// The archived root of bytes verify accepted.
template <typename T>
const Archived<T>& view(llvm::StringRef bytes) {
    return *reinterpret_cast<const Archived<T>*>(bytes.data() + detail::header_size);
}

/// Copy an archived value out into `out`, its writer form.
template <typename T>
void decode(const Archived<T>& in, T& out) {
    using A = Archived<T>;
    if constexpr(detail::is_plain<A>()) {
        out = in;
    } else if constexpr(detail::kind_of<A>() == detail::Kind::Record) {
        detail::for_each_index<A>([&](auto index) {
            decode(kota::meta::field_of<index()>(in), kota::meta::field_of<index()>(out));
        });
    } else if constexpr(detail::is_string_v<A>) {
        out.assign(in.data(), in.size());
    } else if constexpr(detail::is_plain<detail::element_t<A>>()) {
        out.assign(in.begin(), in.end());
    } else {
        out.clear();
        out.reserve(in.size());
        for(auto& element: in) {
            decode(element, out.emplace_back());
        }
    }
}

/// Visit each out-of-line part of the archived `in` with the path of the
/// field holding it (`ranges.packed`; a sequence's elements count under the
/// sequence) and its bytes — what a field costs beyond the inline parts and
/// the padding.
template <typename A>
void measure(const A& in,
             llvm::function_ref<void(llvm::StringRef path, std::uint64_t bytes)> visit,
             llvm::StringRef prefix = {}) {
    detail::for_each_index<A>([&](auto index) {
        using F = detail::field_t<A, index()>;
        if constexpr(!detail::is_plain<F>()) {
            auto path =
                std::format("{}{}", std::string_view(prefix), kota::meta::field_name<index(), A>());
            auto& field = kota::meta::field_of<index()>(in);
            if constexpr(detail::kind_of<F>() == detail::Kind::Record) {
                measure(field, visit, path + ".");
            } else {
                using E = detail::element_t<F>;
                visit(path, std::uint64_t(field.size()) * sizeof(E));
                if constexpr(detail::kind_of<E>() == detail::Kind::Vec) {
                    for(auto& inner: field) {
                        visit(path, std::uint64_t(inner.size()) * sizeof(detail::element_t<E>));
                    }
                } else if constexpr(detail::kind_of<E>() == detail::Kind::Record &&
                                    !detail::is_plain<E>()) {
                    for(auto& element: field) {
                        measure(element, visit, path + ".");
                    }
                }
            }
        }
    });
}

}  // namespace clice::archive
