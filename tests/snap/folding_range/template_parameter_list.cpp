/// # Fold Kinds
///
/// ## Template parameter list folding
///
/// - status: unsupported
/// - order: 12

template<typename T>
struct Less;

template<
    typename Key,                 // ┐
    typename Value,               // │ foldable
    typename Compare = Less<Key>  // ┘
>
class SortedMap { };
