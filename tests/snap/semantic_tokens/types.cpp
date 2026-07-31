/// # Declarations & References
///
/// ## Types — class, struct, union, enum and type aliases, at definitions and references
///
/// - status: supported
/// - order: 2

class §Widget {};
struct §Point {};
union §Storage {
    int i;
    float f;
};
enum §Flags { FlagA };
enum class §Mode { Fast };

typedef §Point §PointAlias;
using §WidgetAlias = §Widget;

§Widget* make_widget();
§PointAlias origin;
§Mode current = §Mode::Fast;
