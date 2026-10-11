// Forward declarations of core's Logger, which f.h names, and util's
// Clock, which it never does.
#pragma once

struct Logger;
struct Clock;

int feature_hook(Logger* logger);
