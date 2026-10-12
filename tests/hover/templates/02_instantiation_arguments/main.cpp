namespace instantiation_args {

template <typename T> struct Wrapper {
  T value;
};

template <typename T> T identity(T x) {
  return x;
}

void demo() {
  §(type_use)Wrapper<int> holder;
  int r = §(call)identity(42);
}

}
