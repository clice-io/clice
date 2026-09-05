/// # Initializer truncation
///
/// - status: partial
///
/// Huge initializers render truncated, not in full
///
/// The rendered definition omits the initializer, but the evaluated
/// `Value` field still spells out all 256 elements.

#define DUPLICATE_FOUR(x) x, x, x, x
#define DUPLICATE_256(x) DUPLICATE_FOUR(DUPLICATE_FOUR(DUPLICATE_FOUR(DUPLICATE_FOUR(x))))
int val§(big_initializer)ues[] = {DUPLICATE_256(3)};
