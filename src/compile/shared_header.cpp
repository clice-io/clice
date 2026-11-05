module clice;

import :compile.compilation;
import :compile.shared_header;

namespace clice {

namespace {

constexpr auto none = SharedHeaderRecord::none;

/// An include the recorder has not seen enter a file yet.
constexpr std::uint32_t unresolved = none - 1;

std::uint64_t name_hash(llvm::StringRef name) {
    return llvm::xxh3_64bits(name);
}

/// The identifiers of a condition's text that no macro names: what it reads
/// as 0. Those naming a macro are expansions or `defined`, which the
/// preprocessor reports itself.
void condition_identifiers(clang::Preprocessor& pp,
                           clang::SourceRange range,
                           llvm::function_ref<void(llvm::StringRef)> read) {
    auto& sm = pp.getSourceManager();
    auto text = clang::Lexer::getSourceText(clang::CharSourceRange::getTokenRange(range),
                                            sm,
                                            pp.getLangOpts());
    std::size_t i = 0;
    while(i < text.size()) {
        char c = text[i];
        if(c == '"' || c == '\'') {
            auto end = text.find(c, i + 1);
            i = end == llvm::StringRef::npos ? text.size() : end + 1;
        } else if(llvm::isAlpha(c) || c == '_') {
            auto start = i;
            while(i < text.size() && (llvm::isAlnum(text[i]) || text[i] == '_')) {
                i += 1;
            }
            auto name = text.substr(start, i - start);
            if(name.starts_with("__has_include") || name == "__has_embed") {
                // Its operand is a header name.
                auto end = text.find(')', i);
                i = end == llvm::StringRef::npos ? text.size() : end + 1;
                continue;
            }
            if(name != "defined" && !name.starts_with("__has_") && name != "true" &&
               name != "false" && !pp.getMacroInfo(pp.getIdentifierInfo(name))) {
                read(name);
            }
        } else if(llvm::isDigit(c)) {
            // A number's suffix (`1UL`) is no identifier.
            while(i < text.size() && (llvm::isAlnum(text[i]) || text[i] == '_' || text[i] == '.')) {
                i += 1;
            }
        } else {
            i += 1;
        }
    }
}

}  // namespace

/// Records, in the build of a shared header's PCH, what each header the
/// PCH holds read and wrote and which includes it ran: a header's run owns
/// what happens while it is the innermost file the PCH may hold on the
/// include stack, the runs of files it enters without a guard included.
class CompilationUnit::SharedHeaderRecorder : public clang::PPCallbacks {
public:
    explicit SharedHeaderRecorder(CompilationUnit& unit) :
        unit(unit), sm(unit.SM()), pp(unit.instance->getPreprocessor()) {}

    /// An identifier the preprocessor handed the parser: one no macro
    /// names here, read as 0.
    void token(const clang::Token& token) {
        auto* info = token.getIdentifierInfo();
        if(!info || token.isAnnotation() || info->hasMacroDefinition()) {
            return;
        }
        read(info->getName(), 0);
    }

