# Computed types

A type a trait computes shows as what it computes

The member `type` of a class template specialization (`common_type<seconds, seconds>::type`), an alias template that computes its type (`enable_if_t<true, int>`) and a builtin template (`__type_pack_element`) give way to their result, named as the code names it: `chrono::seconds`, `int`.
