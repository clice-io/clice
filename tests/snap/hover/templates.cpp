namespace object_instance {
template <typename T, typename... Rest> struct Bundle {
    Bundle(long identifier);
};
Bundle<long, char, bool> bund§(01_var_of_template_type)le{7};
}

namespace implicit_instance {
template <typename T> struct Sequence {};
Sequ§(02_implicit_instantiation)ence<long> values;
}

namespace class_template_declaration {
template <typename, bool...> struct Policy;
template <template <typename, bool...> class Strategy, typename Value = char, long Size = 4,
          bool Enabled = true, typename... Rest>
struct Registry final {};
template <template <typename, bool...> class Strategy>
Reg§(03_class_template)istry<Strategy> registry;
}

namespace function_template_call {
template <typename, bool...> struct Policy;
template <template <typename, bool...> class Strategy, typename Value = char, long Size = 4,
          bool Enabled = true, typename... Rest>
void register_type();
void run() {
    regis§(04_function_template)ter_type<Policy>();
}
}

namespace function_declaration_call {
template <typename, bool...> struct Policy {};
Policy<long, true, false> evaluate(long, bool enabled = true);
void run() {
    eval§(05_function_decl)uate(5);
}
}

namespace partial_specialization_declaration {
template <typename T> struct Container;
template <typename T> struct §(06_partial_specialization)Container<T*> {};
}

namespace partial_specialization_constructor {
template <typename T, typename = void> struct Container;
template <typename T> struct Container<T*> {
    §(07_partial_spec_constructor)Container();
};
}

namespace special_members {
struct Resource {
    §(08_destructor)~Resource();
};

struct Conversion {
    op§(09_conversion_operator)erator long();
};

struct SelfConversion {
    operator §(10_conversion_target)SelfConversion();
};
}

namespace selected_specialization_docs {
/// Generic channel.
template <typename T> struct Channel {};
/// Pointer channel.
template <typename T> struct Channel<T*> {};
Chan§(11_primary_template_doc)nel<long*>* channel;
}

namespace variable_template_declaration {
using count_type = long;
template <long Size> count_type §(12_variable_template)items[Size];
}

namespace variable_template_specialization {
using count_type = long;
template <long Size> count_type items[Size];
template <> count_type §(13_variable_template_spec)items<6>[6];
}

namespace canonical_member_type {
template <typename T> struct Identity {
    using type = T;
};
Identity<long>::type §(14_canonical_type)value;
}

namespace canonical_function_parameter {
template <typename T> void §(15_function_template_type)consume(T value) {}
}

namespace alias_template_declaration {
template <typename T> using §(16_alias_template)identity = T;
}

namespace alias_template_chain {
template <typename T> using identity = T;
template <typename T> using §(17_alias_template_chain)const_identity = const identity<T>;
}

namespace array_types {
using element_type = long;
element_type §(18_constant_array)fixed[12];
extern element_type §(19_incomplete_array)open[];

template <long Size> struct Buffer {
    element_type §(20_dependent_size_array)data[Size];
};
}

namespace dependent_base_names {
template <typename T> struct Source {
    using value_type = T;
    static constexpr long count = 2;
};
template <typename T> struct Sink : Source<T> {
    typename Source<T>::§(21_dependent_type)value_type value;
    static constexpr long size = Source<T>::§(22_dependent_value)count;
    using typename Source<T>::§(23_using_typename)value_type;
    using Source<T>::§(24_using_value)count;
};
}

namespace template_argument {
template <typename T> struct Allocator {};
template <template <typename> class Strategy> struct Owner {};
Owner<§(25_template_template_arg)Allocator> owner;
}

namespace deduction_placeholder {
template <typename T> struct Wrapper {
    Wrapper(T);
};
§(26_ctad)Wrapper wrapper(9L);
}

namespace dependent_overload {
struct Writer {
    void write(long);
    void write(char);
};
template <typename T> void emit(Writer writer, T value) {
    writer.§(27_unresolved_member)write(value);
}
}

namespace pack_size {
template <typename... Types> constexpr auto count = sizeof...(§(28_sizeof_pack)Types);
}

namespace injected_name {
template <typename T> struct Link {
    §(29_injected_class_name)Link* next;
};
}

namespace current_instantiation_members {
template <typename T> struct Base {
    void reset();
    long value;
};
template <typename T> struct Derived : Base<T> {
    void clear() {
        this->§(30_dependent_method)reset();
    }
    long read() {
        return this->§(31_dependent_field)value;
    }
};
}

namespace nondependent_base_name {
struct Common {
    using index_type = long;
};
template <typename T> struct Table : Common {};
template <typename T> typename Table<T>::§(32_fixed_base)index_type index();
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
    return Rank<T*, T*>::§(33_partial_order)value;
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
    pointer->§(34_arrow_chain)visit();
}
}

namespace dependent_using_use {
template <typename T> struct Source {
    using item_type = T;
};
template <typename T> struct Destination : Source<T> {
    using typename Source<T>::item_type;
    §(35_unresolved_using_use)item_type item;
};
}

namespace selected_variable_docs {
/// Generic cache slot.
template <typename T> long cache = 0;
/// Pointer cache slot.
template <typename T> long cache<T*> = 1;
using result_type = decltype(ca§(36_variable_template_doc)che<long*>);
}

namespace dependent_alias_sugar {
template <typename T> struct View {
    using §(37_dependent_member_alias)reference = const T&;
};
}
