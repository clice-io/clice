// Template declarations and uses: the card of a class, function, variable or
// alias template at its declaration and its uses, partial specializations,
// arrays sized by template arguments, packs, injected class names.

namespace object_instance {
template <typename T, typename... Rest> struct Bundle {
    Bundle(long identifier);
};
Bundle<long, char, bool> bund§(var_of_template_type)le{7};
}

namespace implicit_instance {
template <typename T> class Sequence {};
Sequ§(implicit_instantiation)ence<long> values;
}

namespace class_template_declaration {
template <typename, bool...> struct Policy;
template <template <typename, bool...> class Strategy, typename Value = char, long Size = 4,
          bool Enabled = true, typename... Rest>
class Registry final {};
template <template <typename, bool...> class Strategy>
Reg§(class_template)istry<Strategy> registry;
}

namespace function_template_call {
template <typename, bool...> struct Policy;
template <template <typename, bool...> class Strategy, typename Value = char, long Size = 4,
          bool Enabled = true, typename... Rest>
void register_type();
void run() {
    regis§(function_template)ter_type<Policy>();
}
}

namespace function_declaration_call {
template <typename, bool...> struct Policy {};
Policy<long, true, false> evaluate(long, bool enabled = true);
void run() {
    eval§(function_decl)uate(5);
}
}

namespace partial_specialization_declaration {
template <typename T> class Container;
template <typename T> class §(partial_specialization)Container<T*> {};
}

namespace partial_specialization_constructor {
template <typename T, typename = void> struct Container;
template <typename T> struct Container<T*> {
    §(partial_spec_constructor)Container();
};
}

namespace variable_template_declaration {
using count_type = long;
template <long Size> count_type §(variable_template)items[Size];
}

namespace variable_template_specialization {
using count_type = long;
template <long Size> count_type items[Size];
template <> count_type §(variable_template_spec)items<6>[6];
}

namespace canonical_member_type {
template <typename T> struct Identity {
    using type = T;
};
Identity<long>::type §(canonical_type)value;
}

namespace canonical_function_parameter {
template <typename T> void §(function_template_type)consume(T value) {}
}

namespace alias_template_declaration {
template <typename T> using §(alias_template)identity = T;
}

namespace alias_template_chain {
template <typename T> using identity = T;
template <typename T> using §(alias_template_chain)const_identity = const identity<T>;
}

namespace array_types {
using element_type = long;
element_type §(constant_array)fixed[12];
extern element_type §(incomplete_array)open[];

template <long Size> struct Buffer {
    element_type §(dependent_size_array)data[Size];
};
}

namespace template_argument {
template <typename T> struct Allocator {};
template <template <typename> class Strategy> struct Owner {};
Owner<§(template_template_arg)Allocator> owner;
}

namespace deduction_placeholder {
template <typename T> struct Wrapper {
    Wrapper(T);
};
§(ctad)Wrapper wrapper(9L);
}

namespace pack_size {
template <typename... Types> constexpr auto count = sizeof...(§(sizeof_pack)Types);
}

namespace injected_name {
template <typename T> struct Link {
    §(injected_class_name)Link* next;
};
}
