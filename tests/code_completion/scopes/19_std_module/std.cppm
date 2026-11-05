// The project builds the standard library's module, as libc++'s std.cppm
// does: its headers' names, exported.
module;
#include <deque>
export module std;

export namespace std {
using std::deque;
}
