// A macro definition continued over several lines.
#define DECLARE_GETTER(type, name) \
    type get_##name() const {      \
        return name##_;            \
    }

struct Gauge {
    int level_;
    DECLARE_GETTER(int, level)
};
