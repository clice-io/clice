module clice;

import :compile.compilation;
import :compile.diagnostic;
import :compile.tidy;
import :feature.diagnostics;
import :index.indexing.tu_index;
import :project.background;
import :project.command;
import :project.graph.pch;
import :project.graph.pcm;
import :project.graph.precompiled;
import :project.graph.task_graph;
import :project.project;
import :server.client;
import :server.master;
import :server.served_project;
import :server.session;
import :settings.settings;
import :support.failure;
import :support.json;
import :support.logging;
import :support.text;
import :syntax.dependency_graph;
import :vfs.file_table;
import :vfs.path;
import :worker.preamble_index;
import :worker.protocol;

namespace clice {

/// The diagnostics the PCH's build raised in the preamble.
static std::vector<feature::FileDiagnostic>
    preamble_diagnostics(const worker::PreambleIndex& preamble) {
    std::vector<feature::FileDiagnostic> diagnostics;
    [[maybe_unused]] auto status =
        json::from_string<ipc::lsp_format>(preamble.diagnostics, diagnostics);
    return diagnostics;
}

/// The imports of a module several units provide, each resolved to the
/// nearest of them (scan_imports).
static std::vector<feature::ModuleChoice> module_choices(const Imports& imports,
                                                         const Project& project) {
    auto& graph = project.dep_graph;
    assert(ranges::size(imports.declared) == ranges::size(imports.offsets));
    std::vector<feature::ModuleChoice> choices;
    for(auto [dep, offset]: views::zip(imports.declared, imports.offsets)) {
        if(is_unresolved_import(dep)) {
            continue;
        }
        auto chosen = dep.file();
        auto name = graph.module_of(chosen);
        auto providers = graph.lookup_module(name);
        if(providers.size() < 2) {
            continue;
        }
        auto& choice = choices.emplace_back(feature::ModuleChoice{
            .module = name.str(),
            .chosen = project.file_table.display(chosen),
            .offset = offset,
        });
        for(auto provider: providers) {
            if(provider != chosen) {
                choice.others.push_back(project.file_table.display(provider));
            }
        }
    }
    return choices;
}

bool ServedProject::attribute_crash(const std::shared_ptr<Session>& session,
                                    bool tidy,
                                    const std::optional<std::string>& consumed,
                                    const ipc::Error& error) {
    // Under another project the document compiles with another command,
    // which this crash says nothing about (record_crash logs it).
    if(!owns(*session)) {
        record_crash(session,
                     feature::RequestKind::Compile,
                     worker::death_of(error),
                     error.message);
        return false;
    }
    auto path_id = session->path_id;
    auto file = project.file_table.resolve(path_id);
    auto& suspects = session->suspects;
    // The checks run in the parse (their preprocessor callbacks) and after
    // it: they are the first suspect, tried by a compile without them. One
    // that crashes as well clears them.
    if(tidy) {
        LOG_WARN("Compile crashed with clang-tidy on for {}; retrying without it", file);
        suspects.tidy = error;
        return true;
    }
    suspects.tidy.reset();
    // A death while reading a prebuilt pair may be the pair's fault: deep
    // corruption aborts the AST reader (report_fatal_error in the bitstream
    // reader) before any diagnostic can anchor, so the pch_suspect gate
    // never gets a say. The first such death retracts the pair and the
    // attempt runs again on a rebuilt one; a death on the rebuilt pair is the
    // document's own.
    if(consumed && suspects.pch != *consumed) {
        LOG_WARN("Compile crashed reading PCH pair {} for {}; retracting the pair",
                 *consumed,
                 file);
        suspects.pch = *consumed;
        project.pch_consume_failed(*consumed);
        return true;
    }
    record_crash(session, feature::RequestKind::Compile, worker::death_of(error), error.message);
    return false;
}

bool ServedProject::compile_stopped(const Session& session) const {
    auto now = Session::Clock::now();
    return session.crash_stops(feature::RequestKind::Compile, now) ||
           session.crash_stops(feature::RequestKind::PCH, now);
}

void ServedProject::retry_after_save(Session& session) {
    auto path_id = session.path_id;
    session.saved();
    // An AST compiled while a clang-tidy crash stopped the pass stays
    // current until the next edit: the save that retries the pass
    // recompiles.
    if(session.crash_record(feature::RequestKind::Tidy)) {
        project.graph.invalidate(ast_node(path_id));
    }
    // Typing never re-runs the self-containment check, yet an edit can
    // make a header lean on its includer: a save of text with errors, or of
    // a buffer whose compile has not finished yet, runs it again. A result
    // that stands holds for the text saved.
    auto selection = project.select_command(path_id, command_request(path_id));
    if(selection.self_containment) {
        auto compiled = latest_at(path_id, session.version);
        bool errs = !compiled || feature::count_errors(compiled->output->diagnostics.own) > 0;
        if(project.header_saved(*selection.self_containment,
                                project.file_table.intern_version(path_id, session.source->hash()),
                                errs)) {
            project.graph.invalidate(ast_node(path_id));
        }
    }
    // The modules it is and imports, directly or through other modules,
    // retry with it: a build that crashed or failed is otherwise refused
    // until what it read, or is known to have read, changes. A failed
    // build's inputs miss a lookup in a directory that did not exist yet,
    // so a save retries its failed preamble too. Either refusal leaves the
    // compile standing, so only a fresh attempt asks for the PCH or PCM again.
    bool retry = session.crash_record(feature::RequestKind::PCM) != nullptr;
    auto modules = project.module_nodes(session.path_id);
    llvm::DenseSet<NodeId> seen(modules.begin(), modules.end());
    auto add_imports = [&](NodeId importer) {
        for(auto module: imported_modules(project.graph, importer)) {
            if(seen.insert(module).second) {
                modules.push_back(module);
            }
        }
    };
    add_imports(ast_node(session.path_id));
    for(std::size_t i = 0; i < modules.size(); i += 1) {
        retry |= project.forget_module_failure(modules[i]);
        add_imports(modules[i]);
    }
    // A PCH that kept its preamble's errors misses the same lookups as a
    // failed build: a save rebuilds it.
    if(auto compiled = latest(path_id); compiled && compiled->pch_key) {
        if(compiled->pch_failed) {
            retry |= project.forget_pch_failure(*compiled->pch_key);
        } else if(auto& preamble = compiled->preamble_index;
                  preamble && ranges::any_of(preamble_diagnostics(*preamble),
                                             [](const feature::FileDiagnostic& diagnostic) {
                                                 return diagnostic.level >= DiagnosticLevel::Error;
                                             })) {
            project.discard_pch(*compiled->pch_key);
            retry = true;
        }
    }
    if(retry) {
        project.graph.invalidate(ast_node(path_id));
    }
}

void ServedProject::each_input(Fid path_id, llvm::function_ref<void(const Inputs&)> visit) const {
    auto* document = find_document(path_id);
    if(!document) {
        return;
    }
    if(document->inputs) {
        visit(*document->inputs);
    }
    if(auto& compiled = document->latest; compiled && compiled->loaded_pch()) {
        if(auto* pch = project.built_pch(*compiled->loaded_pch())) {
            visit(pch->inputs);
        }
    }
    for(auto module: imported_modules(project.graph, ast_node(path_id))) {
        for(auto* inputs: project.module_inputs(module)) {
            visit(*inputs);
        }
    }
}

void ServedProject::edited(Session& session) {
    auto path_id = session.path_id;
    project.graph.invalidate(ast_node(path_id));
    // The attempt still observes the real reply to its cancelled send
    // (crash accounting depends on it — contract 2).
    if(auto it = compile_interrupts.find(path_id); it != compile_interrupts.end()) {
        it->second->cancel();
    }
    // An edit is the first edit intent: from here on the document is
    // compiled.
    start_compiling(path_id);
}

void ServedProject::choice_changed(Session& session) {
    auto path_id = session.path_id;
    // Invalidate any in-flight compile: without the bump it would pass
    // its commit validity check and publish results for the outdated
    // identity.
    session.generation += 1;
    session.changed(Session::Clock::now());
    auto& document = *find_document(path_id);
    if(document.latest) {
        // The TU index describes the buffer under the old identity, though
        // it compiled the same bytes.
        update_latest(path_id, [](DocumentCompile& compiled) {
            compiled.pch_key.reset();
            compiled.pch_failed = false;
            compiled.base_pch_key.reset();
            compiled.preamble_index.reset();
            compiled.index.reset();
        });
    }
    document.inputs.reset();
    project.graph.invalidate(ast_node(path_id));
    if(auto it = compile_interrupts.find(path_id); it != compile_interrupts.end()) {
        it->second->cancel();
    }
}

void ServedProject::inputs_changed(Session& session) {
    // The in-flight attempt's token fires, but its parse keeps running: the
    // buffer is unchanged, so the product is worth publishing as bounded
    // staleness before the attempt reports Stale.
    project.graph.invalidate(ast_node(session.path_id));
    session.changed(Session::Clock::now());
}

void ServedProject::start_compiling(Fid path_id) {
    auto& document = *find_document(path_id);
    if(document.compiles || project.settings.project.readonly == ReadonlyMode::On) {
        return;
    }
    document.compiles = true;
}

void ServedProject::request_compile(std::shared_ptr<Session> session) {
    auto compile = [](ServedProject& self, std::shared_ptr<Session> session) -> async::task<> {
        co_await self.ensure_compiled(std::move(session));
    };
    if(!started_compiles.spawn(compile(*this, std::move(session)))) {
        LOG_DEBUG("The project stopped; a compile asked for is dropped");
    }
}

async::task<> ServedProject::stop_compiles() {
    loaded.set();
    for(auto& [path_id, interrupt]: compile_interrupts) {
        interrupt->cancel();
    }
    started_compiles.cancel();
    co_await started_compiles.join();
}

bool ServedProject::confirm_current(Session& session) {
    auto path_id = session.path_id;
    if(!current(path_id)) {
        return false;
    }
    // No early return: the changes one check finds cascade together.
    bool stale = false;
    each_input(path_id, [&](const Inputs& inputs) {
        stale = !inputs_hold(project.file_table, inputs) || stale;
    });
    if(!stale) {
        return true;
    }
    // A dependency changed on disk behind this document's back — the lazy
    // twin of the background ticks. The changes the check's looks queued
    // cascade first, then the document recompiles now, whether or not the
    // dependency graph knows the edge (a macro include) — both
    // synchronously: applied on a later turn, the cascade would void the
    // attempt about to compile the new content.
    server.apply_disk_changes();
    if(owns(session)) {
        inputs_changed(session);
    }
    return false;
}

async::task<bool> ServedProject::ensure_compiled(std::shared_ptr<Session> session,
                                                 EnsureOptions options) {
    auto path_id = session->path_id;

    LOG_DEBUG("ensure_compiled: path_id={} version={} gen={} current={}",
              path_id,
              session->version,
              session->generation,
              current(path_id));

    if(!loaded.is_set()) {
        co_await loaded.wait();
    }

    // A document closed or moved to another project never compiles here
    // again, and the join below cannot tell: a closed one's generation
    // stopped moving at the close.
    if(!owns(*session)) {
        co_return false;
    }

    // A document whose compile keeps killing workers waits for a change
    // or a save instead of feeding the same content to one more worker;
    // the crash note was published when the crash was recorded.
    if(compile_stopped(*session)) {
        LOG_DEBUG("ensure_compiled: {} is stopped by a crash", project.file_table.resolve(path_id));
        co_return false;
    }

    if(confirm_current(*session) && (resident(path_id) || !options.ast)) {
        co_return true;
    }
    // The changes confirm_current found can take the document to another
    // project: a context choice whose host stopped including it.
    if(!owns(*session)) {
        co_return false;
    }

    // Join the document's compile, retrying through stale attempts (an
    // invalidation mid-flight yields Stale and the next attempt recompiles)
    // until a terminal result; a current compile whose AST no worker holds
    // compiles again (compile_work's check). A buffer edit or session
    // replacement ends the join instead: the result would describe a buffer
    // that no longer exists, and the editor re-requests after an edit.
    auto gen = session->generation;
    auto result = co_await project.graph.join(
        ast_node(path_id),
        compile_work(path_id),
        {.foreground = true, .retry = [session, gen] { return session->generation == gen; }});
    co_return result == Result::Success;
}

Work ServedProject::compile_work(Fid path_id) {
    return {
        .run = [this, path_id](Attempt& attempt) { return compile(attempt, path_id); },
        // An eviction changes no input: the AST compiles again, and the
        // generation stays.
        .check = [this, path_id] { return resident(path_id) ? Check::Holds : Check::ProductGone; },
    };
}

/// One attempt of an open document's compile, in its steps: the command,
/// the modules, the PCH, the send, the reply, the commit. At most two sends
/// compile the document: a header no self-containment check says compiles
/// alone compiles without its includer's context first, the check, and again
/// with the context when its diagnostics say it needs it — the check's
/// diagnostics are never published. A pair the parse could not read is
/// retracted, and the same send runs once more.
///
/// Each step returns the attempt's result when the attempt ends there, none
/// when it goes on.
struct ServedProject::CompileAttempt {
    CompileAttempt(ServedProject& served, Attempt& attempt, std::shared_ptr<Session> session) :
        served(served), project(served.project), attempt(attempt), session(session),
        path_id(session->path_id), ticket(Ticket::take(session)),
        file_path(project.file_table.resolve(path_id)),
        compile_dispatch(*session, feature::RequestKind::Compile),
        pch_retry(session->crash_record(feature::RequestKind::PCH) != nullptr),
        pch_dispatch(*session, feature::RequestKind::PCH) {}

