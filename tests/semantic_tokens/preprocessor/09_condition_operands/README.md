# Macro names in conditions

A name a condition tests is a `macro`, defined or not

`#ifdef`, `#ifndef`, `defined(…)` and `#undef` name macros whether or not
one is defined; `defined` and `__has_include` are operators, and the file
`__has_include` tests is a header name.
