#include <numeric>

#include "AST/Semantic.h"
#include "Index/SymbolIndex.h"
#include "Support/Binary.h"
#include "Support/Compare.h"

namespace clice::index {

namespace {

namespace memory {

struct Relation {
    RelationKind kind;

    /// The `data` array contains two fields whose meanings depend on the `kind`.
    /// Each `RelationKind` specifies the interpretation of these fields as follows:
    ///
    /// - `Definition` and `Declaration`:
    ///   - `data[0]`: The range of the name token.
    ///   - `data[1]`: The range of the whole symbol.
    ///
    /// - `Reference` and `WeakReference`:
    ///   - `data[0]`: The range of the reference.
    ///   - `data[1]`: Empty (unused).
    ///
    /// - `Interface`, `Implementation`, `TypeDefinition`, `Base`, `Derived`,
    ///   `Constructor`, and `Destructor`:
    ///   - `data[0]`: Empty (unused).
    ///   - `data[1]`: The target symbol.
    ///
    /// - `Caller` and `Callee`:
    ///   - `data[0]`: The range of the call site.
    ///   - `data[1]`: The target symbol (e.g., the called function).
    ///
    std::uint32_t data = -1;
    std::uint32_t data1 = -1;
};

struct Symbol {
    /// The symbol id.
    SymbolID id;

    /// The symbol kind.
    SymbolKind kind;

    /// The relations of this symbol.
    std::vector<Relation> relations;
};

struct Occurrence {
    /// The location(index) of this symbol occurrence.
    std::uint32_t location = -1;

    /// The referenced symbol(index) of the this symbol occurrence.
    std::uint32_t symbol = -1;
};

struct SymbolIndex {
    /// The path of source file.
    std::string path;

    /// The content of source file.
    std::string content;

    /// FIXME: add includes or module names?

    /// All symbols in this file.
    std::vector<Symbol> symbols;

    /// All occurrences in this file.
    std::vector<Occurrence> occurrences;

    /// All ranges in this file.
    std::vector<LocalSourceRange> ranges;
};

}  // namespace memory

class SymbolIndexBuilder : public memory::SymbolIndex {
public:
    SymbolIndexBuilder(ASTInfo& AST) : AST(AST) {}

    std::uint32_t getLocation(LocalSourceRange range) {
        auto key = std::pair(range.begin, range.end);
        auto [iter, success] = locationCache.try_emplace(key, ranges.size());
        if(success) {
            ranges.emplace_back(range);
        }
        return iter->second;
    }

    std::uint32_t getSymbol(const clang::NamedDecl* decl) {
        auto [iter, success] = symbolCache.try_emplace(decl, symbols.size());
        if(success) {
            symbols.emplace_back(memory::Symbol{
                .id = AST.getSymbolID(decl),
                .kind = SymbolKind::from(decl),
            });
        }
        return iter->second;
    }

    std::uint32_t getSymbol(const clang::MacroInfo* macro) {
        auto [iter, success] = symbolCache.try_emplace(macro, symbols.size());
        if(success) {
            symbols.emplace_back(memory::Symbol{
                .id = AST.getSymbolID(macro),
                .kind = SymbolKind::Macro,
            });
        }
        return iter->second;
    }

    void addOccurrence(uint32_t location, uint32_t symbol) {
        occurrences.emplace_back(memory::Occurrence{
            .location = location,
            .symbol = symbol,
        });
    }

    void addRelation(uint32_t symbol,
                     RelationKind kind,
                     uint32_t data,
                     uint32_t data1 = std::numeric_limits<uint32_t>::max()) {
        symbols[symbol].relations.emplace_back(memory::Relation{
            .kind = kind,
            .data = data,
            .data1 = data1,
        });
    }

