# Members of a base

A member alias of a base class reads through the class the code reaches it in

`map.find(key)` gives a `Map<int, long>::iterator`, not the `MapBase<Map<int, long>, int, long>::iterator` its base declares.
