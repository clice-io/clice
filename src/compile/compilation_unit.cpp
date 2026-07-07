#include "compile/implement.h"
#include "index/usr.h"
#include "semantic/ast_utility.h"

#include "kota/ipc/lsp/text.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/xxhash.h"
#include "clang/Serialization/ASTReader.h"
#include "clang/Serialization/ModuleManager.h"

namespace clice {

CompilationKind CompilationUnitRef::kind() {
    return self->kind;
}

CompilationStatus CompilationUnitRef::status() {
    return self->status;
}

auto CompilationUnitRef::file_id(clang::FileEntryRef entry) -> clang::FileID {
    return self->SM().translateFile(entry);
}

auto CompilationUnitRef::file_id(llvm::StringRef file) -> clang::FileID {
    auto entry = self->SM().getFileManager().getFileRef(file);
    if(entry) {
        return file_id(*entry);
    }

    return clang::FileID();
}

auto CompilationUnitRef::decompose_location(clang::SourceLocation location)
    -> std::pair<clang::FileID, std::uint32_t> {
    assert(location.isFileID() && "Decompose macro location is meaningless!");
    return self->SM().getDecomposedLoc(location);
}

auto CompilationUnitRef::decompose_range(clang::SourceRange range)
    -> std::pair<clang::FileID, LocalSourceRange> {
    auto [begin, end] = range;
    assert(begin.isValid() && end.isValid() && "Invalid source range");
    assert(begin.isFileID() && end.isValid() && "Input source range should be a file range");

    if(begin == end) {
        auto [fid, offset] = decompose_location(begin);
        return {
            fid,
            {offset, offset + token_length(end)}
        };
    } else {
        auto [begin_fid, begin_offset] = decompose_location(begin);
        auto [end_fid, end_offset] = decompose_location(end);

        if(begin_fid == end_fid) {
            end_offset += token_length(end);
        } else {
            auto content = file_content(begin_fid);
            end_offset = content.size();
        }

        return {
            begin_fid,
            {begin_offset, end_offset}
        };
    }
}

auto CompilationUnitRef::decompose_expansion_range(clang::SourceRange range)
    -> std::pair<clang::FileID, LocalSourceRange> {
    auto [begin, end] = range;
    if(begin == end) {
        return decompose_range(expansion_location(begin));
    } else {
        return decompose_range(
            clang::SourceRange(expansion_location(begin), expansion_location(end)));
    }
}

auto CompilationUnitRef::file_id(clang::SourceLocation location) -> clang::FileID {
    return self->SM().getFileID(location);
}

auto CompilationUnitRef::file_offset(clang::SourceLocation location) -> std::uint32_t {
    return self->SM().getFileOffset(location);
}

auto CompilationUnitRef::file_path(clang::FileID fid) -> llvm::StringRef {
    if(!fid.isValid())
        return {};
    if(auto it = self->path_cache.find(fid); it != self->path_cache.end()) {
        return it->second;
    }

    auto entry = self->SM().getFileEntryRefForID(fid);
    if(!entry) {
        return {};
    }

    llvm::SmallString<128> path;

    /// Try to get the real path of the file.
    auto name = entry->getName();
    if(auto error = llvm::sys::fs::real_path(name, path)) {
        /// If failed, use the virtual path.
        path = name;
    }
    assert(!path.empty() && "Invalid file path");

    /// Allocate the path in the storage.
    auto size = path.size();
    auto data = self->path_storage.Allocate<char>(size + 1);
    memcpy(data, path.data(), size);
    data[size] = '\0';

    auto [it, inserted] = self->path_cache.try_emplace(fid, llvm::StringRef(data, size));
    assert(inserted && "File path already exists");
    return it->second;
}

auto CompilationUnitRef::file_content(clang::FileID fid) -> llvm::StringRef {
    return self->SM().getBufferData(fid);
}

auto CompilationUnitRef::interested_file() -> clang::FileID {
    return self->SM().getMainFileID();
}

auto CompilationUnitRef::interested_content() -> llvm::StringRef {
    return file_content(interested_file());
}

auto CompilationUnitRef::line_starts() -> std::span<const std::uint32_t> {
    if(self->line_starts_cache.empty()) {
        auto content = interested_content();
        self->line_starts_cache =
            kota::ipc::lsp::build_line_starts({content.data(), content.size()});
    }
    return self->line_starts_cache;
}

bool CompilationUnitRef::is_builtin_file(clang::FileID fid) {
    // No FileEntryRef => built-in/command line/scratch.
    if(!self->SM().getFileEntryRefForID(fid)) {
        if(auto buffer = self->SM().getBufferOrNone(fid)) {
            auto name = buffer->getBufferIdentifier();
            return name == "<built-in>" || name == "<command line>" || name == "<scratch space>";
        }
    }

    return false;
}

auto CompilationUnitRef::start_location(clang::FileID fid) -> clang::SourceLocation {
    return self->SM().getLocForStartOfFile(fid);
}

auto CompilationUnitRef::end_location(clang::FileID fid) -> clang::SourceLocation {
    return self->SM().getLocForEndOfFile(fid);
}

auto CompilationUnitRef::spelling_location(clang::SourceLocation loc) -> clang::SourceLocation {
    return self->SM().getSpellingLoc(loc);
}

auto CompilationUnitRef::expansion_location(clang::SourceLocation location)
    -> clang::SourceLocation {
    return self->SM().getExpansionLoc(location);
}

auto CompilationUnitRef::file_location(clang::SourceLocation location) -> clang::SourceLocation {
    return self->SM().getFileLoc(location);
}

auto CompilationUnitRef::include_location(clang::FileID fid) -> clang::SourceLocation {
    return self->SM().getIncludeLoc(fid);
}

auto CompilationUnitRef::presumed_location(clang::SourceLocation location) -> clang::PresumedLoc {
    return self->SM().getPresumedLoc(location, false);
}

auto CompilationUnitRef::create_location(clang::FileID fid, std::uint32_t offset)
    -> clang::SourceLocation {
    return self->SM().getComposedLoc(fid, offset);
}

auto CompilationUnitRef::spelled_tokens(clang::FileID fid) -> TokenRange {
    return self->buffer->spelledTokens(fid);
}

auto CompilationUnitRef::spelled_tokens(clang::SourceRange range) -> TokenRange {
    auto tokens = self->buffer->spelledForExpanded(self->buffer->expandedTokens(range));
    if(!tokens) {
        return {};
    }

    return *tokens;
}

auto CompilationUnitRef::spelled_tokens_touch(clang::SourceLocation location) -> TokenRange {
    return clang::syntax::spelledTokensTouching(location, *self->buffer);
}

auto CompilationUnitRef::expanded_tokens() -> TokenRange {
    return self->buffer->expandedTokens();
}

auto CompilationUnitRef::expanded_tokens(clang::SourceRange range) -> TokenRange {
    return self->buffer->expandedTokens(range);
}

auto CompilationUnitRef::expansions_overlapping(TokenRange spelled_tokens)
    -> std::vector<clang::syntax::TokenBuffer::Expansion> {
    return self->buffer->expansionsOverlapping(spelled_tokens);
}

auto CompilationUnitRef::token_length(clang::SourceLocation location) -> std::uint32_t {
    return clang::Lexer::MeasureTokenLength(location, self->SM(), self->instance->getLangOpts());
}

auto CompilationUnitRef::token_spelling(clang::SourceLocation location) -> llvm::StringRef {
    return llvm::StringRef(self->SM().getCharacterData(location), token_length(location));
}

bool CompilationUnitRef::is_named_module() {
    return self->instance->getPreprocessor().isInNamedModule();
}

auto CompilationUnitRef::module_name() -> llvm::StringRef {
    return self->instance->getPreprocessor().getNamedModuleName();
}

bool CompilationUnitRef::is_module_interface_unit() {
    return self->instance->getPreprocessor().isInNamedInterfaceUnit();
}

auto CompilationUnitRef::diagnostics() -> std::vector<Diagnostic>& {
    return self->diagnostics;
}

auto CompilationUnitRef::top_level_decls() -> llvm::ArrayRef<clang::Decl*> {
    return self->top_level_decls;
}

std::chrono::milliseconds CompilationUnitRef::build_at() {
    return self->build_at;
}

std::chrono::milliseconds CompilationUnitRef::build_duration() {
    return self->build_duration;
}

clang::LangOptions& CompilationUnitRef::lang_options() {
    return self->instance->getLangOpts();
}

/// Hash a file's content with the same scheme the server layer uses for its
/// dependency snapshots (workspace's `hash_file`). Returns 0 on read failure.
static std::uint64_t hash_file_content(llvm::StringRef path) {
    auto buffer = llvm::MemoryBuffer::getFile(path);
    if(!buffer) {
        return 0;
    }
    return llvm::xxh3_64bits((*buffer)->getBuffer());
}

std::vector<HashedDep> CompilationUnitRef::deps() {
    llvm::StringMap<std::uint64_t> deps;

    /// FIXME: consider `#embed` and `__has_embed`.

    auto add_resident = [&](clang::FileID fid) {
        auto path = file_path(fid);
        if(path.empty()) {
            return;
        }
        auto [it, inserted] = deps.try_emplace(path, 0);
        if(!inserted) {
            return;
        }
        // Hash the buffer the compiler actually consumed rather than
        // re-reading the file: a disk change that lands while the compilation
        // is running can then never be blessed as the built-from content.
        if(auto buffer = self->SM().getBufferOrNone(fid)) {
            it->second = llvm::xxh3_64bits(buffer->getBuffer());
        }
    };

    for(auto& [fid, directive]: directives()) {
        for(auto& include: directive.includes) {
            if(!include.skipped) {
                add_resident(include.fid);
            }
        }

        for(auto& has_include: directive.has_includes) {
            if(has_include.fid.isValid()) {
                add_resident(has_include.fid);
            }
        }
    }

    for(auto& dep: imported_module_sources()) {
        deps.try_emplace(dep.path, dep.hash);
    }

    std::vector<HashedDep> result;
    result.reserve(deps.size());
    for(auto& entry: deps) {
        result.push_back({entry.getKey().str(), entry.getValue()});
    }

    return result;
}

std::vector<HashedDep> CompilationUnitRef::imported_module_sources() {
    std::vector<HashedDep> sources;
    auto reader = self->instance->getASTReader();
    if(!reader) {
        return sources;
    }

    // The module manager chain covers every loaded module file, including
    // transitively loaded ones — exactly the set whose interface sources this
    // compilation's semantics depend on. The sources were consumed through
    // their PCMs, never read here, so there is no resident buffer to hash:
    // read them from disk instead. A save landing between the PCM build and
    // this hash is caught by the push-side invalidation cascade, not by this
    // snapshot.
    for(auto& file: reader->getModuleManager()) {
        if(!file.StandardCXXModule) {
            continue;
        }
        auto& source = file.ActualOriginalSourceFileName.empty()
                           ? file.OriginalSourceFileName
                           : file.ActualOriginalSourceFileName;
        if(!source.empty()) {
            sources.push_back({source, hash_file_content(source)});
        }
    }
    return sources;
}

index::SymbolID CompilationUnitRef::getSymbolID(const clang::NamedDecl* decl) {
    uint64_t hash;
    auto iter = self->symbol_hash_cache.find(decl);
    if(iter != self->symbol_hash_cache.end()) {
        hash = iter->second;
    } else {
        llvm::SmallString<128> usr;
        index::generateUSRForDecl(decl, usr);
        hash = llvm::xxh3_64bits(usr);
        self->symbol_hash_cache.try_emplace(decl, hash);
    }
    return index::SymbolID{hash, ast::name_of(decl)};
}

index::SymbolID CompilationUnitRef::getSymbolID(const clang::MacroInfo* macro) {
    std::uint64_t hash;
    auto name = token_spelling(macro->getDefinitionLoc());
    auto iter = self->symbol_hash_cache.find(macro);
    if(iter != self->symbol_hash_cache.end()) {
        hash = iter->second;
    } else {
        llvm::SmallString<128> usr;
        index::generateUSRForMacro(name, macro->getDefinitionLoc(), self->SM(), usr);
        hash = llvm::xxh3_64bits(usr);
        self->symbol_hash_cache.try_emplace(macro, hash);
    }
    return index::SymbolID{hash, name.str()};
}

const llvm::DenseSet<clang::FileID>& CompilationUnitRef::files() {
    if(self->all_files.empty()) {
        /// FIXME: handle preamble and embed file id.
        for(auto& [fid, directive]: directives()) {
            for(auto& include: directive.includes) {
                if(!include.skipped && include.fid.isValid()) {
                    self->all_files.insert(include.fid);
                }
            }
        }
        self->all_files.insert(self->SM().getMainFileID());
    }
    return self->all_files;
}

clang::TranslationUnitDecl* CompilationUnitRef::tu() {
    return self->instance->getASTContext().getTranslationUnitDecl();
}

llvm::DenseMap<clang::FileID, Directive>& CompilationUnitRef::directives() {
    return self->directives;
}

TemplateResolver& CompilationUnitRef::resolver() {
    assert(self->resolver && "Template resolver is not available");
    return *self->resolver;
}

clang::ASTContext& CompilationUnitRef::context() {
    return self->instance->getASTContext();
}

clang::syntax::TokenBuffer& CompilationUnitRef::token_buffer() {
    return *self->buffer;
}

CompilationUnit::~CompilationUnit() {
    delete self;
}

}  // namespace clice
