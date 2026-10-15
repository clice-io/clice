module;

#include "modules/prelude.h"

module clice:support.format;

template <>
struct std::formatter<llvm::StringRef> : std::formatter<std::string_view> {
    using Base = std::formatter<std::string_view>;

    template <typename FormatContext>
    auto format(llvm::StringRef value, FormatContext& ctx) const {
        return Base::format(std::string_view(value.data(), value.size()), ctx);
    }
};

template <>
struct std::formatter<llvm::StringLiteral> : std::formatter<llvm::StringRef> {};

template <std::size_t N>
struct std::formatter<llvm::SmallString<N>> : std::formatter<llvm::StringRef> {
    using Base = std::formatter<llvm::StringRef>;

    template <typename FormatContext>
    auto format(const llvm::SmallString<N>& value, FormatContext& ctx) const {
        return Base::format(llvm::StringRef(value), ctx);
    }
};

template <>
struct std::formatter<std::error_code> : std::formatter<std::string_view> {
    using Base = std::formatter<std::string_view>;

    template <typename FormatContext>
    auto format(const std::error_code& value, FormatContext& ctx) const {
        return Base::format(value.message(), ctx);
    }
};

template <kota::meta::enum_type E>
struct std::formatter<E> : std::formatter<std::string> {
    using Base = std::formatter<std::string>;

    template <typename FormatContext>
    auto format(const E& value, FormatContext& ctx) const {
        auto name = kota::meta::enum_name(value);
        if(name.empty()) {
            using U = std::underlying_type_t<E>;
            return Base::format(std::format("{}", static_cast<U>(value)), ctx);
        }
        return Base::format(std::string(name), ctx);
    }
};
