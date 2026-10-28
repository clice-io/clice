---
option: rules.resolve_symlinks
---

# One index per linked build

A rule resolving symlinks keeps an index for each build its link points to, and follows the link when it moves

The build system points `out/current` at the build it chose. Pointed at `out/debug`, which the `debug` configuration names too, the server switches to that configuration's index; pointed back at `out/release`, the release index serves again and nothing is indexed anew.
