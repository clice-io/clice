# Members through a dependent specialization

A member of a class template reached through a dependent specialization of it reads the arguments the code names it with

Over `lib::vector<Point<T>>`, `push_back` belongs to `lib::vector<Point<T>>` and its `value_type` is `Point<T>`, not the template's own `_Tp`; the `second` of the `pair<const int, T>` a `map<int, T>` finds is a `T`.
