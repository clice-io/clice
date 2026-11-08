/// What every unit of clice reads: the standard library, the C library's
/// names past it and the third-party module, imported for clice's own units
/// alone, then the short names clice gives the namespaces it writes, and
/// how std::format writes LLVM's strings, error codes and clice's enums.
/// Their macros come with support/macros.h.
export module clice:support.prelude;

import std.compat;
import libc;
import third_party;

namespace clice {

namespace ranges = std::ranges;
namespace views = std::views;
namespace chrono = std::chrono;
namespace this_thread = std::this_thread;

namespace ipc = kotatsu::ipc;
namespace lsp = kotatsu::ipc::lsp;
namespace protocol = kotatsu::ipc::protocol;

namespace codec = kotatsu::codec;
namespace json = kotatsu::codec::json;

namespace fs = kotatsu::fs;
namespace sys = kotatsu::sys;

namespace async = kotatsu::async;

namespace cli = kotatsu::cli;
namespace meta = kotatsu::meta;
namespace zest = kotatsu::zest;

using Bitmap = roaring::Roaring;

}  // namespace clice

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

/// The enumerator's name, or the value of one that has none, for clice's own
/// enumerations: another library's are not clice's to specialize for.
template <typename E>
    requires std::is_enum_v<E> && (kotatsu::meta::qualified_type_name<E>.starts_with("clice::"))
struct std::formatter<E> : std::formatter<std::string> {
    using Base = std::formatter<std::string>;

    template <typename FormatContext>
    auto format(const E& value, FormatContext& ctx) const {
        auto name = kotatsu::meta::enum_name(value);
        if(name.empty()) {
            return Base::format(std::to_string(std::to_underlying(value)), ctx);
        }
        return Base::format(std::string(name), ctx);
    }
};
