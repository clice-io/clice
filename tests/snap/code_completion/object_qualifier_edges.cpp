// - diagnostics: expected

// A parenthesized `decltype(auto)` from a member inside a const member
// function is a const reference, so only the const overload of `get` is
// offered; a volatile object that no overload of `peek` can bind completes
// nothing, even though every overload returns the same type.
struct Mutable {
    void mutate();
};

struct Viewed {
    void view();
};

template <typename T>
struct Box {
    Mutable get();
    Viewed get() const;
    Viewed peek();
    Viewed peek() const;
};

template <typename T>
struct Grid {
    Box<T> cells;

    void show() const {
        decltype(auto) view = (cells);
        view.get().§(const_member_function);
    }
};

template <typename T>
void bar(volatile Box<T>& box) {
    box.peek().§(unbindable_object);
}
