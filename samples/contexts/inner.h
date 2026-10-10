#pragma once

// Non self-contained: uses Point from the include chain.
inline Point inner_origin() {
    return Point{0, 0};
}
