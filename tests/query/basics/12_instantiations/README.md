# Answers through instantiations

References and the call graph include what instantiations and macro expansions resolve, each marked with its `via`

A use the written code leaves open — a dependent call in a template, a name
in a macro body — answers with `"via": "instantiation"` or
`"via": "expansion"`; the written code's answers carry no `via`. A caller
carries one only when instantiations make all its calls.
