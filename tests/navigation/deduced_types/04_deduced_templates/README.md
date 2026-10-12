# Deduced templates and aliases

An `auto` deduced as a template specialization reaches the template, or
the explicit or partial specialization it selects; one deduced through an
alias reaches the alias

A builtin type or a lambda's closure type has no declaration to reach, and
the `auto` of a `new` expression navigates nowhere. A macro spelling `auto`
navigates to the macro.
