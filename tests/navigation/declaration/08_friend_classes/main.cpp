struct Holder {
    template <typename T>
    friend class §(friend_template)Seq;
    friend class §(friend_class)Box;
};

template <typename T>
class Seq {};

class Box {};

void use(§(template_use)Seq<int>& seq, §(class_use)Box& box);
