#include <sycl/sycl.hpp>

#if __has_include("kernel.cpp.expect.h")
#include "kernel.cpp.expect.h"
#endif

#include "util.h"

int doubled_ends(sycl::queue& queue, int* values, int size) {
    queue.parallel_for(sycl::range<1>(size), [=](sycl::id<1> i) { values[i] *= 2; }).wait();
    return util_add(values[0], values[size - 1]);
}
