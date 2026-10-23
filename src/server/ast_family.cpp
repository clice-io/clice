module;

#include "support/prelude.h"

#include "support/anomaly.macros.h"
#include "support/logging.macros.h"

module clice;

import :command.argument;
import :compile.compilation;
import :compile.diagnostic;
import :compile.tidy;
import :feature.diagnostics;
import :index.tu_index;
import :sched.families.build_common;
import :server.ast_family;
import :server.context_service;
import :server.editor_context;
import :support.anomaly;
import :support.bytes;
import :support.json;
import :support.logging;
import :support.text;
import :support.timer;
import :syntax.dependency_graph;
import :syntax.preamble;
import :vfs.file_system;
import :vfs.file_table;
import :vfs.path;
import :worker.common;
import :worker.protocol;

namespace clice {

namespace {

/// What crashed, as the note words it: the work, and the feature it
/// pauses.
struct CrashSubject {
    llvm::StringRef work;
    llvm::StringRef paused;
};

CrashSubject crash_subject(std::uint8_t kind) {
    switch(static_cast<EvidenceKind>(kind)) {
        case EvidenceKind::Compile: return {"compiling", "Semantic features are"};
        case EvidenceKind::Tidy: return {"running clang-tidy on", "clang-tidy is"};
        case EvidenceKind::PCH:
            return {"building the precompiled preamble of", "Semantic features are"};
        case EvidenceKind::PCM: return {"building a module imported by", "The module is"};
        case EvidenceKind::DocumentLink:
            return {"computing document links for", "Document links are"};
        case EvidenceKind::FoldingRange:
            return {"computing folding ranges for", "Folding ranges are"};
        case EvidenceKind::DocumentSymbol:
            return {"computing document symbols for", "Document symbols are"};
        case EvidenceKind::InlayHint: return {"computing inlay hints for", "Inlay hints are"};
        case EvidenceKind::CodeAction: return {"computing code actions for", "Code actions are"};
        case EvidenceKind::SelectionRange:
            return {"computing selection ranges for", "Selection ranges are"};
        case EvidenceKind::Completion: return {"completing code in", "Code completion is"};
        case EvidenceKind::SignatureHelp:
            return {"computing signature help in", "Signature help is"};
        case EvidenceKind::Format: return {"formatting", "Formatting is"};
        case EvidenceKind::SemanticTokens:
            return {"computing semantic highlighting for", "Semantic highlighting is"};
        case EvidenceKind::Hover: return {"computing hover for", "Hover is"};
        case EvidenceKind::Count: break;
    }
    std::unreachable();
}

}  // namespace

void append_crash_notes(const Session& session, std::vector<feature::FileDiagnostic>& diagnostics) {
    if(session.closed) {
        return;
    }
    for(auto& note: session.quarantine->notes()) {
        auto subject = crash_subject(note.kind);
        llvm::StringRef retry =
            note.save_only
                ? "until you save this file"
                : "until this file or a header it includes changes, or until you save it";
        auto repeats =
            note.strikes > 1 ? std::format(" {} times in a row", note.strikes) : std::string();
        diagnostics.push_back(feature::file_warning(
            std::format("clice's worker crashed{} while {} this file ({}). {} paused here {}.",
                        repeats,
                        subject.work,
                        note.cause,
                        subject.paused,
                        retry)));
    }
}

/// The diagnostics the PCH's build raised in the preamble.
static std::vector<feature::FileDiagnostic> preamble_diagnostics(const index::TUIndex& preamble) {
    std::vector<feature::FileDiagnostic> diagnostics;
    [[maybe_unused]] auto status =
        json::from_string<ipc::lsp_config>(preamble.preamble_diagnostics(), diagnostics);
    return diagnostics;
}

static bool kept_errors(const std::shared_ptr<index::TUIndex>& preamble) {
    return preamble && ranges::any_of(preamble_diagnostics(*preamble), [](const auto& diagnostic) {
               return diagnostic.level >= DiagnosticLevel::Error;
           });
}

/// The compile's diagnostics behind the ones its PCH's build raised in the
/// preamble, which the parse consuming the PCH never raises again — those
/// of the command line it does, and they appear once. Files with one
/// preamble share the PCH: notes the build placed in its own focus file
/// move to `path`.
static std::vector<feature::FileDiagnostic> with_preamble(std::vector<feature::FileDiagnostic> own,
                                                          const index::TUIndex& preamble,
                                                          llvm::StringRef path) {
    auto merged = preamble_diagnostics(preamble);
    if(merged.empty()) {
        return own;
    }
    auto builder = preamble.path(preamble.path_count() - 1);
    for(auto& diagnostic: merged) {
        for(auto& note: diagnostic.notes) {
            if(note.path == builder) {
                note.path = path.str();
            }
        }
    }
    llvm::StringSet<> raised;
    for(auto& diagnostic: own) {
        raised.insert(to_client_json(diagnostic, ""));
    }
    std::erase_if(merged, [&](const feature::FileDiagnostic& diagnostic) {
        return raised.contains(to_client_json(diagnostic, ""));
    });
    ranges::move(own, std::back_inserter(merged));
    return merged;
}

/// clang reports the import of a module whose build failed as not found:
/// the import says instead why the module did not build.
static void explain_failed_imports(std::vector<feature::FileDiagnostic>& diagnostics,
                                   llvm::ArrayRef<Fid> imports,
                                   const PCMFamily& pcm,
                                   const DependencyGraph& modules) {
    for(auto module: imports) {
        auto failure = pcm.build_failure(module);
        if(!failure) {
            continue;
        }
        llvm::ArrayRef<Fid> chain = failure->chain;
        std::string message;
        for(auto unit: chain.take_front(failure->cycle.value_or(chain.size()))) {
            message += std::format("could not build module '{}': ", modules.module_of(unit));
        }
        if(failure->cycle) {
            auto cycle = chain.drop_front(*failure->cycle);
            message +=
                std::format("cyclic dependency in module '{}': ", modules.module_of(cycle.front()));
            for(auto unit: cycle) {
                message += std::format("{} -> ", modules.module_of(unit));
            }
            message += modules.module_of(cycle.front());
        } else {
            message += failure->error;
        }
        auto not_found = std::format("module '{}' not found", modules.module_of(module));
        for(auto& diagnostic: diagnostics) {
            if(diagnostic.id == clang::diag::err_module_not_found &&
               diagnostic.message == not_found) {
                diagnostic.message = message;
                // As clang codes it: a cycle only where the imported module
                // is on it, not built where the message starts with one
                // that is not.
                diagnostic.id = failure->cycle == 0 ? clang::diag::err_module_cycle
                                                    : clang::diag::err_module_not_built;
                diagnostic.code = failure->cycle == 0 ? "err_module_cycle" : "err_module_not_built";
            }
        }
    }
}

/// An import of a module several units provide resolves to the nearest
/// of them (PCMFamily::direct_deps); the import says which it took and
/// which it passed over.
static void note_provider_choices(std::vector<feature::FileDiagnostic>& diagnostics,
                                  const PCMFamily::ModuleDeps& imports,
                                  const Project& project,
                                  const Session& session) {
    auto& graph = project.dep_graph;
    assert(ranges::size(imports.declared) == ranges::size(imports.offsets));
    for(auto [dep, offset]: views::zip(imports.declared, imports.offsets)) {
        if(PCMFamily::is_unresolved(dep)) {
            continue;
        }
        auto chosen = Fid{static_cast<std::uint32_t>(dep.key)};
        auto name = graph.module_of(chosen);
        auto providers = graph.lookup_module(name);
        if(providers.size() < 2) {
            continue;
        }
        std::string others;
        for(auto provider: providers) {
            if(provider != chosen) {
                others += others.empty() ? "" : ", ";
                others += project.file_table.display(provider);
            }
        }
        auto warning = feature::file_warning(
            std::format("module '{}' is declared by more than one file; this file imports {}, the "
                        "nearest, not {}",
                        name,
                        project.file_table.display(chosen),
                        others));
        warning.code = "ambiguous-module";
        // At the import when it is the file's own; an import through an
        // included file sits at the include, one of the command line's or
        // the implicit one of an implementation unit at the top.
        if(offset < session.text().size()) {
            auto end = std::min(session.text().find('\n', offset), session.text().size());
            if(auto range = session.positions().range({offset, static_cast<std::uint32_t>(end)},
                                                      PositionEncoding::UTF16)) {
                warning.range = *range;
            }
        }
        diagnostics.push_back(std::move(warning));
    }
}

ASTFamily::PCHPlan ASTFamily::plan_pch(Fid path_id,
                                       llvm::StringRef text,
                                       std::optional<std::uint32_t> point,
                                       const std::string& directory,
                                       const std::vector<std::string>& arguments,
                                       const SynthesizedContext* synthesized,
                                       const PCMFamily::ModuleDeps& imports,
                                       llvm::StringRef base) {
    auto path = project.file_table.resolve(path_id);
    // The part of the prefix inside braces comes after the preamble, which
    // the header's own directives would otherwise precede. A preamble still
    // being typed ends before its unfinished directive: the finished part
    // keeps its PCH — the key of the one before, when the directive goes at
    // the end of the block — and the unfinished one parses with the body,
    // which reports its own error.
    auto bound = preamble_bound(text);
    bool partial = false;
    if(point) {
        auto before = preamble_bound(text, logical_line_begin(text, *point));
        partial = before < bound;
        bound = before;
    }
    bound = synthesized && !synthesized->open.empty() ? 0 : finished_preamble_bound(text, bound);
    if(bound == 0 && !synthesized) {
        // No preamble directives and no injected -include — PCH would be
        // empty. Self-contained header contexts end up here too: they borrow
        // a command but inject nothing. A precompiled header the build
        // declares is loaded by the parse itself.
        return {.partial = partial};
    }

    // With a synthesized prefix, the PCH is worth building even at
    // bound == 0: the -include'd prefix is processed via the predefines
    // buffer and ends up in the PCH, so the (potentially huge) prefix is not
    // re-parsed on every edit. The -include flag is part of the
    // canonicalized arguments below, and the prefix's name hashes its
    // content — through the fragments it includes, the whole chain's — so
    // the key tracks prefix changes automatically.

    // The modules imported by the preamble, or by a file the command line
    // includes, are inputs of the PCH: it depends on what their PCMs were
    // built from, and keys on which PCMs those are — a module that gains or
    // loses its PCM, or whose provider or command changes, starts a new
    // PCH. A PCH loses the imports of a module unit's global module
    // fragment: the module declaration past it takes the fragment for
    // absent and hides what the fragment made visible. Such a preamble
    // goes without one.
    std::string pcm_keys;
    llvm::SmallVector<Fid> modules;
    bool imported = false;
    assert(ranges::size(imports.declared) == ranges::size(imports.offsets));
    for(auto [dep, offset]: views::zip(imports.declared, imports.offsets)) {
        if(offset != 0 && offset >= bound) {
            continue;
        }
        imported = true;
        if(!PCMFamily::is_unresolved(dep)) {
            auto module = Fid{static_cast<std::uint32_t>(dep.key)};
            if(auto it = project.pcm_builds.find(module);
               it != project.pcm_builds.end() && !it->second.path.empty()) {
                pcm_keys += it->second.key;
                modules.push_back(module);
            }
        }
        pcm_keys.push_back('\0');
    }
    if(imported && imports.module_unit) {
        LOG_DEBUG("No PCH for {}: its global module fragment imports", path);
        return {.partial = partial};
    }

    // Key the PCH by preamble text plus the frontend-relevant compile flags,
    // so files with the same preamble text but different flags (-D, -I, -std)
    // produce separate PCHs.  The source file path stays out of the key so
    // files with identical preambles share one PCH — but its DIRECTORY (and
    // the working directory) must stay in: quote includes and relative paths
    // resolve against them, so equal preamble text in different directories
    // can mean different content.  A preamble defining or testing a macro
    // keeps the path in too: the PCH places the macro and its uses in the
    // file that built it, where a file sharing it would find its own.  The
    // clang version guards against reusing
    // blobs a newer bundled clang would reject, and the build configuration
    // keeps the blob with the database that records its dependencies:
    // shared across configurations, one could rebuild it while another's
    // records still confirmed the old content. A PCH built over the
    // build's precompiled header names the very build it chains onto:
    // rebuilt, that one has another name.
    auto preamble_text = text.substr(0, bound);
    llvm::StringRef base_blob;
    if(auto it = project.pch_cache.find(base); it != project.pch_cache.end()) {
        base_blob = it->second.blob;
    }
    auto own_path = preamble_uses_macros(text, bound) ? llvm::StringRef(path) : llvm::StringRef();
    bool skip_bodies = project.config.project.skip_header_bodies;
    auto pch_key = cache_key({clang::getClangFullVersion(),
                              pch_build_rule(skip_bodies),
                              project.build.active_configuration(),
                              directory,
                              path::parent_path(path),
                              own_path,
                              preamble_text,
                              canonicalize(arguments),
                              pcm_keys,
                              base_blob});
    PCHPlan plan{
        .decision = PCHPlan::Decision::Acquire,
        .request =
            {
                      .pch_key = std::move(pch_key),
                      .file = std::string(path),
                      .directory = directory,
                      .arguments = arguments,
                      .content = std::string(text),
                      .preamble_bound = bound,
                      .synthesized = synthesized ? synthesized->files : SynthesizedFiles{},
                      .base = base.str(),
                      .skip_bodies = skip_bodies,
                      },
        .partial = partial,
    };
    if(imported) {
        project.fill_pcm_deps(plan.request.pcms, pcm.closure(imports.resolved).getArrayRef());
        plan.request.modules = std::move(modules);
    }
    return plan;
}

ASTFamily::ASTFamily(Project& project,
                     EditorContext& contexts,
                     TaskGraph& graph,
                     PCMFamily& pcm,
                     PCHFamily& pch,
                     WorkerPool& pool,
                     SessionStore& sessions) :
    project(project), contexts(contexts), graph(graph), pcm(pcm), pch(pch), pool(pool),
    sessions(sessions) {}

void ASTFamily::register_runner() {
    graph.register_family(Family::AST, [this](RoundContext& ctx, NodeId id) {
        return run(ctx, Fid{static_cast<std::uint32_t>(id.key)});
    });
}

void ASTFamily::record_crash(const std::shared_ptr<Session>& session,
                             std::uint8_t kind,
                             const ipc::Error& error) {
    LOG_WARN("{} crashed a worker while serving {}: {}",
             project.file_table.resolve(session->path_id),
             crash_subject(kind).work,
             error.message);
    session->quarantine->on_crash(kind,
                                  worker::death_of(error),
                                  error.message,
                                  Quarantine::Clock::now());
    // A silent first crash leaves the published state alone (see
    // Quarantine invariant 4).
    if(!session->quarantine->shows(kind)) {
        return;
    }
    // A closed session publishes nothing; a reopen sharing its records
    // shows the note.
    if(session->closed) {
        if(auto open = sessions.find(session->path_id);
           open && open->quarantine == session->quarantine) {
            republish(open);
        }
        return;
    }
    // A barred compile leaves no AST behind the old diagnostics: they go,
    // and the note says why.
    auto previous = compiles.latest(session->path_id);
    if(previous && previous->output.has_value() &&
       (kind == evidence_kind(EvidenceKind::Compile) || kind == evidence_kind(EvidenceKind::PCH))) {
        compiles.set_output(session->path_id,
                            CompileOutput{
                                .version = std::nullopt,
                                .source = previous->output->source,
                                .diagnostics = {},
                            });
    }
    republish(session);
}

void ASTFamily::republish(const std::shared_ptr<Session>& session) {
    if(auto previous = compiles.latest(session->path_id);
       !previous || !previous->output.has_value()) {
        compiles.set_output(session->path_id,
                            CompileOutput{
                                .version = std::nullopt,
                                .source = CommandSource::CDBExact,
                                .diagnostics = {},
                            });
    }
    on_output.emit(session);
}

bool ASTFamily::compile_barred(const Session& session) {
    auto now = Quarantine::Clock::now();
    return session.quarantine->barred(evidence_kind(EvidenceKind::Compile), now) ||
           session.quarantine->barred(evidence_kind(EvidenceKind::PCH), now);
}

void ASTFamily::saved(Session& session) {
    session.quarantine->on_save();
    // An AST compiled while a clang-tidy crash barred the pass stays
    // current until the next edit: the save that retries the pass
    // recompiles.
    if(session.quarantine->crashed(evidence_kind(EvidenceKind::Tidy))) {
        invalidate(session.path_id);
    }
    // Typing never re-runs the self-containment trial, yet an edit can
    // make a header lean on its includer: a save showing what an includer
    // would provide, or saving a buffer whose compile has not finished yet,
    // runs it again.
    if(contexts.commands.header_mode(session.path_id) == HeaderMode::SelfContained) {
        if(auto compiled = compiles.latest_at(session.path_id, session.version);
           !compiled || missing_context_errors(compiled->output->diagnostics) > 0) {
            contexts.commands.forget_self_contained(session.path_id);
            session.trial_done = false;
            invalidate(session.path_id);
        }
    }
    // The modules it is and imports, directly or through other modules,
    // retry with it: a crashed build is refused until a consumer holds a
    // license (see depend_modules), a failed one until what it read or
    // looked for changes. A failed build's inputs miss a lookup in a
    // directory that did not exist yet, so a save retries its failed
    // preamble too. Either refusal leaves the compile standing, so only a
    // fresh round asks for the artifact again.
    bool retry = session.quarantine->crashed(evidence_kind(EvidenceKind::PCM));
    llvm::SmallVector<Fid> modules{session.path_id};
    llvm::DenseSet<Fid> seen{session.path_id};
    auto add_imports = [&](NodeId importer) {
        for(auto dep: graph.dependencies(importer)) {
            if(dep.family != Family::PCM || PCMFamily::is_unresolved(dep)) {
                continue;
            }
            auto module = Fid{static_cast<std::uint32_t>(dep.key)};
            if(seen.insert(module).second) {
                modules.push_back(module);
            }
        }
    };
    add_imports(node(session.path_id));
    for(std::size_t i = 0; i < modules.size(); i += 1) {
        pcm.forgive(modules[i]);
        retry |= pcm.forget_failure(modules[i]);
        add_imports({Family::PCM, modules[i].raw});
    }
    // A PCH that kept its preamble's errors misses the same lookups as a
    // failed build: a save rebuilds it.
    if(auto compiled = compiles.latest(session.path_id)) {
        if(compiled->failed_pch_key) {
            retry |= pch.forget_failure(*compiled->failed_pch_key);
        } else if(compiled->pch_key && kept_errors(pch.preamble_state(*compiled->pch_key))) {
            pch.invalidate(*compiled->pch_key);
            retry = true;
        }
    }
    if(retry) {
        invalidate(session.path_id);
    }
}

void ASTFamily::publish_output(const std::shared_ptr<Session>& session, CompileOutput output) {
    compiles.set_output(session->path_id, std::move(output));
    on_output.emit(session);
}

/// Whether two diagnostics show the same: one message at one place.
static bool same_diagnostic(const feature::FileDiagnostic& left,
                            const feature::FileDiagnostic& right) {
    return left.range.start.line == right.range.start.line &&
           left.range.start.character == right.range.start.character &&
           left.message == right.message;
}

std::vector<feature::FileDiagnostic> ASTFamily::shown_skipped(const Session& session,
                                                              const CompileOutput& output) const {
    auto it = skipped.find(session.path_id);
    if(it == skipped.end() || it->second.text_hash != session.hash ||
       output.source != CommandSource::CDBExact || contexts.selection(session.path_id)) {
        return {};
    }
    std::vector<feature::FileDiagnostic> result;
    for(const auto& diagnostic: it->second.diagnostics) {
        auto same = [&](const feature::FileDiagnostic& own) {
            return same_diagnostic(own, diagnostic);
        };
        if(ranges::contains(unused_declaration_warnings, diagnostic.id)) {
            if(auto own = ranges::find_if(output.unconfirmed, same);
               own != output.unconfirmed.end()) {
                result.push_back(*own);
            }
        } else if(ranges::none_of(output.diagnostics, same)) {
            result.push_back(diagnostic);
        }
    }
    return result;
}

bool ASTFamily::lacks_skipped(Fid path_id) const {
    auto session = sessions.find(path_id);
    if(!session || session->serving != ServingMode::Escalated ||
       project.build.entries(path_id).empty()) {
        return false;
    }
    auto it = skipped.find(path_id);
    return it == skipped.end() || it->second.text_hash != session->hash;
}

void ASTFamily::report_skipped(Fid path_id,
                               std::uint64_t text_hash,
                               std::vector<feature::FileDiagnostic> diagnostics) {
    skipped[path_id] = {.text_hash = text_hash, .diagnostics = std::move(diagnostics)};
    // A compile owed shows them as it commits (run).
    auto session = sessions.find(path_id);
    if(!session || !compiles.current(path_id)) {
        return;
    }
    auto compiled = compiles.latest_at(path_id, session->version);
    if(!compiled) {
        return;
    }
    auto shown = shown_skipped(*session, *compiled->output);
    if(ranges::equal(compiled->output->skipped, shown, same_diagnostic)) {
        return;
    }
    auto output = *compiled->output;
    output.skipped = std::move(shown);
    compiles.set_output(path_id, std::move(output));
    on_diagnostics.emit(session);
}

void ASTFamily::each_input(Fid path_id, llvm::function_ref<void(const DepsSnapshot&)> visit) const {
    if(auto it = compiles.entries.find(path_id); it != compiles.entries.end() && it->second.deps) {
        visit(*it->second.deps);
    }
    // Chain files of a header context are embedded in the synthesized
    // preamble, invisible to the deps snapshot.
    if(auto* header_context = contexts.header_context(path_id)) {
        visit(header_context->deps);
    }
    if(auto compiled = compiles.latest(path_id); compiled && compiled->pch_key) {
        if(auto it = project.pch_cache.find(*compiled->pch_key); it != project.pch_cache.end()) {
            visit(it->second.deps);
        }
    }
    for(auto dep: graph.dependencies(node(path_id))) {
        if(dep.family != Family::PCM || PCMFamily::is_unresolved(dep)) {
            continue;
        }
        if(auto* inputs = project.build_inputs(Fid{static_cast<std::uint32_t>(dep.key)})) {
            visit(*inputs);
        }
    }
}

bool ASTFamily::is_stale(const Session& session) {
    // No early return: the changes one check finds cascade together.
    bool stale = false;
    each_input(session.path_id, [&](const DepsSnapshot& deps) {
        stale = deps_changed(project.file_table, deps) || stale;
    });
    return stale;
}

void ASTFamily::closure(Fid path_id, llvm::SmallVectorImpl<Fid>& files) const {
    each_input(path_id, [&](const DepsSnapshot& deps) {
        for(auto& dep: deps) {
            files.push_back(dep.path_id);
        }
    });
}

void ASTFamily::touch(Fid path_id) {
    auto& entry = compiles.entries[path_id];
    entry.current = false;
    entry.epoch += 1;
}

void ASTFamily::supersede(Fid path_id) {
    touch(path_id);
    graph.update(node(path_id));
    // The round still observes the real reply to its cancelled send
    // (crash accounting depends on it — contract 2).
    if(auto it = compile_interrupts.find(path_id); it != compile_interrupts.end()) {
        it->second->cancel();
    }
}

void ASTFamily::invalidate(Fid path_id) {
    touch(path_id);
    // The in-flight round's token fires, but its parse keeps running: the
    // buffer is unchanged, so the product is worth publishing as bounded
    // staleness before the round reports Stale.
    graph.update(node(path_id));
}

void ASTFamily::drop(Fid path_id) {
    graph.update(node(path_id));
    compiles.entries.erase(path_id);
    pcm.forget_buffer(path_id);
}

void ASTFamily::switch_identity(Session& session) {
    // Invalidate any in-flight compile: without the bump it would pass
    // its commit validity check and publish results for the superseded
    // identity.
    session.generation += 1;
    session.trial_done = false;
    session.quarantine->on_change(Quarantine::Clock::now());
    auto& entry = compiles.entries[session.path_id];
    if(entry.latest) {
        // The TU index describes the buffer under the old identity, though
        // it compiled the same bytes.
        auto next = DocumentCompile(*entry.latest);
        next.pch_key.reset();
        next.index.reset();
        entry.latest = std::make_shared<const DocumentCompile>(std::move(next));
    }
    entry.deps.reset();
    supersede(session.path_id);
}

void ASTFamily::escalate(Session& session) {
    if(session.serving != ServingMode::IndexOnly) {
        return;
    }
    if(readonly == ReadonlyMode::On) {
        return;
    }
    session.serving = ServingMode::Escalated;
}

void ASTFamily::request_compile(std::shared_ptr<Session> session) {
    auto kick = [](ASTFamily& self, std::shared_ptr<Session> session) -> kota::task<> {
        co_await self.ensure_compiled(std::move(session));
    };
    if(!kicks.spawn(kick(*this, std::move(session)))) {
        LOG_WARN("request_compile: task group stopped, dropping kick");
    }
}

kota::task<> ASTFamily::stop() {
    for(auto& [path_id, interrupt]: compile_interrupts) {
        interrupt->cancel();
    }
    kicks.cancel();
    co_await kicks.join();
}

kota::task<bool> ASTFamily::ensure_compiled(std::shared_ptr<Session> session) {
    auto path_id = session->path_id;

    LOG_DEBUG("ensure_compiled: path_id={} version={} gen={} current={}",
              path_id,
              session->version,
              session->generation,
              compiles.current(path_id));

    // A closed session never compiles again, and the join below cannot
    // tell: its generation stopped moving at the close.
    if(session->closed) {
        co_return false;
    }

    // A document whose compile keeps killing workers waits for a change
    // or a save instead of feeding the same content to one more worker;
    // the crash note was published when the crash was recorded.
    if(compile_barred(*session)) {
        LOG_DEBUG("ensure_compiled: {} is barred by a crash", project.file_table.resolve(path_id));
        co_return false;
    }

    // The latest compile's current flag alone cannot clear this fast path: another
    // consumer's PCH staleness discovery dirties this node through the
    // graph cascade without touching the compile table, and a PCH
    // rebuilt before the next request would read fresh again here.
    if(compiles.current(path_id) && !graph.is_dirty(node(path_id))) {
        if(!is_stale(*session)) {
            co_return true;
        }
        // A dependency changed on disk behind this session's back — the
        // lazy twin of the background ticks. The document recompiles now,
        // whether or not the dependency graph knows the edge (a macro
        // include). The handler re-resolves the session by path_id; no
        // suspension separates it from this frame, so it finds the same
        // open session this coroutine holds.
        on_stale(path_id);
    }

    // Join the document's round, retrying through stale attempts (a Lost
    // invalidation mid-flight yields Stale and the next attempt recompiles)
    // until a terminal outcome. A buffer edit or session replacement ends
    // the join instead: the result would describe a buffer that no longer
    // exists, and the editor re-requests after an edit.
    auto gen = session->generation;
    auto outcome = co_await graph.request(
        node(path_id),
        {.foreground = true, .validity = [session, gen] { return session->generation == gen; }});
    co_return outcome == JoinOutcome::Success;
}

kota::task<std::optional<PCMFamily::ModuleDeps>>
    ASTFamily::depend_modules(RoundContext& ctx,
                              const std::shared_ptr<Session>& session,
                              const Resolution& resolution,
                              llvm::StringRef directory,
                              const std::vector<std::string>& arguments,
                              llvm::StringRef text) {
    auto path_id = session->path_id;
    // Imports come from the round's buffer snapshot under the round's own
    // resolved command — the same text and flags the parse will consume,
    // so a context choice or donated header host cannot enable an import
    // the scan missed. An unsaved `import m;` builds its PCM now, and the
    // next edit's round re-resolves — the superseded round's build loses
    // interest and winds down on its own (the PCH pattern). Module names
    // still resolve against the on-disk module map; a module unit is
    // somebody else's saved file. The remap stays engaged even for an
    // empty buffer: its own text has no imports, but the command's
    // injected -include prefix can still carry them.
    //
    // The import list is scanner truth, not build output: commit it as
    // durable edges before waiting, so a document whose compile fails
    // stays cascade-reachable from its imports — fixing or providing an
    // import must re-dirty the documents it broke. The per-round resolve
    // keeps the edges honest across CDB, buffer and import changes, and
    // the empty truth of a document that can import nothing is published
    // too: a durable import edge earned earlier must stop cascading here.
    llvm::SmallVector<const char*, 32> argv;
    argv.reserve(arguments.size());
    for(auto& arg: arguments) {
        argv.push_back(arg.c_str());
    }
    auto deps = co_await pcm.direct_deps(path_id,
                                         resolution,
                                         argv,
                                         directory,
                                         std::optional<llvm::StringRef>(text));
    graph.declare(node(path_id), deps.declared);
    // Sentinels join the round's candidates too: a successful commit
    // replaces the declaration with them, and a declare-only edge would
    // vanish with it.
    for(auto dep: deps.declared) {
        if(PCMFamily::is_unresolved(dep)) {
            ctx.reference(dep);
        }
    }
    if(deps.resolved.empty()) {
        session->quarantine->on_success(evidence_kind(EvidenceKind::PCM));
        co_return deps;
    }

    // A module whose build crashed a worker is refused to every importer
    // until one holds a license to retry it: a change to its inputs, or a
    // save (see Quarantine).
    auto kind = evidence_kind(EvidenceKind::PCM);
    // FIXME: an edit within retry_spacing of the module's crash is spent here:
    // the round runs without the module and finishes current, so the module waits
    // for the next edit or save, not for the spacing to pass.
    bool licensed = session->quarantine->crashed(kind) &&
                    !session->quarantine->barred(kind, Quarantine::Clock::now());
    Quarantine::Attempt license(*session->quarantine, kind);
    bool crashed = false;

    // Building a dependency can itself evict another clean module's PCM
    // under budget pressure, reopening the window the previous attempt's
    // revalidation closed — hence the bounded retry until the set is
    // stable. Past the bound the parse fails visibly on the missing file.
    for(int attempt = 0; attempt < 3; attempt += 1) {
        bool any_evicted = pcm.revalidate_blobs();
        if(attempt > 0 && !any_evicted) {
            break;
        }

        for(auto dep: deps.resolved) {
            if(auto* crash = pcm.crashed(dep)) {
                if(!licensed) {
                    // Booked once: a round on unchanged inputs is no new
                    // crash.
                    if(!session->quarantine->crashed(kind)) {
                        record_crash(session, kind, *crash);
                    }
                    crashed = true;
                    // Still an input: a later build of it, by an importer
                    // holding a license, must re-dirty this document.
                    ctx.reference({Family::PCM, dep.raw});
                    continue;
                }
                pcm.forgive(dep);
            }
            switch(co_await ctx.depend({Family::PCM, dep.raw})) {
                case DependResult::Ready: break;
                case DependResult::Failed:
                    LOG_INFO("Import {} of {} failed to build; the parse reports it",
                             project.file_table.resolve(dep),
                             project.file_table.resolve(path_id));
                    if(auto* crash = pcm.crashed(dep)) {
                        record_crash(session, kind, *crash);
                        crashed = true;
                        licensed = false;
                    }
                    break;
                case DependResult::Cancelled: co_return std::nullopt;
            }
        }
    }
    if(!crashed) {
        session->quarantine->on_success(kind);
    }
    co_return deps;
}

kota::task<bool> ASTFamily::fetch_include_tree(Fid host) {
    auto& files = project.file_table;
    worker::IncludeTreeParams params;
    params.file = files.resolve(host).str();
    params.workspace = project.config.workspace_root.str();
    contexts.commands.resolve_command(host, params.directory, params.arguments);
    auto started = std::pair(project.commands_epoch, project.context_epoch);
    // The tree does not depend on the header's buffer: an edit superseding
    // the round must not cancel the run, or typing would restart it.
    auto result = co_await pool.send_stateless(params, worker::Priority::High);
    if(!result.has_value()) {
        LOG_INFO("No include tree for {}: {}", params.file, result.error().message);
        // A host whose preprocessing kills the worker would kill one per
        // edit; any other failure is not the host's.
        if(result.error().code == worker::dispatch_errc::worker_crashed) {
            project.include_trees[host].last_run = started;
        }
        co_return false;
    }
    if(!result.value().success) {
        LOG_INFO("No include tree for {}: {}", params.file, result.value().error);
        project.include_trees[host].last_run = started;
        co_return false;
    }
    auto& tree = result.value();
    llvm::SmallVector<VersionID> versions;
    for(std::size_t i = 0; i < tree.paths.size(); i += 1) {
        auto fid = files.intern(Spelling::absolute(tree.paths[i]));
        auto hash = tree.content_hashes[i];
        if(hash != 0) {
            files.disk.consumed(fid, hash);
        }
        versions.push_back(files.intern_version(fid, hash));
    }
    for(auto& node: tree.nodes) {
        node.file = versions[node.file].raw;
    }
    std::vector<VersionID> absent;
    for(auto& place: tree.absent) {
        absent.push_back(files.intern_version(files.intern(Spelling::absolute(place)), 0));
    }
    // The tree can move the host's contexts: the listings it changes are
    // stale.
    project.context_epoch += 1;
    project.include_trees[host] = {
        .root = versions.back(),
        .nodes = std::move(tree.nodes),
        .absent = std::move(absent),
        .commands_epoch = started.first,
        .last_run = {started.first, started.second + 1},
    };
    co_return true;
}

kota::task<RoundOutcome> ASTFamily::run(RoundContext& ctx, Fid path_id) {
    // Only a join for an open session spawns a round, and the round gets
    // here before its first suspension: nothing can have closed it.
    auto session = sessions.find(path_id);
    assert(session && "AST round for a closed document");

    // The round's content identity. The graph's round identity covers
    // invalidation (ctx.current()); this covers the buffer itself — a
    // didChange or session replacement makes every product describe text
    // that no longer exists, so the commit discards wholesale, while a
    // Lost invalidation (generation unchanged) still publishes salvage.
    auto gen = session->generation;

    // Covers every spawn path, including waiter-driven respawns: content
    // that just crashed a worker must not reach one more.
    if(compile_barred(*session)) {
        co_return RoundOutcome::Failed;
    }

    ScopedTimer timer;
    auto file_path = std::string(project.file_table.resolve(path_id));

    LOG_INFO("compile round: starting path_id={} gen={}", path_id, gen);

    // A round past the gate on a crashed compile or preamble is its retry,
    // and holds the license until its outcome (Quarantine invariant 2).
    auto compile_kind = evidence_kind(EvidenceKind::Compile);
    auto pch_kind = evidence_kind(EvidenceKind::PCH);
    Quarantine::Attempt compile_attempt(*session->quarantine, compile_kind);
    bool pch_licensed = session->quarantine->crashed(pch_kind);
    Quarantine::Attempt pch_attempt(*session->quarantine, pch_kind);

    // clang-tidy runs where `clice lint` checks, unless its configuration
    // does not parse, which `clice lint` refuses too, a crash of its is on
    // trial or bars it, or the compile retries a crash it is acquitted of
    // (on a retracted preamble, or after the document's own). The places
    // resolving its configuration looked at join the deps either way: a
    // fix there is a change that retries a barred pass.
    auto tidy_kind = evidence_kind(EvidenceKind::Tidy);
    std::optional<tidy::TidyResolution> tidy_config;
    std::optional<Quarantine::Attempt> tidy_attempt;
    if(project.config.diagnostics.clang_tidy &&
       project.build.lintable(project.file_table.resolve(path_id))) {
        tidy_config = tidy::resolve_tidy_params(project.file_table.spelling(path_id));
        if(tidy_config->error.empty() && !session->tidy_crash && session->crashed_pch.empty() &&
           !session->quarantine->crashed(compile_kind) &&
           !session->quarantine->barred(tidy_kind, Quarantine::Clock::now())) {
            tidy_attempt.emplace(*session->quarantine, tidy_kind);
        }
    }

    // At most two worker sends: a header with unknown self-containment
    // compiles without a prefix first; if the diagnostics indicate missing
    // includer context, the second send re-compiles with a synthesized
    // prefix. The trial's diagnostics are never published.
    bool artifact_retried = false;
    std::size_t trial_misses = 0;
    auto known_instantiations =
        project.index.known_instantiations(path_id, project.dep_graph.include_closure(path_id));
    for(int attempt = 0; attempt < 2; attempt += 1) {
        worker::CompileParams params;
        params.known_instantiations = known_instantiations;
        params.standard_library_instantiations =
            project.config.project.standard_library_instantiations;
        params.skip_bodies = project.config.project.skip_header_bodies;
        params.path = file_path;
        params.version = session->version;
        params.text = session->text().str();
        params.workspace = project.config.workspace_root.str();
        if(tidy_attempt) {
            params.tidy = tidy_config->params;
        }
        auto resolution = contexts.resolve_command(path_id, params.directory, params.arguments);
        // A context cut along the lexical chain may name directives the
        // host's compile never enters: once a tree can tell — another
        // header's run took it, the index caught up — or one is taken,
        // unless one was since the disk or the commands last changed,
        // resolve again: under it, another host may come first.
        llvm::SmallVector<Fid, 2> retried;
        while(resolution.tree_wanted.valid() &&
              !ranges::contains(retried, resolution.tree_wanted)) {
            auto host = resolution.tree_wanted;
            retried.push_back(host);
            if(!enterings(project, host, path_id)) {
                auto it = project.include_trees.find(host);
                bool ran =
                    it != project.include_trees.end() &&
                    it->second.last_run == std::pair(project.commands_epoch, project.context_epoch);
                if(ran || !co_await fetch_include_tree(host)) {
                    break;
                }
            }
            contexts.drop_header_context(path_id);
            if(session->generation != gen) {
                co_return RoundOutcome::Stale;
            }
            params.directory.clear();
            params.arguments.clear();
            resolution = contexts.resolve_command(path_id, params.directory, params.arguments);
        }
        auto source = resolution.source;
        auto* synthesized = resolution.synthesized.get();

        auto* header_context = contexts.header_context(path_id);
        if(synthesized) {
            params.synthesized = synthesized->files;
        }

        // Whether this round is the self-containment probe: a header
        // deliberately compiled without its includer prefix to see if it
        // stands alone. Decided here, where resolve_command chose to omit
        // the prefix; the commit gates what the probe may write.
        bool trial_round = attempt == 0 && !session->trial_done && header_context &&
                           !header_context->synthesized &&
                           contexts.commands.header_mode(path_id) == HeaderMode::Unknown;

        auto imports = co_await depend_modules(ctx,
                                               session,
                                               resolution,
                                               params.directory,
                                               params.arguments,
                                               params.text);
        if(!imports) {
            co_return RoundOutcome::Stale;
        }

        if(session->generation != gen) {
            LOG_INFO("compile round: superseded before PCH for {}", file_path);
            co_return RoundOutcome::Stale;
        }

        // Build or reuse the PCH through its family — the depend records
        // the Ast→Pch edge, so a staleness discovery on the shared pair
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
        if(readonly != ReadonlyMode::On) {
            if(auto unit = contexts.commands.pch_unit(path_id, resolution)) {
                auto request =
                    pch.precompiled_header(*unit, project.config.project.skip_header_bodies);
                auto key = request.pch_key;
                switch(co_await ctx.depend(pch.prepare(std::move(request)))) {
                    case DependResult::Ready:
                        if(!pch.reads(key, path_id)) {
                            base_pch = key;
                        }
                        break;
                    case DependResult::Failed: break;
                    case DependResult::Cancelled: co_return RoundOutcome::Stale;
                }
            }
            auto plan = plan_pch(path_id,
                                 params.text,
                                 std::nullopt,
                                 params.directory,
                                 params.arguments,
                                 synthesized,
                                 *imports,
                                 base_pch.value_or(""));
            switch(plan.decision) {
                // No PCH build left to crash on.
                case PCHPlan::Decision::None: session->quarantine->on_success(pch_kind); break;
                case PCHPlan::Decision::Acquire: {
                    auto pch_key = plan.request.pch_key;
                    // A preamble whose build crashed a worker — for this
                    // document or another sharing it — is refused until
                    // this one holds a license to retry it.
                    if(auto* crash = pch.crashed(pch_key)) {
                        if(!pch_licensed) {
                            record_crash(session, pch_kind, *crash);
                            co_return RoundOutcome::Failed;
                        }
                        pch.forgive(pch_key);
                    }
                    auto dep = pch.prepare(std::move(plan.request));
                    switch(co_await ctx.depend(dep)) {
                        case DependResult::Ready:
                            // Adoption is gated on the round outcome and on
                            // this round's own validity: a supersede or a
                            // Lost-type invalidation while we waited means
                            // the resolved command may describe nothing —
                            // neither the key nor the evidence wash belongs
                            // to this round anymore.
                            if(session->generation == gen && ctx.current()) {
                                adopted_pch = pch_key;
                                // Adopting a proven-good artifact clears
                                // the session's PCH record as surely as
                                // building one — but only its own; every
                                // consumer clears for itself.
                                session->quarantine->on_success(pch_kind);
                            }
                            break;
                        case DependResult::Failed:
                            // The build itself crashed a worker: this round
                            // stops before the parse, which would feed the
                            // same preamble to one more worker.
                            if(auto* crash = pch.crashed(pch_key)) {
                                record_crash(session, pch_kind, *crash);
                                co_return RoundOutcome::Failed;
                            }
                            // A build that failed without a crash answered:
                            // the preamble no longer crashes.
                            if(session->generation == gen && ctx.current()) {
                                session->quarantine->on_success(pch_kind);
                            }
                            failed_pch = pch_key;
                            break;
                        case DependResult::Cancelled: co_return RoundOutcome::Stale;
                    }
                    break;
                }
            }
        }
        if(adopted_pch.has_value()) {
            if(auto pch_it = project.pch_cache.find(*adopted_pch);
               pch_it != project.pch_cache.end() && !pch_it->second.path.empty()) {
                params.pch = {pch_it->second.path, pch_it->second.bound};
            } else {
                adopted_pch.reset();
            }
        }
        if(!adopted_pch && base_pch) {
            if(auto pch_it = project.pch_cache.find(*base_pch);
               pch_it != project.pch_cache.end() && !pch_it->second.path.empty()) {
                params.pch = {pch_it->second.path, 0};
            } else {
                base_pch.reset();
            }
        }
        // What the parse loads, a crash or a failed read of which may be
        // the pair's fault: the preamble's PCH, else the build's.
        auto consumed_pch = adopted_pch ? adopted_pch : base_pch;

        project.fill_pcm_deps(params.pcms, pcm.closure(imports->resolved).getArrayRef());

        if(session->generation != gen) {
            LOG_INFO("compile round: superseded before send for {}", file_path);
            co_return RoundOutcome::Stale;
        }

        // Seed the worker's inactive-region state from the PCH's preamble:
        // the conditional stack it left open (a #if cut by the bound) and
        // the regions of the preamble share itself. Copy the state out:
        // concurrent compiles can insert into pch_cache across the await
        // below and rehash the map from under a held pointer.
        std::shared_ptr<index::TUIndex> preamble_state;
        if(adopted_pch.has_value()) {
            preamble_state = pch.preamble_state(*adopted_pch);
        }
        if(preamble_state) {
            auto regions = preamble_state->inactive_regions();
            params.preamble_inactive_regions.assign(regions.begin(), regions.end());
            auto conditionals = preamble_state->open_conditionals();
            params.open_conditionals.assign(conditionals.begin(), conditionals.end());
        }

        // The send carries the interrupt, not the round's advisory token:
        // the master must observe the request's real outcome — the crash
        // accounting below depends on it (contract 2) — and a cancelled
        // send still awaits the worker's answer. Only a supersede or stop
        // interrupts the parse (see supersede/stop), and the stale reply
        // is discarded at the validity gate below. A death
        // that is not this compile's doing resends it once — the attempt
        // only stops early once the buffer moved on, since the next round
        // compiles that.
        //
        // Crash accounting runs even for superseded rounds: the crash came
        // from content this document dispatched, and skipping it would let a
        // poison file dodge its bar by being edited between dispatch and the
        // crash response (contract 12).
        //
        // A death while consuming a prebuilt pair may be the pair's fault:
        // deep corruption aborts the AST reader (report_fatal_error in the
        // bitstream reader) before any diagnostic can anchor, so the
        // pch_suspect gate below never gets a say. The first such death
        // retracts the pair and the round respawns on a rebuilt one; a
        // death on the rebuilt pair is the document's own.
        bool consuming_pch = consumed_pch.has_value();
        bool pch_crashed = false;
        bool tidy_crashed = false;
        auto interrupt = std::make_unique<kota::cancellation_source>();
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
            pool,
            true,
            [&]() -> RequestResult<worker::CompileParams> {
                if(session->generation != gen) {
                    co_await kota::fail(
                        ipc::Error{worker::dispatch_errc::cancelled, "Compile superseded"});
                }
                co_return co_await pool.send_stateful(path_id.raw,
                                                      params,
                                                      {.token = interrupt_token});
            },
            [&](const ipc::Error& error) {
                // The checks run in the parse (their preprocessor
                // callbacks) and after it: they are the first suspect,
                // tried by a compile without them. One that crashes as
                // well acquits them.
                if(params.tidy) {
                    tidy_crashed = true;
                    session->tidy_crash = error;
                    return;
                }
                session->tidy_crash.reset();
                if(consuming_pch && session->crashed_pch != *consumed_pch) {
                    pch_crashed = true;
                } else {
                    record_crash(session, compile_kind, error);
                }
            });

        if(tidy_crashed) {
            LOG_WARN("Compile crashed with clang-tidy on for {}; retrying without it", file_path);
            co_return RoundOutcome::Stale;
        }

        if(pch_crashed) {
            LOG_WARN("Compile crashed consuming PCH pair {} for {}; retracting the pair",
                     *consumed_pch,
                     file_path);
            session->crashed_pch = *consumed_pch;
            pch.blame(*consumed_pch);
            co_return RoundOutcome::Stale;
        }

        if(session->generation != gen) {
            LOG_INFO("compile round: superseded reply for {}", file_path);
            co_return RoundOutcome::Stale;
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
            co_return RoundOutcome::Failed;
        }

        // The artifact quality gate: a failed parse whose diagnostics name
        // the consumed PCH (worker-side pch_suspect — setup failure and
        // fatal error alike). The family validated the pair fresh via its
        // deps, yet the frontend could not read it — the bytes on disk
        // are the suspect. Retract the pair (store + cache) and rerun the
        // round once; the next attempt misses and rebuilds both halves.
        // Without this write-back a corrupt blob is trusted for the life
        // of the store and the file stays broken on every restart. A
        // setup failure whose diagnostics do NOT name the blob (bad
        // invocation, broken module input) deliberately falls through to
        // the non-Done path below: retracting a healthy shared PCH over
        // someone else's failure would rebuild it on every request for as
        // long as that failure persists.
        if(result.value().pch_suspect && consuming_pch) {
            LOG_WARN("Compile blamed PCH pair {} for {}; retracting the pair",
                     *consumed_pch,
                     file_path);
            // Retract unconditionally — a blamed pair never survives, even
            // when the retry budget is spent — but rerun only once. The
            // strike table bounds the cross-round shape: this round finishes
            // Stale (the retract re-dirties the key its candidate edge
            // points at), the waiter respawns it, and without the parked
            // key each respawn would rebuild and blame forever.
            pch.blame(*consumed_pch);
            if(!artifact_retried) {
                artifact_retried = true;
                // Rerun the same attempt so the trial semantics are
                // untouched (the loop counter is about probe rounds, not
                // artifact retries).
                attempt -= 1;
                continue;
            }
            // The rebuilt pair is blamed again: the storage itself is
            // failing, and another rebuild would fare no better. The
            // round proceeds — a Done reply publishes its real fatal
            // diagnostics, a non-Done one falls to the honest gap below.
        }

        // A non-Done reply past the gate is a non-result: committing it
        // would freeze the document on a product that never existed —
        // no index, an empty deps snapshot nothing can invalidate.
        // Superseded rounds were already discarded at the validity gate
        // above, so this round's inputs are broken in a way a PCH rebuild
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
            publish_output(
                session,
                CompileOutput{
                    .version = setup_fail ? std::optional(session->version) : std::nullopt,
                    .source = source,
                    .diagnostics = std::move(result.value().diagnostics),
                    .query_failure = resolution.query_failure,
                    .failed = setup_fail,
                });
            co_return RoundOutcome::Failed;
        }

