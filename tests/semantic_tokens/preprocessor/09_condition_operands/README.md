# Macro names in conditions

A name a condition tests is a `macro`, defined or not

`#ifdef`, `#ifndef`, `defined(…)`, `#undef` and the names an `#if` tests
name macros whether or not one is defined; `defined` and checks such as
`__has_include` are operators, what such a check tests is no macro, and the
file `__has_include` tests is a header name.
