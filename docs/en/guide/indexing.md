# Indexing

The answers that reach beyond the files you have open — references, call and type hierarchies, workspace symbols, rename, and hover or navigation on a file not compiled yet — come from clice's index of the project. This page says what goes into it and how to make the first index of a big project take less time.

## What Gets Indexed

When the server starts, the background index compiles every translation unit of the project its saved index does not hold as current: each file of its compilation databases, the sources a rule's `default_command` covers, and the files opened earlier that no database lists. It starts `[workers] background_delay_ms` after work is queued, runs on the background workers, and gives way to the requests of the files you work on. A header is indexed through the units that include it, once for each way they include it, so a header compiled differently by different sources answers for each of those contexts.

For each file the index records its declarations, definitions and references, what calls what, what derives from what, and the template instantiations the units make. Inside a function body it records the names the body uses, unless `index_function_body` leaves the file's bodies out: by default the bodies of system headers (the standard library, an `-isystem` directory) are left out, and so are the instantiations of templates written there. The rest of a system header — its declarations, its types, its templates — is indexed.

The index is saved in the cache directory, every five minutes while it is built and when the run ends. The next server, or `clice index` run, loads it and compiles again only the units whose files changed since. `clice index` builds or updates the index from the command line; while a server of the same clice version serves the project, it asks that server to do it.

One process writes a project's index at a time. A second server on the same project, such as a second editor window, waits up to five seconds for the first to release it, time enough for a restarting server's predecessor to finish saving; if it is still held, the second serves nothing of that project and says which process holds the index.

## A Faster First Index

The first index of a big project compiles every unit once; it is a cost paid once, as later starts compile only what changed, and one interrupted resumes where it stopped. The session log says where its time goes: a line for each unit with how long it took, and with `CLICE_PERF=1` in the environment a `[perf:index]` line per unit splitting that time into indexing and merging, and one for the whole run. Four things shorten it:

- **More workers.** `[workers] background` is how many units the server compiles at once: by default half the machine's physical cores, at least 2, and never more than `[workers] max`, which defaults to all of them. On a machine with many cores and memory to spare, raise it. `clice index` compiles `--jobs` units at once, by default as many as the machine has CPUs.
- **Enough memory.** A unit starts only when the machine's memory leaves room for it, and the newest one stops when memory runs short, so on a machine short of memory more workers do not help: the log says when background work waits for memory. A unit that does not fit even alone is named by `clice index`, with the memory its worker reached.
- **Fewer units.** A rule with `background_index = false` keeps the files it matches from being compiled as units of their own: tests, examples, benchmarks or generated sources you do not navigate in. A header they include is still indexed through the other units including it, and a module unit or the unit writing a precompiled header still compiles when another unit needs its module or header. A compilation database that lists headers, as some generators write them, makes each a unit of its own beside the sources including it; a rule over `**/*.h` with `background_index = false` keeps those out.
- **Skipped bodies.** A rule with `skip_function_body = true` makes every compile but the file's own skip the function bodies of the files it matches, templates included, in the editor and in the background index alike. Put it on third-party code: the units of your project then parse only the declarations of the libraries they include, and a module of the library they import is built with its bodies skipped too. Function templates that forward their arguments, such as `std::make_unique`, are still parsed, and so are `constexpr` and `consteval` functions and those returning `auto`.

```toml
[[rules]]
patterns = ["third_party/**", "external/**"]
skip_function_body = true
background_index = false
lint = false
format = false

[[rules]]
patterns = ["benchmarks/**", "**/*.h"]
background_index = false

[workers]
background = 16
```

What skipping costs: the errors inside those bodies are not reported, nor the errors your code causes when it instantiates their templates, and the names their bodies use are not indexed — references and call hierarchies stop at their declarations. `skip_function_body = false` turns skipping off everywhere, the editor's own included: by default the editor skips the bodies of the headers at the top of an open file and the background index parses them, reporting what it finds there on a file compiled under its own database entry.

## Library Internals

To navigate inside the standard library or another system library — find the callers of a function within it, or the instantiations of its templates — let a rule index its bodies:

```toml
[[rules]]
patterns = ["/usr/include/c++/**"]
index_function_body = true
```

The index grows with every body it records, so this is best kept to the libraries you read.
