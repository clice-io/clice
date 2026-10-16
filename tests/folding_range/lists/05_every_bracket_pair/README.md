# Every bracket pair

Any parentheses, brackets or braces spanning lines fold, whatever writes them

Conditions, casts, `static_assert`, parenthesized expressions, subscripts,
attribute lists, function types and the arguments of a macro invocation fold
like a call's arguments do. An attribute list written `[[...]]` folds once.
