---
issues: [clice#1111]
---

# Default constructor kept

A class without a constructor also gains its default constructor, which the declared copy constructor would otherwise remove

It is `= delete` where the compiler deletes it, as for a reference member nothing initializes. On an aggregate the declared constructors end aggregate initialization since C++20: `Point{1, 2}` no longer compiles once the action is applied.
