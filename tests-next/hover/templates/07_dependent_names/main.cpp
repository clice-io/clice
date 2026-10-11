// Names that depend on a template parameter: members of a dependent base,
// unresolved overloads, current-instantiation members, the partial
// specialization a use would pick, an overloaded arrow chain.

namespace dependent_base_names {
template <typename T> struct Source {
    using value_type = T;
    static constexpr long count = 2;
};
template <typename T> struct Sink : Source<T> {
    typename Source<T>::§(dependent_type)value_type value;
    static constexpr long size = Source<T>::§(dependent_value)count;
    using typename Source<T>::§(using_typename)value_type;
    using Source<T>::§(using_value)count;
};
}

namespace dependent_overload {
struct Writer {
    void write(long);
    void write(char);
};
template <typename T> void emit(Writer writer, T value) {
    writer.§(unresolved_member)write(value);
}
}

namespace current_instantiation_members {
template <typename T> struct Base {
    void reset();
    long value;
};
template <typename T> struct Derived : Base<T> {
    void clear() {
        this->§(dependent_method)reset();
    }
    long read() {
        return this->§(dependent_field)value;
    }
};
}

namespace nondependent_base_name {
struct Common {
    using index_type = long;
};
template <typename T> struct Table : Common {};
template <typename T> typename Table<T>::§(fixed_base)index_type index();
}

namespace partial_ordering {
template <typename Left, typename Right> struct Rank {
    static constexpr long value = 0;
};
template <typename Left, typename Right> struct Rank<Left*, Right> {
    static constexpr long value = 1;
};
template <typename Type> struct Rank<Type*, Type*> {
    static constexpr long value = 2;
};
template <typename T> long rank() {
    return Rank<T*, T*>::§(partial_order)value;
}
}

namespace overloaded_arrow {
template <typename T> struct Node {
    void visit();
};
template <typename T> struct Pointer {
    Node<T>* operator->();
};
template <typename T> void visit(Pointer<T> pointer) {
    pointer->§(arrow_chain)visit();
}
}

namespace dependent_using_use {
template <typename T> struct Source {
    using item_type = T;
};
template <typename T> struct Destination : Source<T> {
    using typename Source<T>::item_type;
    §(unresolved_using_use)item_type item;
};
}

namespace dependent_alias_sugar {
template <typename T> struct View {
    using §(dependent_member_alias)reference = const T&;
};
}
