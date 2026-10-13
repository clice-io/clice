// An explicit template argument names no template parameter: most of their
// names say nothing (`_Tp`, `_Alloc`).
template <typename T, typename U>
T convert(U val);

float converted = convert<float>(42);