        // A probe invalidated mid-flight is discarded whole: its result is
        // a conditional write like the latest compile's current flag (dispatch
        // reset trial_done and the header mode for the recompile to
        // re-earn), and its diagnostics come from a compile deliberately
        // run without includer context — they are never published,
        // including on this path.
        if(trial_round && !ctx.current()) {
            LOG_INFO("Discarding invalidated self-containment probe for {}", file_path);
            co_return RoundOutcome::Stale;
        }

        auto& diagnostics = result.value().diagnostics;
        if(preamble_state && preamble_state->matches_preamble(params.text)) {
            diagnostics = with_preamble(std::move(diagnostics), *preamble_state, file_path);
        }
        explain_failed_imports(diagnostics, imports->resolved, pcm, project.dep_graph);
        note_provider_choices(diagnostics, *imports, project, *session);
        if(tidy_config && !tidy_config->error.empty()) {
            auto warning = feature::file_warning(std::format(
                "clang-tidy does not run on this file until its configuration parses. {}",
                tidy_config->error));
            warning.code = "invalid-clang-tidy-config";
            diagnostics.insert(diagnostics.begin(), std::move(warning));
        }

        // Self-containment trial result. Scored once per stable input
        // state: trial_done is reset whenever compile inputs change for
        // reasons other than buffer edits, so a dependency change re-runs
        // the trial while ordinary typing never does. Only NeedsContext is
        // persisted — SelfContained is recorded in memory alone (dependency
        // changes erase it) so queryContext can dedup identical-flag hosts
        // once the result is actually earned, never on a guess.
        if(trial_round) {
            session->trial_done = true;
            trial_misses = missing_context_errors(diagnostics);
            if(trial_misses == 0) {
                contexts.commands.record_header_mode(path_id, HeaderMode::SelfContained);
            } else {
                LOG_INFO("Header {} needs includer context, re-compiling with prefix", file_path);
                // In memory only until the includer's context proves to
                // supply what the trial missed.
                contexts.commands.record_header_mode(path_id, HeaderMode::NeedsContext);
                continue;
            }
        } else if(synthesized && trial_misses > 0 &&
                  missing_context_errors(diagnostics) < trial_misses) {
            // Scored on the buffer: a restart keeps it only for the same
            // text on disk. A context that misses as much (a database
            // without the include directories the includer needs too) is
            // no result to keep.
            contexts.commands.record_header_mode(path_id, HeaderMode::NeedsContext, session->hash);
        }

