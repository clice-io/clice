module;

#include "modules/prelude.h"

module clice;

import :compile.compilation;
import :compile.tokens;
import :syntax.lexical_scan;
import :vfs.file_system;
import :vfs.path;

namespace clice {

bool CompilationUnit::fatal_error() {
    return fatal;
}

bool CompilationUnit::has_output() {
    return output_ready;
}

auto CompilationUnit::file_id(clang::FileEntryRef entry) -> clang::FileID {
    return SM().translateFile(entry);
}

auto CompilationUnit::file_id(llvm::StringRef file) -> clang::FileID {
    auto entry = SM().getFileManager().getFileRef(file);
    if(entry) {
        return file_id(*entry);
    }

    return clang::FileID();
}

auto CompilationUnit::file_id(clang::SourceLocation location) -> clang::FileID {
    return SM().getFileID(location);
}

static auto decompose(clang::SourceManager& SM, clang::SourceLocation location) -> FileOffset {
    auto [fid, offset] = SM.getDecomposedLoc(location);
    return {fid, offset};
}

auto CompilationUnit::written_at(clang::SourceLocation location) -> FileOffset {
    return decompose(SM(), SM().getSpellingLoc(location));
}

auto CompilationUnit::seen_at(clang::SourceLocation location) -> FileOffset {
    return decompose(SM(), SM().getFileLoc(location));
}

auto CompilationUnit::produced_at(clang::SourceLocation location) -> FileOffset {
    return decompose(SM(), SM().getExpansionLoc(location));
}

/// The parser splits a `>>` closing two template argument lists in two:
/// the first `>` becomes a macro location over its half of the token.
static auto split_half(clang::SourceManager& SM, clang::SourceLocation location)
    -> std::optional<FileRange> {
    if(!location.isMacroID()) {
        return std::nullopt;
    }
    auto range = SM.getExpansionRange(location);
    if(range.isTokenRange()) {
        return std::nullopt;
    }
    auto begin = decompose(SM, range.getBegin());
    auto end = decompose(SM, range.getEnd());
    return FileRange{
        begin.file,
        {begin.offset, end.offset}
    };
}

auto CompilationUnit::written_range(clang::SourceRange range) -> std::optional<FileRange> {
    if(range.isInvalid()) {
        return std::nullopt;
    }
    auto& SM = this->SM();
    if(auto half = split_half(SM, range.getEnd())) {
        if(range.getBegin() == range.getEnd()) {
            return half;
        }
        // Up to the whole `>>`, then back to its first half.
        auto whole =
            written_range({range.getBegin(), SM.getExpansionRange(range.getEnd()).getBegin()});
        if(!whole || whole->file != half->file) {
            return std::nullopt;
        }
        return FileRange{
            half->file,
            {whole->range.begin, half->range.end}
        };
    }

    auto main = SM.getMainFileID();
    if(range.getBegin().isFileID() && range.getEnd().isFileID()) {
        auto begin = decompose(SM, range.getBegin());
        auto end = decompose(SM, range.getEnd());
        if(begin.file != end.file) {
            return std::nullopt;
        }
        // Invalid code can end a range at the eof, which the tokens leave
        // out: an unclosed block ends at its last token.
        bool at_eof = collected_tokens && !collected_tokens->expanded().empty() &&
                      range.getEnd() == collected_tokens->expanded().back().location();
        if(!at_eof) {
            return FileRange{
                begin.file,
                {begin.offset, end.offset + token_length(range.getEnd())}
            };
        }
    }

    // Macros are mapped through the tokens the parse saw: only those of
    // the focus file are collected.
    if(collected_tokens && SM.getFileID(SM.getExpansionLoc(range.getBegin())) == main) {
        auto spelled = collected_tokens->spelled_for(range);
        if(spelled.empty()) {
            return std::nullopt;
        }
        return FileRange{
            main,
            {SM.getFileOffset(spelled.front().location()),
              SM.getFileOffset(spelled.back().endLocation())}
        };
    }

    auto chars = clang::Lexer::makeFileCharRange(clang::CharSourceRange::getTokenRange(range),
                                                 SM,
                                                 lang_options());
    if(chars.isInvalid()) {
        return std::nullopt;
    }
    auto begin = decompose(SM, chars.getBegin());
    auto end = decompose(SM, chars.getEnd());
    return FileRange{
        begin.file,
        {begin.offset, end.offset}
    };
}

auto CompilationUnit::produced_range(clang::SourceRange range) -> FileRange {
    auto& SM = this->SM();
    auto begin = decompose(SM, SM.getExpansionLoc(range.getBegin()));
    auto last = SM.getExpansionRange(range.getEnd());
    auto end = decompose(SM, last.getEnd());
    auto text = text_end(begin.file);
    if(end.file != begin.file) {
        return {
            begin.file,
            {begin.offset, text}
        };
    }
    // A scope left open in a header compiled in its includer's context
    // ends past its text, after the includer's remainder.
    auto stop = end.offset + (last.isTokenRange() ? token_length(last.getEnd()) : 0);
    return {
        begin.file,
        {begin.offset, std::min(stop, text)}
    };
}

auto CompilationUnit::focus_file() -> FocusFile {
    return FocusFile(*this);
}

auto CompilationUnit::file_path(clang::FileEntryRef entry) -> llvm::StringRef {
    if(auto it = path_cache.find(entry); it != path_cache.end()) {
        return it->second;
    }

    /// Absolutized against the compile's working directory, then named by
    /// the identity the master interns: symlinked spellings of a file
    /// collapse into one path here, hardlinked ones do not, and the cache
    /// is keyed by the spelling-level FileEntryRef so each keeps its own
    /// path — the dependency set must cover every spelling the compile
    /// read. A file only memory holds (a header context's fragment) is
    /// named like a missing one, through its directory.
    llvm::SmallString<128> spelled(entry.getName());
    auto& files = SM().getFileManager();
    files.makeAbsolutePath(spelled);
    // An -ivfsoverlay can name a file by a path only the overlay knows;
    // the file read is the one it redirects to.
    if(auto& vfs = files.getVirtualFileSystem(); llvm::isa<llvm::vfs::RedirectingFileSystem>(vfs)) {
        llvm::SmallString<128> redirected;
        if(!vfs.getRealPath(spelled, redirected)) {
            spelled = redirected;
        }
    }
    auto path = CanonicalPath(Spelling::absolute(spelled)).str();
    auto [it, inserted] = path_cache.try_emplace(entry, llvm::StringSaver(path_storage).save(path));
    assert(inserted && "File path already exists");
    return it->second;
}

auto CompilationUnit::file_path(clang::FileID fid) -> llvm::StringRef {
    assert(fid.isValid() && "file_path: invalid fid");

    auto entry = SM().getFileEntryRefForID(fid);
    assert(entry && "file_path: fid has no backing file entry, check is_builtin_file first");
    if(!entry) {
        /// Callers violating the contract get a degraded result in release
        /// builds; the worker must not crash on it.
        return {};
    }

    return file_path(*entry);
}

auto CompilationUnit::workspace() -> llvm::StringRef {
    return params.input.workspace;
}

auto CompilationUnit::file_content(clang::FileID fid) -> llvm::StringRef {
    return SM().getBufferData(fid);
}

auto CompilationUnit::loaded_file_content(clang::FileID fid) -> std::optional<llvm::StringRef> {
    return SM().getBufferDataOrNone(fid);
}

auto CompilationUnit::positions(clang::FileID fid) -> PositionMap {
    auto content = file_content(fid);
    auto [it, inserted] = line_tables.try_emplace(fid);
    if(inserted) {
        it->second.starts = kota::ipc::lsp::line_starts({content.data(), content.size()});
        it->second.non_ascii = kota::ipc::lsp::non_ascii_lines({content.data(), content.size()});
    }
    return {content, it->second.starts, it->second.non_ascii};
}

auto CompilationUnit::comments(clang::FileID fid) -> llvm::ArrayRef<LexicalInfo::Comment> {
    auto [it, inserted] = comment_cache.try_emplace(fid);
    if(inserted) {
        it->second = scan_comments(file_content(fid), &lang_options());
    }
    return it->second;
}

bool CompilationUnit::is_builtin_file(clang::FileID fid) {
    // No FileEntryRef => built-in/command line/scratch.
    if(!SM().getFileEntryRefForID(fid)) {
        if(auto buffer = SM().getBufferOrNone(fid)) {
            auto name = buffer->getBufferIdentifier();
            return name == "<built-in>" || name == "<command line>" || name == "<scratch space>";
        }
    }

    return false;
}

auto CompilationUnit::include_location(clang::FileID fid) -> clang::SourceLocation {
    return SM().getIncludeLoc(fid);
}

const SynthesizedOrigin* CompilationUnit::origin(clang::FileID fid) {
    if(!borrows_context()) {
        return nullptr;
    }
    auto entry = SM().getFileEntryRefForID(fid);
    if(!entry) {
        return nullptr;
    }
    auto it = params.input.synthesized.find(file_path(*entry));
    return it != params.input.synthesized.end() ? &it->second : nullptr;
}

auto CompilationUnit::text_end(clang::FileID fid) -> std::uint32_t {
    auto content = file_content(fid);
    auto size = static_cast<std::uint32_t>(content.size());
    if(!borrows_context() || fid != SM().getMainFileID()) {
        return size;
    }
    auto it = directives().find(fid);
    if(it == directives().end() || it->second.includes.empty() ||
       !synthesized(it->second.includes.back().fid)) {
        return size;
    }
    auto keyword = SM().getFileOffset(it->second.includes.back().location);
    return static_cast<std::uint32_t>(content.rfind('\n', keyword));
}

bool CompilationUnit::synthesized(clang::FileID fid) {
    return origin(fid) != nullptr;
}

bool CompilationUnit::borrows_context() {
    return !params.input.synthesized.empty();
}

auto CompilationUnit::source_path(clang::FileID fid) -> llvm::StringRef {
    if(auto* found = origin(fid)) {
        return found->source;
    }
    return file_path(fid);
}

std::uint32_t CompilationUnit::source_offset(clang::FileID fid, std::uint32_t offset) {
    auto* found = origin(fid);
    if(!found) {
        return offset;
    }
    auto run =
        llvm::upper_bound(found->runs, offset, [](std::uint32_t offset, const SourceRun& run) {
            return offset < run.offset;
        });
    assert(run != found->runs.begin() && offset - std::prev(run)->offset < std::prev(run)->length &&
           "a position the context did not copy from its file");
    run = std::prev(run);
    return run->source_offset + (offset - run->offset);
}

bool CompilationUnit::host_source(clang::FileID fid) {
    if(!borrows_context()) {
        return focus_file().contains(fid);
    }
    if(!synthesized(fid)) {
        return false;
    }
    // The host's first fragment is the one the compile -includes, from
    // the predefines buffer.
    if(!host) {
        host.emplace();
        auto& SM = this->SM();
        auto predefines = instance->getPreprocessor().getPredefinesFileID();
        for(auto& [path, cut]: params.input.synthesized) {
            if(cut.forced) {
                continue;
            }
            auto entry = SM.getFileManager().getOptionalFileRef(path);
            if(!entry) {
                continue;
            }
            auto root = SM.translateFile(*entry);
            if(root.isValid() && SM.getFileID(SM.getIncludeLoc(root)) == predefines) {
                *host = source_path(root);
            }
        }
    }
    return source_path(fid) == *host;
}

bool CompilationUnit::from_context(clang::FileID fid) {
    if(!borrows_context()) {
        return false;
    }
    auto [it, inserted] = context_files.try_emplace(fid);
    if(!inserted) {
        return it->second;
    }
    bool result = synthesized(fid);
    if(!result) {
        auto include = include_location(fid);
        result = include.isValid() && from_context(file_id(include));
    }
    // The recursion may have grown the map: store through a fresh lookup.
    context_files[fid] = result;
    return result;
}

bool CompilationUnit::encloses_main_file(const clang::Decl* decl) {
    auto& SM = this->SM();
    auto start = SM.getLocForStartOfFile(SM.getMainFileID());
    // A scope left open at the end of the unit has no end.
    auto end = SM.getExpansionLoc(decl->getEndLoc());
    return SM.isBeforeInTranslationUnit(SM.getExpansionLoc(decl->getBeginLoc()), start) &&
           (end.isInvalid() || SM.isBeforeInTranslationUnit(start, end));
}

auto CompilationUnit::presumed_location(clang::SourceLocation location) -> clang::PresumedLoc {
    return SM().getPresumedLoc(location, false);
}

auto CompilationUnit::create_location(clang::FileID fid, std::uint32_t offset)
    -> clang::SourceLocation {
    return SM().getComposedLoc(fid, offset);
}

auto FocusFile::file() -> clang::FileID {
    return unit.SM().getMainFileID();
}

bool FocusFile::contains(clang::FileID fid) {
    return fid == file() || (fid.isValid() && fid == unit.SM().getPreambleFileID());
}

auto FocusFile::content() -> llvm::StringRef {
    return unit.file_content(file());
}

auto FocusFile::positions() -> PositionMap {
    return unit.positions(file());
}

auto FocusFile::tokens() -> const TokenMap& {
    assert(unit.collected_tokens && "only a Content compile collects tokens");
    return *unit.collected_tokens;
}

auto FocusFile::location(std::uint32_t offset) -> clang::SourceLocation {
    return unit.SM().getComposedLoc(file(), offset);
}

auto FocusFile::offset(clang::SourceLocation location) -> std::optional<std::uint32_t> {
    auto seen = unit.seen_at(location);
    if(!contains(seen.file)) {
        return std::nullopt;
    }
    return seen.offset;
}

auto FocusFile::written_range(clang::SourceRange range) -> std::optional<LocalSourceRange> {
    auto written = unit.written_range(range);
    if(!written || !contains(written->file)) {
        return std::nullopt;
    }
    return written->range;
}

auto FocusFile::produced_range(clang::SourceRange range) -> std::optional<LocalSourceRange> {
    auto produced = unit.produced_range(range);
    if(!contains(produced.file)) {
        return std::nullopt;
    }
    return produced.range;
}

auto CompilationUnit::token_length(clang::SourceLocation location) -> std::uint32_t {
    return clang::Lexer::MeasureTokenLength(location, SM(), instance->getLangOpts());
}

auto CompilationUnit::token_spelling(clang::SourceLocation location) -> std::string {
    llvm::SmallString<64> buffer;
    return clang::Lexer::getSpelling(location, buffer, SM(), instance->getLangOpts()).str();
}

bool CompilationUnit::is_named_module() {
    return instance->getPreprocessor().isInNamedModule();
}

auto CompilationUnit::module_name() -> llvm::StringRef {
    return instance->getPreprocessor().getNamedModuleName();
}

bool CompilationUnit::defines_module() {
    auto& pp = instance->getPreprocessor();
    return pp.isInNamedModule() && !pp.isInImplementationUnit();
}

auto CompilationUnit::macro_definitions(llvm::StringRef name)
    -> llvm::SmallVector<const clang::MacroInfo*> {
    auto& pp = instance->getPreprocessor();
    llvm::SmallVector<const clang::MacroInfo*> definitions;
    for(const auto* directive = pp.getLocalMacroDirectiveHistory(pp.getIdentifierInfo(name));
        directive;
        directive = directive->getPrevious()) {
        if(const auto* definition = llvm::dyn_cast<clang::DefMacroDirective>(directive)) {
            definitions.push_back(definition->getInfo());
        }
    }
    std::ranges::reverse(definitions);
    return definitions;
}

auto CompilationUnit::diagnostics() -> llvm::ArrayRef<Diagnostic> {
    return collected_diagnostics;
}

auto CompilationUnit::top_level_decls() -> llvm::ArrayRef<clang::Decl*> {
    return collected_decls;
}

std::chrono::milliseconds CompilationUnit::build_at() {
    return build_time;
}

clang::LangOptions& CompilationUnit::lang_options() {
    return instance->getLangOpts();
}

std::vector<DepFile> CompilationUnit::deps() {
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
            if(auto buffer = SM().getBufferDataOrNone(fid)) {
                it->second = llvm::xxh3_64bits(*buffer);
            }
        }
    };

    /// Embedded files have no FileID (clang delivers their contents as a
    /// single annotation token). Processing the directive loaded the file
    /// into the SourceManager's content cache, so this looks up the very
    /// buffer the build consumed; only an existence-only probe whose
    /// content was never read loads it here instead. An embedded file's
    /// buffer holds its bytes, hashed as text like every dependency. An
    /// unreadable file hashes as 0 and the snapshot capture falls back to
    /// a build_at-guarded disk hash.
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
            if(auto buffer = SM().getMemoryBufferForFileOrNone(*file)) {
                it->second = llvm::xxh3_64bits(vfs::without_bom(buffer->getBuffer()));
            }
        }
    };

    for(auto& [fid, directive]: directives()) {
        for(auto& include: directive.includes) {
            /// A failed include leaves an invalid fid — nothing to depend
            /// on; nor does a synthesized one, which no disk file carries.
            if(!include.skipped && include.fid.isValid() && !synthesized(include.fid)) {
                add_fid(include.fid);
            }
        }

        /// FIXME: Not-found `__has_embed` probes leave no trace, so
        /// creating the probed file later cannot invalidate products built
        /// while it was missing (failed includes and `__has_include` are
        /// recorded, see absent()).
        for(auto& has_include: directive.has_includes) {
            add_file(has_include.file);
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
    for(auto& path: absent()) {
        result.push_back({.path = std::move(path), .absent = true});
    }

    return result;
}

std::vector<std::string> CompilationUnit::absent() {
    std::vector<std::string> result;
    for(auto& entry: absent_paths) {
        // The candidates over-approximate where a lookup looked (an
        // `#include_next` does not start at the first directory, a lookup
        // also fails on a directory of that name): only a place holding
        // nothing is absent.
        if(!vfs::exists(entry.getKey())) {
            result.emplace_back(entry.getKey());
        }
    }
    std::ranges::sort(result);
    return result;
}

clang::TranslationUnitDecl* CompilationUnit::tu() {
    return instance->getASTContext().getTranslationUnitDecl();
}

llvm::DenseMap<clang::FileID, Directive>& CompilationUnit::directives() {
    return collected_directives;
}

clang::ASTContext& CompilationUnit::context() {
    return instance->getASTContext();
}

}  // namespace clice
