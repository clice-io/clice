#define LIMIT 8
#define FIELDS int generated_a; int generated_b;

namespace app {

template <typename T>
struct Box {
    T value;
    int first, second;
    friend void inspect(Box&);
    FIELDS

    template <typename U>
    U convert() const;
};

template <typename T>
template <typename U>
U Box<T>::convert() const {
    return U();
}

template struct Box<int>;

typedef struct {
    int width;
} Size;

namespace alias = app;

[[nodiscard]] int scale(int value) {
    int doubled = value * LIMIT;
    return doubled;
}

}  // namespace app
