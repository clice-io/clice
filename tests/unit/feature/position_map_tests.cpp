module;

#include "modules/prelude.h"

module clice;

import :feature.feature;
import :support.anomaly;
import :tests.unit.test.test;

namespace clice::testing {

namespace {

ZEST_SUITE(PositionMap) {

ZEST_CASE(OutOfRangeAnomaly) {
    /// Production trigger for the PositionMapFail anomaly: the checked
    /// feature-layer converters report internally produced offsets that
    /// cannot be mapped back to a position.
    logging::reset_anomaly_for_testing();
    std::vector<logging::AnomalyId> trapped;
    logging::set_anomaly_trap_for_testing([&](logging::AnomalyId id) { trapped.push_back(id); });

    std::string_view content = "int x;\n";
    auto lines = kota::ipc::lsp::line_starts(content);
    feature::PositionMap map{.content = content, .lines = lines};
    ZEXPECT(!map.to_position(100).has_value());
    ZEXPECT(!map.to_range({0, 100}).has_value());

    ZASSERT(trapped.size() == 2u);
    ZEXPECT(trapped[0] == logging::AnomalyId::PositionMapFail);
    ZEXPECT(trapped[1] == logging::AnomalyId::PositionMapFail);

    /// In-range conversions stay silent.
    ZEXPECT(map.to_range({0, 5}));
    ZEXPECT(trapped.size() == 2u);

    logging::reset_anomaly_for_testing();
}

};  // ZEST_SUITE(PositionMap)

}  // namespace

}  // namespace clice::testing
