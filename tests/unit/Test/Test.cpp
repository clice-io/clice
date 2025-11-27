#include "Test/Test2.h"

namespace clice::testing {

TEST_SUITE(MyTest) {
    int x = 0;

    void setup() {
        x += 1;
    }

    void teardown() {
        x -= 2;
    }

    TEST_CASE(World, skip_test) {
        x = 3;
        ///
        return TestState::Skipped;
    };

    TEST_CASE(Hello, focus) {
        x = 4;
        return TestState::Failed;
    };
};

}  // namespace clice::testing
