namespace implicit_conversion {

struct Wrapper {
  Wrapper(int value);
};

void take_float(float x);
void take_wrapper(Wrapper w);

void demo() {
  int n = 0;
  take_float(§(arithmetic)n);
  take_wrapper(§(user_defined)n);
}

}
