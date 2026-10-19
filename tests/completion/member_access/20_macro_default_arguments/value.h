#pragma once

#define FAIL_VALUE 7

struct RegisterValue {
    int as_int(int fail_value = FAIL_VALUE) const;
};
