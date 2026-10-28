# Word-boundary fuzzy match

Prefix `fb` matches the word starts of `foo_bar_baz`

`frobnicate` is only a weak scattered subsequence of `fb` and is dropped; `foo_bar_baz` matches on the `foo` and `bar` word boundaries and survives.
