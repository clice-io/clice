#include "local.h"
#include "stamp/version.h"
#include "widgets/button.h"
#include <acme/detail/core.h>
#include <acme/format.h>
#include <config.h>
#include <tardy/late.h>

int used = local_count() + build_version() + button_width() + config_level() + acme_format() +
           acme_core() + tardy_value();
