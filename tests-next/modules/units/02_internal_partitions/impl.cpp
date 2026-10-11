// Internal partitions import each other and are imported by the module's
// implementation unit, which imports the primary interface implicitly.
module Lib;
import :util;

int total() {
    return §(primary)api() + §(imported)twice();
}
