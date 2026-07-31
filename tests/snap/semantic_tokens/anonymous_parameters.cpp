/// # Token Correctness
///
/// ## Anonymous parameters — unnamed parameters must not produce tokens
///
/// - status: partial
/// - order: 2
///
/// An unnamed parameter currently emits a stray one-byte `parameter`
/// token on the punctuation that follows its type.

void take_one(int§) {}
void take_two(int§, char* c) {}
