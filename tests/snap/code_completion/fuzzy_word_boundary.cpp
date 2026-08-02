/// # Filtering & Ranking
///
/// ## Word-boundary fuzzy match — prefix `fb` matches the word starts of `foo_bar_baz`
///
/// - status: supported
/// - order: 10
///
/// `frobnicate` is only a weak scattered subsequence of `fb` and is dropped;
/// `foo_bar_baz` matches on the `foo`/`bar` word boundaries and survives.

// error-ok: the completion prefix dangles as an unfinished statement.
int foo_bar_baz;
int frobnicate;

void bar() {
    int v = fb§(pos);
}
