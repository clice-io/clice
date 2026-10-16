# Format strings

The placeholders of a format string are `formatSpecifier` tokens

A string passed where a function takes a format string — a
`basic_format_string` parameter as `std::format`, `std::print` and {fmt}
declare it, or the argument a `format(printf, …)` or `format(scanf, …)`
attribute names — splits into its text and its placeholders: `{}` and
`{:>8.2f}` in the first style, `%d` and `%-8.3f` in printf's, `%ms` and
`%15[^,\n]` in scanf's, a raw string's too. Escaped braces and `%%` are
escapes; a string no function reads as a format is plain text.
