#include "test/test.h"
#include "feature/feature.h"
#include "support/anomaly.h"

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

    feature::LineMap map("int x;\n");
    EXPECT(!feature::to_position(map, 100).has_value());
    EXPECT(!feature::to_range(map, {0, 100}).has_value());

    ASSERT(trapped.size() == 2u);
    EXPECT(trapped[0] == logging::AnomalyId::PositionMapFail);
    EXPECT(trapped[1] == logging::AnomalyId::PositionMapFail);

    /// In-range conversions stay silent.
    EXPECT(feature::to_range(map, {0, 5}));
    EXPECT(trapped.size() == 2u);

    logging::reset_anomaly_for_testing();

}  // namespace

};  // ZEST_SUITE(PositionMap)

}  // namespace

}  // namespace clice::testing
