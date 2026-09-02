/// # Alias templates — the alias name carries the type kind and the `templated` modifier
///
/// - status: supported

template <typename T>
using §Ptr = T*;

template <typename T>
struct Box {};

template <typename T>
using §BoxPtr = §Box<T>*;

§Ptr<int> pointer = nullptr;
