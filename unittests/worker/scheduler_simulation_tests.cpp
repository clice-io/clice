module clice;

import :unittests.utils;
import :unittests.worker.simulator;
import :unittests.worker.workloads;
import :worker.scheduler;

namespace clice::testing {

namespace {

constexpr std::uint64_t GiB = 1ull << 30;

SimSpec on_box(llvm::ArrayRef<UnitLoad> units,
               std::uint64_t memory = 16 * GiB,
               std::uint64_t others = 1450 * MiB) {
    SimSpec spec;
    spec.machine.memory = memory;
    spec.machine.others = {
        {0, others}
    };
    spec.scheduler = {.background_jobs = 4, .max_workers = 4};
    spec.units.assign(units.begin(), units.end());
    return spec;
}

/// A GitHub macOS arm64 runner: 3 vCPU, 7 GiB, the rest of the system
/// holding 3 GiB.
SimSpec on_mac(llvm::ArrayRef<UnitLoad> units, std::uint64_t others = 3 * GiB) {
    auto spec = on_box(units, 7 * GiB, others);
    spec.machine.physical = 3;
    spec.machine.logical = 3;
    spec.machine.oom_kills = false;
    spec.machine.mac_reserve = true;
    spec.scheduler = {.background_jobs = 3, .max_workers = 3};
    return spec;
}

/// Whether the unit fits the machine alone, with `reserve` left.
bool fits(const SimSpec& spec, const UnitLoad& unit, std::uint64_t reserve) {
    auto room = spec.machine.memory - spec.machine.others.front().second;
    return std::uint64_t(unit.peak) * MiB + reserve <= room;
}

/// The units that do not fit the machine even alone, with the reserve
/// kept or without: one between the two may finish before the reserve is
/// missed.
std::size_t too_big_for(const SimSpec& spec, bool keep_reserve) {
    auto reserve = keep_reserve ? sim::reserve(spec.machine) : 0;
    return ranges::count_if(spec.units,
                            [&](const UnitLoad& unit) { return !fits(spec, unit, reserve); });
}

double max_wait(const SimResult& result) {
    return result.foreground_waits.empty() ? 0 : ranges::max(result.foreground_waits);
}

/// The properties every run keeps: no kill by the kernel, every unit done
/// or reported too big, and only units that are too big reported so.
void expect_sound(const SimSpec& spec, const SimResult& result) {
    ZEXPECT(result.oom_kills == 0u);
    ZEXPECT(result.deadline_kills == 0u);
    ZEXPECT(result.given_up.empty());
    ZEXPECT(result.completed + result.too_big.size() == spec.units.size());
    ZEXPECT(result.too_big.size() >= too_big_for(spec, false));
    ZEXPECT(result.too_big.size() <= too_big_for(spec, true));
}

/// The same units with their peaks known, as every run after the first
/// knows them: memory kills only a unit that needs the reserve, which the
/// user's own work beside it takes back, or what another program took
/// memory from.
SimResult known_run(SimSpec spec) {
    spec.known_peaks = true;
    auto result = simulate(spec);
    expect_sound(spec, result);
    if(spec.machine.others.size() == 1) {
        for(auto unit: result.memory_killed) {
            ZEXPECT(!fits(spec, spec.units[unit], sim::reserve(spec.machine)));
        }
    }
    return result;
}

/// A first run, which learns the peaks as it goes, at most `slower` times
/// as long as one that knows them.
SimResult expect_near_known(const SimSpec& spec, double slower) {
    auto first = simulate(spec);
    expect_sound(spec, first);
    ZEXPECT(first.wall <= slower * known_run(spec).wall);
    return first;
}

ZEST_SUITE(SchedulerSimulation) {

ZEST_CASE(HeavyUnitsOnBox) {
    expect_near_known(on_box(flang_heavy), 1.1);
}

ZEST_CASE(HeavyUnitsInCgroup) {
    expect_near_known(on_box(flang_heavy, 10 * GiB, 300 * MiB), 1.15);
}

ZEST_CASE(ProjectOnBox) {
    auto result = expect_near_known(on_box(flang_all), 1.1);
    ZEXPECT(result.wall <= 1.05 * result.lower_bound);
}

ZEST_CASE(ProjectInCgroup) {
    expect_near_known(on_box(flang_all, 10 * GiB, 300 * MiB), 1.1);
}

ZEST_CASE(ProjectOnLaptop) {
    expect_near_known(on_box(flang_all, 8 * GiB, 2560 * MiB), 1.1);
}

ZEST_CASE(TooBigUnitReported) {
    std::vector<UnitLoad> units(6, {.seconds = 10, .peak = 2048});
    units.insert(units.begin() + 2, {.seconds = 20, .peak = 9000});
    auto spec = on_box(units, 8 * GiB, 1 * GiB);
    auto result = simulate(spec);
    expect_sound(spec, result);
    ZASSERT(result.too_big.size() == 1u);
    ZEXPECT(result.too_big[0] == 2u);
}

ZEST_CASE(TypingStaysFast) {
    auto spec = on_box(flang_heavy);
    spec.typing = {
        {.from = 10, .until = 120}
    };
    auto result = expect_near_known(spec, 1.1);
    ZEXPECT(max_wait(result) <= 0.5);
}

ZEST_CASE(OtherProgramGrows) {
    auto spec = on_box(flang_all);
    spec.machine.others = {
        {0,   1450 * MiB},
        {300, 10 * GiB  },
        {600, 1450 * MiB}
    };
    auto result = expect_near_known(spec, 1.1);
    ZEXPECT(result.min_headroom > 0);
}

ZEST_CASE(MacNeverThrashes) {
    auto result = expect_near_known(on_mac(flang_heavy, 2200 * MiB), 1.1);
    ZEXPECT(result.min_headroom > 0);
}

ZEST_CASE(MacSmallUnits) {
    auto result = expect_near_known(on_mac(abseil_mac), 1.1);
    ZEXPECT(result.min_headroom > 0);
}

ZEST_CASE(MacProject) {
    auto result = expect_near_known(on_mac(mlir_mac), 1.1);
    ZEXPECT(result.min_headroom > 0);
}

/// Small units first teach the session a small estimate; the heavy ones
/// after them are stopped before memory runs out.
ZEST_CASE(MacSmallThenHeavy) {
    std::vector<UnitLoad> units(std::begin(abseil_mac), std::begin(abseil_mac) + 10);
    units.insert(units.end(), std::begin(flang_heavy), std::end(flang_heavy));
    auto result = expect_near_known(on_mac(units), 1.1);
    ZEXPECT(result.min_headroom > 0);
}

/// Small units first teach the session a small estimate, then heavy units
/// come on many cores and little memory: the kills, one at a time, could
/// not stop as many outgrowing it at once.
ZEST_CASE(ManyJobsSmallFirst) {
    for(std::uint32_t jobs: {8u, 12u}) {
        for(std::uint32_t heavy: {1500u, 2500u, 4000u}) {
            std::vector<UnitLoad> units(3, {.seconds = 1, .peak = 200});
            units.insert(units.end(), 16, {.seconds = 10, .peak = heavy});
            auto spec = on_box(units, 6 * GiB, 2 * GiB);
            spec.machine.physical = 16;
            spec.machine.logical = 32;
            spec.scheduler = {.background_jobs = jobs, .max_workers = jobs};
            expect_sound(spec, simulate(spec));
        }
    }
}

/// Units spread far wider than a real project's, on machines of 4 to 32
/// GiB: a first run learns their peaks as it goes, and the bounds are what
/// that costs.
ZEST_CASE(RandomWorkloads) {
    std::mt19937_64 random(20261020);
    auto uniform = [&](double low, double high) {
        return low + (high - low) * double(random() >> 11) / double(1ull << 53);
    };
    double log_slower = 0;
    constexpr int rounds = 60;
    for(int round = 0; round < rounds; round += 1) {
        SimSpec spec;
        constexpr std::uint64_t sizes[] = {4 * GiB, 8 * GiB, 16 * GiB, 32 * GiB};
        spec.machine.memory = sizes[random() % 4];
        auto others = std::uint64_t(uniform(0.1, 0.4) * double(spec.machine.memory));
        spec.machine.others = {
            {0, others}
        };
        constexpr std::uint32_t cores[] = {2, 4, 8};
        spec.machine.physical = cores[random() % 3];
        spec.machine.logical = 2 * spec.machine.physical;
        auto jobs = std::uint32_t(spec.machine.physical + random() % spec.machine.physical);
        spec.scheduler = {.background_jobs = jobs, .max_workers = spec.machine.logical};
        // Units up to 6 GiB, and up to most of the machine's memory: one
        // too big for it is rare, and has a case of its own.
        auto largest = std::min(6144.0, 0.6 * double(spec.machine.memory / MiB));
        auto count = 30 + random() % 50;
        for(std::size_t i = 0; i < count; i += 1) {
            auto peak = std::exp(uniform(std::log(150.0), std::log(largest)));
            spec.units.push_back(
                {.seconds = peak / 100 * uniform(0.5, 2), .peak = std::uint32_t(peak)});
        }
        if(random() % 2 == 0) {
            spec.typing = {
                {.from = uniform(0, 60), .until = uniform(60, 300)}
            };
        }
        auto first = simulate(spec);
        expect_sound(spec, first);
        ZEXPECT(max_wait(first) <= 1.0);
        auto slower = first.wall / known_run(spec).wall;
        ZEXPECT(slower <= 1.5);
        log_slower += std::log(slower);
    }
    ZEXPECT(std::exp(log_slower / rounds) <= 1.05);
}

};  // ZEST_SUITE(SchedulerSimulation)

}  // namespace

}  // namespace clice::testing
