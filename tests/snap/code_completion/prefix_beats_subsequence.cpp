/// # Filtering & Ranking
///
/// ## Prefix outranks subsequence — an exact-prefix candidate sorts above a scattered subsequence match
///
/// - status: supported
/// - order: 12
/// - diagnostics: expected
///
/// For prefix `fo`, `format_output` is a true prefix and outscores
/// `fast_math_operation`, which only matches as a subsequence.

// The completion prefix dangles as an unfinished statement.
int format_output;
int fast_math_operation;

void bar() {
    int v = fo§(pos);
}
