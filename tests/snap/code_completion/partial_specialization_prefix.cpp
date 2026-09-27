// A typed prefix matching nothing in the partial specialization the
// dependent base resolves to must not bring back the primary template's
// members.
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
    t.val§(filtered);
}
