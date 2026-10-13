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

<!-- BEGIN CAPABILITY: supported -->

**Picked overload**

The overload the user moved the popup to stays shown while typing in the call

Asked again while the call is being typed, signature help keeps the overload
the editor shows active as long as it is still listed, instead of jumping back
to the one that fits the arguments best.

```case
tests/signature_help/overload_signatures/07_kept_selection
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Call Contexts

Signature help follows the call around the cursor, whatever is being called: a constructor, a function pointer, an object with `operator()`, a template.

<!-- BEGIN GENERATED ITEMS: signature_help/call_contexts -->

<!-- BEGIN CAPABILITY: supported clangd#726 -->

**Constructors and aggregates**

A constructor call lists the constructors without a return type, and braced aggregate initialization lists the fields

Written with braces, the constructors show with braces too.

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

<!-- BEGIN CAPABILITY: supported clangd#299 -->

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

Calling an object shows its `operator()` overloads under the object's name

An object the call does not name, such as one a function returns, shows
`operator()` with its qualifiers.

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

<!-- BEGIN CAPABILITY: supported -->

**Class template members**

A member of a class template shows the types the object's class is instantiated with

`Vector<Point>::at` returns `Point &`, not `T &`, and a call through a
function object shows the parameters of the function type it was declared
with. A member template's own parameters stay as it declares them.

```case
tests/signature_help/call_contexts/13_class_template_members
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Parameter Display

Each parameter shows as the call takes it, its default value as the source writes it.

<!-- BEGIN GENERATED ITEMS: signature_help/parameters -->

<!-- BEGIN CAPABILITY: supported -->

**Default arguments**

Parameters with defaults show their default value as the source writes it

A macro stays a macro and an octal number stays octal; a default written over
several lines shows on one.

```case
tests/signature_help/parameters/01_default_arguments
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**C-style variadic functions**

A C-style `...` shows as the last parameter, marked for every argument past the named ones

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

<!-- BEGIN CAPABILITY: supported clangd#2284 -->

**Explicit object parameters**

A C++23 `this` parameter stays out of the label: the object a member is called on is no argument

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

<!-- BEGIN CAPABILITY: supported clangd#638 -->

**Standard forwarding functions**

`make_unique`, `make_shared` and the containers' `emplace` functions show the constructors of what they build

The constructors that can take the arguments written so far are listed, the
best match first, each with its parameters after the function's own ones,
such as the key of a map's `try_emplace`. An aggregate lists its fields.

```case
tests/signature_help/parameters/07_standard_forwarding
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Documentation

The called function's documentation comment shows with its signatures, as plain text, read as hover reads it; each parameter carries its `@param` text, which editors show for the parameter being written.

<!-- BEGIN GENERATED ITEMS: signature_help/documentation -->

<!-- BEGIN CAPABILITY: supported -->

**Documentation comments**

The called function's documentation comment shows with its signature, and the `@param` text of the parameter being written with that parameter

With `documentation = false` in the `[signature_help]` section the popup
shows the signatures alone.

```case
tests/signature_help/documentation/01_doc_comments
```

<!-- END CAPABILITY -->

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
