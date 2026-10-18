# Dependent calls

Parameter names appear even when the callee is only known inside a template

Candidates are matched by argument count and by the arguments whose type is known; only a unique surviving candidate names the parameters, so a call that could still hit several overloads stays bare rather than guessing.
