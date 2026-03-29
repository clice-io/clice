export module Tmpl;
export template<typename T>
T identity(T x) { return x; }
export template<typename T, typename U>
auto pair_sum(T a, U b) { return a + b; }
