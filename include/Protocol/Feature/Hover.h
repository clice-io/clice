#pragma once

#include "../Basic.h"

namespace clice::proto {

struct HoverClientCapabilities {};

using HoverOptions = bool;

using HoverParams = TextDocumentPositionParams;

struct Hover {
    /// The hover's content
    MarkupContent contents;

    /// An optional range is a range inside a text document
    /// that is used to visualize a hover, e.g. by changing the background color.
    /// FIXME: Range range;
    std::optional<Range> range;
};

}  // namespace clice::proto
