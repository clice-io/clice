#pragma once

#include <expected>
#include "Support/Enum.h"
#include "Support/Format.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

namespace clice::toolchain {

struct QueryDriverError {
    struct ErrorKind : refl::Enum<ErrorKind> {
        enum Kind : std::uint8_t {
            NotFoundInPATH,
            NotImplemented,
            FailToCreateTempFile,
            InvokeDriverFail,
            OutputFileNotReadable,
            InvalidOutputFormat,
        };

        using Enum::Enum;
    };

    ErrorKind kind;
    std::string detail;
};

struct QueryResult {
    std::string target;
    llvm::SmallVector<std::string, 8> includes;
};

enum class Kind {};

struct Toolchain {};

/// Query toolchain info according to the original compilation arguments.
Toolchain query_toolchain(llvm::ArrayRef<const char*> arguments);

auto query_driver(llvm::StringRef driver) -> std::expected<QueryResult, QueryDriverError>;

}  // namespace clice::toolchain

template <>
struct std::formatter<clice::toolchain::QueryDriverError> : std::formatter<std::string_view> {
    template <typename FormatContext>
    auto format(const clice::toolchain::QueryDriverError& e, FormatContext& ctx) const {
        return std::format_to(ctx.out(), "{} {}", e.kind.name(), e.detail);
    }
};
