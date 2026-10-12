namespace parameter_kinds {
template <typename §(type_param)Value = long> void accept_type();

template <template <typename> class §(template_template_param)Container>
void accept_template();

template <long §(non_type_param)Count = 8> void accept_value();
}