        // The commit: build the whole package off to the side, then
        // install it with no suspension before the outcome becomes
        // visible (contract 16) — a joiner must never see Success before
        // the latest compile describes the buffer it compiled. A Lost
        // invalidation mid-flight (ctx no longer current, generation
        // unchanged) commits the same package as bounded staleness —
        // recorded, published, but not current — and reports Stale so
        // waiters drive the recompile.
        bool current = ctx.current();

        // The parse consumed the pair and completed without blaming it —
        // and the round still describes live inputs: the key's strikes
        // were transient, not the storage. A voided round earns no
        // acquittal: the void may BE a fresh blame from another consumer
        // of the shared key, and clearing here would reset the strike
        // count it just paid for.
        if(current && consuming_pch && !result.value().pch_suspect) {
            pch.consumed_ok(*consumed_pch);
        }

        auto& index_data = result.value().tu_index_data;
        auto next = std::make_shared<DocumentCompile>();
        next->pch_key = consumed_pch;
        next->failed_pch_key = failed_pch;
        // The AST and the file index commit together — that pairing is
        // what lets navigation trust the index after ensure_compiled. A
        // compile that produced no index data (no AST, or an index it could
        // not hand over) must
        // therefore drop the previous buffer's index rather than leave it
        // posing as current: an honest gap over yesterday's offsets.
        if(result.value().index_in_file) {
            // Copied, not mapped: the index outlives the transfer file.
            if(auto read = vfs::read(transfer->tmp_path, vfs::Read::Bytes)) {
                next->index =
                    std::make_shared<index::TUIndex>(index::TUIndex::from_buffer(std::move(*read)));
            } else {
                diagnostics.push_back(
                    index_unavailable(std::format("reading this file's index from {} failed: {}",
                                                  transfer->tmp_path,
                                                  read.error().message())));
            }
        } else if(!index_data.empty()) {
            next->index = std::make_shared<index::TUIndex>(index::TUIndex::from_buffer(
                llvm::MemoryBuffer::getMemBufferCopy(as_chars(index_data))));
        }

