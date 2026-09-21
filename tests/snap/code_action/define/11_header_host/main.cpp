/// # Define in the host source
///
/// - status: supported
/// - issues: clangd#445
/// - verify: both
/// - snap: separate
/// - indexing: true
///
/// In a header, a member can also be defined in the source file the header is compiled with
///
/// The definition is fully qualified and joins the class's other
/// definitions in that file; members already defined in some source file
/// are not offered again. Templates and inline functions stay in the header.

// snap: The inspect path has no index or host: it pins the definitions the
// snap: host would receive as the unresolved request. The server path,
// snap: with the index built, pins the edit placed into main.cpp after the
// snap: existing definition of Widget::done.
// snap: Widget::done stays missing on the inspect path, which compiles the
// snap: header alone: only the host context sees its definition.

// indexed: done

#include "widget.h"

namespace ns {
void Widget::done() {
}
}

int main() {
    ns::Widget w(1);
    return w.id();
}
