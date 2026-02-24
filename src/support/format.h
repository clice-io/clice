#pragma once

#include <format>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>

#include "eventide/common/meta.h"
#include "eventide/common/ranges.h"
#include "eventide/reflection/enum.h"
#include "eventide/reflection/struct.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"

namespace clice {

template <typename Object>
std::string dump(const Object& object);

}  // namespace clice

template <>
struct std::formatter<llvm::StringRef> : std::formatter<std::string_view> {
    using Base = std::formatter<std::string_view>;

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
struct std::formatter<llvm::SmallString<N>> : std::formatter<llvm::StringRef> {
    using Base = std::formatter<llvm::StringRef>;

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
struct std::formatter<llvm::Error> : std::formatter<llvm::StringRef> {
    using Base = std::formatter<llvm::StringRef>;

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
struct std::formatter<std::error_code> : std::formatter<std::string_view> {
    using Base = std::formatter<std::string_view>;

    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx) {
        return Base::parse(ctx);
    }

    template <typename FormatContext>
    auto format(const std::error_code& value, FormatContext& ctx) const {
        return Base::format(value.message(), ctx);
    }
};

template <eventide::refl::enum_type E>
struct std::formatter<E> : std::formatter<std::string> {
    using Base = std::formatter<std::string>;

    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx) {
        return Base::parse(ctx);
    }

    template <typename FormatContext>
    auto format(const E& value, FormatContext& ctx) const {
        auto name = eventide::refl::enum_name(value);
        if(name.empty()) {
            using U = std::underlying_type_t<E>;
            return Base::format(std::format("{}", static_cast<U>(value)), ctx);
        }
        return Base::format(std::string(name), ctx);
    }
};

template <typename T>
concept clice_reflectable_class =
    eventide::refl::reflectable_class<T> && !eventide::sequence_range<T> &&
    !eventide::set_range<T> && !eventide::map_range<T>;

template <clice_reflectable_class T>
struct std::formatter<T> : std::formatter<std::string> {
    using Base = std::formatter<std::string>;

    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx) {
        return Base::parse(ctx);
    }

    template <typename FormatContext>
    auto format(const T& value, FormatContext& ctx) const {
        return Base::format(clice::dump(value), ctx);
    }
};

namespace clice {

template <typename Object>
std::string dump(const Object& object) {
    using T = std::remove_cvref_t<Object>;

    if constexpr(std::is_same_v<T, std::string>) {
        return std::format("\"{}\"", object);
    } else if constexpr(std::is_same_v<T, std::string_view>) {
        return std::format("\"{}\"", object);
    } else if constexpr(std::is_same_v<T, llvm::StringRef>) {
        return std::format("\"{}\"", object);
    } else if constexpr(eventide::map_range<T>) {
        std::string result = "{";
        bool first = true;
        for(auto&& [key, value]: object) {
            if(!first) {
                result += ", ";
            }
            first = false;
            result += std::format("{}: {}", dump(key), dump(value));
        }
        result += "}";
        return result;
    } else if constexpr(eventide::set_range<T> || eventide::sequence_range<T>) {
        std::string result = eventide::set_range<T> ? "{" : "[";
        bool first = true;
        for(auto&& value: object) {
            if(!first) {
                result += ", ";
            }
            first = false;
            result += dump(value);
        }
        result += eventide::set_range<T> ? "}" : "]";
        return result;
    } else if constexpr(eventide::refl::enum_type<T>) {
        auto name = eventide::refl::enum_name(object);
        if(!name.empty()) {
            return std::format("\"{}\"", name);
        }
        using U = std::underlying_type_t<T>;
        return std::format("{}", static_cast<U>(object));
    } else if constexpr(clice_reflectable_class<T>) {
        std::string result = "{";
        bool first = true;
        eventide::refl::for_each(object, [&](auto field) {
            if(!first) {
                result += ", ";
            }
            first = false;
            result += std::format("\"{}\": {}", decltype(field)::name(), dump(field.value()));
        });
        result += "}";
        return result;
    } else if constexpr(eventide::Formattable<T>) {
        return std::format("{}", object);
    } else {
        return "<unformattable>";
    }
}

template <typename Object>
std::string pretty_dump(const Object& object, std::size_t /*indent*/ = 2) {
    return dump(object);
}

}  // namespace clice
