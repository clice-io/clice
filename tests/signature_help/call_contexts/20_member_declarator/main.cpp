// A member function's parameter list is no call, though the bodies of the
// members before it call through a member the class declares later.
#include "matchers.h"

namespace {

struct MatchBuilder {
    auto ignoreParenAndFloatingCasting(Matcher matcher) const {
        return any(matcher, literal(near(1.0, DiffThreshold)).bind("constant"));
    }

    auto matchFloatValueNear(§(declarator)const double Val) const {
        const auto Float = literal(near(Val, DiffThreshold));
        return Float;
    }

    double DiffThreshold;
};

}  // namespace
