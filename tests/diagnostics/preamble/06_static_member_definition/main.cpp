#include "counter.h"

template <typename T>
int Counter<T>::value = T::missing;
