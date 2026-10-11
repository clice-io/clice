# Cosmetic signature differences

Parameter names, and a top-level `const` on a parameter, are not part of a
function's type: the declaration and the definition below spell the same
function differently, yet go-to-declaration still connects a use to the
prototype
