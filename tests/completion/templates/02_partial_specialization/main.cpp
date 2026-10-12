// A dependent specialization that matches a partial specialization
// completes through that specialization, not the primary template; a typed
// prefix matching nothing there does not bring back the primary's members.
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
    t.§(pos);
    t.val§(filtered);
}
