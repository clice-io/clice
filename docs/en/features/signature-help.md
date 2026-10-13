# Signature Help

## Overload Signatures

A call lists every overload it may reach, with the parameter under the cursor marked.

<!-- BEGIN GENERATED ITEMS: signature_help/overload_signatures -->

<!-- BEGIN CAPABILITY: supported -->

**Overloads by arity**

Every overload is listed with the parameter under the cursor marked

```case
tests/signature_help/overload_signatures/01_overloads_by_arity
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Active parameter**

The parameter the argument under the cursor binds to is marked in the label

```case
tests/signature_help/overload_signatures/03_active_parameter
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#50 -->

**Const overloads**

Of a const and a non-const overload taking the same parameters, only the one the object calls shows

A non-const object calls the non-const overload; a const object can call only
the const one, whose label ends in `const`.

```case
tests/signature_help/overload_signatures/04_const_overloads
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Call Contexts

Signature help follows the call around the cursor, whatever is being called: a constructor, a function pointer, an object with `operator()`, a template.

<!-- BEGIN GENERATED ITEMS: signature_help/call_contexts -->

<!-- BEGIN CAPABILITY: supported clangd#726 clangd#2541 -->

**Constructors and aggregates**

A constructor call lists the constructors without a return type, and braced aggregate initialization lists the fields

```case
tests/signature_help/call_contexts/01_constructors
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Function pointer calls**

A call through a function pointer shows the parameter names its type spells

```case
tests/signature_help/call_contexts/02_function_pointers
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#299 clangd#1387 -->

**Template argument lists**

Template parameters show as the signature, a class template ending in its kind instead of a return type

```case
tests/signature_help/call_contexts/03_template_arguments
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Nested calls**

Inside a nested call the help follows the innermost call around the cursor

```case
tests/signature_help/call_contexts/04_nested_calls
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Functor calls**

Calling an object shows its `operator()` overloads

```case
tests/signature_help/call_contexts/05_functors
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#86 -->

**Lambda calls**

Calling a lambda shows its parameters under the name of the variable holding it

A lambda called where it is written has no name, so its parameters stand alone.

```case
tests/signature_help/call_contexts/06_lambdas
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial clangd#1967 -->

**New expression**

A new-expression's constructor arguments drive signature help

Written with parentheses, it lists the constructors; written with braces, it
shows nothing yet.

```case
tests/signature_help/call_contexts/07_new_expressions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#2472 -->

**Subscript operators**

An overloaded `operator[]` shows no signature help yet

```case
tests/signature_help/call_contexts/08_subscripts
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#1363 -->

**Inherited constructors**

Constructors a class inherits are missing from its constructor list

Only the implicit copy and move constructors show, not the `Base(int, int)`
that `using Base::Base` brings in and the call actually uses.

```case
tests/signature_help/call_contexts/09_inherited_constructors
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#795 -->

**Function-like macros**

A function-like macro's arguments show the macro's parameters, not those of what it expands to

A function called inside one of the arguments shows its own signature.

```case
tests/signature_help/call_contexts/10_macros
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Parameter Display

Each parameter shows as the call takes it, its default value as the source writes it.

<!-- BEGIN GENERATED ITEMS: signature_help/parameters -->

<!-- BEGIN CAPABILITY: supported -->

**Default arguments**

Parameters with defaults show their default value in the label

```case
tests/signature_help/parameters/01_default_arguments
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**C-style variadic functions**

The named parameters are listed, but the label leaves out the trailing `...`

Past the named parameters no parameter is marked, and nothing in the label
says the function takes more arguments.

```case
tests/signature_help/parameters/02_c_variadics
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Parameter packs**

A parameter pack shows as the template declares it, marked for every argument it takes

```case
tests/signature_help/parameters/03_parameter_packs
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#2284 -->

**Explicit object parameters**

A C++23 `this` parameter shows in the label and takes the first argument's mark

The object a member is called on is not an argument, yet the label lists
`this Shape &self` and marks it while the cursor is on the argument for `dx`.

```case
tests/signature_help/parameters/05_explicit_object_parameters
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported clangd#517 clangd#638 -->

**Forwarding functions**

A call that forwards its arguments to a constructor shows the forwarding pack, not the constructor's parameters

```case
tests/signature_help/parameters/06_forwarding
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Documentation

The called function's documentation comment shows with its signatures, as plain text.

<!-- BEGIN GENERATED ITEMS: signature_help/documentation -->

<!-- END GENERATED ITEMS -->

## Trigger Characters

Registered: `(`, `)`, `{`, `}`, `<`, `>`, `,`

| Character | Context            | Behavior                 |
| --------- | ------------------ | ------------------------ |
| `(`       | Function call      | Show overload signatures |
| `)`       | Close paren        | Update context           |
| `{`       | Brace init         | Show overload signatures |
| `}`       | Close brace        | Update context           |
| `<`       | Template args      | Show overload signatures |
| `>`       | Template close     | Update context           |
| `,`       | Argument separator | Update active parameter  |
