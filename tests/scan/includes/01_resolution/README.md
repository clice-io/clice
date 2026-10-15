# Resolve includes

Each file lists the headers its includes resolve to, and the includes no search finds

The scan resolves every `#include` the way the compiler searches for it: a quoted name from the includer's directory first, then the search path the command gives. It follows the headers it finds, and reports the includes it could not resolve by name.
