namespace constrained_object {
template <typename T>
concept Numeric = true;

§(01_concept_on_var)Numeric auto total = 7;
}

namespace constrained_parameter {
template <typename T>
concept Printable = true;

template <Print§(02_concept_on_tparam)able T> void emit(T value) {}
}

namespace parameter_declaration {
template <typename T>
concept Serializable = true;

template <Serializable Pa§(03_constrained_tparam)yload> void send(Payload value) {}
}

namespace abbreviated_parameter {
template <typename T>
concept Hashable = true;

void store(Hash§(04_concept_on_auto_param)able auto value) {}
}

namespace concept_expression {
template <typename T>
concept Ordered = true;

constexpr bool ordered_int = Orde§(05_concept_reference)red<int>;
}
