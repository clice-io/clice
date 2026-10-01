/// # Pragma classification
///
/// - status: supported
///
/// Only the first argument token decides region/endregion

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
