#pragma once

#define CROWD(n) int crowd_##n(void);
#define CROWD_10(n)                                                                                \
    CROWD(n##0) CROWD(n##1) CROWD(n##2) CROWD(n##3) CROWD(n##4)                                    \
    CROWD(n##5) CROWD(n##6) CROWD(n##7) CROWD(n##8) CROWD(n##9)

CROWD_10(0)
CROWD_10(1)
CROWD_10(2)
CROWD_10(3)
CROWD_10(4)
CROWD_10(5)
CROWD_10(6)
CROWD_10(7)
CROWD_10(8)
CROWD_10(9)
CROWD_10(10)
CROWD_10(11)
