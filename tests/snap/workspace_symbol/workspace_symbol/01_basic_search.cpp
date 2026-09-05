/// # Basic workspace-wide symbol search
///
/// - status: supported
/// - verify: server
///
/// Workspace symbol search matches names by case-insensitive substring
///
/// A query matches any symbol whose name contains it, ignoring case:
/// functions, types, enumerators and macros all participate, and a query
/// with no match returns an empty list rather than an error.

// query: widget
// query: parse_config
// query: MODE
// query: fast
// query: no_such_symbol

struct Widget {
    int width;
};

enum class Mode { Fast, Safe };

#define MODE_DEFAULT 1

void parse_config() {}
