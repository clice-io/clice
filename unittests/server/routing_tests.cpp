module clice;

import :feature.diagnostics;
import :index.indexer;
import :index.tu_index;
import :project.background;
import :project.project;
import :server.master;
import :server.served_project;
import :server.session;
import :settings.settings;
import :support.text;
import :unittests.server.served_project_fixture;
import :unittests.utils.cdb_helper;
import :unittests.utils.merge_unit;
import :unittests.utils.platform;
import :unittests.utils.tester;
import :unittests.utils;

namespace clice::testing {
namespace {

/// What answers a request on an open document (ServedProject::pick_route),
/// over what is ready: a current AST, an index source, whether the document
/// is compiled, and what the request asks.
ZEST_SUITE(Routing, Tester) {

ServedProjectFixture fx;
Project& project = fx.served->project;

/// The path of the focus file, main.cpp.
std::string focus;

/// Compile main.cpp holding `text`.
void compile_focus(llvm::StringRef text) {
    add_focus("main.cpp", text);
    kind = CompilationKind::Whole;
    ZASSERT(compile());
    focus = TestVFS::path("main.cpp");
}

/// Compile main.cpp holding `text` and merge its index into the project's,
/// as the background index does.
void index_focus(llvm::StringRef text) {
    compile_focus(text);
    Fid file;
    merge_unit(project, *unit, file);
}

/// Open main.cpp with the text compiled, compiled or not: under readonly
/// "auto" a document opens not compiled.
std::shared_ptr<Session> open(bool compiled) {
    project.settings.project.readonly = compiled ? ReadonlyMode::Off : ReadonlyMode::Auto;
    return fx.open(focus, sources.all_files.lookup("main.cpp").content);
}

/// The route of a request on the ticket asking `options`.
Route route(const Ticket& ticket, RouteOptions options = {}) {
    std::optional<Route> picked;
    auto body = [&]() -> async::task<> {
        picked = co_await fx.served->pick_route(ticket, options);
    };
    auto task = body();
    fx.loop.schedule(task);
    fx.loop.run();
    ZASSERT(picked.has_value());
    return *picked;
}

Route route(const std::shared_ptr<Session>& session, RouteOptions options = {}) {
    return route(Ticket::take(session), options);
}

ZEST_CASE(NothingServesUncompiled) {
    compile_focus("int x;\n");
    auto session = open(false);
    ZEXPECT(route(session) == Route::Empty);
    ZEXPECT(route(session, {.needs_ast = true}) == Route::Empty);
    // Nothing indexes the file: there is no index attempt to wait for.
    ZEXPECT(route(session, {.waits_for_index = true}) == Route::Empty);
}

ZEST_CASE(CompiledWaitsForAST) {
    compile_focus("int x;\n");
    auto session = open(true);
    ZEXPECT(route(session) == Route::AST);
    ZEXPECT(route(session, {.needs_ast = true}) == Route::AST);
}

ZEST_CASE(ShardServesUncompiled) {
    index_focus("int x;\n");
    auto session = open(false);
    ZEXPECT(route(session) == Route::Index);
    ZEXPECT(route(session, {.lexes_whole_buffer = true}) == Route::Index);
    ZEXPECT(route(session, {.needs_ast = true}) == Route::Empty);
}

ZEST_CASE(ShardServesWhileCompiling) {
    index_focus("int x;\n");
    auto session = open(true);
    ZEXPECT(route(session, {.needs_ast = true}) == Route::AST);

    // The index answers at once, without waiting for the compile.
    ZEXPECT(route(session) == Route::Index);
}

ZEST_CASE(ContextNeedsOwnCompile) {
    index_focus("int x;\n");
    auto session = open(false);
    auto file = session->path_id;
    // The shard merges every context: a document not compiled has no
    // other answer.
    fx.server.own_context_only.insert(file);
    ZEXPECT(route(session) == Route::Index);

    fx.served->start_compiling(file);
    ZEXPECT(route(session) == Route::AST);
    fx.server.own_context_only.erase(file);
    ZEXPECT(route(session, {.needs_ast = true}) == Route::AST);
    fx.server.choices[file] = {.project = fx.served.get()};
    ZEXPECT(route(session) == Route::AST);
}

ZEST_CASE(CurrentASTAnswers) {
    compile_focus("int x;\n");
    auto session = open(true);
    auto file = session->path_id;
    auto own = index::TUIndex::from_buffer(
        llvm::MemoryBuffer::getMemBufferCopy(as_chars(index::build_tu_index(*unit))));
    fx.update_latest(file, [&](DocumentCompile& compiled) {
        compiled.index = std::make_shared<index::TUIndex>(std::move(own));
    });
    fx.compiled_now(file);
    ZEXPECT(route(session) == Route::AST);
    ZEXPECT(route(session, {.lexes_whole_buffer = true}) == Route::AST);

    // A crash stops the compile: its own index still answers, the AST
    // only a request needing it.
    session->crashed(feature::RequestKind::Compile, "d1", "crashed", Session::Clock::now());
    ZEXPECT(route(session) == Route::Index);
    ZEXPECT(route(session, {.needs_ast = true}) == Route::AST);
}

ZEST_CASE(OversizedBufferSkipsIndex) {
    index_focus("int x;\n" + std::string(max_lexed_buffer, ' '));
    auto session = open(false);
    ZEXPECT(route(session) == Route::Index);
    ZEXPECT(route(session, {.lexes_whole_buffer = true}) == Route::Empty);
    fx.served->start_compiling(session->path_id);
    ZEXPECT(route(session, {.lexes_whole_buffer = true}) == Route::AST);
}

ZEST_CASE(WaitsForIndex) {
    compile_focus("int x;\n");
    auto session = open(false);
    auto file = session->path_id;
    list_unit(project.cdb, focus);
    ZASSERT(project.background_index.enqueue(file, ReindexReason::ContentChanged));
    ZEXPECT(route(session) == Route::Empty);

    std::optional<Route> picked;
    auto ask = [&]() -> async::task<> {
        picked = co_await fx.served->pick_route(Ticket::take(session), {.waits_for_index = true});
    };
    auto asked = ask();
    fx.loop.schedule(asked);
    fx.loop.run();
    ZEXPECT(!picked.has_value());

    // The attempt indexed the file: the route is decided again from its
    // shard.
    Fid indexed;
    merge_unit(project, *unit, indexed);
    auto finish = [&]() -> async::task<> {
        co_await project.background_index.stop();
    };
    auto finished = finish();
    fx.loop.schedule(finished);
    fx.loop.run();
    ZEXPECT(picked == Route::Index);
}

ZEST_CASE(OutdatedTicket) {
    compile_focus("int x;\n");
    auto session = open(true);
    auto ticket = Ticket::take(session);
    session->generation += 1;
    ZEXPECT(route(ticket) == Route::Outdated);
}

};  // ZEST_SUITE(Routing)

}  // namespace
}  // namespace clice::testing
