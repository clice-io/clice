struct Holder {
    template <typename T>
    friend class Seq;
    friend class Box;
};

template <typename T>
class Seq {};

class Box {};

void use(§(template_use)Seq<int>& seq, §(class_use)Box& box);
