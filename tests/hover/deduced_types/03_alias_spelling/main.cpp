namespace sugared_auto {

using Inner = int;
using Outer = Inner;

Outer make();

void demo() {
  §(auto)auto value = make();
}

}
