namespace template_docs {
/// Holds one payload.
template <typename T> class Packet {};

/// Sends one payload.
template <typename T> void send() {}

/// Counts payloads.
template <typename T> T counter;

void inspect() {
    au§(01_auto_class_doc)to packet = Packet<long>();
    Pack§(02_class_ref_doc)et<long> other;
    se§(03_function_doc)nd<long>();
    au§(04_auto_var_tmpl_doc)to count = coun§(05_var_tmpl_ref_doc)ter<Packet<long>>;
    coun§(06_var_tmpl_assign_doc)ter<int> = 1;
}
}

namespace specialization_docs {
/// Generic storage.
template <typename T> class §(07_primary_doc)Storage {};

/// Integer storage.
template <> class §(08_full_spec_doc)Storage<int> {};

/// Pointer storage.
template <typename T> class §(09_partial_spec_doc)Storage<T*> {};

void inspect() {
    Sto§(10_primary_ref_doc)rage<char> generic;
    Sto§(11_full_spec_ref_doc)rage<int> integer;
    Sto§(12_partial_spec_ref_doc)rage<char*> pointer;
}
}
