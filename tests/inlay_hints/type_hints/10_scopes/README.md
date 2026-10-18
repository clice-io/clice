# Namespaces

A hint names a type with the namespaces code at the hint would write, but `std`

Class scopes stay, as do the namespaces inside `std` (`chrono::seconds`, `pmr::string`); a namespace around the hint or one a using-directive there brings in is left out, an anonymous one never shows. Template arguments read the same way.
