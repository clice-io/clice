#pragma once

namespace net {

typedef unsigned long Size;

using Handle = int*;

template <class T>
using Ptr = T*;

struct Widget {
    using id_type = int;
};

}  // namespace net

namespace di = net;

inline int local() {
    using Hidden = int;
    return Hidden{};
}
