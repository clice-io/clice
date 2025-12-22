#pragma once

#include "AST/SourceCode.h"
#include "AST/SymbolID.h"
#include "Index/Shared.h"
#include "Support/JSON.h"

namespace clice::config {

struct InlayHintsOptions {
    /// If false, inlay hints are completely disabled.
    bool enabled = true;

    // Whether specific categories of hints are enabled.
    bool parameters = true;
    bool deduced_types = true;
    bool designators = true;
    bool block_end = false;
    bool default_arguments = false;

    // Limit the length of type names in inlay hints. (0 means no limit)
    uint32_t type_name_limit = 32;
};

}  // namespace clice::config

namespace clice::feature {

enum class InlayHintKind {
    Parameter,
    InvalidEnum,
    DefaultArgument,
    Type,
    Designator,
    BlockEnd,
};

struct InlayHint {
    /// The position offset of the inlay hint in the source code.
    std::uint32_t offset;

    /// The kind/category of the inlay hint.
    InlayHintKind kind;

    /// The label parts of the inlay hint.
    /// Each SymbolID consists of two parts: the symbol name and its USR hash.
    /// For symbols without a USR (e.g., built-in types or function parameters),
    /// the symbol hash will be empty.
    /// Otherwise, the symbol hash is non-empty and can be used for "go-to-definition".
    std::vector<index::SymbolID> parts;
};

auto inlay_hints(CompilationUnit& unit,
                 LocalSourceRange target,
                 const config::InlayHintsOptions& options) -> std::vector<InlayHint>;

}  // namespace clice::feature
