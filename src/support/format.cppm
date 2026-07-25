module;

export module clice.support:format;

import stdlib;
import llvm;
import kota;

export namespace clice {

template <typename Object>
std::string dump(const Object& object);

/// First-party replacement for std::format in module TUs. Calling std::format
/// makes overload resolution complete the wide-char basic_format_string; on
/// clang 22 the module serializer demotes its member definitions and libc++
/// hard-errors the re-instantiation at every call site (llvm#174858, fixed in
/// clang 23 by PR#184287 — collapse this back to std::format then). vformat's
/// overloads take concrete parameter types, so nothing wide is ever completed;
/// the format_string parameter keeps the compile-time format check.
template <typename... Args>
[[nodiscard]] std::string format(std::format_string<Args...> fmt, Args&&... args) {
    return std::vformat(fmt.get(), std::make_format_args(args...));
}

}  // namespace clice

// The std::formatter specializations below live in the module purview, OUTSIDE
// any export block: specializations are found by reachability in importers, and
// `export` on an explicit specialization is ill-formed. This mirrors the
// third-party adapter pattern in deps/clang.cppm (std::tuple_size<SourceRange>).
namespace std {

template <>
struct formatter<llvm::StringRef> : formatter<std::string_view> {
    using Base = formatter<std::string_view>;

    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx) {
        return Base::parse(ctx);
    }

    template <typename FormatContext>
    auto format(llvm::StringRef value, FormatContext& ctx) const {
        return Base::format(std::string_view(value.data(), value.size()), ctx);
    }
};

template <std::size_t N>
struct formatter<llvm::SmallString<N>> : formatter<llvm::StringRef> {
    using Base = formatter<llvm::StringRef>;

    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx) {
        return Base::parse(ctx);
    }

    template <typename FormatContext>
    auto format(const llvm::SmallString<N>& value, FormatContext& ctx) const {
        return Base::format(llvm::StringRef(value), ctx);
    }
};

template <>
struct formatter<llvm::Error> : formatter<llvm::StringRef> {
    using Base = formatter<llvm::StringRef>;

    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx) {
        return Base::parse(ctx);
    }

    template <typename FormatContext>
    auto format(const llvm::Error& value, FormatContext& ctx) const {
        llvm::SmallString<128> buffer;
        llvm::raw_svector_ostream os(buffer);
        os << value;
        return Base::format(llvm::StringRef(buffer), ctx);
    }
};

template <>
struct formatter<std::error_code> : formatter<std::string_view> {
    using Base = formatter<std::string_view>;

    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx) {
        return Base::parse(ctx);
    }

    template <typename FormatContext>
    auto format(const std::error_code& value, FormatContext& ctx) const {
        return Base::format(value.message(), ctx);
    }
};

template <kota::meta::enum_type E>
struct formatter<E> : formatter<std::string> {
    using Base = formatter<std::string>;

    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx) {
        return Base::parse(ctx);
    }

    template <typename FormatContext>
    auto format(const E& value, FormatContext& ctx) const {
        auto name = kota::meta::enum_name(value);
        if(name.empty()) {
            using U = std::underlying_type_t<E>;
            return Base::format(clice::format("{}", static_cast<U>(value)), ctx);
        }
        return Base::format(std::string(name), ctx);
    }
};

}  // namespace std

// Named helper (public gate for the reflective struct formatter below), so it
// is exported for importers that reference it directly.
export template <typename T>
concept clice_reflectable_class = kota::meta::reflectable_class<T> && !kota::sequence_range<T> &&
                                  !kota::set_range<T> && !kota::map_range<T>;

namespace std {

template <clice_reflectable_class T>
struct formatter<T> : formatter<std::string> {
    using Base = formatter<std::string>;

    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx) {
        return Base::parse(ctx);
    }

    template <typename FormatContext>
    auto format(const T& value, FormatContext& ctx) const {
        return Base::format(clice::dump(value), ctx);
    }
};

}  // namespace std

export namespace clice {

template <typename Object>
std::string dump(const Object& object) {
    using T = std::remove_cvref_t<Object>;

    if constexpr(std::is_same_v<T, std::string>) {
        return clice::format("\"{}\"", object);
    } else if constexpr(std::is_same_v<T, std::string_view>) {
        return clice::format("\"{}\"", object);
    } else if constexpr(std::is_same_v<T, llvm::StringRef>) {
        return clice::format("\"{}\"", object);
    } else if constexpr(kota::map_range<T>) {
        std::string result = "{";
        bool first = true;
        for(auto&& [key, value]: object) {
            if(!first) {
                result += ", ";
            }
            first = false;
            result += clice::format("{}: {}", dump(key), dump(value));
        }
        result += "}";
        return result;
    } else if constexpr(kota::set_range<T> || kota::sequence_range<T>) {
        std::string result = kota::set_range<T> ? "{" : "[";
        bool first = true;
        for(auto&& value: object) {
            if(!first) {
                result += ", ";
            }
            first = false;
            result += dump(value);
        }
        result += kota::set_range<T> ? "}" : "]";
        return result;
    } else if constexpr(kota::meta::enum_type<T>) {
        auto name = kota::meta::enum_name(object);
        if(!name.empty()) {
            return clice::format("\"{}\"", name);
        }
        using U = std::underlying_type_t<T>;
        return clice::format("{}", static_cast<U>(object));
    } else if constexpr(clice_reflectable_class<T>) {
        std::string result = "{";
        bool first = true;
        kota::meta::for_each(object, [&](auto field) {
            if(!first) {
                result += ", ";
            }
            first = false;
            result += clice::format("\"{}\": {}", decltype(field)::name(), dump(field.value()));
        });
        result += "}";
        return result;
    } else if constexpr(kota::Formattable<T>) {
        return clice::format("{}", object);
    } else {
        return "<unformattable>";
    }
}

template <typename Object>
std::string pretty_dump(const Object& object, std::size_t /*indent*/ = 2) {
    return dump(object);
}

}  // namespace clice
