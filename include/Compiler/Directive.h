#pragma once

#include "AST/SourceCode.h"
#include "clang/Lex/MacroInfo.h"
#include "llvm/ADT/DenseMap.h"

namespace clice {

/// Information about `#include` directive.
struct Include {
    /// whether this header is skipped because of #pragma once
    /// or a header guard macro.
    bool skipped;

    /// The file id of included file.
    clang::FileID fid;

    /// Location of the `include`.
    clang::SourceLocation location;

    /// The range of filename(includes `""` or `<>`).
    clang::SourceRange filename_range;
};

/// Information about `__has_include` directive.
struct HasInclude {
    /// The file id of included file, may be empty if there is
    /// not such file.
    clang::FileID fid;

    /// Location of the filename token start.
    clang::SourceLocation location;
};

/// Information about `#if`, `#ifdef`, `#ifndef`, `#elif`,
/// `#elifdef`, `#else`, `#endif` directive.
struct Condition {
    enum class BranchKind : uint8_t {
        If = 0,
        Elif,
        Ifdef,
        Elifdef,
        Ifndef,
        Elifndef,
        Else,
        EndIf,
    };

    using enum BranchKind;

    enum class ConditionValue : uint8_t {
        True = 0,
        False,
        Skipped,
        None,
    };

    using enum ConditionValue;

    /// Kind of the branch.
    BranchKind kind;

    /// Value of the condition.
    ConditionValue value;

    /// Location of the directive identifier.
    clang::SourceLocation loc;

    /// Range of the condition.
    clang::SourceRange condition_range;
};

/// Information about macro definition, reference and undef.
struct MacroRef {
    enum class Kind : uint8_t {
        Def = 0,
        Ref,
        Undef,
    };

    using enum Kind;

    /// The macro definition information.
    const clang::MacroInfo* macro;

    /// Kind of the macro reference.
    Kind kind;

    /// The location of the macro name.
    clang::SourceLocation loc;
};

/// Information about `#pragma` directive.
struct Pragma {
    enum class Kind : uint8_t {
        Region,
        EndRegion,

        // Other unused cases in clice, For example: `#pragma once`.
        Other,
    };

    using enum Kind;

    /// The pragma text in that line, for example:
    ///     "#pragma region"
    ///     "#pragma once"
    ///     "#pragma GCC error"
    llvm::StringRef stmt;

    /// Kind of the pragma.
    Kind kind;

    /// Location of the `#` token.
    clang::SourceLocation loc;
};

struct Import {
    /// The name of imported module.
    std::string name;

    /// The location of import keyword, may comes from macro expansion.
    clang::SourceLocation location;

    /// The locations of tokens that make up the token name, may comes
    /// from macro expansion.
    std::vector<clang::SourceLocation> name_locations;
};

struct Embed {
    /// The file name in the embed directive, not including quotes or angle brackets.
    llvm::StringRef file_name;

    /// The actual file that may be embedded by this embed directive.
    clang::OptionalFileEntryRef file;

    /// Whether the file name is angled.
    bool is_angled;

    /// Location of the `#` token.
    clang::SourceLocation loc;

    /// TODO: Currently we do not store parameters of the embed directive.
    /// See clang::LexEmbedParametersResult for details.
};

struct HasEmbed {
    /// The file name in the embed directive, not including quotes or angle brackets.
    llvm::StringRef file_name;

    /// The actual file that may be embedded by this embed directive.
    clang::OptionalFileEntryRef file;

    /// Whether the file name is angled.
    bool is_angled;

    /// Location of the `__has_embed` token.
    clang::SourceLocation loc;
};

struct Directive {
    std::vector<Include> includes;
    std::vector<HasInclude> has_includes;
    std::vector<Condition> conditions;
    std::vector<MacroRef> macros;
    std::vector<Pragma> pragmas;
    std::vector<Import> imports;
    std::vector<Embed> embeds;
    std::vector<HasEmbed> has_embeds;

    /// Tell preprocessor to collect directives information and store them in `directives`.
    static void attach(clang::Preprocessor& pp,
                       llvm::DenseMap<clang::FileID, Directive>& directives);
};

}  // namespace clice