    /// Hand what was recorded to the unit, the headers whose guard the PCH
    /// left undefined left out: a unit enters them again.
    void finish() {
        auto& record = unit.recorded_shared;
        // An include names a file before the build enters it, which it then
        // has entered by the end.
        for(auto [file, entry]: views::zip(record.files, file_entries)) {
            if(auto fid = unit.file_id(entry); fid.isValid()) {
                if(auto content = unit.loaded_file_content(fid)) {
                    file.content_hash = llvm::xxh3_64bits(*content);
                }
            }
        }
        auto& search = pp.getHeaderSearchInfo();
        std::vector<std::uint32_t> kept(headers.size(), none);
        for(auto [index, header]: views::enumerate(headers)) {
            auto& info = search.getFileInfo(header.entry);
            auto* guard = info.getControllingMacro(nullptr);
            if(header.forced || info.isPragmaOnce || (guard && pp.getMacroInfo(guard))) {
                kept[index] = static_cast<std::uint32_t>(record.headers.size());
                record.headers.push_back(std::move(header.record));
                if(header.forced) {
                    record.forced.push_back(kept[index]);
                }
            }
        }
        // An include the guard skipped names the header's first run.
        std::vector<std::uint32_t> held_files(record.files.size(), none);
        for(auto [index, header]: views::enumerate(record.headers)) {
            held_files[header.file] = static_cast<std::uint32_t>(index);
        }
        for(auto& header: record.headers) {
            for(auto& include: header.includes) {
                if(include.held == unresolved) {
                    include.held = held_files[include.file];
                } else if(include.held != none) {
                    include.held = kept[include.held];
                }
            }
        }
        for(auto& [name, write]: written) {
            record.written.push_back({.name = name,
                                      .spelling = write.spelling.str(),
                                      .initial = initial.lookup(name),
                                      .project = write.project});
        }
        ranges::sort(record.written, {}, &SharedHeaderRecord::Written::name);
        for(auto& [name, state]: initial) {
            if(state != 0 && !written.contains(name)) {
                record.initial.push_back({.name = name, .state = state});
            }
        }
        ranges::sort(record.initial, {}, &MacroState::name);
    }

private:
    struct Frame {
        clang::FileID fid;

        clang::OptionalFileEntryRef entry;

        bool system = false;

        /// Entered by the command's -include.
        bool forced = false;

        /// The include of the file below it on the stack that entered it.
        std::uint32_t entered_by = none;

        llvm::DenseSet<std::uint64_t> read_names;

        /// What it and the runs inside it wrote.
        llvm::DenseSet<std::uint64_t> written;

        std::vector<MacroState> reads;

        std::vector<MacroState> writes;

        std::vector<SharedHeaderRecord::Include> includes;
    };

    void FileChanged(clang::SourceLocation location,
                     FileChangeReason reason,
                     clang::SrcMgr::CharacteristicKind kind,
                     clang::FileID) override {
        if(reason == EnterFile) {
            auto fid = sm.getFileID(location);
            Frame frame{.fid = fid,
                        .entry = sm.getFileEntryRefForID(fid),
                        .system = clang::SrcMgr::isSystem(kind)};
            if(!stack.empty()) {
                auto& below = stack.back();
                frame.forced = below.fid == pp.getPredefinesFileID();
                if(below.entry && !below.includes.empty() &&
                   below.includes.back().held == unresolved) {
                    frame.entered_by = static_cast<std::uint32_t>(below.includes.size() - 1);
                    below.includes.back().held = none;
                }
            }
            stack.push_back(std::move(frame));
        } else if(reason == ExitFile && stack.size() > 1) {
            auto frame = std::move(stack.back());
            stack.pop_back();
            exit(std::move(frame));
        }
    }

    void exit(Frame frame) {
        auto& below = stack.back();
        auto& search = pp.getHeaderSearchInfo();
        bool guarded = frame.entry && (search.isFileMultipleIncludeGuarded(*frame.entry) ||
                                       search.getFileInfo(*frame.entry).isPragmaOnce);
        auto index = static_cast<std::uint32_t>(headers.size());
        if(frame.entry && (frame.forced || guarded) &&
           held.try_emplace(&frame.entry->getFileEntry(), index).second) {
            headers.push_back({.entry = *frame.entry,
                               .forced = frame.forced,
                               .record = {.file = file_index(*frame.entry),
                                          .system = frame.system,
                                          .reads = std::move(frame.reads),
                                          .writes = std::move(frame.writes),
                                          .includes = std::move(frame.includes)}});
            if(frame.entered_by != none) {
                below.includes[frame.entered_by].held = index;
            }
            below.written.insert(frame.written.begin(), frame.written.end());
            return;
        }
        // A file entered without a guard, again by every unit that
        // includes it: its run is the run of the header including it.
        auto base = static_cast<std::uint32_t>(below.includes.size());
        auto writes = static_cast<std::uint32_t>(below.writes.size());
        for(auto include: frame.includes) {
            include.parent = include.parent == none ? frame.entered_by : include.parent + base;
            include.writes += writes;
            below.includes.push_back(include);
        }
        for(auto read: frame.reads) {
            if(!below.written.contains(read.name) && below.read_names.insert(read.name).second) {
                below.reads.push_back(read);
            }
        }
        below.writes.append_range(frame.writes);
        below.written.insert(frame.written.begin(), frame.written.end());
    }

