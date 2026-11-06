#include <memory>

struct Widget {
    Widget(int width, int height);
};

class Socket {
public:
    template <typename... Args>
    static Socket* open(Args&&... args) {
        return new Socket(std::forward<Args>(args)...);
    }

private:
    Socket(int port);
};

template <typename T, typename... Args>
T* make(Args&&... args) {
    return new T(static_cast<Args&&>(args)...);
}

template <typename T, typename... Args>
std::unique_ptr<T> wrap(Args&&... args) {
    return std::make_unique<T>(std::forward<Args>(args)...);
}

template <typename T, typename... Args>
T build(Args&&... args) {
    T value{std::forward<Args>(args)...};
    return value;
}

template <typename T>
class Slot {
public:
    template <typename... Args>
    T& emplace(Args&&... args) {
        return *new (storage) T(std::forward<Args>(args)...);
    }

private:
    alignas(T) unsigned char storage[sizeof(T)];
};

template <typename... Ts>
struct Saved {
    Saved(Ts... values);
};

template <typename... Args>
void record(Args&&... args) {
    Saved<Args...> saved(std::forward<Args>(args)...);
}

Widget* widget = make<Widget>(§(forwarded)800, 600);
Widget* second = make<Widget>(800, §(second_argument)600);
Socket* socket = Socket::open(§(private_constructor)80);
std::unique_ptr<Widget> owned = wrap<Widget>(§(through_make_unique)800, 600);
Widget built = build<Widget>(§(braced_variable)800, 600);

void fill(Slot<Widget>& slot) {
    slot.emplace(§(class_template_member)800, 600);
    record(§(own_types)800, 600);
}
