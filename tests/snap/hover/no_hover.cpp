// Every marked position is expected to produce no hover.

§(01_builtin_type)int empty_result() {}

void empty_body() {§(02_empty_braces)}

decltype(au§(03_decltype_auto_inner)to) inferred = 1;

auto generic = [](a§(04_lambda_auto_param)uto value) {};

§(05_static_assert)static_assert(true, "valid");

template <typename T> void dependent_expression() {
    (void)size§(06_dependent_sizeof)of(T);
}

auto truth = t§(07_bool_literal)rue;
auto compound = §(08_compound_literal)(long){7};
auto decimal = §(09_float_literal)3.5;
auto imaginary = §(10_imaginary_literal)6.0i;
auto integer = §(11_int_literal)99;
auto pointer = §(12_nullptr_literal)nullptr;
