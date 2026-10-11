// A variable declared in a condition is a declaration like any other, with
// an init-statement or without.

int fetch();

int conditions() {
    int total = 0;
    if (int §(in_if)in_if = fetch()) {
        total += in_if;
    }
    if (int §(with_init)with_init = fetch(); with_init > 0) {
        total += with_init;
    }
    switch (int §(in_switch)in_switch = fetch()) {
    case 0:
        total += in_switch;
    }
    for (; int §(in_for)in_for = fetch();) {
        total += in_for;
    }
    while (bool §(flag)flag = fetch() > 0) {
        total += flag;
    }
    return total;
}
