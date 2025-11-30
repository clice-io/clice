#pragma once

#include <format>
#include <print>

#include "Support/JSON.h"
#include "Support/Ranges.h"

#include "llvm/Support/Error.h"

template <>
struct std::formatter<llvm::StringRef> : std::formatter<std::string_view> {
    using Base = std::formatter<std::string_view>;

    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx) {
        return Base::parse(ctx);
    }

    template <typename FormatContext>
    auto format(llvm::StringRef s, FormatContext& ctx) const {
        return Base::format(std::string_view(s.str()), ctx);
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
    auto format(const llvm::SmallString<N>& s, FormatContext& ctx) const {
        return Base::format(llvm::StringRef(s), ctx);
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
    auto format(const llvm::Error& e, FormatContext& ctx) const {
        llvm::SmallString<128> buffer;
        llvm::raw_svector_ostream os(buffer);
        os << e;
        return Base::format(buffer, ctx);
    }
};

template <>
struct std::formatter<std::error_code> : std::formatter<llvm::StringRef> {
    using Base = std::formatter<llvm::StringRef>;

    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx) {
        return Base::parse(ctx);
    }

    template <typename FormatContext>
    auto format(const std::error_code& e, FormatContext& ctx) const {
        return Base::format(e.message(), ctx);
    }
};

template <>
struct std::formatter<clice::json::Value> : std::formatter<llvm::StringRef> {
    using Base = std::formatter<llvm::StringRef>;

    int indent = 0;

    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx) {
        auto it = ctx.begin();
        auto end = ctx.end();
        if(it == end) {
            return it;
        }

        int parsed_indent = 0;
        while(it != end && *it >= '0' && *it <= '9') {
            parsed_indent = parsed_indent * 10 + (*it - '0');
            ++it;
        }
        indent = parsed_indent;

        return it;
    }

    template <typename FormatContext>
    auto format(const clice::json::Value& value, FormatContext& ctx) const {
        llvm::SmallString<128> buffer;
        llvm::raw_svector_ostream os{buffer};
        llvm::json::OStream(os, indent).value(value);
        return Base::format(buffer, ctx);
    }
};

template <clice::refl::reflectable_enum E>
struct std::formatter<E> : std::formatter<std::string_view> {
    using Base = std::formatter<std::string_view>;

    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx) {
        return Base::parse(ctx);
    }

    template <typename FormatContext>
    auto format(const E& e, FormatContext& ctx) const {
        return Base::format(e.name(), ctx);
    }
};

namespace clice {

/// Dump object to string for debugging. Note that it is not efficient
/// and should not be used except for debugging.
template <typename Object>
std::string dump(const Object& object) {
    if constexpr(std::is_fundamental_v<Object>) {
        return std::format("{}", object);
    } else if constexpr(std::is_same_v<Object, std::string> ||
                        std::is_same_v<Object, std::string_view> ||
                        std::is_same_v<Object, llvm::StringRef>) {
        return std::format("\"{}\"", object);
    } else if constexpr(ranges::range<Object>) {
        constexpr bool is_sequence = sequence_range<Object>;
        std::string result = is_sequence ? "[" : "{";
        if constexpr(map_range<Object>) {
            for(auto&& [key, value]: object) {
                result += std::format("\"{}\": {}, ", dump(key), dump(value));
            }
        } else {
            for(auto&& value: object) {
                result += std::format("{}, ", dump(value));
            }
        }
        if(!object.empty()) {
            result.pop_back();
            result.pop_back();
        }
        result += is_sequence ? "]" : "}";
        return result;
    } else if constexpr(std::is_enum_v<Object>) {
        return std::format("\"{}\"", refl::enum_name(object));
    } else if constexpr(refl::reflectable_enum<Object>) {
        return std::format("\"{}\"", object);
    } else if constexpr(refl::reflectable_struct<Object>) {
        std::string result = "{";
        refl::foreach(object, [&](auto name, auto value) {
            result += std::format("\"{}\": {}, ", name, dump(value));
        });
        if(refl::member_count<Object>() != 0) {
            result.pop_back();
            result.pop_back();
        }
        result += "}";
        return result;
    } else {
        static_assert(dependent_false<Object>, "Cannot dump object");
    }
}

template <typename Object>
std::string pretty_dump(const Object& object, std::size_t indent = 2) {
    std::string repr = dump(object);
    auto json = json::parse(repr);
    if(!json) {
        std::println("{} {}", json.takeError(), repr);
        std::abort();
    }
    llvm::SmallString<128> buffer = {std::format("{{0:{}}}", indent)};
    return llvm::formatv(buffer.c_str(), *json);
}

}  // namespace clice
