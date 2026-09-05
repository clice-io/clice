namespace std {
template <typename T> T&& move(T&& value);
}

namespace accessors {
struct Gauge {
    int level;
    double §(01_getter)getLevel() { return level; }
};

struct Dial {
    int angle;
    void §(02_setter)setAngle(double value) { angle = value; }
};

struct Builder {
    int width;
    Builder& §(03_setter_builder)setWidth(double value) {
        width = value;
        return *this;
    }
};

struct Mover {
    int offset;
    void §(04_setter_move)setOffset(double value) { offset = std::move(value); }
};
}