    void InclusionDirective(clang::SourceLocation hash,
                            const clang::Token&,
                            llvm::StringRef,
                            bool,
                            clang::CharSourceRange,
                            clang::OptionalFileEntryRef file,
                            llvm::StringRef,
                            llvm::StringRef,
                            const clang::Module*,
                            bool,
                            clang::SrcMgr::CharacteristicKind) override {
        auto& frame = stack.back();
        if(!frame.entry || !file) {
            return;
        }
        frame.includes.push_back({.line = sm.getPresumedLoc(hash, false).getLine(),
                                  .file = file_index(*file),
                                  .held = unresolved,
                                  .writes = static_cast<std::uint32_t>(frame.writes.size())});
    }

    void MacroDefined(const clang::Token& name, const clang::MacroDirective* directive) override {
        const clang::MacroInfo* previous = nullptr;
        if(auto* before = directive->getPrevious()) {
            previous = before->getDefinition().getMacroInfo();
        }
        write(name, previous, directive->getMacroInfo());
    }

    void MacroUndefined(const clang::Token& name,
                        const clang::MacroDefinition& definition,
                        const clang::MacroDirective*) override {
        write(name, definition.getMacroInfo(), nullptr);
    }

    void MacroExpands(const clang::Token& name,
                      const clang::MacroDefinition& definition,
                      clang::SourceRange,
                      const clang::MacroArgs*) override {
        read(name, definition);
    }

    void Defined(const clang::Token& name,
                 const clang::MacroDefinition& definition,
                 clang::SourceRange) override {
        read(name, definition);
    }

    void Ifdef(clang::SourceLocation,
               const clang::Token& name,
               const clang::MacroDefinition& definition) override {
        read(name, definition);
    }

    void Ifndef(clang::SourceLocation,
                const clang::Token& name,
                const clang::MacroDefinition& definition) override {
        read(name, definition);
    }

    void Elifdef(clang::SourceLocation,
                 const clang::Token& name,
                 const clang::MacroDefinition& definition) override {
        read(name, definition);
    }

    void Elifndef(clang::SourceLocation,
                  const clang::Token& name,
                  const clang::MacroDefinition& definition) override {
        read(name, definition);
    }

    void If(clang::SourceLocation, clang::SourceRange range, ConditionValueKind) override {
        condition_identifiers(pp, range, [&](llvm::StringRef name) { read(name, 0); });
    }

    void Elif(clang::SourceLocation,
              clang::SourceRange range,
              ConditionValueKind,
              clang::SourceLocation) override {
        condition_identifiers(pp, range, [&](llvm::StringRef name) { read(name, 0); });
    }

    /// A file of the build reads nothing that is the command's to give
    /// while no file the PCH may hold is on the stack.
    bool tracked() const {
        return !stack.empty() && stack.back().entry;
    }

    void read(const clang::Token& name, const clang::MacroDefinition& definition) {
        auto* info = name.getIdentifierInfo();
        read(info->getName(), unit.macro_state(definition.getMacroInfo(), *info));
    }

    void read(llvm::StringRef spelling, std::uint64_t state) {
        if(!tracked()) {
            return;
        }
        auto name = name_hash(spelling);
        initial.try_emplace(name, state);
        auto& frame = stack.back();
        if(!frame.written.contains(name) && frame.read_names.insert(name).second) {
            frame.reads.push_back({.name = name, .state = state});
        }
    }

