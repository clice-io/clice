// No card where nothing names a symbol: a builtin type, an empty body, a
// static_assert, a dependent sizeof, a lambda parameter never deduced,
// bool, pointer and compound literals.

§(builtin_type)int empty_result() {}

void empty_body() {§(empty_braces)}

int* allocated = new au§(new_auto)to(1);

auto generic = [](a§(lambda_auto_param)uto value) {};

§(static_assert)static_assert(true, "valid");

template <typename T> void dependent_expression() {
    (void)size§(dependent_sizeof)of(T);
}

auto truth = t§(bool_literal)rue;
auto compound = §(compound_literal)(long){7};
auto pointer = §(nullptr_literal)nullptr;
