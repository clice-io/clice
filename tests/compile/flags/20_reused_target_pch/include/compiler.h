#pragma once

#ifdef BUILD_STATIC
#define ABI
#else
#define ABI __attribute__((visibility("default")))
#endif
