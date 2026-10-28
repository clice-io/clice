# Deduced variable members

A variable declared `auto` from a dependent initializer completes the members of the class it deduces to

`auto& row = rows[0]; row.` lists the members of `Vec<T>`. The declarator applies as in a real deduction: `const auto&` makes the object const, a by-value `auto` drops the initializer's const, `auto&&` and `decltype(auto)` keep it, and `auto*` takes the pointee. From a data member, `decltype(auto)` takes the type the member is declared with, or with parentheses the const reference the expression is.
