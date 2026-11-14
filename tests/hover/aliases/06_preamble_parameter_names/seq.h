#pragma once

using size_t = decltype(sizeof(0));

template <class T, T... Values>
struct sequence {};

template <class T, T Size>
using make_sequence = __make_integer_seq<sequence, T, Size>;

template <size_t Size>
using make_index_sequence = make_sequence<size_t, Size>;