        LOG_PERF("request", "kind=Compile file={} total_ms={:.2f}", file_path, timer.ms_f());
        std::vector<feature::FileDiagnostic> unconfirmed;
        if(params.skip_bodies) {
            auto unused = [](const feature::FileDiagnostic& diagnostic) {
                return ranges::contains(unused_declaration_warnings, diagnostic.id);
            };
            ranges::copy_if(diagnostics, std::back_inserter(unconfirmed), unused);
            std::erase_if(diagnostics, unused);
        }
        next->output = CompileOutput{
            .version = session->version,
            .source = source,
            .diagnostics = std::move(diagnostics),
            .unconfirmed = std::move(unconfirmed),
            .unmatched_host = resolution.unmatched_host.valid()
                                  ? project.file_table.display(resolution.unmatched_host)
                                  : std::string(),
            .query_failure = resolution.query_failure,
        };
        next->output->skipped = shown_skipped(*session, *next->output);

        auto& entry = compiles.entries[path_id];
        // A document moving off a preamble it failed on releases that
        // failure: only the preamble's consumers would ever retry it.
        // FIXME: released even while another open document still fails on that
        // preamble: its next compile runs the failing build once more.
        if(entry.latest && entry.latest->failed_pch_key &&
           entry.latest->failed_pch_key != failed_pch) {
            pch.forget_failure(*entry.latest->failed_pch_key);
        }
        entry.latest = std::move(next);
        if(tidy_config) {
            result.value().deps.append_range(tidy_config->files);
        }
        if(tidy_attempt) {
            session->quarantine->on_success(tidy_kind);
        } else if(session->tidy_crash) {
            record_crash(session, tidy_kind, *session->tidy_crash);
            session->tidy_crash.reset();
        }
        entry.deps =
            capture_deps_snapshot(project.file_table, result.value().deps, result.value().build_at);
        entry.current = current;
        session->quarantine->on_success(compile_kind);
        session->crashed_pch.clear();
        on_output.emit(session);
        // The push above told clients to re-pull what the fresh AST now
        // answers better; one refresh per commit.
        session->index_served = false;
        if(on_indexing_needed) {
            on_indexing_needed();
        }
        co_return current ? RoundOutcome::Success : RoundOutcome::Stale;
    }
    // Every arm of the two-send loop commits or returns; the trial's
    // continue only fires on the first send, the artifact retry only once.
    std::unreachable();
}

