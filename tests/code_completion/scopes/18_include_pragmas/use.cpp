#include "lib/api.h"
#include "lib/api/facade.h"
#include "lib/bundle.h"
#include "lib/detail/alone.h"
#include "lib/detail/middle.h"
#include "lib/umbrella.h"

int use = lib::impl_value() + lib::exported_value() + lib::relative_value() +
          lib::bundled_value() + lib::alone_value() + lib::inner_value();
