/// # Raw string literal folding
///
/// - status: supported
///
/// Multiline raw string literals fold on their delimiters
///
/// The placeholder repeats the encoding prefix, a custom delimiter and a
/// literal suffix. A raw string written in a macro argument folds where it is
/// written.

auto sql = R"(
    SELECT *
    FROM users
    WHERE active = true
)";

auto delimited = u8R"html(
    <p>")" stays inside</p>
)html";

auto single = R"(one line)";

struct Query {};
Query operator""_query(const char*, decltype(sizeof 0));

auto query = R"(
    SELECT 1
)"_query;

#define QUOTE(text) text

auto quoted = QUOTE(R"(
    spelled in a macro argument
)");

const char* call(const char*);

auto argument = call(R"(
    nested in a call
)");

auto joined = R"(
    each piece of a concatenation
)" R"(
    folds on its own
)";

#if 0
auto skipped = R"(
    raw strings in skipped branches fold too
)";
#endif
