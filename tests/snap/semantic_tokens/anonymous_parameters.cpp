/// # Token Correctness
///
/// ## Anonymous parameters — unnamed parameters produce no tokens
///
/// - status: supported
/// - order: 2
///
/// The punctuation after an unnamed parameter's type stays token-free.

void take_one(int§) {}
void take_two(int§, char* c) {}
