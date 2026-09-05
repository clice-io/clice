/// # Types
///
/// - status: supported
///
/// Type definitions and references keep their respective type kinds

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
