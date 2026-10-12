namespace §(demo_def)demo {
namespace §(inner_def)inner {
int value = 1;
}
}

namespace demo::inner::§(more)more {}

namespace §(alias_def)alias = §(demo_use)demo::§(inner_use)inner;

int use_alias = §(alias_use)alias::§(value)value;
