module clice;

import :compile.compilation;
import :compile.diagnostic;
import :compile.tidy;
import :feature.diagnostics;
import :index.tu_index;
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
import :support.anomaly;
import :support.bytes;
import :support.failure_record;
import :support.json;
import :support.logging;
import :support.timer;
import :syntax.dependency_graph;
import :vfs.file_table;
import :vfs.path;
import :worker.protocol;

namespace clice {

/// The diagnostics the PCH's build raised in the preamble.
static std::vector<feature::FileDiagnostic> preamble_diagnostics(const index::TUIndex& preamble) {
    std::vector<feature::FileDiagnostic> diagnostics;
    [[maybe_unused]] auto status =
        json::from_string<ipc::lsp_config>(preamble.preamble_diagnostics(), diagnostics);
    return diagnostics;
}

/// Why each import that failed to build did not.
static std::vector<feature::ModuleFailure> module_failures(llvm::ArrayRef<Fid> imports,
                                                           const Project& project) {
    std::vector<feature::ModuleFailure> failures;
    for(auto module: imports) {
        if(auto failure = project.module_failure(module)) {
            failures.push_back(std::move(*failure));
        }
    }
    return failures;
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

std::optional<PCHRequest> ServedProject::declared_pch(Fid path_id,
                                                      const Resolution& resolution,
                                                      const Imports& imports) {
    auto unit = project.pch_writer(path_id, resolution);
    if(!unit) {
        return std::nullopt;
    }
    return project.declared_pch_request(*unit,
                                        project.settings.project.skip_header_bodies &&
                                            !imports.modular());
}

void ServedProject::record_crash(const std::shared_ptr<Session>& session,
                                 feature::RequestKind kind,
                                 llvm::StringRef death,
                                 std::string cause) {
    auto path_id = session->path_id;
    // A document that left the project compiles under another command now:
    // what crashed here is no record of its.
    if(!owns(*session)) {
        LOG_WARN("{} crashed a worker after it left the project: {}",
                 project.file_table.resolve(path_id),
                 cause);
        return;
    }
    bool shown = session->crashed(kind, death, std::move(cause), Session::Clock::now());
    auto* record = session->crash_record(kind);
    LOG_WARN("{} crashed a worker: {}",
             project.file_table.resolve(path_id),
             feature::crash_note({.kind = kind,
                                  .cause = record->cause,
                                  .count = record->count,
                                  .save_only = record->save_only(Session::policy)})
                 .message);
    // A silent first crash leaves the published state alone (see Session
    // invariant 4).
    if(!shown) {
        return;
    }
    // A stopped compile leaves no AST behind the old diagnostics: they go,
    // and the note says why.
    auto previous = latest(path_id);
    if(previous && previous->output.has_value() &&
       (kind == feature::RequestKind::Compile || kind == feature::RequestKind::PCH)) {
        set_output(path_id,
                   CompileOutput{
                       .text = session->source,
                       .diagnostics = {.source = previous->output->diagnostics.source},
                   });
    }
    republish(session);
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

void ServedProject::republish(const std::shared_ptr<Session>& session) {
    if(!owns(*session)) {
        return;
    }
    if(auto previous = latest(session->path_id); !previous || !previous->output.has_value()) {
        set_output(session->path_id, CompileOutput{.text = session->source});
    }
    push_output(*session, true);
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
        invalidate(path_id);
    }
    // Typing never re-runs the self-containment check, yet an edit can
    // make a header lean on its includer: a save showing what an includer
    // would provide, or saving a buffer whose compile has not finished yet,
    // runs it again. A result that stands holds for the text saved.
    auto selection =
        project.select_command(path_id, {.choice = active_choice(path_id), .open = true});
    if(selection.self_containment) {
        auto compiled = latest_at(path_id, session.version);
        bool leans =
            !compiled || feature::missing_context_errors(compiled->output->diagnostics.own) > 0;
        if(project.header_saved(*selection.self_containment,
                                project.file_table.intern_version(path_id, session.hash),
                                leans)) {
            invalidate(path_id);
        }
    }
    // The modules it is and imports, directly or through other modules,
    // retry with it: a build that crashed or failed is otherwise refused
    // until what it read, or is known to have read, changes. A failed
    // build's inputs miss a lookup in a directory that did not exist yet,
    // so a save retries its failed preamble too. Either refusal leaves the
    // compile standing, so only a fresh attempt asks for the PCH or PCM again.
    bool retry = session.crash_record(feature::RequestKind::PCM) != nullptr;
    llvm::SmallVector<Fid> modules{session.path_id};
    llvm::DenseSet<Fid> seen{session.path_id};
    auto add_imports = [&](NodeId importer) {
        for(auto dep: project.graph.dependencies(importer)) {
            if(dep.kind != NodeKind::PCM || is_unresolved_import(dep)) {
                continue;
            }
            auto module = dep.file();
            if(seen.insert(module).second) {
                modules.push_back(module);
            }
        }
    };
    add_imports(node(session.path_id));
    for(std::size_t i = 0; i < modules.size(); i += 1) {
        retry |= project.forget_build_failure(modules[i]);
        add_imports(module_node(modules[i]));
    }
    // A PCH that kept its preamble's errors misses the same lookups as a
    // failed build: a save rebuilds it.
    if(auto compiled = latest(path_id); compiled && compiled->pch_key) {
        if(compiled->pch_failed) {
            retry |= project.forget_build_failure(*compiled->pch_key);
        } else if(auto preamble = project.preamble_index(*compiled->pch_key);
                  preamble && ranges::any_of(preamble_diagnostics(*preamble),
                                             [](const feature::FileDiagnostic& diagnostic) {
                                                 return diagnostic.level >= DiagnosticLevel::Error;
                                             })) {
            project.discard_pch(*compiled->pch_key);
            retry = true;
        }
    }
    if(retry) {
        invalidate(path_id);
    }
}

std::vector<feature::FileDiagnostic>
    ServedProject::shown_skipped_diagnostics(const Session& session,
                                             const CompileOutput& output) const {
    auto it = skipped.find(session.path_id);
    if(it == skipped.end() || it->second.text_hash != session.hash ||
       output.diagnostics.source != CommandSource::Database || active_choice(session.path_id)) {
        return {};
    }
    return feature::confirmed_skipped(it->second.diagnostics, output.diagnostics);
}

bool ServedProject::lacks_skipped_diagnostics(Fid path_id) const {
    auto* document = find_document(path_id);
    if(!document || document->serving != ServingMode::Escalated ||
       project.entries(path_id).empty()) {
        return false;
    }
    auto it = skipped.find(path_id);
    return it == skipped.end() || it->second.text_hash != document->session->hash;
}

void ServedProject::report_skipped_diagnostics(Fid path_id,
                                               std::uint64_t text_hash,
                                               std::vector<feature::FileDiagnostic> diagnostics) {
    skipped[path_id] = {.text_hash = text_hash, .diagnostics = std::move(diagnostics)};
    // A compile owed shows them as it commits (run).
    auto* document = find_document(path_id);
    if(!document || !document->current) {
        return;
    }
    auto& session = *document->session;
    auto compiled = latest_at(path_id, session.version);
    if(!compiled) {
        return;
    }
    auto shown = shown_skipped_diagnostics(session, *compiled->output);
    if(ranges::equal(compiled->output->diagnostics.skipped, shown, feature::same_diagnostic)) {
        return;
    }
    auto output = *compiled->output;
    output.diagnostics.skipped = std::move(shown);
    set_output(path_id, std::move(output));
    push_output(session, false);
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
    for(auto dep: project.graph.dependencies(node(path_id))) {
        if(dep.kind != NodeKind::PCM || is_unresolved_import(dep)) {
            continue;
        }
        if(auto* inputs = project.module_inputs(dep.file())) {
            visit(*inputs);
        }
    }
}

void ServedProject::mark_not_current(Fid path_id) {
    if(auto* document = find_document(path_id)) {
        document->current = false;
        document->epoch += 1;
    }
}

void ServedProject::edited(Session& session) {
    auto path_id = session.path_id;
    mark_not_current(path_id);
    project.graph.invalidate(node(path_id));
    // The attempt still observes the real reply to its cancelled send
    // (crash accounting depends on it — contract 2).
    if(auto it = compile_interrupts.find(path_id); it != compile_interrupts.end()) {
        it->second->cancel();
    }
    // Editing is the canonical escalation trigger: from here on the
    // document invests in PCH/AST.
    escalate(path_id);
}

void ServedProject::invalidate(Fid path_id) {
    mark_not_current(path_id);
    // The in-flight attempt's token fires, but its parse keeps running: the
    // buffer is unchanged, so the product is worth publishing as bounded
    // staleness before the attempt reports Stale.
    project.graph.invalidate(node(path_id));
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
        auto next = DocumentCompile(*document.latest);
        next.pch_key.reset();
        next.pch_failed = false;
        next.base_pch_key.reset();
        next.index.reset();
        document.latest = std::make_shared<const DocumentCompile>(std::move(next));
    }
    document.inputs.reset();
    mark_not_current(path_id);
    project.graph.invalidate(node(path_id));
    if(auto it = compile_interrupts.find(path_id); it != compile_interrupts.end()) {
        it->second->cancel();
    }
}

void ServedProject::inputs_changed(Session& session) {
    invalidate(session.path_id);
    session.changed(Session::Clock::now());
}

void ServedProject::escalate(Fid path_id) {
    auto& document = *find_document(path_id);
    if(document.serving != ServingMode::IndexOnly ||
       project.settings.project.readonly == ReadonlyMode::On) {
        return;
    }
    document.serving = ServingMode::Escalated;
}

void ServedProject::request_compile(std::shared_ptr<Session> session) {
    auto compile = [](ServedProject& self, std::shared_ptr<Session> session) -> async::task<> {
        co_await self.ensure_compiled(std::move(session));
    };
    if(!started_compiles.spawn(compile(*this, std::move(session)))) {
        LOG_WARN("request_compile: task group stopped, dropping the compile");
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

async::task<bool> ServedProject::ensure_compiled(std::shared_ptr<Session> session) {
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

    // The latest compile's current flag alone cannot clear this fast path: another
    // consumer's PCH staleness discovery dirties this node through the
    // graph cascade without touching the compile table, and a PCH
    // rebuilt before the next request would read fresh again here.
    if(current(path_id) && !project.graph.is_dirty(node(path_id))) {
        // No early return: the changes one check finds cascade together.
        bool stale = false;
        each_input(path_id, [&](const Inputs& inputs) {
            stale = !inputs_hold(project.file_table, inputs) || stale;
        });
        if(!stale) {
            co_return true;
        }
        // A dependency changed on disk behind this document's back — the
        // lazy twin of the background ticks. The changes the check's looks
        // queued cascade first, then the document recompiles now, whether
        // or not the dependency graph knows the edge (a macro include) —
        // both synchronously: applied on a later turn, the cascade would
        // void the attempt about to compile the new content.
        server.apply_disk_changes();
        // The changes can take the document to another project: a context
        // choice whose host stopped including it.
        if(!owns(*session)) {
            co_return false;
        }
        inputs_changed(*session);
    }

    // Join the document's compile, retrying through stale attempts (a Lost
    // invalidation mid-flight yields Stale and the next attempt recompiles)
    // until a terminal result. A buffer edit or session replacement ends
    // the join instead: the result would describe a buffer that no longer
    // exists, and the editor re-requests after an edit.
    auto gen = session->generation;
    auto result = co_await project.graph.join(
        node(path_id),
        compile_work(path_id),
        {.foreground = true, .retry = [session, gen] { return session->generation == gen; }});
    co_return result == Result::Success;
}

Work ServedProject::compile_work(Fid path_id) {
    return {.run = [this, path_id](Attempt& attempt) { return compile(attempt, path_id); }};
}

async::task<std::optional<Imports>> ServedProject::depend_modules(Attempt& attempt,
                                                                  const Ticket& ticket,
                                                                  const Resolution& resolution,
                                                                  llvm::StringRef text) {
    auto& session = ticket.session;
    auto path_id = session->path_id;
    // Imports come from the attempt's buffer snapshot under its own
    // resolved command — the same text and flags the parse will consume,
    // so a context choice or donated header host cannot enable an import
    // the scan missed. An unsaved `import m;` builds its PCM now, and the
    // next edit's attempt re-resolves — the outdated attempt's build loses
    // interest and winds down on its own (the PCH pattern). Module names
    // still resolve against the on-disk module map; a module unit is
    // somebody else's saved file. The remap stays engaged even for an
    // empty buffer: its own text has no imports, but the command's
    // injected -include prefix can still carry them.
    //
    // The import list is scanner truth, not build output: declare it as
    // the node's edges before waiting, so a document whose compile fails
    // stays cascade-reachable from its imports — fixing or providing an
    // import must re-dirty the documents it broke. The per-attempt resolve
    // keeps the edges honest across CDB, buffer and import changes, and
    // the empty truth of a document that can import nothing is published
    // too: an import edge declared earlier must stop cascading here.
    auto imports =
        co_await project.scan_imports(path_id, resolution, std::optional<llvm::StringRef>(text));
    project.graph.set_edges(attempt.node(), imports.declared);
    auto kind = feature::RequestKind::PCM;
    auto modules = imports.resolved();
    if(modules.empty()) {
        ticket.session->succeeded(kind, ticket.generation);
        co_return imports;
    }

    // A module whose build crashed a worker is refused to every importer
    // until the record of one lets it retry: a change to its inputs, or a
    // save (see Session).
    // FIXME: an edit within the spacing of the module's crash is spent here:
    // the attempt runs without the module and finishes current, so the module waits
    // for the next edit or save, not for the spacing to pass.
    bool retry = session->crash_record(kind) && !session->crash_stops(kind, Session::Clock::now());
    Session::Dispatch pcm_dispatch(*session, kind);
    bool crashed = false;
    // Still an edge when skipped: the declaration above keeps it, so a later
    // build of it, by an importer whose record lets it retry, re-dirties this
    // document.
    llvm::SmallVector<Fid> waited;
    for(auto dep: modules) {
        if(auto* crash = project.build_crash(dep)) {
            if(!retry) {
                // Booked once: an attempt on unchanged inputs is no new
                // crash.
                if(!session->crash_record(kind)) {
                    record_crash(session, kind, crash->death, crash->cause);
                }
                crashed = true;
                continue;
            }
            project.forget_build_failure(dep);
        }
        waited.push_back(dep);
    }
    auto waits = co_await project.wait_modules(attempt, waited, [&](Fid dep) {
        LOG_INFO("Import {} of {} failed to build; the parse reports it",
                 project.file_table.resolve(dep),
                 project.file_table.resolve(path_id));
        if(auto* crash = project.build_crash(dep)) {
            record_crash(session, kind, crash->death, crash->cause);
            crashed = true;
        }
    });
    if(waits == Result::Stale) {
        co_return std::nullopt;
    }
    if(!crashed) {
        ticket.session->succeeded(kind, ticket.generation);
    }
    co_return imports;
}

async::task<Result> ServedProject::compile(Attempt& attempt, Fid path_id) {
    // Only a join for a document of this project starts an attempt, and the
    // attempt gets here before its first suspension: nothing can have closed
    // or moved it.
    auto* document = find_document(path_id);
    assert(document && "AST attempt for a document not this project's");
    auto session = document->session;
    auto idle = llvm::make_scope_exit([&] { enter_phase(path_id, CompilePhase::Idle); });

    // The attempt's content identity. The graph's attempt identity covers
    // invalidation (attempt.current()); this covers the buffer itself — a
    // didChange or session replacement makes every product describe text
    // that no longer exists, so the commit discards wholesale, while a
    // Lost invalidation (generation unchanged) still publishes salvage.
    auto ticket = Ticket::take(session);

    // Covers every spawn path, including waiter-driven respawns: content
    // that just crashed a worker must not reach one more.
    if(compile_stopped(*session)) {
        co_return Result::Failed;
    }

    ScopedTimer timer;
    auto file_path = std::string(project.file_table.resolve(path_id));

    LOG_INFO("compile: starting path_id={} gen={}", path_id, ticket.generation);
    enter_phase(path_id, CompilePhase::Command);

    // An attempt past the gate on a crashed compile or preamble is its retry,
    // and stops the others until its result (Session invariant 2).
    using enum feature::RequestKind;
    Session::Dispatch compile_dispatch(*session, Compile);
    bool pch_retry = session->crash_record(PCH) != nullptr;
    Session::Dispatch pch_dispatch(*session, PCH);

    // clang-tidy runs where `clice lint` checks, unless its configuration
    // does not parse, which `clice lint` refuses too, a crash of its is
    // suspected or stops it, or the compile retries a crash it is cleared of
    // (on a retracted preamble, or after the document's own). The places
    // resolving its configuration looked at join the deps either way: a
    // fix there is a change that retries a stopped pass.
    std::optional<tidy::TidyResolution> tidy_config;
    std::optional<Session::Dispatch> tidy_dispatch;
    if(project.settings.diagnostics.clang_tidy &&
       project.rules.lintable(project.file_table.resolve(path_id))) {
        tidy_config = tidy::resolve_tidy_params(project.file_table.spelling(path_id));
        auto& suspects = session->suspects;
        if(tidy_config->error.empty() && !suspects.tidy && suspects.pch.empty() &&
           !session->crash_record(Compile) && !session->crash_stops(Tidy, Session::Clock::now())) {
            tidy_dispatch.emplace(*session, Tidy);
        }
    }

    // At most two worker sends: a header with unknown self-containment
    // compiles without a prefix first; if the diagnostics indicate missing
    // includer context, the second send re-compiles with a synthesized
    // prefix. The check's diagnostics are never published.
    bool precompiled_retried = false;
    auto known_instantiations =
        project.index.known_instantiations(path_id, project.dep_graph.include_closure(path_id));
    for(int send = 0; send < 2; send += 1) {
        worker::CompileParams params;
        params.known_instantiations = known_instantiations;
        params.standard_library_instantiations =
            project.settings.project.standard_library_instantiations;
        params.skip_bodies = project.settings.project.skip_header_bodies;
        params.path = file_path;
        params.version = session->version;
        params.text = session->text().str();
        params.project_root = project.root.str();
        if(tidy_dispatch) {
            params.tidy = tidy_config->params;
        }
        // The include trees the header's host search needs are taken first:
        // a header compiles under the host every process finds for it.
        auto resolution =
            co_await document_command(path_id, nullptr, [&] { return ticket.fresh(); });
        params.directory = resolution.directory;
        params.arguments = resolution.arguments;
        if(!ticket.fresh()) {
            co_return Result::Stale;
        }
        auto source = resolution.ref.source;
        auto* synthesized = resolution.synthesized.get();
        if(synthesized) {
            params.synthesized = synthesized->files;
        }
        params.hidden = resolution.hidden;

        // A header no self-containment check says compiles alone compiles
        // without its includer's context first: this compile is the check,
        // and the commit gates what it may write.
        auto self_containment =
            send == 0 && !resolution.self_contained ? resolution.self_containment : std::nullopt;

        enter_phase(path_id, CompilePhase::Modules);
        auto imports = co_await depend_modules(attempt, ticket, resolution, params.text);
        if(!imports) {
            co_return Result::Stale;
        }

        if(!ticket.fresh()) {
            LOG_INFO("compile: outdated before PCH for {}", file_path);
            co_return Result::Stale;
        }

        // Build or reuse the PCH through depend — it records the AST→PCH
        // edge, so a staleness discovery on the shared pair
        // cascades here. Under readonly = "on" the build compiles without
        // a preamble instead — completion and signature help pay full
        // parses, the profile's stated trade. A failed pair is the same
        // degradation: the compile proceeds preamble-less.
        std::optional<std::string> adopted_pch;
        std::optional<std::string> failed_pch;
        // The precompiled header the build declares for the command: the
        // preamble's PCH is built over it, and with none the parse loads
        // it itself. One that did not build leaves the header the command
        // forces in to parse what it holds.
        std::optional<std::string> base_pch;
        if(project.settings.project.readonly != ReadonlyMode::On) {
            enter_phase(path_id, CompilePhase::Preamble);
            if(auto request = declared_pch(path_id, resolution, *imports)) {
                auto key = request->pch_key;
                switch(
                    co_await attempt.depend(pch_node(key), project.pch_work(std::move(*request)))) {
                    case Result::Success:
                        if(!project.pch_reads(key, path_id)) {
                            base_pch = key;
                        }
                        break;
                    case Result::Failed: break;
                    case Result::Stale: co_return Result::Stale;
                }
            }
            auto plan = project.preamble_pch(path_id,
                                             params.text,
                                             std::nullopt,
                                             resolution,
                                             *imports,
                                             base_pch.value_or(""));
            if(!plan.request) {
                // No PCH build left to crash on.
                ticket.session->succeeded(PCH, ticket.generation);
            } else {
                auto pch_key = plan.request->pch_key;
                // A preamble whose build crashed a worker — for this
                // document or another sharing it — is refused until
                // this one's record lets it retry.
                if(auto* crash = project.build_crash(pch_key)) {
                    if(!pch_retry) {
                        record_crash(session, PCH, crash->death, crash->cause);
                        co_return Result::Failed;
                    }
                    project.forget_build_failure(pch_key);
                }
                switch(co_await attempt.depend(pch_node(pch_key),
                                               project.pch_work(std::move(*plan.request)))) {
                    case Result::Success:
                        // Adoption is gated on the attempt's result and
                        // its own validity: a newer generation or a
                        // Lost-type invalidation while we waited means
                        // the resolved command may describe nothing —
                        // neither the key nor the record it clears
                        // belongs to this attempt anymore. Adopting a
                        // proven-good PCH clears the document's
                        // PCH record as surely as building one — but
                        // only its own; every consumer clears for
                        // itself.
                        if(attempt.current() && ticket.fresh()) {
                            adopted_pch = pch_key;
                            ticket.session->succeeded(PCH, ticket.generation);
                        }
                        break;
                    case Result::Failed:
                        // The build itself crashed a worker: this attempt
                        // stops before the parse, which would feed the
                        // same preamble to one more worker.
                        if(auto* crash = project.build_crash(pch_key)) {
                            record_crash(session, PCH, crash->death, crash->cause);
                            co_return Result::Failed;
                        }
                        // A build that failed without a crash answered:
                        // the preamble no longer crashes.
                        if(attempt.current()) {
                            ticket.session->succeeded(PCH, ticket.generation);
                        }
                        failed_pch = pch_key;
                        break;
                    case Result::Stale: co_return Result::Stale;
                }
            }
        }
        if(adopted_pch.has_value()) {
            if(auto* pch = project.built_pch(*adopted_pch)) {
                params.pch = {pch->path, pch->bound};
            } else {
                adopted_pch.reset();
            }
        }
        if(!adopted_pch && base_pch) {
            if(auto* pch = project.built_pch(*base_pch)) {
                params.pch = {pch->path, 0};
            } else {
                base_pch.reset();
            }
        }
        // What the parse loads, a crash or a failed read of which may be
        // the pair's fault: the preamble's PCH, else the build's.
        auto consumed_pch = adopted_pch ? adopted_pch : base_pch;

        params.pcms = project.imported_pcms(imports->resolved());

        if(!ticket.fresh()) {
            LOG_INFO("compile: outdated before send for {}", file_path);
            co_return Result::Stale;
        }

        // Seed the worker's inactive-region state from the PCH's preamble:
        // the conditional stack it left open (a #if cut by the bound) and
        // the regions of the preamble share itself. Copy the state out:
        // concurrent compiles can insert into Project::pchs across the await
        // below and rehash the map from under a held pointer.
        std::shared_ptr<index::TUIndex> preamble_index;
        if(adopted_pch.has_value()) {
            preamble_index = project.preamble_index(*adopted_pch);
        }
        if(preamble_index) {
            auto regions = preamble_index->inactive_regions();
            params.preamble_inactive_regions.assign(regions.begin(), regions.end());
            auto conditionals = preamble_index->open_conditionals();
            params.open_conditionals.assign(conditionals.begin(), conditionals.end());
        }

        enter_phase(path_id, CompilePhase::Parsing);

        // The send carries the interrupt, not the attempt's advisory token:
        // the master must observe the request's real result — the crash
        // accounting below depends on it (contract 2) — and a cancelled
        // send still awaits the worker's answer. Only an edit, a choice change or a stop
        // interrupts the parse (see edited, choice_changed and stop_compiles), and the stale reply
        // is discarded at the validity gate below. A death
        // that is not this compile's doing resends it once — the attempt
        // only stops early once the buffer moved on, since the next attempt
        // compiles that.
        //
        // Crash accounting runs even for outdated attempts: the crash came
        // from content this document dispatched, and skipping it would let a
        // poison file dodge its stop by being edited between dispatch and
        // the crash response (contract 12).
        bool consuming_pch = consumed_pch.has_value();
        bool retry_without_suspect = false;
        auto interrupt = std::make_unique<async::cancellation_source>();
        auto interrupt_token = interrupt->token();
        auto* own_interrupt = interrupt.get();
        compile_interrupts[path_id] = std::move(interrupt);
        auto release_interrupt = llvm::make_scope_exit([&] {
            auto it = compile_interrupts.find(path_id);
            if(it != compile_interrupts.end() && it->second.get() == own_interrupt) {
                compile_interrupts.erase(it);
            }
        });
        std::optional<CacheStore::PendingEntry> transfer;
        if(project.store) {
            transfer = project.store->begin_transfer();
            params.index_output_path = transfer->tmp_path;
        }
        auto result = co_await deliver(
            server.pool,
            true,
            [&]() -> RequestResult<worker::CompileParams> {
                if(!ticket.fresh()) {
                    co_await async::fail(
                        ipc::Error{worker::dispatch_errc::cancelled, "Compile outdated"});
                }
                co_return co_await server.pool.send_stateful(path_id.raw,
                                                             params,
                                                             {.token = interrupt_token});
            },
            [&](const ipc::Error& error) {
                retry_without_suspect =
                    attribute_crash(session, params.tidy.has_value(), consumed_pch, error);
            });
        if(retry_without_suspect) {
            co_return Result::Stale;
        }

        if(!ticket.fresh()) {
            LOG_INFO("compile: outdated reply for {}", file_path);
            co_return Result::Stale;
        }

        if(!result.has_value()) {
            if(worker::is_operational_error(result.error())) {
                LOG_WARN("Compile did not complete for {}: {}", file_path, result.error().message);
            } else {
                // The worker accepts arbitrary user code; a non-operational
                // failure at this layer is IPC/worker breakage, never a
                // user-code problem.
                LOG_ANOMALY(CompileFail,
                            "Compile failed for {}: {}",
                            file_path,
                            result.error().message);
            }
            // Short of a crash of its own (published as it was recorded),
            // the document keeps what it showed: a worker outage or a
            // second death of somebody else's doing says nothing about its
            // code.
            co_return Result::Failed;
        }

        // The PCH quality gate: a failed parse whose diagnostics name
        // the consumed PCH (worker-side pch_suspect — setup failure and
        // fatal error alike). The PCH attempt validated the pair fresh via its
        // deps, yet the frontend could not read it — the bytes on disk
        // are the suspect. Retract the pair (store + cache) and send once
        // more; the PCH's next attempt misses and rebuilds both halves.
        // Without this write-back a corrupt blob is trusted for the life
        // of the store and the file stays broken on every restart. A
        // setup failure whose diagnostics do NOT name the blob (bad
        // invocation, broken module input) deliberately falls through to
        // the non-Done path below: retracting a healthy shared PCH over
        // someone else's failure would rebuild it on every request for as
        // long as that failure persists.
        if(result.value().pch_suspect && consuming_pch) {
            LOG_WARN("Compile could not read PCH pair {} for {}; retracting the pair",
                     *consumed_pch,
                     file_path);
            // Retract unconditionally — an unreadable pair never survives,
            // even when its consumption is stopped — but rerun only once.
            // The consumption's record bounds the cross-attempt shape: this
            // attempt finishes Stale (waiting on the retracted key again
            // finds it changed, which overtakes this attempt), the waiter
            // starts another, and without the stopped key each would
            // rebuild and fail forever.
            project.pch_consume_failed(*consumed_pch);
            if(!precompiled_retried) {
                precompiled_retried = true;
                // Redo the same send so the check's semantics are untouched
                // (the loop counter is about the check's sends, not PCH
                // retries).
                send -= 1;
                continue;
            }
            // The rebuilt pair is unreadable again: the storage itself is
            // failing, and another rebuild would fare no better. The
            // attempt proceeds — a Done reply publishes its real fatal
            // diagnostics, a non-Done one falls to the honest gap below.
        }

        // A non-Done reply past the gate is a non-result: committing it
        // would freeze the document on a product that never existed —
        // no index, an empty deps snapshot nothing can invalidate.
        // Outdated attempts were already discarded at the validity gate
        // above, so this attempt's inputs are broken in a way a PCH rebuild
        // cannot fix.
        if(result.value().status != worker::CompileStatus::Done) {
            LOG_WARN("Compile produced no result for {} (status={})",
                     file_path,
                     static_cast<int>(result.value().status));
            // The latest compile stays non-current: the next request
            // recompiles instead of trusting the phantom product. What is
            // published is the honest gap: why the compile failed before
            // parsing, of this text, else versionless empty diagnostics
            // rather than a stale list posing as current.
            bool setup_fail = result.value().status == worker::CompileStatus::SetupFail;
            set_output(path_id,
                       CompileOutput{
                           .version = setup_fail ? std::optional(session->version) : std::nullopt,
                           .text = session->source,
                           .diagnostics =
                               {
                                             .source = source,
                                             .own = std::move(result.value().diagnostics),
                                             .query_failure = resolution.query_failure,
                                             },
                           .failed = setup_fail,
            });
            push_output(*session, true);
            co_return Result::Failed;
        }

        // A check invalidated mid-flight is discarded whole: its result is
        // a conditional write like the latest compile's current flag, and its
        // diagnostics come from a compile deliberately run without includer
        // context — they are never published, including on this path.
        if(self_containment && !attempt.current()) {
            LOG_INFO("Discarding invalidated self-containment check for {}", file_path);
            co_return Result::Stale;
        }

        auto& diagnostics = result.value().diagnostics;
        if(preamble_index && preamble_index->matches_preamble(params.text)) {
            diagnostics = feature::with_preamble(std::move(diagnostics),
                                                 preamble_diagnostics(*preamble_index));
        }
        feature::explain_failed_imports(diagnostics, module_failures(imports->resolved(), project));
        for(auto& choice: module_choices(*imports, project)) {
            diagnostics.push_back(feature::ambiguous_module(choice, session->text()));
        }
        if(tidy_config && !tidy_config->error.empty()) {
            diagnostics.insert(diagnostics.begin(),
                               feature::invalid_tidy_config(tidy_config->error));
        }

        // The check's result stands for every process while what it read
        // holds; typing never runs it again (self_contained), a save may.
        if(self_containment) {
            auto inputs =
                intern_inputs(project.file_table, result.value().deps, result.value().started_at);
            // The compile reports nothing it read through the PCH.
            if(consumed_pch) {
                if(auto* pch = project.built_pch(*consumed_pch)) {
                    inputs.append(pch->inputs.begin(), pch->inputs.end());
                }
            }
            project.commit_self_containment(
                *self_containment,
                diagnostics,
                project.file_table.intern_version(path_id, session->hash),
                std::move(inputs));
            if(feature::missing_context_errors(diagnostics) > 0) {
                LOG_INFO("Header {} needs includer context, re-compiling with prefix", file_path);
                continue;
            }
        }

        // The commit: build the whole package off to the side, then
        // install it with no suspension before the result becomes
        // visible (contract 16) — a joiner must never see Success before
        // the latest compile describes the buffer it compiled. A Lost
        // invalidation mid-flight (the attempt no longer current, generation
        // unchanged) commits the same package as bounded staleness —
        // recorded, published, but not current — and reports Stale so
        // waiters drive the recompile.
        bool still_current = attempt.current();

        // The parse read the pair and completed without finding it
        // unreadable — and the attempt still describes live inputs: the
        // key's consumption failures were transient, not the storage. An
        // overtaken attempt clears nothing: what overtook it may BE another consumer's
        // fresh failure to read the shared key, and clearing here would
        // reset the count it just paid for.
        if(still_current && consuming_pch && !result.value().pch_suspect) {
            project.pch_consume_succeeded(*consumed_pch);
        }

        auto& index_data = result.value().tu_index_data;
        auto next = std::make_shared<DocumentCompile>();
        next->pch_key = failed_pch ? failed_pch : consumed_pch;
        next->pch_failed = failed_pch.has_value();
        next->base_pch_key = base_pch;
        next->host = resolution.host;
        next->command = resolution.ref.command;
        // The AST and the file index commit together — that pairing is
        // what lets navigation trust the index after ensure_compiled. A
        // compile that produced no index data (no AST, or an index it could
        // not hand over) must
        // therefore drop the previous buffer's index rather than leave it
        // posing as current: an honest gap over yesterday's offsets.
        if(result.value().index_in_file) {
            // Copied, not mapped: the index outlives the transfer file.
            auto file = fs::File::open(transfer->tmp_path);
            auto read = file ? llvm::MemoryBuffer::getOpenFile(file->native_handle(),
                                                               transfer->tmp_path,
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
                                transfer->tmp_path,
                                read.getError().message())));
            }
        } else if(!index_data.empty()) {
            next->index = std::make_shared<index::TUIndex>(index::TUIndex::from_buffer(
                llvm::MemoryBuffer::getMemBufferCopy(as_chars(index_data))));
        }

        LOG_PERF("request", "kind=Compile file={} total_ms={:.2f}", file_path, timer.ms_f());
        auto unconfirmed = params.skip_bodies ? feature::take_unconfirmed(diagnostics)
                                              : std::vector<feature::FileDiagnostic>();
        next->output = CompileOutput{
            .version = session->version,
            .text = session->source,
            .diagnostics =
                {
                              .source = source,
                              .own = std::move(diagnostics),
                              .unconfirmed = std::move(unconfirmed),
                              .unmatched_host = resolution.unmatched_host.valid()
                                          ? project.file_table.display(resolution.unmatched_host)
                                          : std::string(),
                              .query_failure = resolution.query_failure,
                              },
        };
        next->output->diagnostics.skipped = shown_skipped_diagnostics(*session, *next->output);

        // The fresh ticket above holds the document here: a move bumps the
        // generation, and nothing suspends since.
        auto& entry = *find_document(path_id);
        // A document moving off a preamble it failed on releases that
        // failure: only the preamble's consumers would ever retry it. A
        // crash stays, for the next consumer to book without a build.
        // FIXME: released even while another open document still fails on that
        // preamble: its next compile runs the failing build once more.
        if(entry.latest && entry.latest->pch_failed && entry.latest->pch_key != next->pch_key &&
           !project.build_crash(*entry.latest->pch_key)) {
            project.forget_build_failure(*entry.latest->pch_key);
        }
        entry.latest = std::move(next);
        if(tidy_config) {
            result.value().deps.append_range(tidy_config->files);
        }
        // A compile that finishes clears its suspects: the pair it read
        // was at fault, or the pass it ran without.
        auto& suspects = session->suspects;
        suspects.pch.clear();
        auto tidy_crash = std::exchange(suspects.tidy, std::nullopt);
        if(tidy_dispatch) {
            ticket.session->succeeded(Tidy, ticket.generation);
        } else if(tidy_crash) {
            record_crash(session, Tidy, worker::death_of(*tidy_crash), tidy_crash->message);
        }
        // The files the synthesized context embeds are read too, though
        // clang never opens them.
        entry.inputs =
            intern_inputs(project.file_table, result.value().deps, result.value().started_at);
        entry.inputs->append(resolution.inputs.begin(), resolution.inputs.end());
        entry.current = still_current;
        ticket.session->succeeded(Compile, ticket.generation);
        push_output(*session, true);
        // The push above told the client to re-pull what the fresh AST now
        // answers better; one refresh per commit.
        if(server.client && owns(*session)) {
            server.client->index_served.erase(session->path_id);
        }
        project.background_index.schedule();
        co_return still_current ? Result::Success : Result::Stale;
    }
    // Every arm of the two-send loop commits or returns; the check's
    // continue only fires on the first send, the PCH retry only once.
    std::unreachable();
}

