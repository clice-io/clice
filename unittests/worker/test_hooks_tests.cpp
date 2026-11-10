module clice;

import :support.signal;
import :unittests.utils;
import :worker.protocol;
import :worker.test_hooks;

namespace clice::testing {
namespace {

ZEST_SUITE(TestHooks, zest::LoopFixture) {

TestHooks hooks;

/// The holds announced so far, by id.
std::vector<std::uint64_t> held;
Signal<std::uint64_t>::Connection held_conn =
    hooks.on_held.connect([this](std::uint64_t id) { held.push_back(id); });

ZEST_CASE(counts_by_kind) {
    run(hooks.returned(BuildKind::Compile, "/w/main.cpp"),
        hooks.returned(BuildKind::Compile, "/w/main.cpp"),
        hooks.returned(BuildKind::PCH, "/w/main.cpp"),
        hooks.returned(BuildKind::Index, "/w/lib.cpp"));

    auto& main = hooks.builds["/w/main.cpp"];
    ZEXPECT(main[std::to_underlying(BuildKind::Compile)] == 2u);
    ZEXPECT(main[std::to_underlying(BuildKind::PCH)] == 1u);
    ZEXPECT(main[std::to_underlying(BuildKind::PCM)] == 0u);
    ZEXPECT(hooks.builds["/w/lib.cpp"][std::to_underlying(BuildKind::Index)] == 1u);
}

ZEST_CASE(counts_builds_run) {
    // A build counts once its worker ran it to its end; one the master
    // withdrew or no worker took does not, nor does any other request.
    using Compiled = kotatsu::outcome<worker::CompileResult, ipc::Error>;
    worker::CompileParams compile{.path = "/w/main.cpp"};
    Compiled withdrawn = kotatsu::outcome_error(
        ipc::Error{worker::dispatch_errc::cancelled, "more urgent work came first"});
    Compiled stopped = worker::CompileResult{.status = worker::CompileStatus::Cancelled};
    Compiled unsent = kotatsu::outcome_error(
        ipc::Error{worker::dispatch_errc::worker_unavailable, "No stateful workers available"});
    Compiled done = worker::CompileResult{};
    worker::FormatParams format{.file = "/w/main.cpp"};
    kotatsu::outcome<worker::FormatResult, ipc::Error> formatted = worker::FormatResult{};
    run(hooks.replied(compile, withdrawn),
        hooks.replied(compile, stopped),
        hooks.replied(compile, unsent),
        hooks.replied(compile, done),
        hooks.replied(format, formatted));
    ZEXPECT(hooks.builds["/w/main.cpp"][std::to_underlying(BuildKind::Compile)] == 1u);
}

ZEST_CASE(kind_names_round_trip) {
    for(auto kind: {BuildKind::Compile, BuildKind::PCH, BuildKind::PCM, BuildKind::Index}) {
        ZEXPECT(parse_build_kind(build_kind_name(kind)) == kind);
    }
    ZEXPECT(!parse_build_kind("query"));
}

ZEST_CASE(hold_parks_until_release) {
    // The matching reply parks and is announced; the release lets it on.
    auto id = hooks.hold(BuildKind::Compile, "/w/main.cpp");
    bool delivered = false;
    auto reply = [&]() -> async::task<> {
        co_await hooks.returned(BuildKind::Compile, "/w/main.cpp");
        delivered = true;
    };
    auto test = [&]() -> async::task<> {
        while(held.empty()) {
            co_await async::yield();
        }
        ZEXPECT(held == std::vector{id});
        ZEXPECT(hooks.holds().front().parked);
        ZEXPECT(!delivered);
        ZEXPECT(hooks.release(id));
    };
    run(reply(), test());
    ZEXPECT(delivered);
    ZEXPECT(hooks.holds().empty());
}

ZEST_CASE(hold_parks_one_reply) {
    // A hold parks the first matching reply only; replies of another kind
    // or file never park.
    auto id = hooks.hold(BuildKind::Index, "/w/lib.cpp");
    int delivered = 0;
    auto reply = [&](BuildKind kind, llvm::StringRef file) -> async::task<> {
        co_await hooks.returned(kind, file);
        delivered += 1;
    };
    auto test = [&]() -> async::task<> {
        while(held.empty()) {
            co_await async::yield();
        }
        // The other kind and the other file went through; the second index
        // reply finds the hold taken and goes through too.
        ZEXPECT(delivered == 3);
        ZEXPECT(hooks.release(id));
    };
    run(reply(BuildKind::Compile, "/w/lib.cpp"),
        reply(BuildKind::Index, "/w/main.cpp"),
        reply(BuildKind::Index, "/w/lib.cpp"),
        reply(BuildKind::Index, "/w/lib.cpp"),
        test());
    ZEXPECT(delivered == 4);
    ZEXPECT(held.size() == 1u);
}

ZEST_CASE(release_before_arrival) {
    // A hold released before any reply reached it drops: the reply passes.
    auto id = hooks.hold(BuildKind::PCM, "/w/m.cppm");
    ZEXPECT(hooks.release(id));
    ZEXPECT(!hooks.release(id));
    bool delivered = false;
    auto reply = [&]() -> async::task<> {
        co_await hooks.returned(BuildKind::PCM, "/w/m.cppm");
        delivered = true;
    };
    run(reply());
    ZEXPECT(delivered);
    ZEXPECT(held.empty());
}

ZEST_CASE(release_all_frees_parked) {
    hooks.hold(BuildKind::Compile, "/w/a.cpp");
    hooks.hold(BuildKind::PCH, "/w/b.cpp");
    int delivered = 0;
    auto reply = [&](BuildKind kind, llvm::StringRef file) -> async::task<> {
        co_await hooks.returned(kind, file);
        delivered += 1;
    };
    auto test = [&]() -> async::task<> {
        while(held.size() < 2) {
            co_await async::yield();
        }
        hooks.release_all();
    };
    run(reply(BuildKind::Compile, "/w/a.cpp"), reply(BuildKind::PCH, "/w/b.cpp"), test());
    ZEXPECT(delivered == 2);
    ZEXPECT(hooks.holds().empty());
}

ZEST_CASE(gates_share_hold_ids) {
    // Gates and holds draw from one id space and release by id alike; a
    // gate no request went out under drops.
    auto hold = hooks.hold(BuildKind::Compile, "/w/a.cpp");
    auto gate = hooks.gate("clice/worker/completion /w/a.cpp");
    ZEXPECT(gate == hold + 1);
    ZEXPECT(hooks.gates().size() == 1u);

    // Another request's tag leaves the gate standing, unsent.
    hooks.sending("clice/worker/format /w/a.cpp", {});
    ZEXPECT(!hooks.gates().front().sent);

    ZEXPECT(hooks.release(gate));
    ZEXPECT(!hooks.release(gate));
    ZEXPECT(hooks.gates().empty());
    ZEXPECT(hooks.release(hold));
}

ZEST_CASE(gate_park_announced) {
    // The worker's park of a standing gate is announced like a hold's; one
    // of a gate already released is not.
    auto gate = hooks.gate("clice/worker/compile /w/a.cpp");
    hooks.gate_parked(gate);
    ZEXPECT(held == std::vector{gate});
    ZEXPECT(hooks.gates().front().parked);

    hooks.release_all();
    ZEXPECT(hooks.gates().empty());
    hooks.gate_parked(gate);
    ZEXPECT(held.size() == 1u);
}

ZEST_CASE(answer_drops_unreached_gate) {
    // A request that came back before its work reached the gate drops the
    // gate; one whose work parked keeps it until the release.
    auto unreached = hooks.gate("clice/worker/format /w/a.cpp");
    auto parked = hooks.gate("clice/worker/compile /w/a.cpp");
    hooks.gate_parked(parked);
    hooks.answered(unreached);
    hooks.answered(parked);
    ZEXPECT(hooks.gates().size() == 1u);
    ZEXPECT(hooks.gates().front().id == parked);
}

};  // ZEST_SUITE(TestHooks)

}  // namespace
}  // namespace clice::testing
