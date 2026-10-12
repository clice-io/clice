// Deprecation in its other forms: without a message, on a type, an
// enumerator and a variable, in the GNU spelling, and added by a later
// redeclaration, which every use sees.

namespace deprecation_forms {

struct [[deprecated]] LegacyConfig {};

enum class Mode { fast, safe [[deprecated("use fast")]] };

__attribute__((deprecated("use limit_v2"))) extern int limit_v1;

void reset();
[[deprecated("use reset_all")]] void reset();

void use() {
    §(type)LegacyConfig config;
    Mode mode = Mode::§(enumerator)safe;
    int limit = §(gnu_spelling)limit_v1;
    §(redeclared)reset();
}

}
