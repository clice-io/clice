#pragma once

#include "AST/SymbolKind.h"
#include "Server/Protocol.h"
#include "Support/Struct.h"

namespace clice::proto {

struct WorkDoneProgressOptions {
    /// Report on work done progress.
    bool workDoneProgress = false;
};

struct PartialResultParams {};

/// The options of the all lookup.
using LookupOptions = WorkDoneProgressOptions;

/// The parameters of the simple lookup(definition, declaration,
/// type definition, implementation and reference).
inherited_struct(ReferenceParams, TextDocumentPositionParams, PartialResultParams){};

/// The result of the simple lookup.
using ReferenceResult = std::vector<Location>;

/// The parameters of the all hierarchy resolve(call hierarchy and type hierarchy)
using HierarchyPrepareParams = TextDocumentPositionParams;

struct HierarchyItem {
    /// The name of the item.
    string name;

    /// The kind of the item.
    SymbolKind kind;

    /// The resource identifier of this item.
    DocumentUri uri;

    /// The range enclosing this symbol not including leading/trailing whitespace
    /// but everything else, e.g. comments and code.
    Range range;

    /// The range that should be selected and revealed when this symbol is being
    /// picked, e.g. the name of a function. Must be contained by the
    /// [`range`](#CallHierarchyItem.range).
    Range selectionRange;

    /// A customized data of the item. We use it to store
    /// the USR hash of the item.
    uint64_t data = 0;
};

using HierarchyPrepareResult = std::vector<HierarchyItem>;

/// The parameters of the both call hierarchy and type hierarchy.
inherited_struct(HierarchyParams, TextDocumentPositionParams, PartialResultParams) {
    HierarchyItem item;
};

struct CallHierarchyIncomingCall {
    /// The item that makes the call.
    HierarchyItem from;

    /// The ranges at which the calls appear. This is relative to the caller
    /// denoted by [`this.from`](#CallHierarchyIncomingCall.from).
    std::vector<Range> fromRanges;
};

using CallHierarchyIncomingCallsResult = std::vector<CallHierarchyIncomingCall>;

struct CallHierarchyOutgoingCall {
    /// The item that is called.
    HierarchyItem to;

    /// The range at which this item is called. This is the range relative to
    /// the caller, e.g the item passed to `callHierarchy/outgoingCalls` request.
    std::vector<Range> fromRanges;
};

using CallHierarchyOutgoingCallsResult = std::vector<CallHierarchyOutgoingCall>;

/// The result of the both super and sub type hierarchy.
using TypeHierarchyResult = std::vector<HierarchyItem>;

}  // namespace clice::proto
