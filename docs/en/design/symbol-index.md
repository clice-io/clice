# Symbol Index

## Background

Go to definition, find references, call and type hierarchies cross files, so a language server keeps a project-wide index of where each symbol appears and how symbols relate. C++ makes that index hard in two ways.

**Contexts.** A header means different things to different includers:

```cpp
// crypto.h
#ifdef USE_OPENSSL
    using TLSContext = OpenSSLContext;
#else
    using TLSContext = BoringSSLContext;
#endif
```

An index that keeps one includer's reading loses the other's references. clangd's background index keeps the last compile of each header, so whichever unit is indexed last wins.

**Scale.** A project of a few thousand sources has hundreds of thousands of symbols; LLVM or Chromium millions. Memory, build time and query latency all have to hold at that size. clangd stores a symbol's data with the file declaring it, which keeps reference counts from adding up across files ([clangd#23](https://github.com/clangd/clangd/issues/23)) and leaves references missing until their files are opened ([clangd#516](https://github.com/clangd/clangd/issues/516), [clangd#802](https://github.com/clangd/clangd/issues/802)); a changed command does not trigger a reindex ([clangd#199](https://github.com/clangd/clangd/issues/199)).

clice separates a small global directory of symbols from per-file data, keeps every context of a file as a variant deduplicated by content, stores everything in one embedded database read in place, and overlays the open files' own compiles so answers follow the buffers being edited.

## Design

### Symbol Identity

A symbol is a 64-bit hash of its **entity**: what every redeclaration shares, so the hash comes out the same in every unit and every process. That is its chain of enclosing scopes, its name, and what tells apart declarations sharing a name — a function's parameter types, a template's head and constraints, a specialization's arguments. Types enter canonically, so a typedef hashes as the type it names. A declaration of internal linkage also carries the file declaring it: a `static` helper in a header is one symbol in every unit including it, two of the same name in two sources are two. A file inside the project enters the hash by its path relative to the project root, so a moved checkout keeps every hash.

### Occurrences and Relations

The index stores two kinds of entries. An **occurrence** is a range and the symbol written there: it answers "what is under the cursor", by binary search over positions. A **relation** is a kind, a range and a target: definition, declaration, reference, call, base and derived, override, type definition, specialization, and a few more. Relations answer "where is this symbol defined, used, called". The two are stored apart because one is looked up by position and the other by symbol.

### Layers

```
unit index        what one compile produced, discarded once merged
    ↓ merge
project index     the directory: external symbols, and which units contributed which files
shard             one file's entries, a variant per context, loaded on demand
instantiations    what instantiations and macro expansions resolve in one file
    ↑ overlay
live index        the open buffer's latest compile
```

- The **project index** holds one small record per externally visible symbol — what it is, never where: its name, its parent, kind, flags, and the sets of files naming, declaring and defining it — and records of which units contributed which files. A query reads a handful of records; opening the index reads only the database's header and the name search.
- A **shard** holds a file's entries, with a variant for each context that preprocesses it differently. Names local to the file — internal linkage, function locals — keep their metadata in the shard; one several files name, such as a `static` helper in a header, gets a project record of the files holding its entries.
- An open file's **live index** comes from its last compile and is never written to the global index: incomplete code being typed must not change what other files see. While the buffer is the text the shard indexed, the code the compile's context skipped answers from the shard, so a header's other contexts show too.

### One Variant per Distinct Content

A header included by N sources yields N sets of entries, nearly all identical. Each variant is encoded canonically — same entries, same bytes — and identified by the hash of those bytes. Merging a unit whose variant exists already only records the unit; nothing is decoded or written. That path covers the vast majority of merges, which is what makes indexing a whole project affordable. Which variants are live follows the units still vouching for them.

### Instantiations and Macro Expansions

A template's own entries say what its body names before any argument is known: in `Box<T>::show`, `value.print()` names nothing, since `value` is a `T`. What it names depends on the instantiation — `A::print` in `Box<A>`, `B::print` in `Box<B>` — and writing either into the header's shard would make the shard vary with the unit that indexed it. A macro body is alike: `#define AREA (WIDTH * WIDTH)` names whatever `WIDTH` is where it expands.

