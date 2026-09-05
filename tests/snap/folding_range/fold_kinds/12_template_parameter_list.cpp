/// # Template parameter list folding
///
/// - status: unsupported
///
/// Multiline template parameter lists form folding ranges

template<typename T>
struct Less;

template<
    typename Key,                 // ┐
    typename Value,               // │ foldable
    typename Compare = Less<Key>  // ┘
>
class SortedMap { };
