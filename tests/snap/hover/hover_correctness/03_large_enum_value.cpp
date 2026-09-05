/// # Large unsigned enum constant
///
/// - status: supported
///
/// Hovering a `0xFFFF...ULL` enumerator does not crash
///
/// The card renders the full unsigned value without overflowing or failing.

namespace big_enum {

enum class Flags : unsigned long long {
    Ma§(max_value)x = 0xFFFFFFFFFFFFFFFFULL,
};

}
