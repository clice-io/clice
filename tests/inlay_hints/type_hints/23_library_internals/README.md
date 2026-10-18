# Library internals

A type the library keeps to itself shows by the public name for it, or not at all

A reserved name in namespace `std` or a reserved one, and a member of a class with one, is the library's own: `__alloc_traits<allocator<int>>::value_type &` shows as `int &`. A class of its own that an algorithm returns shows as the member alias naming it in a class of its arguments or of the initializer, `vector<int>::iterator` for `__normal_iterator<int *, vector<int>>`; one nothing public names gets no hint.
