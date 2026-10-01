#pragma once

namespace outer {

template <int N>
struct Flag {};

template <template <class> class TT>
struct Holder {};

}  // namespace outer

namespace {

int hidden_value = 0;

}  // namespace
