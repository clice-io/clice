# Move-only fields

A field whose class moves but does not copy is taken by value and moved from, and an rvalue reference field binds its argument through `std::move`

The file gains `#include <utility>` when nothing declares `std::move` before the class. A class that neither copies nor moves gets no constructor.
