/// - verify: server

template <typename T>
concept §(concept_def)Addable = requires(T value) {
    value + value;
};

template <§(type_constraint)Addable T>
T twice(T value) {
    return value + value;
}

template <typename T>
    requires §(requires_use)Addable<T>
T combine(T left, T right) {
    return left + right;
}

int use_concepts() {
    return §(constrained_call)twice(2) + combine(3, 4);
}