kota::task<std::optional<std::string>>
    ASTFamily::ensure_pch(const std::shared_ptr<Session>& session,
                          llvm::StringRef text,
                          std::optional<std::uint32_t> point,
                          std::uint64_t license_generation,
                          std::uint64_t license_epoch,
                          const std::string& directory,
                          const std::vector<std::string>& arguments,
                          const Resolution& resolution,
                          const PCMFamily::ModuleDeps& imports) {
    auto path_id = session->path_id;
    auto license = [&] {
        return session->generation == license_generation &&
               compiles.epoch(path_id) == license_epoch;
    };

    // The precompiled header the build declares for the command, as a
    // compile round takes it (see run()).
    std::optional<std::string> base;
    if(auto unit = contexts.commands.pch_unit(path_id, resolution)) {
        auto request = pch.precompiled_header(*unit, project.config.project.skip_header_bodies);
        auto key = request.pch_key;
        if(co_await pch.acquire(std::move(request)) == PCHFamily::Outcome::Ready &&
           !pch.reads(key, path_id)) {
            base = key;
        }
    }

    auto plan = plan_pch(path_id,
                         text,
                         point,
                         directory,
                         arguments,
                         resolution.synthesized.get(),
                         imports,
                         base.value_or(""));
    switch(plan.decision) {
        case PCHPlan::Decision::None:
            if(license() && !plan.partial) {
                compiles.set_pch_key(path_id, base);
                session->quarantine->on_success(evidence_kind(EvidenceKind::PCH));
            }
            co_return base;
        case PCHPlan::Decision::Acquire: break;
    }
    auto pch_key = plan.request.pch_key;

    // A preamble that crashed a worker bars the document's content builds
    // (the caller checks); only a compile round's license forgives it.
    auto pch_kind = evidence_kind(EvidenceKind::PCH);
    if(auto* crash = pch.crashed(pch_key)) {
        // Booked once: a request on unchanged inputs is no new crash.
        if(!session->quarantine->crashed(pch_kind)) {
            record_crash(session, pch_kind, *crash);
        }
        co_return base;
    }
    if(co_await pch.acquire(std::move(plan.request)) != PCHFamily::Outcome::Ready) {
        if(auto* crash = pch.crashed(pch_key)) {
            record_crash(session, pch_kind, *crash);
        }
        co_return base;
    }

    // Adoption is gated on the round outcome, never on leftover cache
    // paths, and on this request's own license: a supersede or a
    // Lost-type invalidation while we waited means the resolved command
    // may describe nothing — neither the key write nor the evidence
    // wash belongs to this request anymore. The request itself still
    // compiles against the artifact: it was planned from the text it sends.
    if(license() && !plan.partial) {
        compiles.set_pch_key(path_id, pch_key);
        // Adopting a proven-good artifact clears the session's PCH record as
        // surely as building one — but only its own; every consumer clears
        // for itself.
        session->quarantine->on_success(pch_kind);
    }
    co_return pch_key;
}

