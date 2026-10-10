module;

#include "modules/prelude.h"

module clice;

import :support.signal;
import :tests.unit.test.test;
import :worker.probe;

namespace clice::testing {
namespace {

ZEST_SUITE(BuildProbe, kota::zest::LoopFixture) {

BuildProbe probe;

/// The holds announced so far, by id.
std::vector<std::uint64_t> held;
Signal<std::uint64_t>::Connection held_conn =
    probe.on_held.connect([this](std::uint64_t id) { held.push_back(id); });

ZEST_CASE(counts_by_kind) {
    run(probe.returned(BuildKind::Compile, "/w/main.cpp"),
        probe.returned(BuildKind::Compile, "/w/main.cpp"),
        probe.returned(BuildKind::PCH, "/w/main.cpp"),
        probe.returned(BuildKind::Index, "/w/lib.cpp"));

    auto& main = probe.builds["/w/main.cpp"];
    ZEXPECT(main[std::to_underlying(BuildKind::Compile)] == 2u);
    ZEXPECT(main[std::to_underlying(BuildKind::PCH)] == 1u);
    ZEXPECT(main[std::to_underlying(BuildKind::PCM)] == 0u);
    ZEXPECT(probe.builds["/w/lib.cpp"][std::to_underlying(BuildKind::Index)] == 1u);
}

ZEST_CASE(kind_names_round_trip) {
    for(auto kind: {BuildKind::Compile, BuildKind::PCH, BuildKind::PCM, BuildKind::Index}) {
        ZEXPECT(parse_build_kind(build_kind_name(kind)) == kind);
    }
    ZEXPECT(!parse_build_kind("query"));
}

ZEST_CASE(hold_parks_until_release) {
    // The matching reply parks and is announced; the release lets it on.
    auto id = probe.hold(BuildKind::Compile, "/w/main.cpp");
    bool delivered = false;
    auto reply = [&]() -> kota::task<> {
        co_await probe.returned(BuildKind::Compile, "/w/main.cpp");
        delivered = true;
    };
    auto test = [&]() -> kota::task<> {
        while(held.empty()) {
            co_await kota::yield();
        }
        ZEXPECT(held == std::vector{id});
        ZEXPECT(probe.holds().front().parked);
        ZEXPECT(!delivered);
        ZEXPECT(probe.release(id));
    };
    run(reply(), test());
    ZEXPECT(delivered);
    ZEXPECT(probe.holds().empty());
}

ZEST_CASE(hold_claims_one_reply) {
    // A hold parks the first matching reply only; replies of another kind
    // or file never park.
    auto id = probe.hold(BuildKind::Index, "/w/lib.cpp");
    int delivered = 0;
    auto reply = [&](BuildKind kind, llvm::StringRef file) -> kota::task<> {
        co_await probe.returned(kind, file);
        delivered += 1;
    };
    auto test = [&]() -> kota::task<> {
        while(held.empty()) {
            co_await kota::yield();
        }
        // The other kind and the other file went through; the second index
        // reply finds the hold taken and goes through too.
        ZEXPECT(delivered == 3);
        ZEXPECT(probe.release(id));
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
    auto id = probe.hold(BuildKind::PCM, "/w/m.cppm");
    ZEXPECT(probe.release(id));
    ZEXPECT(!probe.release(id));
    bool delivered = false;
    auto reply = [&]() -> kota::task<> {
        co_await probe.returned(BuildKind::PCM, "/w/m.cppm");
        delivered = true;
    };
    run(reply());
    ZEXPECT(delivered);
    ZEXPECT(held.empty());
}

ZEST_CASE(release_all_frees_parked) {
    probe.hold(BuildKind::Compile, "/w/a.cpp");
    probe.hold(BuildKind::PCH, "/w/b.cpp");
    int delivered = 0;
    auto reply = [&](BuildKind kind, llvm::StringRef file) -> kota::task<> {
        co_await probe.returned(kind, file);
        delivered += 1;
    };
    auto test = [&]() -> kota::task<> {
        while(held.size() < 2) {
            co_await kota::yield();
        }
        probe.release_all();
    };
    run(reply(BuildKind::Compile, "/w/a.cpp"), reply(BuildKind::PCH, "/w/b.cpp"), test());
    ZEXPECT(delivered == 2);
    ZEXPECT(probe.holds().empty());
}

ZEST_CASE(gates_share_hold_ids) {
    // Gates and holds draw from one id space and release by id alike; a
    // gate no request went out under drops.
    auto hold = probe.hold(BuildKind::Compile, "/w/a.cpp");
    auto gate = probe.gate("clice/worker/completion /w/a.cpp");
    ZEXPECT(gate == hold + 1);
    ZEXPECT(probe.gates().size() == 1u);

    // Another request's tag leaves the gate standing, unsent.
    probe.sending("clice/worker/format /w/a.cpp", {});
    ZEXPECT(!probe.gates().front().sent);

    ZEXPECT(probe.release(gate));
    ZEXPECT(!probe.release(gate));
    ZEXPECT(probe.gates().empty());
    ZEXPECT(probe.release(hold));
}

ZEST_CASE(gate_park_announced) {
    // The worker's park of a standing gate is announced like a hold's; one
    // of a gate already released is not.
    auto gate = probe.gate("clice/worker/compile /w/a.cpp");
    probe.gate_parked(gate);
    ZEXPECT(held == std::vector{gate});
    ZEXPECT(probe.gates().front().parked);

    probe.release_all();
    ZEXPECT(probe.gates().empty());
    probe.gate_parked(gate);
    ZEXPECT(held.size() == 1u);
}

};  // ZEST_SUITE(BuildProbe)

}  // namespace
}  // namespace clice::testing
