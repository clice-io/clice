// - diagnostics: expected

// A line starting with a name `module` is code outside a module unit: the
// directive goes to the file's start without opening a module fragment.

namespace module {
struct Registry {};
}

module::Registry registry;
using Strings = std::§(string)string;