    void sort() {
        /// We will serialize the index to binary format and compare the data to
        /// check whether they are the index. So here we need to sort all vectors
        /// to make sure that the data is in the same order even they are in different
        /// files.

        /// Map the old index to new index.
        std::vector<uint32_t> symbolMap(symbols.size());
        std::vector<uint32_t> locationMap(ranges.size());

        {
            /// Sort symbols and update the symbolMap.
            std::vector<uint32_t> new2old(symbols.size());
            for(uint32_t i = 0; i < symbols.size(); ++i) {
                new2old[i] = i;
            }

            ranges::sort(views::zip(symbols, new2old), refl::less, [](const auto& element) {
                auto& symbol = std::get<0>(element);
                return std::tuple(symbol.id, symbol.kind);
            });

            for(uint32_t i = 0; i < symbols.size(); ++i) {
                symbolMap[new2old[i]] = i;
            }
        }

        {
            /// Sort locations and update the locationMap.
            std::vector<uint32_t> new2old(ranges.size());
            for(uint32_t i = 0; i < ranges.size(); ++i) {
                new2old[i] = i;
            }

            ranges::sort(views::zip(ranges, new2old), refl::less, [](const auto& element) {
                return std::get<0>(element);
            });

            for(uint32_t i = 0; i < ranges.size(); ++i) {
                locationMap[new2old[i]] = i;
            }
        }

        /// Sort occurrences and update the symbol and location references.
        for(auto& occurrence: occurrences) {
            occurrence.symbol = {symbolMap[occurrence.symbol]};
            occurrence.location = {locationMap[occurrence.location]};
        }

        /// Sort all occurrences and update the symbol and location references.
        ranges::sort(occurrences, refl::less, [](const auto& occurrence) {
            return occurrence.location;
        });
        auto range = ranges::unique(occurrences, refl::equal);
        occurrences.erase(range.begin(), range.end());

        using enum RelationKind::Kind;
        /// Sort all relations and update the symbol and location references.
        for(auto& symbol: symbols) {
            for(auto& relation: symbol.relations) {
                auto kind = relation.kind;
                if(kind.isDeclOrDef()) {
                    relation.data = locationMap[relation.data];
                    relation.data1 = locationMap[relation.data1];
                } else if(kind.isReference()) {
                    relation.data = locationMap[relation.data];
                } else if(kind.isBetweenSymbol()) {
                    relation.data1 = symbolMap[relation.data1];
                } else if(kind.isCall()) {
                    relation.data = locationMap[relation.data];
                    relation.data1 = symbolMap[relation.data1];
                } else {
                    assert(false && "Invalid relation kind");
                }
            }

            ranges::sort(symbol.relations, refl::less);

            auto range = ranges::unique(symbol.relations, refl::equal);
            symbol.relations.erase(range.begin(), range.end());
        }
    }

    memory::SymbolIndex dump() {
        return std::move(static_cast<memory::SymbolIndex>(*this));
    }

private:
    ASTInfo& AST;
    llvm::DenseMap<const void*, uint32_t> symbolCache;
    llvm::DenseMap<std::pair<uint32_t, uint32_t>, uint32_t> locationCache;
};

class SymbolIndexCollector : public SemanticVisitor<SymbolIndexCollector> {
public:
    SymbolIndexCollector(ASTInfo& AST) : SemanticVisitor(AST, false) {}

    SymbolIndexBuilder& getBuilder(clang::FileID fid) {
        auto [it, success] = builders.try_emplace(fid, AST);
        return it->second;
    }

public:
    void handleDeclOccurrence(const clang::NamedDecl* decl,
                              RelationKind kind,
                              clang::SourceLocation location) {
        assert(decl && "Invalid decl");
        decl = normalize(decl);

        if(location.isMacroID()) {
            auto spelling = AST.getSpellingLoc(location);
            auto expansion = AST.getExpansionLoc(location);

            /// FIXME: For location from macro, we only handle the case that the
            /// spelling and expansion are in the same file currently.
            if(AST.getFileID(spelling) != AST.getFileID(expansion)) {
                return;
            }

            /// For occurrence, we always use spelling location.
            location = spelling;
        }

        /// Add the occurrence.
        auto [fid, range] = AST.toLocalRange(location);
        auto& builder = getBuilder(fid);
        auto loc = builder.getLocation(range);
        auto symbol = builder.getSymbol(decl);
        builder.addOccurrence(loc, symbol);
    }

    void handleMacroOccurrence(const clang::MacroInfo* def,
                               RelationKind kind,
                               clang::SourceLocation location) {
        /// FIXME: Figure out when location is MacroID.
        if(location.isMacroID()) {
            return;
        }

        /// Add macro occurrence.
        auto [fid, range] = AST.toLocalRange(location);
        auto& builder = getBuilder(fid);
        auto loc = builder.getLocation(range);
        auto symbol = builder.getSymbol(def);
        builder.addOccurrence(loc, symbol);

        /// If the macro is a definition, set definition range for it.
        std::uint32_t definitionLoc = std::numeric_limits<std::uint32_t>::max();

        if(kind & RelationKind::Definition) {
            auto begin = def->getDefinitionLoc();
            auto end = def->getDefinitionEndLoc();
            assert(begin.isFileID() && end.isFileID() && "Invalid location");
            auto [fid2, range] = AST.toLocalRange(clang::SourceRange(begin, end));
            assert(fid == fid2 && "Invalid macro definition location");
            definitionLoc = builder.getLocation(range);
        }

        builder.addRelation(symbol, kind, loc, definitionLoc);
    }

