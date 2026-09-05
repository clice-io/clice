/// # Template parameter list folding
///
/// - status: unsupported
///

template<typename T>
struct Less;

template<
    typename Key,                 // ┐
    typename Value,               // │ foldable
    typename Compare = Less<Key>  // ┘
>
class SortedMap { };
