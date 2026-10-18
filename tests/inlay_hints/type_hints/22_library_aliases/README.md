# Library member aliases

A member alias of a class template gives way to what it names where that reads shorter

`vector<int>::size_type` shows as `size_t` and `vector<int>::reference` as `int &`, but `vector<int>::iterator` stays: what it names is the library's own. An alias a namespace declares (`int64_t`, the code's `UserId`) or a class that is no template (`steady_clock::time_point`) stays as named; one that only stands for a builtin (`duration<long>::rep`) shows the builtin.
