// The card of a type, enumerator, alias or namespace at a use names what
// was used, whatever the spelling around it.

namespace structure_use {
namespace model {
struct Coordinate {};
}
model::§(struct)Coordinate* current;
}

namespace class_use {
namespace model {
class Controller {};
}
model::§(class)Controller* current;
}

namespace union_use {
namespace model {
union Storage {
    long integer;
    double decimal;
};
}
model::§(union)Storage current;
}

namespace forward_use {
class Session;
class Session {};
§(forward_class)Session* active();
}

namespace enum_use {
enum Direction { North, East, South, West };
§(enum)Direction current = North;
}

namespace enumerator_use {
enum Priority { Low, Normal, High };
Priority current = §(enumerator)High;
}

namespace imported_enumerator {
enum class Mode { Read, Write };
using enum Mode;
Mode current = §(using_enum)Write;
}

namespace anonymous_enumerator {
enum { Pending = 4, Complete = 8 };
int state = §(anon_enumerator)Complete;
}

namespace alias_use {
typedef long Identifier;
§(typedef)Identifier next;
}

namespace embedded_alias_use {
typedef struct Payload {
    long size;
} Packet;
§(typedef_embedded)Packet queued;
}

namespace namespace_use {
namespace protocol {
struct Client {
    static void connect();
};
}
void start() {
    §(namespace)protocol::Client::connect();
}
}

namespace anonymous_structure {
static struct {
    long total;
} metrics;
void increment() {
    metrics.§(anon_struct_field)total++;
}
}

namespace anonymous_union {
struct Result {
    union {
        long code;
        long count;
    } data;
};
void increment(Result& result) {
    result.data.§(anon_union_field)count++;
}
}

namespace function_template_use {
template <typename T> T default_value() {
    return {};
}
long value = §(templated_function)default_value<long>();
}

namespace instantiated_method_use {
template <typename T> struct Container {
    long size();
};
auto size = Container<int>{}.§(template_method)size();
}

namespace nested_instance_value {
template <typename T> struct Wrapper {};
Wrapper<Wrapper<Wrapper<long>>> val§(nested_template_var)ue;
}

namespace nested_instance_type {
template <typename T> struct Wrapper {};
§(nested_template_class)Wrapper<Wrapper<Wrapper<long>>> value;
}

struct Incomplete;
int storage;
auto incomplete_pointer = (Incom§(forward_struct_value)plete*)&storage;