    async::task<Result> run();

    /// The command the send compiles under, the include trees the header's
    /// host search needs taken first: a header compiles under the host every
    /// process finds for it. `first`: the send that may be the
    /// self-containment check.
    async::task<std::optional<Result>> command(bool first);

    /// The modules the buffer imports, built: an import whose build failed is
    /// left to the parse, which reports it on the import next to the file's
    /// own diagnostics; one whose build crashed a worker also goes into the
    /// document's records, and is refused until the document's record lets
    /// it retry.
    async::task<std::optional<Result>> modules();

    /// The PCHs the parse loads, built, and the preamble state the worker
    /// seeds its inactive regions from. A preamble whose build crashed a
    /// worker stops the attempt before the parse, which would feed the same
    /// preamble to one more worker.
    async::task<std::optional<Result>> pch();

    /// Send the parse to the worker holding the document, and take its
    /// reply into `reply`: the PCH the parse could not read retracted,
    /// `resend` set once for it; a reply that is no result ends the
    /// attempt.
    async::task<std::optional<Result>> send(worker::CompileResult& reply, bool& resend);

    /// What the reply raised, with what clice knows of how the parse ran:
    /// the preamble's diagnostics, the imports that failed or that several
    /// modules provide, a clang-tidy configuration that does not parse.
    void assemble(std::vector<feature::FileDiagnostic>& diagnostics);

