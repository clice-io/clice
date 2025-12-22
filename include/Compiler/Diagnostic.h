#pragma once

#include <cstdint>
#include <string>

#include "AST/SourceCode.h"
#include "Compiler/Tidy.h"

#include "clang/Basic/Diagnostic.h"

namespace clang {

class DiagnosticConsumer;

}

namespace clice {

enum class DiagnosticLevel : std::uint8_t {
    Ignored,
    Note,
    Remark,
    Warning,
    Error,
    Fatal,
    Invalid,
};

enum class DiagnosticSource : std::uint8_t {
    Unknown,
    Clang,
    ClangTidy,
    Clice,
};

struct DiagnosticID {
    /// The diagnostic id value.
    std::uint32_t value;

    /// The level of this diagnostic.
    DiagnosticLevel level;

    /// The source of diagnostic.
    DiagnosticSource source;

    llvm::StringRef name;

    /// Get the diagnostic code.
    llvm::StringRef diagnostic_code() const;

    /// Get help diagnostic uri for the diagnostic.
    std::optional<std::string> diagnostic_document_uri() const;

    /// Whether this diagnostic represents an deprecated diagnostic.
    bool is_deprecated() const;

    /// Whether this diagnostic represents an unused diagnostic.
    bool is_unused() const;
};

class DiagnosticCollector : public clang::DiagnosticConsumer {
public:
    tidy::ClangTidyChecker* checker = nullptr;
};

struct Diagnostic {
    /// The diagnostic id.
    DiagnosticID id;

    /// The file location of this diagnostic.
    clang::FileID fid;

    /// The source range of this diagnostic(may be invalid, if this diagnostic
    /// is from command line. e.g. unknown command line argument).
    LocalSourceRange range;

    /// The error message of this diagnostic.
    std::string message;

    static std::unique_ptr<DiagnosticCollector>
        create(std::shared_ptr<std::vector<Diagnostic>> diagnostics);
};

}  // namespace clice