Each instantiated definition and each macro expansion is therefore projected on its own, at the template's or the `#define`'s text, keeping only what that text's entries leave open. These records are a file's **instantiations**, stored beside its shard, and references, call hierarchy and rename read them as places the code reaches through an instantiation or an expansion. Walking every instantiation in every unit would double indexing time; since one instantiation means the same in every unit, a unit skips the ones another fresh unit has walked already.

A system header's function bodies, and the instantiations of its templates, are left out unless a rule's `index_function_body` asks for them: what `std::vector` does inside is the library's, and every unit including it would walk it again.

### Storage

All index data — records, shards, unit manifests, the name search — lives in one LMDB database. Each blob is encoded canonically and compactly, validated once when read, then used in place from the memory map, with no deserialization step. The database records files inside the project by their paths relative to the project root, so a moved checkout finds its index current. A save writes the changes in one transaction; a record that fails its checks means damaged storage, and the next save discards the database and indexes again. LMDB never returns freed pages, so once indexing is idle and much of the file is free, the writer replaces it with a compact copy.

A unit's **manifest** keeps its context once — which files it saw, through which include tree — so shards need not repeat it. Staleness checks and reconciliation (a unit gone from the database, a changed command, a changed dependency) run from the manifests.

### Queries

Find references, for instance: the occurrence under the cursor gives the symbol; its record lists the files naming it, and the files whose instantiations do; each file answers from its live index when it is open and current, else from its shard's live variants and its instantiations; the relations of the asked kind are collected and converted to positions. Shards keep their line tables, so positions convert without opening the file. An open file whose preamble is compiled into a PCH also answers from an index of that preamble, built with the PCH while its AST is still in memory.

### Name Search

Searching by name (`workspace/symbol`, `clice query` locators) runs over a search index derived from the records, persisted beside them in segments. Its tokens are trigrams along the paths a fuzzy match can take through a name's words (`gsh` and `symhash` both reach `getSymbolHash`), so intersecting a query's trigrams selects every candidate the matcher could accept, and only those are scored; shorter queries key a name's first words, and a pass tolerating a typo fills a result left short. Entries are kept in quality order, so the scan stops once the rest cannot enter the result. Each save adds what changed as a new segment run, folding small runs into larger ones, so a symbol is rewritten a logarithmic number of times.

### Staleness

Each indexed artifact records the content versions of its inputs, checked by the shared freshness check (see [Incremental Compilation](incremental-parse.md)). A unit is reindexed when an input's content actually changed, or when its command hash changed. At startup the writer checks every file record against the disk and queues the units owed; the stored shards answer until then.

## FAQ

- **Why a directory and shards, not one index?** Positions in the directory would make every record large and every query read far more than it answers; without the directory, a query would have to open every shard to find the files holding a symbol.
- **Why canonical encoding, not a serialization framework?** Because byte identity is what deduplication compares: the encoding is a strict function of the entries, so same content means same bytes and a merge is a hash comparison. It also lets blobs serve queries straight from the memory map.
- **Is a hash collision a risk?** A 64-bit collision would make two different variants share one copy — wrong entries for one context — but cannot corrupt the database or crash the server. The cheap hash on the merge path is deliberate.
- **Why do cross-file queries run in the master process?** They combine every open file's live index with the persisted index. Spread over workers, each query would ask several processes and merge; in the master it is one read, and the master is where the buffers' versions are known.
- **Why a database for the index, but plain files for PCHs and modules?** Index blobs are many, small and written in bursts, where batched transactions and one memory map win. PCHs and PCMs are few, large and memory-mapped by Clang itself, which could not read them out of a database.

## Known Limitations

- **What an instantiation resolves follows the unit that walked it.** A unit skips an instantiation another unit walked, assuming the one-definition rule; a program breaking it — an overload found by argument-dependent lookup in a header only some units include — answers as the walking unit saw it.
- **Instantiations do not name their arguments.** The index says what an instantiation's dependent code resolves to, not with which arguments or where it was instantiated.
- **What a preamble's macros expand to answers from the background index**: the names the macro bodies of an open file's preamble headers spell answer once the background index has walked those headers.
