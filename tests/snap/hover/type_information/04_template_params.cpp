/// # Template parameters
///
/// - status: supported
///
/// Type, template-template, non-type
///
/// Each template parameter kind reports its form: a type parameter, a
/// template-template parameter, and a non-type parameter with its default.

namespace parameter_kinds {
template <typename §(01_type_param)Value = long> void accept_type();

template <template <typename> class §(02_template_template_param)Container>
void accept_template();

template <long §(03_non_type_param)Count = 8> void accept_value();
}
