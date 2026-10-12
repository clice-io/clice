namespace record_value_misleading {

struct Tag {};

constexpr int rank(Tag) {
  return 7;
}

void demo() {
  Tag t;
  int r = rank(§(record_arg)t);
}

}
