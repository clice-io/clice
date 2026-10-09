/// # Comment folding
///
/// - status: supported
///
/// Multiline block comments and runs of line comments fold
///
/// Line comments on consecutive lines fold as one run below the first line,
/// which stays visible; a blank line or a line of code ends the run. A block
/// comment folds on its delimiters like a brace pair. A comment trailing code
/// does not fold.

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

int limit = 0; /* nor does a block comment
                  trailing code fold */

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
