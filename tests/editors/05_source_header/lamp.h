#pragma once

int lamp_brightness();
void lamp_toggle();

inline bool lamp_on() {
    return lamp_brightness() > 0;
}