    /// Install the compile's products with no suspension before the result
    /// becomes visible (contract 16): a joiner must never see Success before
    /// the latest compile describes the buffer it compiled. One invalidated
    /// mid-flight commits the same products as bounded staleness — recorded,
    /// published, but not current — and reports Stale so waiters drive the
    /// recompile.
    Result commit(worker::CompileResult& reply);

    ServedProject& served;

    Project& project;

    Attempt& attempt;

    std::shared_ptr<Session> session;

    Fid path_id;

    /// The attempt's content identity. The graph's attempt identity covers
    /// invalidation (attempt.current()); this covers the buffer itself — a
    /// didChange or session replacement makes every product describe text
    /// that no longer exists, so the commit discards wholesale, while an
    /// invalidation that left the buffer as it was still publishes salvage.
    Ticket ticket;

    std::string file_path;

    Timer timer;

    /// An attempt past the gate on a crashed compile or preamble is its
    /// retry, and stops the others until its result (Session invariant 2).
    Session::Dispatch compile_dispatch;

    bool pch_retry;

    Session::Dispatch pch_dispatch;

    /// clang-tidy runs where `clice lint` checks, unless its configuration
    /// does not parse, which `clice lint` refuses too, a crash of its is
    /// suspected or stops it, or the compile retries a crash it is cleared of
    /// (on a retracted preamble, or after the document's own). The places
    /// resolving its configuration looked at join the deps either way: a fix
    /// there is a change that retries a stopped pass.
    std::optional<tidy::TidyResolution> tidy_config;

