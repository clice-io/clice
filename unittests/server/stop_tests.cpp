module clice;

import :project.background;
import :server.master;
import :server.served_project;
import :settings.rules;
import :unittests.server.served_project_fixture;
import :unittests.utils.async;
import :unittests.utils.cdb_helper;
import :unittests.utils.temp_dir;
import :unittests.utils;
import :unittests.worker.helpers;
import :worker.pool;
import :worker.protocol;
import :worker.test_hooks;

namespace clice::testing {

namespace {

ZEST_SUITE(MasterStop) {

ZEST_CASE(PassEndsBeforeWorkers) {
    // A stopping pool fails every dispatch at once: a pass still feeding
    // would walk its whole queue through it, each unit failing and
    // requeued, before the stop could save.
    TempDir tmp;
    ServedProjectFixture fixture(tmp.root);
    auto& project = fixture.served->project;
    project.settings.project.idle_timeout_ms = 0;
    project.settings.rules.push_back(Rule{.patterns = {"**"}, .query_toolchain = false});
    project.rules = Rules(project.settings, project.root, "");
    std::vector<Fid> units;
    for(int i = 0; i < 8; i += 1) {
        auto name = std::format("u{}.cpp", i);
        tmp.touch(name, "");
        units.push_back(list_unit(project.cdb, tmp.path(name)));
    }

    bool done = false;
    auto body = [&]() -> async::task<> {
        WorkerPoolOptions opts;
        opts.program = clice_binary();
        opts.stateless_count = 1;
        opts.stateful_count = 0;
        ZASSERT(fixture.server.pool.start(opts));
        fixture.server.pool.hooks = &fixture.server.hooks;
        auto gate = fixture.server.hooks.gate(worker::crash_tag(
            worker::CompileUnitParams{.file = std::string(project.file_table.resolve(units[0]))}));
        project.background_index.start({});
        co_await parked(fixture.server.hooks, gate);

        co_await fixture.server.stop();
        ZEXPECT(project.background_index.progress().completed == 0u);
        for(auto unit: units) {
            ZEXPECT(project.background_index.queue().contains(unit));
        }
        done = true;
    };
    auto task = body();
    fixture.loop.schedule(task);
    fixture.loop.run();
    ZEXPECT(done);
}

};  // ZEST_SUITE(MasterStop)

}  // namespace

}  // namespace clice::testing
