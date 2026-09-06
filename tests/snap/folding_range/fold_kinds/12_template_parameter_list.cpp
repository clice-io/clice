/// # Template parameter list folding
///
/// - status: unsupported
///
/// Multiline template parameter lists do not form folding ranges yet

template<typename T>
struct Less;

template<
    typename Key,                 // ┐
    typename Value,               // │ foldable
    typename Compare = Less<Key>  // ┘
>
class SortedMap { };