    std::optional<Session::Dispatch> tidy_dispatch;

    std::vector<std::uint64_t> known_instantiations;

    /// What the current send compiles.
    worker::CompileParams params;

    Resolution resolution;

    /// The self-containment check this send is.
    std::optional<SelfContainmentKey> self_containment;

    BuiltImports imports;

    BuiltPCH built;

    /// The pch.idx index of the preamble's PCH the parse loads.
    std::shared_ptr<worker::PreambleIndex> preamble_index;

    /// Where a worker writes an index too large for its reply.
    std::optional<CacheStore::PendingEntry> transfer;

    /// The pair the parse could not read was retracted, and sent once more.
    bool pch_resent = false;
};

async::task<Result> ServedProject::CompileAttempt::run() {
    using enum feature::RequestKind;
    LOG_DEBUG("Compiling {} (generation {})",
              project.file_table.resolve(path_id),
              ticket.generation);
    served.enter_phase(path_id, CompilePhase::Command);
    if(project.settings.diagnostics.clang_tidy &&
       project.rules.lintable(project.file_table.resolve(path_id))) {
        tidy_config = tidy::resolve_tidy_params(project.file_table.spelling(path_id));
        auto& suspects = session->suspects;
        if(tidy_config->error.empty() && !suspects.tidy && suspects.pch.empty() &&
           !session->crash_record(Compile) && !session->crash_stops(Tidy, Session::Clock::now())) {
            tidy_dispatch.emplace(*session, Tidy);
        }
    }
    known_instantiations =
        project.index.known_instantiations(path_id, project.dep_graph.include_closure(path_id));

    /// A header whose check compile alone has errors compiles in its host's
    /// context too: the one with fewer errors stands, the header alone on a
    /// tie, which a third send compiles again for the worker to hold. A
    /// context that could not be synthesized (the include chain not followed)
    /// compares nothing: the header stays in need of it, which its
    /// diagnostics say.
    struct Comparison {
        SelfContainmentKey key;
        std::uint32_t errors;
        VersionID text;
        Inputs inputs;
    };
    std::optional<Comparison> comparison;
    // The alone compile's verdict only steers the host's: an attempt ending
    // before the host's compile answers leaves the result unknown, for the
    // next attempt to check again.
    auto unfinished = llvm::make_scope_exit([&] {
        if(comparison) {
            project.forget_self_containment(comparison->key);
        }
    });
    bool first = true;
    while(true) {
        if(auto stop = co_await command(first)) {
            co_return *stop;
        }
        if(auto stop = co_await modules()) {
            co_return *stop;
        }
        if(auto stop = co_await pch()) {
            co_return *stop;
        }
        worker::CompileResult reply;
        bool resend = false;
        if(auto stop = co_await this->send(reply, resend)) {
            co_return *stop;
        }
        if(resend) {
            continue;
        }
        first = false;

        auto& diagnostics = reply.diagnostics;
        assemble(diagnostics);

        // The check's result stands for every process while what it read
        // holds; typing never runs it again (self_contained), a save may.
        // The parse counts nothing the preamble's build raised.
        auto errors = reply.errors;
        if(preamble_index && preamble_index->matches(params.text)) {
            errors += static_cast<std::uint32_t>(
                feature::count_errors(preamble_diagnostics(*preamble_index)));
        }
        auto read = [&] {
            auto inputs = intern_inputs(project.file_table, reply.deps, reply.started_at);
            // The compile reports nothing it read through the PCH, nor the
            // includer chain a synthesized context embeds.
            if(auto* pch = built.loaded ? project.built_pch(*built.loaded) : nullptr) {
                inputs.append(pch->inputs.begin(), pch->inputs.end());
            }
            inputs.append(resolution.inputs.begin(), resolution.inputs.end());
            return inputs;
        };
        if(self_containment) {
            auto inputs = read();
            auto text = project.file_table.intern_version(path_id, session->source->hash());
            project.commit_self_containment(*self_containment, errors == 0, text, inputs);
            if(errors > 0) {
                LOG_INFO("{} has errors compiled on its own; compiling it in its host's context",
                         file_path);
                comparison = {*self_containment, errors, text, std::move(inputs)};
                continue;
            }
        } else if(comparison) {
            // The verdict rests on both compiles: a change to what either
            // read compares again.
            auto alone = std::exchange(comparison, std::nullopt);
            auto inputs = std::move(alone->inputs);
            auto in_host = read();
            inputs.append(in_host.begin(), in_host.end());
            bool self_contained = resolution.synthesized && errors >= alone->errors;
            project.commit_self_containment(alone->key,
                                            self_contained,
                                            alone->text,
                                            std::move(inputs));
            if(self_contained) {
                LOG_INFO("{} has no fewer errors in its host's context; compiling it on its own",
                         file_path);
                continue;
            }
        }
        LOG_INFO("Compiled {} (version {}) in {} ms: {}",
                 file_path,
                 params.version,
                 timer.ms(),
                 counted(diagnostics.size(), "diagnostic", "diagnostics"));
        co_return commit(reply);
    }
}

async::task<std::optional<Result>> ServedProject::CompileAttempt::command(bool first) {
    params = {};
    params.indexing.known_instantiations = known_instantiations;
    params.indexing.standard_library_instantiations =
        project.settings.project.standard_library_instantiations;
    params.skip_bodies = project.settings.project.skip_header_bodies;
    params.path = file_path;
    params.version = session->version;
    params.text = session->text().str();
    params.project_root = project.root.str();
    if(tidy_dispatch) {
        params.tidy = tidy_config->params;
    }
    resolution = co_await project.resolve_with_trees(
        path_id,
        [&] { return served.command_request(path_id); },
        [&] { return ticket.fresh(); });
    if(!ticket.fresh()) {
        co_return Result::Stale;
    }
    params.directory = resolution.directory;
    params.arguments = resolution.arguments;
    if(resolution.synthesized) {
        params.synthesized = resolution.synthesized->files;
    }
    params.hidden = resolution.hidden;
    self_containment =
        first && !resolution.self_contained ? resolution.self_containment : std::nullopt;
    co_return std::nullopt;
}

async::task<std::optional<Result>> ServedProject::CompileAttempt::modules() {
    auto kind = feature::RequestKind::PCM;
    served.enter_phase(path_id, CompilePhase::Modules);
    // FIXME: an edit within the spacing of the module's crash is spent here:
    // the attempt runs without the module and finishes current, so the module waits
    // for the next edit or save, not for the spacing to pass.
    bool retry = session->crash_record(kind) && !session->crash_stops(kind, Session::Clock::now());
    Session::Dispatch pcm_dispatch(*session, kind);
    auto waited = co_await project.build_imports(path_id,
                                                 params.text,
                                                 resolution,
                                                 {.attempt = &attempt, .retry_crashed = retry});
    if(!waited) {
        co_return Result::Stale;
    }
    imports = std::move(*waited);
    bool crashed = false;
    for(auto& crash: imports.refused) {
        // Booked once: an attempt on unchanged inputs is no new crash.
        if(!session->crash_record(kind)) {
            served.record_crash(session, kind, crash.death, crash.cause);
        }
        crashed = true;
    }
    for(auto module: imports.failed) {
        if(auto* crash = project.module_build_crash(module)) {
            served.record_crash(session, kind, crash->death, crash->cause);
            crashed = true;
        }
    }
    if(!crashed) {
        session->succeeded(kind, ticket.generation);
    }
    if(!ticket.fresh()) {
        LOG_DEBUG("The compile of {} is outdated before its PCH; dropped", file_path);
        co_return Result::Stale;
    }
    co_return std::nullopt;
}

async::task<std::optional<Result>> ServedProject::CompileAttempt::pch() {
    using feature::RequestKind::PCH;
    if(project.settings.project.readonly != ReadonlyMode::On) {
        served.enter_phase(path_id, CompilePhase::Preamble);
    }
    auto waited = co_await project.build_pch(path_id,
                                             params.text,
                                             std::nullopt,
                                             resolution,
                                             imports.imports,
                                             {.attempt = &attempt, .retry_crashed = pch_retry});
    if(!waited) {
        co_return Result::Stale;
    }
    built = std::move(*waited);
    if(built.preamble) {
        auto* crash = built.refused                    ? &*built.refused
                      : built.result == Result::Failed ? project.pch_build_crash(*built.preamble)
                                                       : nullptr;
        if(crash) {
            served.record_crash(session, PCH, crash->death, crash->cause);
            co_return Result::Failed;
        }
        // Gated on the attempt's own validity: an edit or an invalidation
        // while it waited means the resolved command may
        // describe nothing — the record it clears belongs to this attempt no
        // more. A proven-good PCH clears the document's record as surely as
        // building one, and a build that failed without a crash answered:
        // the preamble no longer crashes. Every consumer clears for itself.
        if(attempt.current() && ticket.fresh()) {
            session->succeeded(PCH, ticket.generation);
        }
    } else {
        // No PCH build left to crash on.
        session->succeeded(PCH, ticket.generation);
    }
    params.pch = built.pch;
    params.pcms = project.imported_pcms(imports.imports.resolved());

    if(!ticket.fresh()) {
        LOG_DEBUG("The compile of {} is outdated before it is sent; dropped", file_path);
        co_return Result::Stale;
    }

    // Seed the worker's inactive-region state from the PCH's preamble: the
    // conditional stack it left open (a #if cut by the bound) and the
    // regions of the preamble share itself. Copy the state out: concurrent
    // compiles can insert into Project::pchs across the send and rehash the
    // map from under a held pointer.
    preamble_index = built.loaded && built.loaded == built.preamble
                         ? project.preamble_index(*built.loaded)
                         : nullptr;
    if(preamble_index) {
        params.preamble_inactive_regions = preamble_index->inactive_regions;
        params.open_conditionals = preamble_index->open_conditionals;
    }
    co_return std::nullopt;
}

async::task<std::optional<Result>> ServedProject::CompileAttempt::send(worker::CompileResult& reply,
                                                                       bool& resend) {
    served.enter_phase(path_id, CompilePhase::Parsing);

    // The send carries the interrupt, not the attempt's advisory token: the
    // master must observe the request's real result — the crash accounting
    // below depends on it (contract 2) — and a cancelled send still awaits
    // the worker's answer. Only an edit, a choice change or a stop
    // interrupts the parse (see edited, choice_changed and stop_compiles),
    // and the stale reply is discarded at the validity gate below. A death
    // that is not this compile's doing resends it once — the attempt only
    // stops early once the buffer moved on, since the next attempt compiles
    // that.
    //
    // Crash accounting runs even for outdated attempts: the crash came from
    // content this document dispatched, and skipping it would let a poison
    // file dodge its stop by being edited between dispatch and the crash
    // response (contract 12).
    bool retry_without_suspect = false;
    bool died = false;
    auto interrupt = std::make_unique<async::cancellation_source>();
    auto interrupt_token = interrupt->token();
    auto* own_interrupt = interrupt.get();
    served.compile_interrupts[path_id] = std::move(interrupt);
    auto release_interrupt = llvm::make_scope_exit([&] {
        auto it = served.compile_interrupts.find(path_id);
        if(it != served.compile_interrupts.end() && it->second.get() == own_interrupt) {
            served.compile_interrupts.erase(it);
        }
    });
    if(project.store) {
        transfer = project.store->begin_transfer();
        params.indexing.output_path = transfer->tmp_path;
    }
    auto& pool = served.server.pool;
    auto result = co_await deliver(
        pool,
        true,
        [&]() -> RequestResult<worker::CompileParams> {
            if(!ticket.fresh()) {
                co_await async::fail(
                    ipc::Error{worker::dispatch_errc::cancelled, "Compile outdated"});
            }
            co_return co_await pool.send_stateful(path_id.raw, params, {.token = interrupt_token});
        },
        [&](const ipc::Error& error) {
            died = true;
            retry_without_suspect =
                served.attribute_crash(session, params.tidy.has_value(), built.loaded, error);
        });
    if(retry_without_suspect) {
        co_return Result::Stale;
    }

    if(!ticket.fresh()) {
        LOG_DEBUG("The compile of {} is outdated by its reply; dropped", file_path);
        co_return Result::Stale;
    }

    if(!result.has_value()) {
        if(served.server.shutting_down()) {
            LOG_DEBUG("The compile of {} stopped with the server", file_path);
        } else if(died) {
            LOG_DEBUG("The compile of {} did not complete: {}", file_path, result.error().message);
        } else if(worker::is_operational_error(result.error())) {
            LOG_WARN("The compile of {} did not complete: {}", file_path, result.error().message);
        } else {
            // The worker accepts arbitrary user code; a non-operational
            // failure at this layer is IPC/worker breakage, never a
            // user-code problem.
            LOG_ANOMALY(CompileFail,
                        "Compile failed for {}: {}",
                        file_path,
                        result.error().message);
        }
        // Short of a crash of its own (published as it was recorded), the
        // document keeps what it showed: a worker outage or a second death
        // of somebody else's doing says nothing about its code.
        co_return Result::Failed;
    }
    reply = std::move(*result);

    // The PCH quality gate: a failed parse whose diagnostics name the
    // consumed PCH (worker-side pch_suspect — setup failure and fatal error
    // alike). The PCH attempt validated the pair fresh via its deps, yet the
    // frontend could not read it — the bytes on disk are the suspect.
    // Retract the pair (store + cache) and send once more; the PCH's next
    // attempt misses and rebuilds both halves. Without this write-back a
    // corrupt blob is trusted for the life of the store and the file stays
    // broken on every restart. A setup failure whose diagnostics do NOT name
    // the blob (bad invocation, broken module input) deliberately falls
    // through to the non-Done path below: retracting a healthy shared PCH
    // over someone else's failure would rebuild it on every request for as
    // long as that failure persists.
    if(reply.pch_suspect && built.loaded) {
        LOG_WARN("Compile could not read PCH pair {} for {}; retracting the pair",
                 *built.loaded,
                 file_path);
        // Retract unconditionally — an unreadable pair never survives, even
        // when its consumption is stopped — but resend only once. The
        // consumption's record bounds the cross-attempt shape: this attempt
        // finishes Stale (waiting on the retracted key again finds it
        // changed, which overtakes this attempt), the waiter starts another,
        // and without the stopped key each would rebuild and fail forever.
        project.pch_consume_failed(*built.loaded);
        if(!pch_resent) {
            pch_resent = true;
            resend = true;
            co_return std::nullopt;
        }
        // The rebuilt pair is unreadable again: the storage itself is
        // failing, and another rebuild would fare no better. The attempt
        // proceeds — a Done reply publishes its real fatal diagnostics, a
        // non-Done one falls to the honest gap below.
    }

    // A non-Done reply past the gate is a non-result: committing it would
    // freeze the document on a product that never existed — no index, an
    // empty deps snapshot nothing can invalidate. Outdated attempts were
    // already discarded at the validity gate above, so this attempt's
    // inputs are broken in a way a PCH rebuild cannot fix.
    if(reply.status != worker::CompileStatus::Done) {
        LOG_WARN("Compile produced no result for {} (status={})",
                 file_path,
                 static_cast<int>(reply.status));
        // The latest compile describes no generation: the next request
        // recompiles instead of trusting the phantom product. What is
        // published is the honest gap: why the compile failed before
        // parsing, of this text, else versionless empty diagnostics rather
        // than a stale list posing as current.
        bool setup_fail = reply.status == worker::CompileStatus::SetupFail;
        served.update_latest(path_id, [&](DocumentCompile& compiled) {
            compiled.generation.reset();
            compiled.output = CompileOutput{
                .version = setup_fail ? std::optional(session->version) : std::nullopt,
                .text = session->source,
                .diagnostics =
                    {
                                  .source = resolution.ref.source,
                                  .own = std::move(reply.diagnostics),
                                  .query_failure = resolution.query_failure,
                                  },
                .failed = setup_fail,
            };
        });
        served.push_output(*session, true);
        co_return Result::Failed;
    }

    // A check invalidated mid-flight is discarded whole: its result is a
    // conditional write like the commit's, and its diagnostics come from a
    // compile deliberately run without includer context — they are never
    // published, including on this path.
    if(self_containment && !attempt.current()) {
        LOG_INFO("Discarding invalidated self-containment check for {}", file_path);
        co_return Result::Stale;
    }
    co_return std::nullopt;
}

void ServedProject::CompileAttempt::assemble(std::vector<feature::FileDiagnostic>& diagnostics) {
    if(preamble_index && preamble_index->matches(params.text)) {
        diagnostics =
            feature::with_preamble(std::move(diagnostics), preamble_diagnostics(*preamble_index));
    }
    feature::explain_failed_imports(diagnostics,
                                    project.module_failures(imports.imports.resolved()));
    for(auto& choice: module_choices(imports.imports, project)) {
        diagnostics.push_back(feature::ambiguous_module(choice, session->text()));
    }
    if(tidy_config && !tidy_config->error.empty()) {
        diagnostics.insert(diagnostics.begin(), feature::invalid_tidy_config(tidy_config->error));
    }
}

Result ServedProject::CompileAttempt::commit(worker::CompileResult& reply) {
    using enum feature::RequestKind;
    bool still_current = attempt.current();

    // The parse read the pair and completed without finding it unreadable —
    // and the attempt still describes live inputs: the key's consumption
    // failures were transient, not the storage. An overtaken attempt clears
    // nothing: what overtook it may BE another consumer's fresh failure to
    // read the shared key, and clearing here would reset the count it just
    // paid for.
    if(still_current && built.loaded && !reply.pch_suspect) {
        project.pch_consume_succeeded(*built.loaded);
    }

    auto& diagnostics = reply.diagnostics;
    auto next = std::make_shared<DocumentCompile>();
    next->generation = attempt.generation();
    bool pch_failed = built.preamble && built.result == Result::Failed;
    next->pch_key = pch_failed ? built.preamble : built.loaded;
    next->pch_failed = pch_failed;
    next->base_pch_key = built.base;
    next->preamble_index = preamble_index;
    next->host = resolution.host;
    next->command = resolution.ref.command;
    next->inactive_regions = std::move(reply.inactive_regions);
    // The AST and the file index commit together — that pairing is what lets
    // navigation trust the index after ensure_compiled. A compile that
    // produced no index data (no AST, or an index it could not send) must
    // therefore drop the previous buffer's index rather than leave it posing
    // as current: an honest gap over yesterday's offsets.
    if(reply.index_in_file) {
        auto& tmp_path = transfer->tmp_path;
        // Copied, not mapped: the index outlives the transfer file.
        auto file = fs::File::open(tmp_path);
        auto read = file ? llvm::MemoryBuffer::getOpenFile(file->native_handle(),
                                                           tmp_path,
                                                           /*FileSize=*/-1,
                                                           /*RequiresNullTerminator=*/false,
                                                           /*IsVolatile=*/true)
                         : file.error();
        if(read) {
            next->index =
                std::make_shared<index::TUIndex>(index::TUIndex::from_buffer(std::move(*read)));
        } else {
            diagnostics.push_back(feature::index_unavailable(
                std::format("reading this file's index from {} failed: {}",
                            tmp_path,
                            read.getError().message())));
        }
    } else if(!reply.tu_index_data.empty()) {
        next->index = std::make_shared<index::TUIndex>(index::TUIndex::from_buffer(
            llvm::MemoryBuffer::getMemBufferCopy(as_chars(reply.tu_index_data))));
    }

    LOG_PERF("request", "kind=compile file={} total_ms={:.2f}", file_path, timer.precise_ms());
    auto unconfirmed = params.skip_bodies ? feature::take_unconfirmed(diagnostics)
                                          : std::vector<feature::FileDiagnostic>();
    next->output = CompileOutput{
        .version = session->version,
        .text = session->source,
        .diagnostics =
            {
                          .source = resolution.ref.source,
                          .own = std::move(diagnostics),
                          .unconfirmed = std::move(unconfirmed),
                          .unmatched_host = resolution.unmatched_host.valid()
                                      ? project.file_table.display(resolution.unmatched_host)
                                      : std::string(),
                          .query_failure = resolution.query_failure,
                          },
    };
    next->output->diagnostics.skipped = served.shown_skipped_diagnostics(*session, *next->output);

    // The fresh ticket the send checked holds the document here: a move bumps
    // the generation, and nothing suspends since.
    auto& entry = *served.find_document(path_id);
    // A document moving off a preamble it failed on releases that failure:
    // only the preamble's consumers would ever retry it. A crash stays, for
    // the next consumer to book without a build.
    // FIXME: released even while another open document still fails on that
    // preamble: its next compile runs the failing build once more.
    if(entry.latest && entry.latest->pch_failed && entry.latest->pch_key != next->pch_key &&
       !project.pch_build_crash(*entry.latest->pch_key)) {
        project.forget_pch_failure(*entry.latest->pch_key);
    }
    entry.latest = std::move(next);
    if(tidy_config) {
        reply.deps.append_range(tidy_config->files);
    }
    // A compile that finishes clears its suspects: the pair it read was at
    // fault, or the pass it ran without.
    auto& suspects = session->suspects;
    suspects.pch.clear();
    auto tidy_crash = std::exchange(suspects.tidy, std::nullopt);
    if(tidy_dispatch) {
        session->succeeded(Tidy, ticket.generation);
    } else if(tidy_crash) {
        served.record_crash(session, Tidy, worker::death_of(*tidy_crash), tidy_crash->message);
    }
    // The files the synthesized context embeds are read too, though clang
    // never opens them.
    entry.inputs = intern_inputs(project.file_table, reply.deps, reply.started_at);
    entry.inputs->append(resolution.inputs.begin(), resolution.inputs.end());
    session->succeeded(Compile, ticket.generation);
    served.push_output(*session, true);
    project.background_index.schedule();
    return still_current ? Result::Success : Result::Stale;
}

async::task<Result> ServedProject::compile(Attempt& attempt, Fid path_id) {
    // Only a join for a document of this project starts an attempt, and the
    // attempt gets here before its first suspension: nothing can have closed
    // or moved it.
    assert(find_document(path_id) && "AST attempt for a document not this project's");
    auto session = server.find_session(path_id);
    auto idle = llvm::make_scope_exit([&] { enter_phase(path_id, CompilePhase::Idle); });
    // Covers every spawn path, including waiter-driven respawns: content
    // that just crashed a worker must not reach one more.
    if(compile_stopped(*session)) {
        co_return Result::Failed;
    }
    CompileAttempt run(*this, attempt, std::move(session));
    co_return co_await run.run();
}

void ServedProject::adopt_pch(const Ticket& ticket,
                              std::uint64_t generation,
                              const BuiltPCH& built) {
    auto& session = ticket.session;
    auto path_id = session->path_id;
    auto kind = feature::RequestKind::PCH;
    // A preamble that crashed a worker stops the document's content builds
    // (the caller checks); only a compile the document's record lets retry
    // builds it again.
    if(built.refused) {
        // Booked once: a request on unchanged inputs is no new crash.
        if(!session->crash_record(kind)) {
            record_crash(session, kind, built.refused->death, built.refused->cause);
        }
        return;
    }
    if(built.preamble && built.result != Result::Success) {
        if(auto* crash = project.pch_build_crash(*built.preamble)) {
            record_crash(session, kind, crash->death, crash->cause);
        }
        return;
    }
    // Adopting a proven-good PCH clears the document's record as surely as
    // building one — but only its own; every consumer clears for itself.
    if(!ticket.fresh() || project.graph.generation(ast_node(path_id)) != generation ||
       built.partial) {
        return;
    }
    auto preamble = built.preamble ? project.preamble_index(*built.preamble) : nullptr;
    update_latest(path_id, [&](DocumentCompile& compiled) {
        compiled.pch_key = built.preamble ? built.preamble : built.base;
        compiled.pch_failed = false;
        compiled.base_pch_key = built.base;
        compiled.preamble_index = std::move(preamble);
    });
    session->succeeded(kind, ticket.generation);
}

void ServedProject::enter_phase(Fid path_id, CompilePhase phase) {
    if(server.client) {
        server.client->report_compile_phase(path_id, phase);
    }
}

}  // namespace clice
