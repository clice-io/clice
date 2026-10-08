/// # Comments
///
/// - status: supported
///
/// A comment's text is selected without its markers, then the comment, then
/// the run of comment lines it belongs to

// The first line of a run,
// the §(run)second line of it.
int value = 0;

/** A §(block)documentation block. */
int documented() {
    int count = 0; ///< a §(member_doc)member note
    return value + count; // a §(trailing)trailing note
}
