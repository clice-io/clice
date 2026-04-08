# Chained PCH Benchmark Results

clang 20.1.8, libstdc++ 14.2.0, Linux x86_64

## Summary

| Metric                          | Monolithic PCH       | Chained PCH             |
| ------------------------------- | -------------------- | ----------------------- |
| Initial build (20 headers)      | 869ms                | 1488ms (1.7x slower)    |
| Incremental rebuild (+1 header) | 917ms (full rebuild) | 226ms (append one link) |
| Incremental speedup             | —                    | **4x**                  |

## Bug: `<cmath>` breaks macro visibility in chained PCH

**Reproduction**: Any PCH chain that includes `<cmath>` followed by `<deque>` fails:

```
algorithm.pch -> cmath.pch -> deque.pch  ← FAIL: undeclared '__N'
```

But both of these work:

```
cmath.pch -> deque.pch                   ← PASS (2 links, no prior chain)
algorithm.pch -> deque.pch               ← PASS (no cmath)
```

**Affected headers** (all fail when chained after `<cmath>`):

- `<deque>`, `<fstream>`, `<functional>`, `<map>`, `<queue>`,
  `<regex>`, `<stack>`, `<unordered_map>`, `<unordered_set>`, `<execution>`

All use `__N` macro from libstdc++'s `bits/c++config.h`.

**Root cause**: The `__N` macro IS present in the chained PCH (verified via `-E`),
but becomes invisible to certain headers during compilation with the chained PCH.
This is likely a clang serialization bug where macro visibility scope is lost
across PCH chain boundaries when the defining header (`c++config.h`) was included
in a link prior to `<cmath>`.

**Workaround for clice**: When building chained PCH, merge `<cmath>` and its
predecessor into a single link, or use a monolithic PCH when `<cmath>` is present.

## Chain depth limit

Tested up to **50 trivial links** and **90 real-header links** — no depth limit
observed. The bug is specific to `<cmath>` + libstdc++ `__N` macro interaction,
not chain depth.

## Files

- `bench_pch_chain.sh` — Basic benchmark (20 headers, monolithic vs chain, incremental)
- `bench_all_headers.sh` — Exhaustive test with all 100 C++ standard headers
- `find_breakpoint.sh` — Binary search for chain depth breakpoint
- `find_exact.sh` — Precise breakpoint isolation
- `isolate_cmath.sh` — Isolation of `<cmath>` as the trigger
