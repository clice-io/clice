#define LIMIT 8
#define CHECK(cond) ((cond) ? 0 : 1)

namespace app::inline v1 {

template <typename T>
concept Number = requires(T value) { value + value; };

template <Number T>
[[nodiscard]] T clamp(T value, T low, T high);

struct Shape {
    virtual ~Shape() = default;
    virtual double area() const = 0;
    template <typename U> int range() const { return begin<U>(); }
    template <typename U> int begin() const { return 0; }
};

class Circle final : public Shape {
public:
    explicit Circle(double radius);
    double area() const override;
    static Circle unit();
    friend bool operator==(const Circle&, const Circle&) = default;
    friend void inspect(Circle&);

private:
    double radius_, diameter_;
};

double Circle::area() const {
    return radius_ * radius_;
}

template <typename T>
struct Box {
    T value;

    template <typename U>
    U convert() const;
};

template <typename T>
template <typename U>
U Box<T>::convert() const {
    return U();
}

template <>
struct Box<void> {};

template <typename T>
struct Box<T*> {
    T* pointee;
};

template struct Box<int>;

template <typename T>
Box(T) -> Box<T>;

typedef struct {
    int width;
} Size;

typedef enum { Red, Green } Color;

namespace alias = app;

long double operator""_km(long double);

#define OBJECT                     \
public:                            \
    static const int meta;         \
    virtual int metacall(int);

class Window {
    OBJECT
public:
    void show();
};

#define HANDLER(name) void name()

HANDLER(on_ready);

int compute() {
    int local_sum = 0;
    auto twice = [](int x) { return 2 * x; };
    struct Local {
        int field;
    };
    return local_sum + twice(LIMIT);
}

}  // namespace app::inline v1
