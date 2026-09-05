/// # Namespaces
///
/// - status: supported
///
/// Namespace definitions, references, nesting and aliases receive namespace
/// tokens

namespace §demo {
namespace §inner {
int value = 1;
}
}

namespace demo::inner::§more {}

namespace §alias = §demo::§inner;

int use_alias = §alias::§value;
