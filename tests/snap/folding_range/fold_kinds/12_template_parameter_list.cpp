/// # Template parameter list folding
///
/// - status: supported
///
/// Multiline template parameter lists fold on their angle brackets
///
/// This covers class, function, variable and alias templates, partial
/// specializations, the lists an out-of-line member definition repeats,
/// template template parameters and lambdas with explicit template
/// parameters.

template<typename T>
struct Less;

template<
    typename Key,                 // ┐
    typename Value,               // │ foldable
    typename Compare = Less<Key>  // ┘
>
class SortedMap {
    template<
        typename Other,
        typename = void
    >
    void merge(const Other&);
};

template<
    typename Key,
    typename Value,
    typename Compare
>
template<
    typename Other,
    typename
>
void SortedMap<Key, Value, Compare>::merge(const Other&) {}

template<
    typename Value,
    typename Compare
>
class SortedMap<int, Value, Compare> {};

template<template<
    typename,
    typename
> class Container>
struct Adapter {};

template<
    typename T
>
constexpr bool always = true;

template<typename T,
         typename U>
using First = T;

auto generic = []<
    typename T,
    typename U
>(T left, U right) { return left + right; };
