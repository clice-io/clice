// Every name written in an explicit instantiation, declaration or
// definition, is painted: of a class, a function and a variable template.
struct Widget {};

namespace classes {

template <typename T>
struct Holder {
    T value;
};

extern template struct §(class_extern)Holder<§(class_extern_arg)Widget>;

template struct §(class_def)Holder<§(class_def_arg)Widget>;

}  // namespace classes

namespace functions {

template <typename T>
void convert(T value) {}

extern template void §(function_extern)convert<§(function_extern_arg)Widget>(§(function_extern_param)Widget);

template void §(function_def)convert<§(function_def_arg)Widget>(§(function_def_param)Widget);

}  // namespace functions

namespace variables {

template <typename T>
T zero = T();

extern template §(variable_extern_type)Widget §(variable_extern)zero<§(variable_extern_arg)Widget>;

template §(variable_def_type)Widget §(variable_def)zero<§(variable_def_arg)Widget>;

}  // namespace variables