    void handleRelation(const clang::NamedDecl* decl,
                        RelationKind kind,
                        const clang::NamedDecl* target,
                        clang::SourceRange range) {
        auto [fid, relationRange] = AST.toLocalExpansionRange(range);
        auto& builder = getBuilder(fid);

        /// Calculate the data for the relation.
        std::uint32_t data[2] = {
            std::numeric_limits<std::uint32_t>::max(),
            std::numeric_limits<std::uint32_t>::max(),
        };

        using enum RelationKind::Kind;

        if(kind.isDeclOrDef()) {
            auto [fid2, definitionRange] = AST.toLocalExpansionRange(decl->getSourceRange());
            assert(fid == fid2 && "Invalid definition location");
            data[0] = builder.getLocation(relationRange);
            data[1] = builder.getLocation(definitionRange);
        } else if(kind.isReference()) {
            data[0] = builder.getLocation(relationRange);
        } else if(kind.isBetweenSymbol()) {
            data[1] = builder.getSymbol(normalize(target));
        } else if(kind.isCall()) {
            data[0] = builder.getLocation(relationRange);
            data[1] = builder.getSymbol(normalize(target));
        } else {
            std::unreachable();
        }

        /// Add the relation.
        auto symbol = builder.getSymbol(normalize(decl));
        builder.addRelation(symbol, kind, data[0], data[1]);
    }