kota::task<bool> ASTFamily::prepare_stateless_inputs(const Ticket& ticket,
                                                     llvm::StringRef text,
                                                     std::uint32_t point,
                                                     const std::string& directory,
                                                     const std::vector<std::string>& arguments,
                                                     const Resolution& resolution,
                                                     StatelessInputs& inputs) {
    auto* synthesized = resolution.synthesized.get();
    auto& session = ticket.session;
    auto path_id = session->path_id;
    auto license_epoch = compiles.epoch(path_id);

    // The scan runs under the request's command with the text the dispatch
    // will compile, so an unsaved `import m;` builds its PCM before the parse
    // needs it.
    llvm::SmallVector<const char*, 32> argv;
    argv.reserve(arguments.size());
    for(auto& arg: arguments) {
        argv.push_back(arg.c_str());
    }
    auto imports = co_await pcm.prepare_deps(path_id, resolution, argv, directory, text);
    if(!imports) {
        co_return false;
    }

    if(readonly != ReadonlyMode::On) {
        auto pch_key = co_await ensure_pch(session,
                                           text,
                                           point,
                                           ticket.generation,
                                           license_epoch,
                                           directory,
                                           arguments,
                                           resolution,
                                           *imports);
        if(pch_key) {
            if(auto pch_it = project.pch_cache.find(*pch_key); pch_it != project.pch_cache.end()) {
                inputs.pch = {pch_it->second.path, pch_it->second.bound};
            }
        }
    }

    project.fill_pcm_deps(inputs.pcms, pcm.closure(imports->resolved).getArrayRef());

    co_return true;
}

}  // namespace clice
