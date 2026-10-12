// A concept at each place it is named: constraining a variable, a template
// parameter, an abbreviated parameter, and tested in an expression.

namespace constrained_object {
template <typename T>
concept Numeric = true;

§(concept_on_var)Numeric auto total = 7;
}

namespace constrained_parameter {
template <typename T>
concept Printable = true;

template <Print§(concept_on_tparam)able T> void emit(T value) {}
}

namespace parameter_declaration {
template <typename T>
concept Serializable = true;

template <Serializable Pa§(constrained_tparam)yload> void send(Payload value) {}
}

namespace abbreviated_parameter {
template <typename T>
concept Hashable = true;

void store(Hash§(concept_on_auto_param)able auto value) {}
}

namespace concept_expression {
template <typename T>
concept Ordered = true;

constexpr bool ordered_int = Orde§(concept_reference)red<int>;
}