async::task<std::optional<std::string>>
    ServedProject::ensure_pch(const Ticket& ticket,
                              std::uint64_t epoch,
                              llvm::StringRef text,
                              std::optional<std::uint32_t> point,
                              const Resolution& resolution,
                              const Imports& imports) {
    auto& session = ticket.session;
    auto path_id = session->path_id;
    auto adoptable = [&] {
        auto* document = find_document(path_id);
        return ticket.fresh() && document && document->epoch == epoch;
    };

    // The precompiled header the build declares for the command, as a
    // compile takes it (see compile()).
    std::optional<std::string> base;
    if(auto request = declared_pch(path_id, resolution, imports)) {
        auto key = request->pch_key;
        if(co_await project.acquire_pch(std::move(*request)) == Result::Success &&
           !project.pch_reads(key, path_id)) {
            base = key;
        }
    }

    auto plan = project.preamble_pch(path_id, text, point, resolution, imports, base.value_or(""));
    if(!plan.request) {
        if(adoptable() && !plan.partial) {
            set_pch_keys(path_id, base, base);
            ticket.session->succeeded(feature::RequestKind::PCH, ticket.generation);
        }
        co_return base;
    }
    auto pch_key = plan.request->pch_key;

    // A preamble that crashed a worker stops the document's content builds
    // (the caller checks); only a compile the document's record lets
    // retry builds it again.
    auto kind = feature::RequestKind::PCH;
    if(auto* crash = project.build_crash(pch_key)) {
        // Booked once: a request on unchanged inputs is no new crash.
        if(!session->crash_record(kind)) {
            record_crash(session, kind, crash->death, crash->cause);
        }
        co_return base;
    }
    if(co_await project.acquire_pch(std::move(*plan.request)) != Result::Success) {
        if(auto* crash = project.build_crash(pch_key)) {
            record_crash(session, kind, crash->death, crash->cause);
        }
        co_return base;
    }

    // Adoption is gated on the acquisition's result, never on leftover cache
    // paths, and on the buffer and compile this request was taken on: a
    // newer generation or a Lost-type invalidation while we waited means the
    // resolved command may describe nothing — neither the key write nor
    // the record it clears belongs to this request anymore. The request
    // itself still compiles against the PCH: it was planned from the
    // text it sends. Adopting a proven-good PCH clears the document's
    // PCH record as surely as building one — but only its own; every
    // consumer clears for itself.
    if(adoptable() && !plan.partial) {
        set_pch_keys(path_id, pch_key, base);
        ticket.session->succeeded(kind, ticket.generation);
    }
    co_return pch_key;
}

