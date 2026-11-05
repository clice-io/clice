namespace call_arity_filter {

template <typename T>
struct S {
    int foo(int);
    int foo(int, int);
    int foo(T);

    void call(T t) {
        // One argument: not `foo(int, int)`.
        §(call_arity_filter)foo(t);
    }
};

}  // namespace call_arity_filter

namespace qualified_call_arity {

template <typename T>
struct base {
    static int foo(int);
    static int foo(int, int);
    static int foo(T);
};

template <typename T>
struct S {
    void call(T t) {
        // One argument: not `foo(int, int)`.
        base<T>::§(qualified_call_arity)foo(t);
    }
};

}  // namespace qualified_call_arity

namespace pack_argument_call {

template <typename T>
struct S {
    int foo(int);
    int foo(int, int);

    template <typename... Us>
    void call(Us... us) {
        // `us...` may be any number of arguments: both overloads.
        §(pack_argument_call)foo(us...);
    }
};

}  // namespace pack_argument_call

namespace explicit_object_arity {

template <typename T>
struct S {
    int foo(this S&, int);
    int foo(this S&, int, int);

    void call(T t) {
        // One argument after the object: only `foo(this S&, int)`.
        this->§(explicit_object_arity)foo(t);
    }
};

}  // namespace explicit_object_arity

namespace resolved_callee {

void take(int first, int second);

template <int N>
void call() {
    // Only value-dependent: clang resolved the callee itself.
    §(resolved_callee)take(N, 1);
}

}  // namespace resolved_callee

namespace arrow_without_pointer {

template <typename T>
struct P {
    int value;

    void f(P p) {
        // `P` has no `operator->`: `p->value` finds nothing.
        p->§(arrow_without_pointer)value;
    }
};

}  // namespace arrow_without_pointer

namespace arrow_through_smart_pointer {

template <typename T>
struct Node {
    int val;
};

template <typename T>
struct SP {
    T* operator->();
};

template <typename T>
void f(SP<Node<T>> p) {
    p->§(arrow_through_smart_pointer)val;
}

}  // namespace arrow_through_smart_pointer

namespace member_chain_lookup {

template <typename T>
struct Leaf {
    int leaf;
};

template <typename T>
struct Box {};

template <typename T>
struct Box<T*> {
    Leaf<T> inner;
};

template <typename X>
int two_hops(Box<X*> box) {
    return box.inner.§(member_chain_lookup)leaf;
}

}  // namespace member_chain_lookup

namespace pack_parameter_member {

template <typename T>
struct Box {
    T value;
};

template <typename... Ts>
void use(Ts... values);

template <typename... Ts>
void unwrap(Box<Ts>... boxes) {
    use(boxes.§(pack_parameter_member)value...);
}

}  // namespace pack_parameter_member

namespace reference_parameter_member {

template <typename T>
struct Gauge {
    T level;
};

template <typename T>
T read(Gauge<T>& gauge) {
    return gauge.§(reference_parameter_member)level;
}

}  // namespace reference_parameter_member

namespace decltype_call_scope {

template <typename T>
struct Holder {
    using type = T;
};

template <typename T>
struct Factory {};

template <typename T>
struct Factory<T*> {
    static Holder<T> make();
};

template <typename X>
struct test {
    using §(decltype_call_scope)input = typename decltype(Factory<X*>::make())::type;
};

}  // namespace decltype_call_scope

namespace decltype_explicit_template_call {

template <typename U>
struct Result {
    using type = char;
};

template <typename U>
struct Result<U*> {
    using type = int;
};

template <typename U>
Result<U> make();

template <typename X>
struct test {
    using §(decltype_explicit_template_call)input =
        typename decltype(decltype_explicit_template_call::make<X*>())::type;
};

}  // namespace decltype_explicit_template_call

namespace decltype_deduced_template_call {

template <typename U>
struct Result {
    using type = char;
};

template <typename U>
struct Result<U*> {
    using type = int;
};

template <typename U>
Result<U> make(U);

template <typename X>
struct test {
    // `U` is deduced as `X*`: the partial of `Result<X*>`.
    using §(decltype_deduced_template_call)input =
        typename decltype(decltype_deduced_template_call::make(static_cast<X*>(nullptr)))::type;
};

}  // namespace decltype_deduced_template_call

