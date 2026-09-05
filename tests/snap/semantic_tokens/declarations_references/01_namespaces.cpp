/// # Namespaces
///
/// - status: supported
///
/// definitions, references, nested namespaces and namespace aliases

namespace §demo {
namespace §inner {
int value = 1;
}
}

namespace demo::inner::§more {}

namespace §alias = §demo::§inner;

int use_alias = §alias::§value;
