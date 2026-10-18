# Library aliases

The type of a variable keeps the alias its initializer names, but none the library keeps to itself

`values.size()` reads as `std::vector<int>::size_type` with what it stands for after it; the member alias of `__alloc_traits` that `front()` returns reads as `int &`, and a trait's result, `invoke_result_t<…>`, as what it computes. An alias over a class the library keeps to itself names that class after it: `std::vector<int>::iterator` is `__gnu_cxx::__normal_iterator<int *, std::vector<int>>`.
