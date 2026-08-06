/// # Fold Kinds
///
/// ## Pragma classification — only the first argument token decides region/endregion
///
/// - status: supported
/// - order: 7

// The leading declaration ends the preamble so the pragmas below reach the
// main-file parse on both the inspect and the server path.
int before = 0;

// Neither a region name nor another pragma's argument mentioning
// "endregion" may close the fold early.
#pragma region endregion_pair
int retries = 3;
#pragma mark see endregion notes
int limit = 10;
#pragma endregion

// The tail of a multiline comment before the introducer must not hide
// the region either.
/* spans
a line */ #pragma region after_comment
int after = 1;
#pragma endregion
