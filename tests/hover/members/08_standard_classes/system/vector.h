#pragma once

namespace std {

template <typename _Tp>
struct allocator {};

template <typename _Tp, typename _Alloc>
struct _Vector_base {
    _Tp* _M_start;
    _Tp* _M_finish;
    _Tp* _M_end_of_storage;
};

/// A standard container which offers fixed time access to individual
/// elements in any order.
template <typename _Tp, typename _Alloc = allocator<_Tp>>
class vector : protected _Vector_base<_Tp, _Alloc> {
    typedef _Vector_base<_Tp, _Alloc> _Base;

public:
    typedef _Tp value_type;
    typedef _Tp& reference;
    typedef unsigned long size_type;

    /// Add data to the end of the vector.
    /// @param __x Data to be added.
    void push_back(const value_type& __x);

    size_type size() const;
};

}  // namespace std