    void write(const clang::Token& token,
               const clang::MacroInfo* previous,
               const clang::MacroInfo* now) {
        if(!tracked()) {
            return;
        }
        auto* info = token.getIdentifierInfo();
        auto name = name_hash(info->getName());
        initial.try_emplace(name, unit.macro_state(previous, *info));
        auto [it, _] = written.try_emplace(name, Write{.spelling = info->getName()});
        if(!sm.isInSystemHeader(sm.getExpansionLoc(token.getLocation()))) {
            it->second.project = true;
        }
        auto& frame = stack.back();
        frame.written.insert(name);
        frame.writes.push_back({.name = name, .state = unit.macro_state(now, *info)});
    }

    std::uint32_t file_index(clang::FileEntryRef entry) {
        auto [it, inserted] =
            files.try_emplace(&entry.getFileEntry(),
                              static_cast<std::uint32_t>(unit.recorded_shared.files.size()));
        if(inserted) {
            unit.recorded_shared.files.push_back({.path = unit.file_path(entry).str()});
            file_entries.push_back(entry);
        }
        return it->second;
    }

private:
    struct Header {
        clang::FileEntryRef entry;

        bool forced = false;

        SharedHeaderRecord::Header record;
    };

    CompilationUnit& unit;

    clang::SourceManager& sm;

    clang::Preprocessor& pp;

    std::vector<Frame> stack;

    /// Each file's first run that may be held, into `headers`.
    llvm::DenseMap<const clang::FileEntry*, std::uint32_t> held;

    std::vector<Header> headers;

    /// Into SharedHeaderRecord::files.
    llvm::DenseMap<const clang::FileEntry*, std::uint32_t> files;

    std::vector<clang::FileEntryRef> file_entries;

    /// The state of each macro read or written where it was first.
    llvm::DenseMap<std::uint64_t, std::uint64_t> initial;

    struct Write {
        llvm::StringRef spelling;

        /// A file outside the system headers wrote it.
        bool project = false;
    };

    /// The macros written.
    llvm::DenseMap<std::uint64_t, Write> written;
};

/// Checks, in a unit loading a shared header's PCH, that parsing each held
/// header where the unit's text includes it would have read what its build
/// read, and that the unit's text reads what it would without the PCH;
/// the macros of the unit without the PCH are simulated: the ones the
/// command gave, then the writes of the unit's own text and of each held
/// header it includes, in order. A difference is waved through only inside
/// the system library: a system header reading a macro only system headers
/// write (ISO C 7.1.2 lets standard headers be included in any order).
/// Grafts what each held header included on the unit's include tree.
class CompilationUnit::SharedHeaderJudge : public clang::PPCallbacks {
public:
    SharedHeaderJudge(CompilationUnit& unit, const SharedHeaderRecord& record) :
        unit(unit), sm(unit.SM()), pp(unit.instance->getPreprocessor()), record(record),
        entered(record.headers.size()) {
        for(auto [index, file]: views::enumerate(record.files)) {
            files.try_emplace(file.path, static_cast<std::uint32_t>(index));
        }
        held.assign(record.files.size(), none);
        for(auto [index, header]: views::enumerate(record.headers)) {
            held[header.file] = static_cast<std::uint32_t>(index);
        }
        for(auto& written: record.written) {
            if(written.initial != 0) {
                states[written.name] = written.initial;
            }
        }
        for(auto& initial: record.initial) {
            states[initial.name] = initial.state;
        }
    }

    void token(const clang::Token& token) {
        auto* info = token.getIdentifierInfo();
        if(!info || token.isAnnotation() || info->hasMacroDefinition()) {
            return;
        }
        check(info->getName(), 0, token.getLocation());
    }

private:
    void FileChanged(clang::SourceLocation,
                     FileChangeReason reason,
                     clang::SrcMgr::CharacteristicKind,
                     clang::FileID previous) override {
        // The headers the command forces in come first in both, after its
        // macros: clang leaves them out of a unit's predefines once the
        // PCH holds them.
        if(reason == ExitFile && previous == pp.getPredefinesFileID()) {
            for(auto header: record.forced) {
                if(!entered[header]) {
                    enter(header, previous, none, 0);
                }
            }
        }
    }

public:
    /// The parse is over: a held header the unit never included it would
    /// not have read.
    void finish() {
        for(auto [index, header]: views::enumerate(record.headers)) {
            if(!entered[index]) {
                reject(std::format("{} is in the PCH but never included",
                                   record.files[header.file].path));
                break;
            }
        }
        unit.grafted = std::move(grafted);
    }

private:

