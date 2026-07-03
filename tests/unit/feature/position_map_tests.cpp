#include "test/test.h"
#include "feature/feature.h"
#include "support/anomaly.h"

namespace clice::testing {

namespace {

TEST_SUITE(PositionMap) {

TEST_CASE(OutOfRangeAnomaly) {
    /// Production trigger for the PositionMapFail anomaly: the checked
    /// feature-layer converters report internally produced offsets that
    /// cannot be mapped back to a position.
    logging::reset_anomaly_for_testing();
    std::vector<logging::AnomalyId> trapped;
    logging::set_anomaly_trap_for_testing([&](logging::AnomalyId id) { trapped.push_back(id); });

    feature::LineMap map("int x;\n");
    EXPECT_FALSE(feature::to_position(map, 100).has_value());
    EXPECT_FALSE(feature::to_range(map, {0, 100}).has_value());

    ASSERT_EQ(trapped.size(), 2u);
    EXPECT_EQ(trapped[0], logging::AnomalyId::PositionMapFail);
    EXPECT_EQ(trapped[1], logging::AnomalyId::PositionMapFail);

    /// In-range conversions stay silent.
    EXPECT_TRUE(feature::to_range(map, {0, 5}).has_value());
    EXPECT_EQ(trapped.size(), 2u);

    logging::reset_anomaly_for_testing();
}

};  // TEST_SUITE(PositionMap)

}  // namespace

}  // namespace clice::testing
