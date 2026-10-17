// An include guard defining its macro to a value is one too: the module
// interface does without it.
#ifndef CORE_TEXT_H
#define CORE_TEXT_H 1

namespace core {

struct Text {
    int size() const;
};

}  // namespace core

#endif
