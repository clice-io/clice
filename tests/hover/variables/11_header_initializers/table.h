#pragma once

#define DUPLICATE_FOUR(x) x, x, x, x
#define DUPLICATE_64(x) DUPLICATE_FOUR(DUPLICATE_FOUR(DUPLICATE_FOUR(x)))
const unsigned char table[] = {DUPLICATE_64(1), DUPLICATE_64(2)};
