#pragma once

#include "AST/SourceCode.h"
#include "Protocol/Feature/Formatting.h"

#include "llvm/ADT/StringRef.h"

namespace clice::feature {

std::vector<proto::TextEdit> document_format(llvm::StringRef file,
                                             llvm::StringRef content,
                                             std::optional<LocalSourceRange>);

}
