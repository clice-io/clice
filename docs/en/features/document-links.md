# Document Links

Clickable links from source directives to their resolved target files.

<!-- The cards in the sections below are generated from the documented cases
     in tests/document_links/. Do not edit the regions between the
     GENERATED markers by hand — edit the cases' README.md and run
     `pixi run update-feature-docs`. -->

## Include Directives

The header name of an `#include`, and the path a `__has_include` checks, link to the file they resolve to.

<!-- BEGIN GENERATED ITEMS: document_links/include_directives -->

<!-- BEGIN CAPABILITY: supported -->

**Quoted and angled includes**

Each include links its header name to the file it resolves to

```case
tests/document_links/include_directives/01_header_paths
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2375 -->

**Macro-expanded paths**

`#include MACRO` links the macro name to the header it expands to

```case
tests/document_links/include_directives/03_macro_include
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**`#include_next`**

`#include_next` and `__has_include_next` link to the next header of that name on the search path

Here `first/wrap.h` shadows `second/wrap.h`, and its `#include_next` reaches
the second copy.

```case
tests/document_links/include_directives/04_include_next
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`__has_include`**

The checked path links to the file it probes

```case
tests/document_links/include_directives/05_has_include
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Embed Directives

The resource of an `#embed`, and the path a `__has_embed` checks, link to the file they name.

<!-- BEGIN GENERATED ITEMS: document_links/embed_directives -->

<!-- BEGIN CAPABILITY: supported -->

**`#embed`**

The resource path links to the embedded file

```case
tests/document_links/embed_directives/01_embed
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`__has_embed`**

The checked path links to the probed resource

```case
tests/document_links/embed_directives/02_has_embed
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Module Declarations

The module name in an `import` links to the file that declares the module.

<!-- BEGIN GENERATED ITEMS: document_links/module_declarations -->

<!-- BEGIN CAPABILITY: supported -->

**Module targets**

Each imported module name links to the file that declares the module

```case
tests/document_links/module_declarations/01_module_imports
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->
