#pragma once

#include <vector>

#include "Shared.h"
#include "Feature/SemanticTokens.h"
#include "Feature/FoldingRange.h"
#include "Feature/DocumentLink.h"
#include "Feature/DocumentSymbol.h"

#include "llvm/ADT/DenseMap.h"
#include "clang/Basic/SourceLocation.h"

namespace clice::index {

class FeatureIndex {
public:
    FeatureIndex(char* base, std::size_t size, bool own = true) :
        base(base), size(size), own(own) {}

    FeatureIndex(const FeatureIndex&) = delete;

    FeatureIndex(FeatureIndex&& other) noexcept :
        base(other.base), size(other.size), own(other.own) {
        other.base = nullptr;
        other.size = 0;
        other.own = false;
    }

    ~FeatureIndex() {
        if(own) {
            std::free(base);
        }
    }

    std::vector<feature::SemanticToken> semanticTokens() const;

    std::vector<feature::FoldingRange> foldingRanges() const;

    std::vector<feature::DocumentLink> documentLinks() const;

    std::vector<feature::DocumentSymbol> documentSymbols() const;

public:
    char* base;
    std::size_t size;
    bool own;
};

Shared<FeatureIndex> indexFeature(ASTInfo& info);

}  // namespace clice::index
