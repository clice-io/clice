module;

#include "config.h"
#include "platform.h"

export module app;

export import :core;

// Internal partitions.
import :detail;
import :util;

export int run();
