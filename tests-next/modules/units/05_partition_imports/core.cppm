// A partition importing another module: the primary interface re-exporting
// the partition builds it, and the module it imports, first.
export module App:Core;
import Ext;

export int core_fn() {
    return §(imported)ext_val() + 1;
}
