/// # Partial specialization members
///
/// - status: supported
/// - diagnostics: expected
///
/// A dependent specialization that matches a partial specialization
/// completes through that specialization, not the primary template

// The member access expression is left dangling at the point.
struct ByValue {
    int value_field;
};

struct ByPointer {
    int pointer_field;
};

template <typename T>
struct Traits {
    using type = ByValue;
};

template <typename T>
struct Traits<T*> {
    using type = ByPointer;
};

template <typename T>
void bar() {
    typename Traits<T*>::type t;
    t.§(pos)
}
