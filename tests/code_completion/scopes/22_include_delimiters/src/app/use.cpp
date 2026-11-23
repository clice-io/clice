#include "local.h"
#include "stamp/detail/impl.h"
#include "stamp/version.h"
#include "standard.h"
#include "widgets/button.h"
#include <acme/detail/core.h>
#include <acme/format.h>
#include <config.h>
#include <tardy/late.h>

int used = local_count() + build_version() + button_width() + config_level() + acme_format() +
           acme_core() + stamp_detail() + tardy_value();
