module;

#include "config.h"
#include "platform.h"

export module app;

export import :core;
import :detail;
import :util;

export int run();