    auto build() {
        run();

        llvm::DenseMap<clang::FileID, std::vector<char>> result;
        for(auto& [fid, builder]: builders) {
            builder.sort();
            auto index = builder.dump();
            index.path = AST.getFilePath(fid);
            index.content = AST.getFileContent(fid);
            auto [buffer, _] = binary::serialize(index);
            result.try_emplace(fid, std::move(buffer));
        }

        return std::move(result);
    }

private:
    llvm::DenseMap<const void*, uint64_t> symbolIDs;
    llvm::DenseMap<clang::FileID, SymbolIndexBuilder> builders;
};

}  // namespace

RelationKind Relation::kind() const {
    binary::Proxy<memory::Relation> proxy{base, data};
    return proxy->kind;
}

LocalSourceRange Relation::range() const {
    assert(!kind().isBetweenSymbol());
    binary::Proxy<memory::SymbolIndex> index{base, base};
    binary::Proxy<memory::Relation> proxy{base, data};
    return index.get<"ranges">().as_array()[proxy->data];
}

LocalSourceRange Relation::sourceRange() const {
    assert(kind().isDeclOrDef() && "only declaration or definition has sourceRange range");
    binary::Proxy<memory::SymbolIndex> index{base, base};
    binary::Proxy<memory::Relation> proxy{base, data};
    return index.get<"ranges">().as_array()[proxy->data1];
}

Symbol Relation::target() const {
    assert((kind().isBetweenSymbol() || kind().isCall()) && "only symbols has target");
    binary::Proxy<memory::SymbolIndex> index{base, base};
    binary::Proxy<memory::Relation> proxy{base, data};
    return Symbol{base, &index.get<"symbols">().as_array()[proxy->data1]};
}

SymbolID Symbol::id() const {
    binary::Proxy<memory::Symbol> proxy{base, data};
    return binary::deserialize(proxy.get<"id">());
}

std::uint64_t Symbol::hash() const {
    binary::Proxy<memory::Symbol> proxy{base, data};
    return proxy.get<"id">().get<"hash">().value();
}

llvm::StringRef Symbol::name() const {
    binary::Proxy<memory::Symbol> proxy{base, data};
    return proxy.get<"id">().get<"name">().as_string();
}

SymbolKind Symbol::kind() const {
    binary::Proxy<memory::Symbol> proxy{base, data};
    return proxy.get<"kind">();
}

template <typename U, typename T>
static LazyArray<U> getLazyArray(binary::Proxy<std::vector<T>> proxy) {
    return LazyArray<U>{
        proxy.base,
        &proxy.as_array()[0],
        proxy.size(),
        sizeof(binary::binarify_t<T>),
    };
}

LazyArray<Relation> Symbol::relations() const {
    binary::Proxy<memory::Symbol> symbol{base, data};
    return getLazyArray<Relation>(symbol.get<"relations">());
}

LocalSourceRange Occurrence::range() const {
    binary::Proxy<memory::SymbolIndex> index{base, base};
    binary::Proxy<memory::Occurrence> occurrence{base, data};
    return index.get<"ranges">().as_array()[occurrence->location];
}

Symbol Occurrence::symbol() const {
    binary::Proxy<memory::SymbolIndex> index{base, base};
    binary::Proxy<memory::Occurrence> occurrence{base, data};
    return Symbol{
        base,
        &index.get<"symbols">().as_array()[occurrence->symbol],
    };
}

llvm::StringRef SymbolIndex::path() const {
    binary::Proxy<memory::SymbolIndex> index{data, data};
    return index.get<"path">().as_string();
}

llvm::StringRef SymbolIndex::content() const {
    binary::Proxy<memory::SymbolIndex> index{data, data};
    return index.get<"content">().as_string();
}

LazyArray<Symbol> SymbolIndex::symbols() const {
    binary::Proxy<memory::SymbolIndex> index{data, data};
    return getLazyArray<Symbol>(index.get<"symbols">());
}

LazyArray<Occurrence> SymbolIndex::occurrences() const {
    binary::Proxy<memory::SymbolIndex> index{data, data};
    return getLazyArray<Occurrence>(index.get<"occurrences">());
}

std::vector<Symbol> SymbolIndex::locateSymbol(uint32_t offset) const {
    auto occurrences = this->occurrences();
    auto iter = ranges::lower_bound(occurrences, offset, {}, [](const Occurrence& occurrence) {
        return occurrence.range().end;
    });

    std::vector<Symbol> result;
    while(iter != occurrences.end()) {
        auto occurrence = *iter;
        if(occurrence.range().begin > offset) {
            break;
        }
        result.emplace_back(occurrence.symbol());
        ++iter;
    }
    return result;
}

std::optional<Symbol> SymbolIndex::locateSymbol(const SymbolID& id) const {
    auto symbols = this->symbols();
    auto iter = ranges::lower_bound(symbols, id.hash, {}, [](const Symbol& symbol) {
        return symbol.hash();
    });

    auto symbol = *iter;
    if(symbol.hash() == id.hash && symbol.name() == id.name) {
        return symbol;
    }

    return std::nullopt;
}

Shared<std::vector<char>> SymbolIndex::build(ASTInfo& AST) {
    return SymbolIndexCollector(AST).build();
}

json::Value SymbolIndex::toJSON(bool line) {
    json::Array symbols;
    for(auto symbol: this->symbols()) {
        json::Array relations;

        for(auto relation: symbol.relations()) {
            relations.push_back(json::Object{
                {"kind", llvm::StringRef(relation.kind().name())},
            });

            using enum RelationKind::Kind;
            auto kind = relation.kind();

            if(kind.isDeclOrDef()) {
                relations.back().getAsObject()->insert(
                    {"definitionRange", json::serialize(relation.sourceRange())});
            }

            if(!kind.isBetweenSymbol()) {
                relations.back().getAsObject()->insert(
                    {"range", json::serialize(relation.range())});
            } else {
                relations.back().getAsObject()->insert(
                    {"symbol", json::serialize(relation.target().id())});
            }

            if(kind.isCall()) {
                relations.back().getAsObject()->insert(
                    {"symbol", json::serialize(relation.target().id())});
            }
        }

        symbols.push_back(json::Object{
            {"hash",      symbol.hash()                        },
            {"name",      symbol.name()                        },
            {"kind",      llvm::StringRef(symbol.kind().name())},
            {"relations", std::move(relations)                 },
        });
    }

    json::Array occurrences;
    for(auto occurrence: this->occurrences()) {
        occurrences.push_back(json::Object{
            {"range", json::serialize(occurrence.range())      },
            {"id",    json::serialize(occurrence.symbol().id())}
        });
    }

    return json::Object{
        {"symbols",     std::move(symbols)    },
        {"occurrences", std::move(occurrences)},
    };
}

}  // namespace clice::index
