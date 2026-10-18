#pragma once

namespace text {
template <class Char>
struct basic_string {
    typedef unsigned long size_type;
    static const size_type npos = static_cast<size_type>(-1);
};

template <class Char>
const typename basic_string<Char>::size_type basic_string<Char>::npos;

typedef basic_string<char> string;
}  // namespace text