    void FileSkipped(const clang::FileEntryRef& entry,
                     const clang::Token& name,
                     clang::SrcMgr::CharacteristicKind) override {
        auto it = files.find(unit.file_path(entry));
        if(it == files.end() || held[it->second] == none) {
            return;
        }
        auto location = sm.getExpansionLoc(name.getLocation());
        auto includer = sm.getFileID(location);
        auto line = sm.getPresumedLoc(location, false).getLine();
        auto header = held[it->second];
        if(entered[header]) {
            grafted.push_back(
                {.includer = includer, .line = line, .file = it->second, .skipped = true});
            return;
        }
        enter(header, includer, none, line);
    }

    /// The unit includes `header` the first time: the header's run is
    /// checked, its writes applied, the held headers it includes entered
    /// in turn, and its includes grafted.
    void enter(std::uint32_t index, clang::FileID includer, std::uint32_t parent, std::uint32_t line) {
        entered.set(index);
        auto& header = record.headers[index];
        auto node = static_cast<std::uint32_t>(grafted.size());
        grafted.push_back({.includer = includer, .parent = parent, .line = line, .file = header.file});
        for(auto& read: header.reads) {
            if(state(read.name) != read.state && !(header.system && !project_written(read.name))) {
                reject(std::format("{} would read {} otherwise",
                                   record.files[header.file].path,
                                   spelling(read.name)));
            }
        }
        llvm::SmallVector<std::uint32_t> nodes(header.includes.size(), none);
        std::uint32_t written = 0;
        for(auto [i, include]: views::enumerate(header.includes)) {
            for(; written < include.writes; written += 1) {
                states[header.writes[written].name] = header.writes[written].state;
            }
            auto above = include.parent == none ? node : nodes[include.parent];
            if(include.held == none) {
                nodes[i] = static_cast<std::uint32_t>(grafted.size());
                grafted.push_back({.parent = above, .line = include.line, .file = include.file});
            } else if(entered[include.held]) {
                grafted.push_back(
                    {.parent = above, .line = include.line, .file = include.file, .skipped = true});
            } else {
                enter(include.held, {}, above, include.line);
            }
        }
        for(; written < header.writes.size(); written += 1) {
            states[header.writes[written].name] = header.writes[written].state;
        }
    }

    void MacroDefined(const clang::Token& name, const clang::MacroDirective* directive) override {
        write(name, directive->getMacroInfo());
    }

    void MacroUndefined(const clang::Token& name,
                        const clang::MacroDefinition&,
                        const clang::MacroDirective*) override {
        write(name, nullptr);
    }

    void MacroExpands(const clang::Token& name,
                      const clang::MacroDefinition& definition,
                      clang::SourceRange,
                      const clang::MacroArgs*) override {
        check(name, definition);
    }

    void Defined(const clang::Token& name,
                 const clang::MacroDefinition& definition,
                 clang::SourceRange) override {
        check(name, definition);
    }

    void Ifdef(clang::SourceLocation,
               const clang::Token& name,
               const clang::MacroDefinition& definition) override {
        check(name, definition);
    }

    void Ifndef(clang::SourceLocation,
                const clang::Token& name,
                const clang::MacroDefinition& definition) override {
        check(name, definition);
    }

    void Elifdef(clang::SourceLocation,
                 const clang::Token& name,
                 const clang::MacroDefinition& definition) override {
        check(name, definition);
    }

    void Elifndef(clang::SourceLocation,
                  const clang::Token& name,
                  const clang::MacroDefinition& definition) override {
        check(name, definition);
    }

    void If(clang::SourceLocation location, clang::SourceRange range, ConditionValueKind) override {
        condition_identifiers(pp, range, [&](llvm::StringRef name) { check(name, 0, location); });
    }

