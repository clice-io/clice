# Header macros defined per unit

Go-to-definition on a macro a shared header uses reaches every definition the units including it expand

Two units include `config.h`, one with `FAST` defined: the header's `MODE`
expands to a different definition in each, and from a closed header
go-to-definition lists both.
