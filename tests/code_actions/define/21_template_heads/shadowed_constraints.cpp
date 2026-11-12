template <class>
concept C = true;

template <class C, ::C U>
struct Box {
    void §(concept_named_like_parameter)f();
};

struct G {};

template <class T, class U>
concept Related = true;

template <class G, Related<::G> T>
struct Link {
    void §(concept_argument)f();
};

namespace app {
struct C;

template <::C T>
struct Shape {
    void §(concept_hidden_in_namespace)f();
};
}
