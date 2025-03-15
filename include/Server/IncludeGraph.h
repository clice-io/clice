#pragma once

#include "Config.h"
#include "Protocol.h"
#include "Async/Async.h"
#include "Server/SourceConverter.h"
#include "Compiler/Command.h"
#include "Compiler/Compilation.h"
#include "Support/JSON.h"
#include "Index/SymbolIndex.h"

namespace clice {

struct TranslationUnit;

struct HeaderIndex {
    /// The index file path(not include suffix, e.g. `.sidx` and `.fidx`).
    std::string path;

    /// The hash of the symbol index.
    llvm::XXH128_hash_t symbolHash;

    /// The hash of the feature index.
    llvm::XXH128_hash_t featureHash;
};

struct Context {
    /// The index of header context in indices.
    uint32_t index = -1;

    /// The location index in corresponding tu's
    /// all include locations.
    uint32_t include = -1;
};

struct IncludeLocation {
    /// The location of the include directive.
    uint32_t line = -1;

    /// The index of the file that includes this header.
    uint32_t include = -1;

    /// The file name of the header in the string pool. Beacuse
    /// a header may be included by multiple files, so we use
    /// a string pool to cache the file name to reduce the memory
    /// usage.
    uint32_t file = -1;
};

struct Header;

struct TranslationUnit {
    /// The source file path.
    std::string srcPath;

    /// The index file path(not include suffix, e.g. `.sidx` and `.fidx`).
    std::string indexPath;

    /// All headers included by this translation unit.
    llvm::DenseSet<Header*> headers;

    /// The time when this translation unit is indexed. Used to determine
    /// whether the index file is outdated.
    std::chrono::milliseconds mtime;

    /// All include locations introduced by this translation unit.
    /// Note that if a file has guard macro or pragma once, we will
    /// record it at most once.
    std::vector<IncludeLocation> locations;

    /// The version of the translation unit.
    uint32_t version = 0;
};

struct HeaderContext {
    TranslationUnit* tu = nullptr;

    Context context;

    bool valid() {
        return tu != nullptr;
    }
};

struct Header {
    /// The path of the header file.
    std::string srcPath;

    /// The active header context.
    HeaderContext active;

    /// All indices of the header.
    std::vector<HeaderIndex> indices;

    /// All header contexts of this header.
    llvm::DenseMap<TranslationUnit*, std::vector<Context>> contexts;

    /// Given a translation unit and a include location, return its
    /// its corresponding index.
    std::optional<uint32_t> getIndex(TranslationUnit* tu, uint32_t include) {
        auto it = contexts.find(tu);
        if(it == contexts.end()) {
            return std::nullopt;
        }

        for(auto& context: it->second) {
            if(context.include == include) {
                return context.index;
            }
        }

        return std::nullopt;
    }
};

class IncludeGraph {
protected:
    IncludeGraph(const config::IndexOptions& options) : options(options) {}

    ~IncludeGraph();

    void load(const json::Value& json);

    json::Value dump();

    async::Task<> index(llvm::StringRef file, CompilationDatabase& database);

private:
    std::string getIndexPath(llvm::StringRef file);

    /// Check whether the given file needs to be updated. If so,
    /// return the translation unit. Otherwise, return nullptr.
    async::Task<TranslationUnit*> check(llvm::StringRef file);

    /// Add all possible header contexts for the tu from the AST info.
    uint32_t addIncludeChain(std::vector<IncludeLocation>& locations,
                             llvm::DenseMap<clang::FileID, uint32_t>& files,
                             clang::SourceManager& SM,
                             clang::FileID fid,
                             ASTInfo& AST);

    void addContexts(ASTInfo& info,
                     TranslationUnit* tu,
                     llvm::DenseMap<clang::FileID, uint32_t>& files);

    async::Task<> updateIndices(ASTInfo& info,
                                TranslationUnit* tu,
                                llvm::DenseMap<clang::FileID, uint32_t>& files);

protected:
    const config::IndexOptions& options;
    llvm::StringMap<Header*> headers;
    llvm::StringMap<TranslationUnit*> tus;
    std::vector<std::string> pathPool;
    llvm::StringMap<std::uint32_t> pathIndices;
    SourceConverter SC;
};

}  // namespace clice