async::task<bool> ServedProject::prepare_precompiled(const Ticket& ticket,
                                                     llvm::StringRef text,
                                                     std::uint32_t point,
                                                     const Resolution& resolution,
                                                     StatelessInputs& inputs) {
    auto* synthesized = resolution.synthesized.get();
    auto path_id = ticket.session->path_id;
    auto* document = find_document(path_id);
    auto epoch = document ? document->epoch : 0;

    // The scan runs under the request's command with the text the dispatch
    // will compile, so an unsaved `import m;` builds its PCM before the parse
    // needs it.
    auto imports = co_await project.build_imports(node(path_id), path_id, resolution, text);
    if(!imports) {
        co_return false;
    }

    if(project.settings.project.readonly != ReadonlyMode::On) {
        auto pch_key = co_await ensure_pch(ticket, epoch, text, point, resolution, *imports);
        if(pch_key) {
            if(auto* pch = project.built_pch(*pch_key)) {
                inputs.pch = {pch->path, pch->bound};
            }
        }
    }

    inputs.pcms = project.imported_pcms(imports->resolved());

    co_return true;
}

void ServedProject::enter_phase(Fid path_id, CompilePhase phase) {
    if(server.client) {
        server.client->report_compile_phase(path_id, phase);
    }
}

void ServedProject::push_output(const Session& session, bool compiled) {
    if(server.client && owns(session)) {
        server.client->push_output(*this, session, compiled);
    }
}

