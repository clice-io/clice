/// # Comment folding
///
/// - status: supported
///
/// Multiline block comments and runs of line comments fold
///
/// Line comments on consecutive lines fold as one run below the first line,
/// which stays visible; a blank line or a line of code ends the run, and a
/// comment trailing code joins none. A block comment folds on its delimiters
/// like a brace pair.

// This is a long
// multi-line comment
// that folds as one run

// A blank line starts another run
// of two lines

/*
 * Block comment
 * also folds
 */

/* single-line block comments stay unfolded */
// so does a lone line comment

int counter = 0;  // a comment trailing code
                  // does not start a run

/* a block comment
   next to */
// a run of
// line comments
int separate();

void nested() {
    // runs fold inside bodies
    // like anywhere else
    counter += 1;
}

#define SCALE(x) \
    /* a block comment
       inside a macro */ (x) * 2

#if 0
// comments in skipped branches
// fold as well
#endif
