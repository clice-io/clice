#define BEGIN_API namespace api {
#define END_API }
struct Handle {};
BEGIN_API
#include "decls.h"
END_API
int main() { return api::open(Handle{}); }
