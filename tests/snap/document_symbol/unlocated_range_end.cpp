// - flags: ["-std=c++2c"]

// Clang leaves the end of these aliases unlocated: their symbols span the
// name instead of running to the end of the file.

template <class... Ts>
using First = Ts...[0];

using v8hi = short __attribute__((vector_size(16)));

int after = 0;
