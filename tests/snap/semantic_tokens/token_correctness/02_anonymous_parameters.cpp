/// # Anonymous parameters
///
/// - status: supported
///
/// Unnamed parameters produce no tokens
///
/// The punctuation after an unnamed parameter's type stays token-free.

void take_one(int§) {}
void take_two(int§, char* c) {}
