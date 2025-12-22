#pragma once

#include <cstdint>
#include <vector>

#include "Protocol/Feature/SignatureHelp.h"

#include "llvm/ADT/StringRef.h"

namespace clice {

struct CompilationParams;

namespace config {

struct SignatureHelpOption {};

}  // namespace config

namespace feature {

proto::SignatureHelp signature_help(CompilationParams& params,
                                    const config::SignatureHelpOption& option);

}  // namespace feature

}  // namespace clice
