class §(Widget_def)Widget {};
struct §(Point_def)Point {};
union §(Storage)Storage {
    int i;
    float f;
};
enum §(Flags)Flags { FlagA };
enum class §(Mode_def)Mode { Fast };

typedef §(Point_use)Point §(PointAlias_def)PointAlias;
using §(WidgetAlias)WidgetAlias = §(Widget_use)Widget;

§(Widget_use2)Widget* make_widget();
§(PointAlias_use)PointAlias origin;
§(Mode_use)Mode current = §(Mode_use2)Mode::Fast;
