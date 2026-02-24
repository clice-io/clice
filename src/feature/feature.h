#pragma once

#include <cstdint>

#include "compile/compilation_unit.h"
#include "eventide/language/protocol.h"
#include "support/position.h"

namespace clice::feature {

namespace protocol = eventide::language::protocol;

using clice::PositionEncoding;
using clice::parse_position_encoding;

auto semantic_tokens(CompilationUnitRef unit, PositionEncoding encoding = PositionEncoding::UTF16)
    -> protocol::SemanticTokens;

}  // namespace clice::feature