namespace decltype_call_needs_adl {

struct Wrong {
    using type = int;
};

Wrong make(...);

namespace N {
template <typename T>
struct Arg {};

struct Right {
    using type = char;
};

template <typename T>
Right make(Arg<T>);
}  // namespace N

template <typename X>
struct test {
    // Argument-dependent lookup finds `N::make` only at instantiation: it stays dependent.
    using §(decltype_call_needs_adl)input = typename decltype(make(N::Arg<X>{}))::type;
};

}  // namespace decltype_call_needs_adl

namespace using_two_specializations {

template <typename U>
struct Base {
    U get(U value);
};

template <typename T>
struct Derived : Base<T>, Base<T*> {
    using Base<T>::get;
    using Base<T*>::get;
};

template <typename T>
void use(Derived<T>& derived, T value) {
    auto §(using_two_specializations)got = derived.get(value);
}

}  // namespace using_two_specializations

namespace reference_bound_field {

struct Left {};

struct Right {};

struct Getter {
    Left get() &;
    Right get() &&;
};

template <typename U>
struct Holder {
    U value;
    static Holder make();
};

template <typename T>
struct Test {
    void run() {
        // `value` is a `Getter&` in `Holder<Getter&>`: an lvalue even through a temporary.
        auto §(reference_bound_field)got = Holder<T&>::make().value.get();
    }
};

template struct Test<Getter>;

}  // namespace reference_bound_field

namespace reference_qualification {

struct Good {};

struct Bad {};

template <typename U>
Good choose(U& value);

Bad choose(const int& value);

template <typename T>
struct Source {
    using type = int;
};

template <typename T>
void run(typename Source<T>::type& value) {
    // Binding `int&` beats binding `const int&`: the template wins.
    auto §(reference_qualification)chosen = reference_qualification::choose(value);
}

}  // namespace reference_qualification

namespace explicit_arguments_bound {

struct Base {};

struct A : Base {};

struct B : Base {};

struct Good {};

struct Bad {};

template <typename U>
Good choose(U* left, U* right);

template <typename U>
Bad choose(U left, B* right);

template <typename T>
struct Source {
    using type = A*;
};

template <typename T>
void run(typename Source<T>::type left, B* right) {
    auto §(explicit_arguments_bound)chosen =
        explicit_arguments_bound::choose<Base>(left, right);
}

}  // namespace explicit_arguments_bound

namespace long_call_chain {

template <typename T>
struct box {};

template <typename U>
struct builder {
    builder& add(U);
    U build();
};

template <typename T>
struct test {
    builder<box<T>> parts;
    box<T> part;
    // Each call's object is evaluated once: a chain this long stays well within the budget.
    using §(long_call_chain)input = decltype(parts.add(part)
                                                 .add(part)
                                                 .add(part)
                                                 .add(part)
                                                 .add(part)
                                                 .add(part)
                                                 .add(part)
                                                 .add(part)
                                                 .add(part)
                                                 .add(part)
                                                 .add(part)
                                                 .add(part)
                                                 .add(part)
                                                 .add(part)
                                                 .add(part)
                                                 .add(part)
                                                 .build())*;
};

}  // namespace long_call_chain

namespace deduced_reference_binding {

struct Good {};

struct Bad {};

template <typename U>
Bad choose(const U&&);

Good choose(long);

template <typename T>
struct box {
    int value;
};

template <typename T>
struct test {
    box<T> held;
    // `const U&&` deduces to `const int&&`, which takes no lvalue: `choose(long)` is called.
    using §(deduced_reference_binding)input =
        decltype(::deduced_reference_binding::choose(held.value))*;
};

}  // namespace deduced_reference_binding

namespace failed_constraint {

struct Good {};

struct Bad {};

template <typename U>
Bad choose(U)
    requires (!__is_same(U, int));

Good choose(long);

template <typename T>
struct box {
    int value;
};

template <typename T>
struct test {
    box<T> held;
    // `U` deduces to `int`, which the constraint rules out: `choose(long)` is called.
    using §(failed_constraint)input = decltype(::failed_constraint::choose(held.value))*;
};

}  // namespace failed_constraint
