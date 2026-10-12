# Header Contexts

What a header means depends on the file that includes it: the macros and declarations that come before the `#include`. clice compiles a header as part of a file that includes it, its host, so the header reads the way the build sees it; a source the build compiles several ways has a context for each command. The editor shows the active context and can switch to another. [Compilation Context](../design/compilation-context.md) explains the design.

<!-- The cards in the sections below are generated from the documented cases
     in tests/contexts/. Do not edit the regions between the
     GENERATED markers by hand — edit the cases' README.md and run
     `pixi run update-feature-docs`. -->

## Headers That Need Their Includer

A header that compiles only after what its includer declares first works in that includer's context.

<!-- BEGIN GENERATED ITEMS: contexts/self_containment -->

<!-- BEGIN CAPABILITY: supported -->

**Headers that need their includer**

A header that uses what its includer declares first still compiles, in the includer's context

`ruler.h` uses a type and a macro that `main.cpp` declares before including it, so the header does not compile on its own. clice finds that out and compiles it after what `main.cpp` declares, so it has no errors and hover works inside it.

```case
tests/contexts/self_containment/01_host_declarations
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Choosing a Host

A header with several includers reads the way the chosen one makes it, and the editor can switch to another.

<!-- BEGIN GENERATED ITEMS: contexts/switching -->

<!-- BEGIN CAPABILITY: supported -->

**Choosing a header's host**

A header that relies on its includer reads the way the chosen includer makes it

`shared.h` uses a macro that each source defines before including it. Both sources are contexts of the header: clice picks one, the editor can switch to the other, and hover then shows the type that host gives. The choice is kept across restarts until it is reset.

```case
tests/contexts/switching/01_two_hosts
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Included Fragments

A file included in the middle of a declaration, such as a class body or an enumerator list, compiles where it is included.

<!-- BEGIN GENERATED ITEMS: contexts/fragments -->

<!-- BEGIN CAPABILITY: supported -->

**Class body fragments**

A file included inside a class body compiles as part of that class

`body.inc` holds members that `rec.cpp` includes between the braces of `Record`. Opened on its own, it compiles inside the class, so it has no errors, lists its members, and hover and go-to-definition work on them.

```case
tests/contexts/fragments/01_class_body
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**X-macro lists**

A list of macro calls compiles where its includer first expands it

`errors.def` is an X-macro list: `codes.cpp` includes it once to declare the enumerators and once to name them. Opened on its own, it compiles inside the enum, so its entries show as enumerators.

```case
tests/contexts/fragments/02_enumerator_list
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Files Built Several Ways

A source the build compiles with several commands has a context for each of them.

<!-- BEGIN GENERATED ITEMS: contexts/hosts -->

<!-- BEGIN CAPABILITY: supported -->

**Files built several ways**

A source the database lists under several commands offers each one as a context

When a build compiles one file more than once, for example once per configuration, every command is a context the editor can switch between, labeled by its flags.

```case
tests/contexts/hosts/02_multiple_entries
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->
