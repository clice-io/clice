// Designator hints are currently unimplemented (VisitInitListExpr is a
// FIXME stub), so aggregate initializations produce no hints; only the
// constructor call at the end yields a parameter hint. This fixture pins
// the absence: implementing designators will surface here as a diff.

namespace designators {

struct Point {
    int x;
    int y;
};

Point origin{1, 2};

struct Box {
    Point top_left;
    Point bottom_right;
};

Box box{{1, 2}, {3, 4}};

int coordinates[2] = {7, 8};

struct Mixed {
    struct {
        int inner;
    };
    int outer;
};

Mixed mixed{{5}, 6};

struct NotAggregate {
    NotAggregate(int amount);
};

NotAggregate built{5};

}  // namespace designators