std::shared_ptr<const DocumentCompile> ServedProject::latest(Fid path_id) const {
    auto* document = find_document(path_id);
    return document ? document->latest : nullptr;
}

std::shared_ptr<const DocumentCompile> ServedProject::latest_at(Fid path_id, int version) const {
    auto compiled = latest(path_id);
    return compiled && compiled->output && compiled->output->version == version ? compiled
                                                                                : nullptr;
}

bool ServedProject::current(Fid path_id) const {
    auto* document = find_document(path_id);
    return document && document->current;
}

bool ServedProject::index_current(Fid path_id) const {
    auto* document = find_document(path_id);
    return document && document->current && document->latest && document->latest->index &&
           document->latest->index->loaded();
}

void ServedProject::set_pch_keys(Fid path_id,
                                 std::optional<std::string> pch_key,
                                 std::optional<std::string> base_pch_key) {
    auto& document = *find_document(path_id);
    auto next = document.latest ? DocumentCompile(*document.latest) : DocumentCompile();
    next.pch_key = std::move(pch_key);
    next.pch_failed = false;
    next.base_pch_key = std::move(base_pch_key);
    document.latest = std::make_shared<const DocumentCompile>(std::move(next));
}

void ServedProject::set_output(Fid path_id, CompileOutput output) {
    auto& document = *find_document(path_id);
    auto next = document.latest ? DocumentCompile(*document.latest) : DocumentCompile();
    next.output = std::move(output);
    document.latest = std::make_shared<const DocumentCompile>(std::move(next));
}

std::vector<feature::FileDiagnostic>
    ServedProject::shown_diagnostics(const Session& session, const CompileOutput& output) const {
    auto diagnostics = feature::shown_diagnostics(output.diagnostics, project.settings.diagnostics);
    for(auto& note: session.crash_notes()) {
        diagnostics.push_back(feature::crash_note(note));
    }
    return diagnostics;
}

std::vector<feature::FileDiagnostic>
    ServedProject::finished_diagnostics(const Session& session) const {
    if(session.closed) {
        return {};
    }
    // A compile that failed or is stopped left an output of an older
    // buffer, whose ranges a versionless report would place on this one.
    if(auto compiled = latest_at(session.path_id, session.version)) {
        return shown_diagnostics(session, *compiled->output);
    }
    return shown_diagnostics(session, {});
}

}  // namespace clice
