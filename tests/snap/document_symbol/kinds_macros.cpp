/// # Symbol Kinds
///
/// ## Macro definitions — object-like and function-like macro definitions in the outline, a parameter list as the function-like detail
///
/// - status: supported
/// - issues: clangd#1744
/// - order: 6

// The assertion holds the directives out of the preamble region, whose
// live record the server path does not yet see.
static_assert(true);

#define MAX_BUFFER_SIZE 4096
#define CHECK(cond, msg) ((cond) ? 0 : (msg))
#define TRACE(...) log(__VA_ARGS__)
#define SPLIT_\
LIMIT 7

struct Config {
#define CONFIG_VERSION 3
    int version = CONFIG_VERSION;
};
