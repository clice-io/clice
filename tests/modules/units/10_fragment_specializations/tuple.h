#pragma once

namespace std {

template <class T>
struct tuple_size;

template <decltype(sizeof 0) I, class T>
struct tuple_element;

}  // namespace std
