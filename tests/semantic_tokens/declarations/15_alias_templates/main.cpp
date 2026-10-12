// The alias name carries the type kind and the `templated` modifier.
template <typename T>
using §(Ptr_def)Ptr = T*;

template <typename T>
struct Box {};

template <typename T>
using §(BoxPtr)BoxPtr = §(Box)Box<T>*;

§(Ptr_use)Ptr<int> pointer = nullptr;
