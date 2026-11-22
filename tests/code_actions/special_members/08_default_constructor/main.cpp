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

void use() {
    Counter counter;
    Counter other = counter;
    Point point;
    Point origin = point;
}
