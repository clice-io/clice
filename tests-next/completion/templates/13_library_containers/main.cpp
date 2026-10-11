// A container built the way the standard library builds std::vector: a
// protected base holding the allocator, its `get_allocator` brought in with
// a using-declaration. The element of a dependent `vector<vector<T>>`
// completes as the element of a concrete one does.
template <class T>
struct allocator {};

template <class T, class A>
struct vector_base {
    using allocator_type = A;
    allocator_type get_allocator() const;
    ~vector_base();

protected:
    T* start;
};

template <class T, class A = allocator<T>>
class vector : protected vector_base<T, A> {
    using base = vector_base<T, A>;

public:
    using reference = T&;
    using base::get_allocator;
    reference operator[](int index);
    int size() const;
};

struct Point {
    int x;
};

void points(vector<Point> list) {
    list.§(concrete);
}

template <class T>
void rows(vector<vector<T>> grid) {
    grid[0].§(dependent);
}
