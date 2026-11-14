# Indexing

The answers that reach beyond the files you have open — references, call and type hierarchies, workspace symbols, rename, and hover or navigation on a file not compiled yet — come from clice's index of the project. This page says what goes into it and how to make the first index of a big project take less time.

## What Gets Indexed

When the server starts, the background index compiles every translation unit of the project: each file of its compilation databases, the sources a rule's `default_command` covers, and the files opened earlier that no database lists. It waits until you have been idle for `background_delay_ms`, runs on the background workers, and gives way to the requests of the files you work on. A header is indexed through the units that include it, once for each way they include it, so a header compiled differently by different sources answers for each of those contexts.

For each file the index records its declarations, definitions and references, what calls what, what derives from what, and the template instantiations the units make. Inside a function body it records the names the body uses, unless `index_function_body` leaves the file's bodies out: by default the bodies of system headers (the standard library, an `-isystem` directory) are left out, and so are the instantiations of templates written there. The rest of a system header — its declarations, its types, its templates — is indexed.

The index is saved in the cache directory. The next server, or `clice index` run, loads it and compiles again only the units whose files changed since. `clice index` builds or updates the index from the command line; while a server serves the project, it asks that server to do it.

## A Faster First Index

The first index of a big project compiles every unit once. Three things shorten it:

- **More workers.** `[workers] background` is how many units compile at once: by default half the machine's physical cores. On a machine with many cores and memory to spare, raise it.
- **Fewer units.** A rule with `background_index = false` keeps the files it matches from being compiled as units of their own: tests, examples, benchmarks or generated sources you do not navigate in. A header they include is still indexed through the other units including it.
- **Skipped bodies.** A rule with `skip_function_body = true` makes every compile but the file's own skip the function bodies of the files it matches, templates included, in the editor and in the background index alike. Put it on third-party code: the units of your project then parse only the declarations of the libraries they include, and a module of the library they import is built with its bodies skipped too. Function templates that forward their arguments, such as `std::make_unique`, are still parsed, and so are `constexpr` functions and those returning `auto`, which the compiler needs.

```toml
[[rules]]
patterns = ["third_party/**", "external/**"]
skip_function_body = true
background_index = false
lint = false
format = false

[[rules]]
patterns = ["benchmarks/**"]
background_index = false

[workers]
background = 16
```

What skipping costs: the errors inside those bodies are not reported, nor the errors your code causes when it instantiates their templates, and the names their bodies use are not indexed — references and call hierarchies stop at their declarations. `skip_function_body = false` turns skipping off everywhere, the editor's own included: by default the editor skips the bodies of the headers at the top of an open file and the background index parses them, reporting what it finds there on the file.

## Library Internals

To navigate inside the standard library or another system library — find the callers of a function within it, or the instantiations of its templates — let a rule index its bodies:

```toml
[[rules]]
patterns = ["/usr/include/c++/**"]
index_function_body = true
```

The index grows with every body it records, so this is best kept to the libraries you read.
