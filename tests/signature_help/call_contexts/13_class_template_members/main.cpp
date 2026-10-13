template <typename T>
struct Vector {
    T& at(int index);
    void assign(int count, const T& value);
    template <typename U>
    U convert(U fallback) const;
};

template <typename Signature>
struct Function;

template <typename Result, typename... Args>
struct Function<Result(Args...)> {
    Result operator()(Args... args) const;
};

struct Point {
    int x;
    int y;
};

void use(Vector<Point> points, Function<bool(int, double)> callback) {
    points.at(§(member)0);
    points.assign(3, §(parameter)Point{});
    points.convert(§(member_template)1.0);
    callback(§(function_object)1, 2.0);
}
