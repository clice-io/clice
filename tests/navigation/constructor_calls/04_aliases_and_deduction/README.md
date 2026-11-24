# Aliases and deduction

A construction through an alias or a deduced template leads to its constructor

`Alias(1)` calls the constructor of the class the alias names, and `Box(2)`
the constructor of the specialization its arguments deduce: both lead
there, go-to-type-definition to the type. `Box c(3)` declares a variable,
whose name the call is: `Box` names the template alone.
