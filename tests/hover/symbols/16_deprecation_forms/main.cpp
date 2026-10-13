// Deprecation in its other forms: without a message, on a type, an
// enumerator and a variable, in the GNU spelling, added by a later
// redeclaration, which only the uses after it see, and on a class template
// named as a template argument.

namespace deprecation_forms {

struct [[deprecated]] LegacyConfig {};

enum class Mode { fast, safe [[deprecated("use fast")]] };

__attribute__((deprecated("use limit_v2"))) extern int limit_v1;

void reset();
void reset_early() { §(before_redeclaration)reset(); }
[[deprecated("use reset_all")]] void reset();

template <typename T> struct [[deprecated("use Box")]] OldBox {};
template <template <typename> class Holder> struct Uses {};

void use() {
    §(type)LegacyConfig config;
    Mode mode = Mode::§(enumerator)safe;
    int limit = §(gnu_spelling)limit_v1;
    §(redeclared)reset();
    Uses<§(template_argument)OldBox> uses;
}

}
