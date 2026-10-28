#pragma once

namespace lib {
template <class _Tp>
struct vector {
    using value_type = _Tp;
    void push_back(const value_type& __x);
    _Tp& front();
    vector& append(const vector& __other);
};

template <class _T1, class _T2>
struct pair {
    _T1 first;
    _T2 second;
};

template <class _Key, class _Val>
struct map {
    pair<const _Key, _Val>* find(const _Key& key);
};
}  // namespace lib
