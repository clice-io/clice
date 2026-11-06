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

namespace value_against_reference {

struct Good {};

struct Bad {};

template <typename U>
Bad choose(U value);

Good choose(const int& value);

template <typename T>
struct Source {
    using type = int;
};

template <typename T>
void run(typename Source<T>::type& value) {
    // Taking a value and binding `const int&` rank alike: the function that is no template wins.
    auto §(value_against_reference)chosen = value_against_reference::choose(value);
}

}  // namespace value_against_reference

namespace forwarding_pack {

struct Good {};

struct Bad {};

template <typename... Us>
Good choose(Us&&... values);

Bad choose(const int& value);

template <typename T>
struct Source {
    using type = int;
};

template <typename T>
void run(typename Source<T>::type& value) {
    // `Us&&` binds `int&`, which beats binding `const int&`: the template wins.
    auto §(forwarding_pack)chosen = forwarding_pack::choose(value);
}

}  // namespace forwarding_pack

namespace undeduced_parameter {

struct Good {};

struct Bad {};

template <typename U, typename V>
Bad choose(U);

Good choose(long);

template <typename T>
struct Source {
    using type = int;
};

template <typename T>
void run(typename Source<T>::type value) {
    // Nothing deduces `V`: `choose(long)` is called.
    auto §(undeduced_parameter)chosen = undeduced_parameter::choose(value);
}

}  // namespace undeduced_parameter

namespace uninstantiated_friend {

struct Good {};

struct Bad {};

namespace lib {

template <typename T>
struct Box {
    friend Good choose(Box);
};

}  // namespace lib

Bad choose(...);

template <typename T>
struct Source {
    using type = lib::Box<int>;
};

template <typename T>
void run(typename Source<T>::type value) {
    // Instantiating `Box<int>` declares the friend lookup finds: until then the call is not known.
    auto §(uninstantiated_friend)chosen = choose(value);
}

}  // namespace uninstantiated_friend

namespace constrained_member {

struct Good {};

struct Bad {};

template <typename T>
struct Box {
    Bad pick(int)
        requires false;

    template <typename U>
    Good pick(U);
};

template <typename T>
struct Source {
    using type = int;
};

template <typename T>
void run(Box<T>& box, typename Source<T>::type value) {
    // The constraint rules the member that is no template out.
    auto §(constrained_member)chosen = box.pick(value);
}

}  // namespace constrained_member

namespace constrained_without_arguments {

struct Good {};

struct Bad {};

template <typename T>
struct Box {
    Bad pick()
        requires false;

    template <typename... Us>
    Good pick(Us...);
};

template <typename T>
void run(Box<T>& box) {
    // The constraint rules the member that is no template out, with no argument to rank.
    auto §(constrained_without_arguments)chosen = box.pick();
}

}  // namespace constrained_without_arguments

namespace const_rvalue_binding {

struct Good {};

struct Bad {};

template <typename U>
Good choose(const U&& value);

Bad choose(const int& value);

template <typename T>
struct Source {
    using type = int;
};

template <typename T>
void run(typename Source<T>::type value) {
    auto §(const_rvalue_binding)chosen =
        const_rvalue_binding::choose(static_cast<typename Source<T>::type&&>(value));
}

}  // namespace const_rvalue_binding

namespace explicit_object_template {

struct Good {};

struct Bad {};

template <typename T>
struct Box {
    template <typename U>
    Good pick(this Box&, U&);

    Bad pick(this Box&, const int&);
};

template <typename T>
struct Source {
    using type = int;
};

template <typename T>
void run(Box<T>& box, typename Source<T>::type& value) {
    auto §(explicit_object_template)chosen = box.pick(value);
}

}  // namespace explicit_object_template

namespace inline_child_overload {

struct Good {};

struct Bad {};

namespace lib {

struct X {};

inline namespace v1 {

Good choose(X);

}  // namespace v1

}  // namespace lib

Bad choose(...);

template <typename T>
struct Source {
    using type = lib::X;
};

template <typename T>
void run(typename Source<T>::type value) {
    // Lookup in `lib` finds what its inline namespaces declare.
    auto §(inline_child_overload)chosen = choose(value);
}

}  // namespace inline_child_overload

namespace object_binding_tie {

struct Good {};

struct Bad {};

template <typename T>
struct Box {
    Bad pick(int) const;

    template <typename U>
    Good pick(U);
};

template <typename T>
void run(Box<T>& box) {
    auto §(object_binding_tie)chosen = box.pick(0);
}

}  // namespace object_binding_tie

namespace explicit_object_binding {

struct Good {};

struct Bad {};

template <typename T>
struct Box {
    Good pick(this Box&, long);

    Bad pick(this Box&&, int);
};

template <typename T>
struct Source {
    using type = int;
};

template <typename T>
void run(Box<T>& box, typename Source<T>::type value) {
    auto §(explicit_object_binding)chosen = box.pick(value);
}

}  // namespace explicit_object_binding

namespace constraint_per_call {

struct Good {};

struct Bad {};

template <typename U>
struct Wrap {
    using type = U;
};

template <typename U>
Good choose(U)
    requires __is_same(typename Wrap<U>::type, int);

Bad choose(...);

template <typename T>
struct Source {
    using integer = int;
    using floating = float;
};

template <typename T>
void run(typename Source<T>::integer whole, typename Source<T>::floating fraction) {
    // Each call reads the constraint with its own `U`.
    auto §(constraint_first_call)first = constraint_per_call::choose(whole);
    auto §(constraint_second_call)second = constraint_per_call::choose(fraction);
}

}  // namespace constraint_per_call
