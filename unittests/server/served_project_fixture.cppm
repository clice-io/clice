module clice:unittests.server.served_project_fixture;

import :command.database;
import :project.command;
import :project.graph.task_graph;
import :server.master;
import :server.served_project;
import :server.session;
import :settings.rules;
import :support.prelude;
import :vfs.file_table;
import :vfs.path;

namespace clice::testing {

/// A master with one served project, as the server holds them, and no
/// client: the pool stays workerless until a test starts it, and the
/// project is not started (ServedProject::start), so nothing loads from
/// disk unless a test asks. Documents open through the master's table, as
/// didOpen puts them, and the project adopts them. The private state the
/// tests set up or check goes through the accessors below, ServedProject's
/// only friend.
struct ServedProjectFixture {
    async::event_loop loop;
    MasterServer server{loop, ""};
    std::shared_ptr<ServedProject> served;

    explicit ServedProjectFixture(llvm::StringRef root = "/proj") {
        served = std::make_shared<ServedProject>(server, CanonicalPath(Spelling::absolute(root)));
        served->project.rules = Rules(served->project.settings, served->project.root, "");
        server.projects.push_back(served);
        // The server applies the file table's changes, and the project
        // flushes its metadata, on a later turn of its running event loop;
        // the tests run none between their steps, and hand the changes over
        // themselves.
        server.files.disk.on_change = nullptr;
        hold_flushes(*served);
    }

    /// `project` schedules no metadata flush: one counts as scheduled.
    static void hold_flushes(ServedProject& project) {
        project.metadata_flush_scheduled = true;
    }

    Fid intern(llvm::StringRef path) {
        return server.files.intern(Spelling::absolute(path));
    }

    /// Open `path` holding `text`, routed to the project.
    std::shared_ptr<Session> open(llvm::StringRef path, std::string text, int version = 0) {
        auto path_id = intern(path);
        auto session = std::make_shared<Session>(path_id, std::move(text), version);
        server.documents[path_id] = {.session = session, .owner = served.get()};
        served->adopt(session);
        return session;
    }

    /// The project's entry of an open document.
    ServedDocument& entry(Fid path_id) {
        auto* document = served->find_document(path_id);
        assert(document && "the document is open");
        return *document;
    }

    /// The generation of the document's graph node: every invalidation of
    /// it moves it on.
    std::uint64_t generation(Fid path_id) const {
        return served->project.graph.generation(ast_node(path_id));
    }

    /// Whether an attempt of the document's compile is live.
    bool compiling(Fid path_id) const {
        return served->project.graph.is_compiling(ast_node(path_id));
    }

    // Accessors of the compile tests (compile_tests.cpp).

    void update_latest(Fid path_id, llvm::function_ref<void(DocumentCompile&)> edit) {
        served->update_latest(path_id, edit);
    }

    /// The document's latest compile describes its buffer, as one that ran
    /// at its node's generation now does.
    void compiled_now(Fid path_id) {
        update_latest(path_id, [&](DocumentCompile& compiled) {
            compiled.generation = generation(path_id);
        });
    }

    bool attribute_crash(const std::shared_ptr<Session>& session, bool tidy, ipc::Error error) {
        return served->attribute_crash(session, tidy, std::nullopt, error);
    }

    /// The PCH step of a request's own parse (completion, signature help)
    /// under the builtin command: build what a request on the session's
    /// buffer loads, then let the document adopt it as a request taken at
    /// `ticket` and the node's `generation` would (ServedProject::adopt_pch).
    /// Whether the request compiles against a PCH.
    async::task<bool> build_pch(const std::shared_ptr<Session>& session,
                                std::uint64_t ticket,
                                std::uint64_t generation,
                                const std::string& directory,
                                const std::vector<std::string>& arguments) {
        auto& project = served->project;
        Resolution resolution;
        resolution.ref = {
            .command = project.fallback_command(project.file_table.resolve(session->path_id))};
        resolution.directory = directory;
        resolution.arguments = arguments;
        auto built = co_await project.build_pch(session->path_id,
                                                session->text(),
                                                std::nullopt,
                                                resolution,
                                                {},
                                                {});
        if(!built) {
            co_return false;
        }
        served->adopt_pch(Ticket{session, ticket}, generation, *built);
        co_return built->loaded.has_value();
    }

    // Accessors of the change tests (changes_tests.cpp).

    void commands_changed(CDBDiff delta) {
        served->commands_changed(std::move(delta));
    }
};

}  // namespace clice::testing
