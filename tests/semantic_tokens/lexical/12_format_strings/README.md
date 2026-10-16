# Format strings

The placeholders of a format string are `formatSpecifier` tokens

A string passed where a function takes a format string — a
`basic_format_string` parameter as `std::format`, `std::print` and {fmt}
declare it, or the argument a `format(printf, …)` attribute names — splits
into its text and its placeholders: `{}` and `{:>8.2f}` in the first style,
`%d` and `%-8.3f` in printf's. Escaped braces and `%%` stay text, and so does
a string no function reads as a format.
