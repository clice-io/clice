#pragma once
namespace ns {
template <class T> struct list {};
template <class T, class P> int erase_if(list<T>& c, P p) { return 1; }
}
