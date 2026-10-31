module clice:unittests.server.served_project_fixture;

import :command.database;
import :project.command_resolver;
import :server.master;
import :server.served_project;
import :server.session;
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
    kota::event_loop loop;
    MasterServer server{loop, ""};
    std::shared_ptr<ServedProject> served;

    explicit ServedProjectFixture(llvm::StringRef root = "/proj") {
        served = std::make_shared<ServedProject>(server, CanonicalPath(Spelling::absolute(root)));
        server.projects.push_back(served);
        // The server applies the file table's changes, and the project
        // flushes its metadata, on a later turn of its running event loop;
        // the tests run none between their steps, and hand the changes over
        // themselves.
        server.files.disk.on_change = nullptr;
        served->project.request_flush = nullptr;
    }

    ServedProject& project() {
        return *served;
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
        auto* document = served->find(path_id);
        assert(document && "the document is open");
        return *document;
    }

    // Accessors of the compile tests (compile_tests.cpp).

    void set_pch_keys(Fid path_id,
                      std::optional<std::string> pch_key,
                      std::optional<std::string> base_pch_key) {
        served->set_pch_keys(path_id, std::move(pch_key), std::move(base_pch_key));
    }

    void set_output(Fid path_id, CompileOutput output) {
        served->set_output(path_id, std::move(output));
    }

    /// The project's PCH adoption step (ServedProject::ensure_pch) under the
    /// builtin command: whether a request on the session's buffer, taken at
    /// `generation` and the document's `epoch`, compiles against a PCH.
    kota::task<bool> ensure_pch(const std::shared_ptr<Session>& session,
                                std::uint64_t generation,
                                std::uint64_t epoch,
                                const std::string& directory,
                                const std::vector<std::string>& arguments) {
        auto& project = served->project;
        Resolution resolution{
            .ref = {.command = project.build.builtin(project.file_table.resolve(session->path_id))},
        };
        auto key = co_await served->ensure_pch(Ticket{session, generation},
                                               epoch,
                                               session->text(),
                                               std::nullopt,
                                               directory,
                                               arguments,
                                               resolution,
                                               {});
        co_return key.has_value();
    }

    // Accessors of the change tests (changes_tests.cpp, databases_tests.cpp).

    void build_changed(CDBDiff delta) {
        served->build_changed(std::move(delta));
    }

    void recheck_choices() {
        served->recheck_choices();
    }

    /// Watch the project's databases and its git checkout as start() does,
    /// from the loads the test made itself.
    void watch_databases() {
        served->watching = true;
        for(std::size_t i = 0; i < served->project.cdb.source_count(); i += 1) {
            served->watch_database(SourceID(i));
        }
        served->watch_checkout();
    }

    /// The delta reload_databases follows (build_changed), not followed: the
    /// databases found at the watched places registered, then every watched
    /// database ticked.
    CDBDiff database_delta(bool force) {
        if(!served->project.build.declares_sources()) {
            served->discover_databases();
        }
        CDBDiff delta;
        for(auto& database: served->watched) {
            served->tick_database(database, force, delta);
        }
        return delta;
    }

    // Accessors of the live sources' tests (query_overlay_tests.cpp,
    // query_freshness_tests.cpp).
};

}  // namespace clice::testing
