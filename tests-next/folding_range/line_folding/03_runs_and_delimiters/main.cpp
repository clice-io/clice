// run of
// line comments
#include "a.h"
#include "b.h"
#include "c.h"
namespace lib {
int a, b;
}
using lib::a;
using lib::b;
/* block
   comment
*/
const char* text = R"(
raw
)";
template <typename T,
          typename U,
          typename V>
struct S {};
// a run ending the file
// without a final newline