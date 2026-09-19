/// # Query syntax
///
/// - status: supported
/// - verify: server
///
/// Quotes, wildcards, scopes and filters narrow a search
///
/// `"process"` matches the whole name only and `proc*` whatever it globs;
/// `io::*` lists a namespace's members and `io::**` its whole subtree;
/// `kind:function` keeps one kind. Terms combine, separated by spaces.

// query: "process"
// query: proc*
// query: io::*
// query: io::**
// query: process kind:function

namespace io {

struct Stream {};

void process(Stream& stream);
void process_all();

namespace detail {
void processor();
}  // namespace detail

}  // namespace io

int processed;
