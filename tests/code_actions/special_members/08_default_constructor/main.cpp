class §(counter)Counter {
public:
    int next() {
        count += 1;
        return count;
    }

private:
    int count = 0;
};

struct §(point)Point {
    int x;
    int y;
};

struct §(bound)Bound {
    int& target;
};

void use(Bound& bound) {
    Counter counter;
    Counter other = counter;
    Point point;
    Point origin = point;
    Bound alias = bound;
}