    void Elif(clang::SourceLocation location,
              clang::SourceRange range,
              ConditionValueKind,
              clang::SourceLocation) override {
        condition_identifiers(pp, range, [&](llvm::StringRef name) { check(name, 0, location); });
    }

    void write(const clang::Token& token, const clang::MacroInfo* now) {
        auto* info = token.getIdentifierInfo();
        auto name = name_hash(info->getName());
        names.try_emplace(name, info->getName());
        states[name] = unit.macro_state(now, *info);
        if(!sm.isInSystemHeader(sm.getExpansionLoc(token.getLocation()))) {
            own_project.insert(name);
        }
    }

    void check(const clang::Token& name, const clang::MacroDefinition& definition) {
        auto* info = name.getIdentifierInfo();
        check(info->getName(),
              unit.macro_state(definition.getMacroInfo(), *info),
              name.getLocation());
    }

    /// The unit's text reads the macro in the state `now`: the state the
    /// PCH left, unless the unit wrote it since.
    void check(llvm::StringRef spelling, std::uint64_t now, clang::SourceLocation location) {
        auto name = name_hash(spelling);
        auto it = ranges::lower_bound(record.written, name, {}, &SharedHeaderRecord::Written::name);
        if(it == record.written.end() || it->name != name || state(name) == now) {
            return;
        }
        location = sm.getExpansionLoc(location);
        if(sm.isInSystemHeader(location) && !it->project && !own_project.contains(name)) {
            return;
        }
        reject(std::format("{} would read {} otherwise", sm.getFilename(location), spelling));
    }

    std::uint64_t state(std::uint64_t name) const {
        return states.lookup(name);
    }

    bool project_written(std::uint64_t name) const {
        if(own_project.contains(name)) {
            return true;
        }
        auto it = ranges::lower_bound(record.written, name, {}, &SharedHeaderRecord::Written::name);
        return it != record.written.end() && it->name == name && it->project;
    }

    std::string spelling(std::uint64_t name) const {
        if(auto it = names.find(name); it != names.end()) {
            return it->second.str();
        }
        auto it = ranges::lower_bound(record.written, name, {}, &SharedHeaderRecord::Written::name);
        if(it != record.written.end() && it->name == name) {
            return it->spelling;
        }
        return std::format("the macro hashed {:016x}", name);
    }

    void reject(std::string reason) {
        if(unit.rejection.empty()) {
            unit.rejection = std::move(reason);
        }
    }

private:
    CompilationUnit& unit;

    clang::SourceManager& sm;

    clang::Preprocessor& pp;

    const SharedHeaderRecord& record;

    /// The record's files by path.
    llvm::StringMap<std::uint32_t> files;

    /// Per file of the record, the held header it is, none for no one.
    std::vector<std::uint32_t> held;

    /// The headers the unit included.
    llvm::BitVector entered;

    /// The macros of the unit without the PCH, by name; 0 for one none
    /// defines.
    llvm::DenseMap<std::uint64_t, std::uint64_t> states;

    /// The spelling of the macros the unit wrote.
    llvm::DenseMap<std::uint64_t, llvm::StringRef> names;

    /// The macros the unit's text outside the system headers wrote.
    llvm::DenseSet<std::uint64_t> own_project;

    std::vector<GraftedInclude> grafted;
};

std::unique_ptr<clang::PPCallbacks> CompilationUnit::shared_header_recorder() {
    assert(params.kind == CompilationKind::PrecompiledHeader);
    auto recorder = std::make_unique<SharedHeaderRecorder>(*this);
    auto* watch = recorder.get();
    watch_tokens([watch](const clang::Token& token) { watch->token(token); });
    after_parse.push_back([watch] { watch->finish(); });
    return recorder;
}

std::unique_ptr<clang::PPCallbacks> CompilationUnit::shared_header_judge() {
    auto judge = std::make_unique<SharedHeaderJudge>(*this, *params.shared);
    auto* watch = judge.get();
    watch_tokens([watch](const clang::Token& token) { watch->token(token); });
    after_parse.push_back([watch] { watch->finish(); });
    return judge;
}

}  // namespace clice
