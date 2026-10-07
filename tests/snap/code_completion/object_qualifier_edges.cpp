// - diagnostics: expected

// A parenthesized `decltype(auto)` from a member inside a const member
// function is a const reference, so only the const overload of `get` is
// offered; a volatile object that no overload of `front` can bind completes
// nothing.
struct Mutable {
    void mutate();
};

struct Viewed {
    void view();
};

struct Cell {
    Mutable get();
    Viewed get() const;
};

template <typename T>
struct Vec {
    T& front();
    const T& front() const;
};

template <typename T>
struct Grid {
    Cell cells;

    void show() const {
        decltype(auto) view = (cells);
        view.get().§(const_member_function);
    }
};

template <typename T>
void bar(volatile Vec<Vec<T>>& rows) {
    rows.front().§(unbindable_object);
}
