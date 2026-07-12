#include "compile/implement.h"
#include "index/usr.h"
#include "semantic/ast_utility.h"
#include "support/filesystem.h"

#include "kota/ipc/lsp/text.h"

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

auto CompilationUnitRef::file_path(clang::FileEntryRef entry) -> llvm::StringRef {
    if(auto it = self->path_cache.find(entry); it != self->path_cache.end()) {
        return it->second;
    }

    auto& fm = self->SM().getFileManager();

    /// Absolutize against the compile's working directory first, then
    /// resolve through the compiler's VFS so remapped and in-memory
    /// files canonicalize like on-disk ones. Symlinked spellings of a
    /// file collapse into one path here; hardlinked spellings do not
    /// (`real_path` does not fold them), and the cache is keyed by the
    /// spelling-level FileEntryRef so each keeps its own path — the
    /// dependency set must cover every spelling the compile read.
    llvm::SmallString<128> path(entry.getName());
    fm.makeAbsolutePath(path);

    llvm::SmallString<128> real;
    if(auto error = fm.getVirtualFileSystem().getRealPath(path, real)) {
        /// The VFS cannot resolve it; keep the absolute path with dot
        /// segments removed rather than a raw spelling — consumers stat
        /// these paths from a different working directory.
        path::remove_dots(path, /*remove_dot_dot=*/true);
    } else {
        path = real;
    }
    assert(!path.empty() && "Invalid file path");

    /// Allocate the path in the storage.
    auto size = path.size();
    auto data = self->path_storage.Allocate<char>(size + 1);
    memcpy(data, path.data(), size);
    data[size] = '\0';

    auto [it, inserted] = self->path_cache.try_emplace(entry, llvm::StringRef(data, size));
    assert(inserted && "File path already exists");
    return it->second;
}

auto CompilationUnitRef::file_path(clang::FileID fid) -> llvm::StringRef {
    assert(fid.isValid() && "file_path: invalid fid");

    auto entry = self->SM().getFileEntryRefForID(fid);
    assert(entry && "file_path: fid has no backing file entry, check is_builtin_file first");
    if(!entry) {
        /// Callers violating the contract get a degraded result in release
        /// builds; the worker must not crash on it.
        return {};
    }

    return file_path(*entry);
}

auto CompilationUnitRef::file_content(clang::FileID fid) -> llvm::StringRef {
    return self->SM().getBufferData(fid);
}

auto CompilationUnitRef::loaded_file_content(clang::FileID fid) -> std::optional<llvm::StringRef> {
    return self->SM().getBufferDataOrNone(fid);
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

std::vector<DepFile> CompilationUnitRef::deps() {
    llvm::StringMap<std::uint64_t> deps;

    // Hash the buffer the compiler read for this fid — the consumed bytes.
    // The same path can appear under several fids (repeated includes); any
    // of their buffers holds the same content, so the first one wins.
    auto add_fid = [&](clang::FileID fid) {
        auto path = file_path(fid);
        if(path.empty()) {
            return;
        }
        auto it = deps.try_emplace(path, 0).first;
        if(it->second == 0) {
            if(auto buffer = self->SM().getBufferDataOrNone(fid)) {
                it->second = llvm::xxh3_64bits(*buffer);
            }
        }
    };

    /// Embedded files have no FileID (clang delivers their contents as a
    /// single annotation token). Processing the directive loaded the file
    /// into the SourceManager's content cache, so this looks up the very
    /// buffer the build consumed; only an existence-only probe whose
    /// content was never read loads it here instead. An unreadable file
    /// hashes as 0 and the snapshot capture falls back to a
    /// build_at-guarded disk hash.
    auto add_file = [&](clang::OptionalFileEntryRef file) {
        if(!file) {
            return;
        }
        auto path = file_path(*file);
        if(path.empty()) {
            return;
        }
        auto it = deps.try_emplace(path, 0).first;
        if(it->second == 0) {
            if(auto buffer = self->SM().getMemoryBufferForFileOrNone(*file)) {
                it->second = llvm::xxh3_64bits(buffer->getBuffer());
            }
        }
    };

    for(auto& [fid, directive]: directives()) {
        for(auto& include: directive.includes) {
            /// A failed include leaves an invalid fid — nothing to depend on.
            if(!include.skipped && include.fid.isValid()) {
                add_fid(include.fid);
            }
        }

        /// FIXME: Not-found `__has_include`/`__has_embed` probes leave no
        /// trace here, so creating the probed file later cannot invalidate
        /// products built while it was missing. `clang -MD` drops misses
        /// the same way — the build ecosystem accepts this hole, and even
        /// clang's preamble simulates the failed lookup's candidate paths
        /// only for `#include` misses. Rather than stat'ing candidate sets
        /// per freshness check, the right home is the invalidation
        /// pipeline: persist unresolved lookups and match them against
        /// file-creation events from the workspace watcher.
        for(auto& has_include: directive.has_includes) {
            if(has_include.fid.isValid()) {
                add_fid(has_include.fid);
            }
        }

        for(auto& embed: directive.embeds) {
            add_file(embed.file);
        }

        for(auto& has_embed: directive.has_embeds) {
            add_file(has_embed.file);
        }
    }

    std::vector<DepFile> result;
    result.reserve(deps.size());

    for(auto& dep: deps) {
        result.emplace_back(dep.getKey().str(), dep.getValue());
    }

    return result;
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
