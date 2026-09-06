namespace structure_use {
namespace model {
struct Coordinate {};
}
model::§(01_struct)Coordinate* current;
}

namespace class_use {
namespace model {
class Controller {};
}
model::§(02_class)Controller* current;
}

namespace union_use {
namespace model {
union Storage {
    long integer;
    double decimal;
};
}
model::§(03_union)Storage current;
}

namespace forward_use {
class Session;
class Session {};
§(04_forward_class)Session* active();
}

namespace enum_use {
enum Direction { North, East, South, West };
§(05_enum)Direction current = North;
}

namespace enumerator_use {
enum Priority { Low, Normal, High };
Priority current = §(06_enumerator)High;
}

namespace imported_enumerator {
enum class Mode { Read, Write };
using enum Mode;
Mode current = §(07_using_enum)Write;
}

namespace anonymous_enumerator {
enum { Pending = 4, Complete = 8 };
int state = §(08_anon_enumerator)Complete;
}

namespace alias_use {
typedef long Identifier;
§(09_typedef)Identifier next;
}

namespace embedded_alias_use {
typedef struct Payload {
    long size;
} Packet;
§(10_typedef_embedded)Packet queued;
}

namespace namespace_use {
namespace protocol {
struct Client {
    static void connect();
};
}
void start() {
    §(11_namespace)protocol::Client::connect();
}
}

namespace anonymous_structure {
static struct {
    long total;
} metrics;
void increment() {
    metrics.§(12_anon_struct_field)total++;
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
    result.data.§(13_anon_union_field)count++;
}
}

namespace function_template_use {
template <typename T> T default_value() {
    return {};
}
long value = §(14_templated_function)default_value<long>();
}

namespace instantiated_method_use {
template <typename T> struct Container {
    long size();
};
auto size = Container<int>{}.§(15_template_method)size();
}

namespace nested_instance_value {
template <typename T> struct Wrapper {};
Wrapper<Wrapper<Wrapper<long>>> val§(16_nested_template_var)ue;
}

namespace nested_instance_type {
template <typename T> struct Wrapper {};
§(17_nested_template_class)Wrapper<Wrapper<Wrapper<long>>> value;
}
