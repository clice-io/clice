// The getter and setter shapes the synthesized description accepts: a
// converting getter, a setter returning *this, a setter that moves. A
// deleted setter has no body to describe.

namespace std {
template <typename T> T&& move(T&& value);
}

namespace accessors {
struct Gauge {
    int level;
    double §(getter)getLevel() { return level; }
};

struct Dial {
    int angle;
    void §(setter)setAngle(double value) { angle = value; }
};

struct Builder {
    int width;
    Builder& §(setter_builder)setWidth(double value) {
        width = value;
        return *this;
    }
};

struct Mover {
    int offset;
    void §(setter_move)setOffset(double value) { offset = std::move(value); }
};

struct Frozen {
    int depth;
    void §(setter_deleted)setDepth(int value) = delete;
};
}
