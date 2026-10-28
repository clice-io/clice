// An explicit instantiation of a function is named as written, with the
// arguments it writes; one after an explicit specialization names it once.

template <typename T, typename U = int>
void convert(T value) {}

template void convert<char>(char);
template void convert(double);

template <>
void convert<long>(long value) {}

template void convert<long>(long);
