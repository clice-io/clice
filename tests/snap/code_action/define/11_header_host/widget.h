#pragma once

namespace ns {
class §(cls)Widget {
public:
    Widget(int id);
    int §(id)id() const;
    template <class T>
    void visit(T& visitor);
    inline void touch() {}
    void done();
private:
    int id_;
};
}
