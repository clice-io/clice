/// # Macro shadowing a declaration
///
/// - status: supported
/// - diagnostics: expected
///
/// A name redefined as a macro completes as the macro, not the shadowed
/// declaration

void GUARD(int);
#define GUARD 1

int BOUND(int lo, int hi);
#define BOUND(lo, hi) ((lo) < (hi) ? (lo) : (hi))

int a = GUAR§(object);
int b = BOUN§(function);
