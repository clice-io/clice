// The header uses Counter<int>::value, whose definition main.cpp gives, in
// a function body the editor's compile skips. The background index compiles
// the whole file, instantiates the definition at the end and reports the
// error in its initializer.
#include "counter.h"

template <typename T>
int Counter<T>::value = T::missing;
