// A name pulled in with `using` completes unqualified.
namespace lib {

int helper_fn(int x);

}

using lib::helper_fn;

void bar() {
    int v = help§(pos);
}
