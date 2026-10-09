module;

#include "modules/prelude.h"

module clice;

import :server.counterparts;
import :tests.unit.test.test;

namespace clice::testing {
namespace {

using query::Evidence;

std::vector<std::string> order(const query::Ranking& ranking) {
    std::vector<std::string> paths;
    for(auto& candidate: ranking.candidates) {
        paths.push_back(candidate.path);
    }
    return paths;
}

ZEST_SUITE(Counterparts) {

ZEST_CASE(NoCandidates) {
    auto ranking = query::rank_counterparts({});
    ZEXPECT(ranking.candidates.empty());
    ZEXPECT(!ranking.decisive);
}

ZEST_CASE(SingleCandidateIsDecisive) {
    auto ranking = query::rank_counterparts({
        {.path = "shapes.cppm", .module = "shapes"}
    });
    ZEXPECT(ranking.decisive);
}

ZEST_CASE(EvidenceOrdersCandidates) {
    auto ranking = query::rank_counterparts({
        {.path = "module.cppm", .module = "lib"},
        {.path = "tests/foo.cpp", .same_name = true},
        {.path = "bar.cpp", .overlap = 9},
        {.path = "foo.cpp", .same_name = true, .overlap = 2},
    });
    ZEXPECT(order(ranking) ==
            std::vector<std::string>{"foo.cpp", "bar.cpp", "tests/foo.cpp", "module.cppm"});
    // bar.cpp shares more declarations than the first.
    ZEXPECT(!ranking.decisive);
}

ZEST_CASE(NameAndOverlapWin) {
    auto ranking = query::rank_counterparts({
        {.path = "tests/shape.cpp", .same_name = true},
        {.path = "src/shape.cpp", .same_name = true, .overlap = 2},
        {.path = "module.cppm", .module = "lib"},
    });
    ZEXPECT(order(ranking).front() == "src/shape.cpp");
    ZEXPECT(ranking.decisive);
}

ZEST_CASE(TwiceTheOverlapWins) {
    auto ranking = query::rank_counterparts({
        {.path = "text_case.cpp", .overlap = 1},
        {.path = "text_ops.cpp",  .overlap = 3},
    });
    ZEXPECT(order(ranking).front() == "text_ops.cpp");
    ZEXPECT(ranking.decisive);
}

ZEST_CASE(CloseOverlapAsks) {
    auto ranking = query::rank_counterparts({
        {.path = "a.cpp", .overlap = 4},
        {.path = "b.cpp", .overlap = 3},
    });
    ZEXPECT(!ranking.decisive);
}

ZEST_CASE(EqualEvidenceAsks) {
    auto ranking = query::rank_counterparts({
        {.path = "src/foo.cpp",   .same_name = true, .distance = 3},
        {.path = "tests/foo.cpp", .same_name = true, .distance = 3},
    });
    ZEXPECT(order(ranking) == std::vector<std::string>{"src/foo.cpp", "tests/foo.cpp"});
    ZEXPECT(!ranking.decisive);
}

ZEST_CASE(NameAgainstOverlapAsks) {
    // A source of the same name the index has not reached yet must not lose
    // to another one defining a single declaration.
    auto ranking = query::rank_counterparts({
        {.path = "foo.cpp", .same_name = true},
        {.path = "bar.cpp", .overlap = 1     },
    });
    ZEXPECT(order(ranking).front() == "bar.cpp");
    ZEXPECT(!ranking.decisive);
}

ZEST_CASE(ModulePairingBreaksTie) {
    auto ranking = query::rank_counterparts({
        {.path = "other/shapes.cpp", .same_name = true},
        {.path = "shapes.cpp", .same_name = true, .module = "shapes"},
    });
    ZEXPECT(order(ranking).front() == "shapes.cpp");
    ZEXPECT(ranking.decisive);
}

ZEST_CASE(ModuleUnitsAsk) {
    auto ranking = query::rank_counterparts({
        {.path = "a.cpp", .module = "lib"},
        {.path = "b.cpp", .module = "lib"},
    });
    ZEXPECT(!ranking.decisive);
}

};  // ZEST_SUITE(Counterparts)

}  // namespace
}  // namespace clice::testing
