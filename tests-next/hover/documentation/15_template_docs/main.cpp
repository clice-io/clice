// A template's documentation reaches its uses: through auto, a class, a
// function and a variable template, each specialization its own, the
// specialization a use selects.

namespace template_docs {
/// Holds one payload.
template <typename T> class Packet {};

/// Sends one payload.
template <typename T> void send() {}

/// Counts payloads.
template <typename T> T counter;

void inspect() {
    au§(auto_class_doc)to packet = Packet<long>();
    Pack§(class_ref_doc)et<long> other;
    se§(function_doc)nd<long>();
    au§(auto_var_tmpl_doc)to count = coun§(var_tmpl_ref_doc)ter<Packet<long>>;
    coun§(var_tmpl_assign_doc)ter<int> = 1;
}
}

namespace specialization_docs {
/// Generic storage.
template <typename T> class Storage {};

/// Integer storage.
template <> class §(full_spec_doc)Storage<int> {};

/// Pointer storage.
template <typename T> class Storage<T*> {};

void inspect() {
    Storage<char> generic;
    Sto§(full_spec_ref_doc)rage<int> integer;
    Storage<char*> pointer;
}
}

namespace selected_specialization_docs {
/// Generic channel.
template <typename T> class Channel {};
/// Pointer channel.
template <typename T> class Channel<T*> {};
Chan§(primary_template_doc)nel<long*>* channel;
}

namespace selected_variable_docs {
/// Generic cache slot.
template <typename T> long cache = 0;
/// Pointer cache slot.
template <typename T> long cache<T*> = 1;
using result_type = decltype(ca§(variable_template_doc)che<long*>);
}
