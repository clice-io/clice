// A partition with a global module fragment: its macro sets what it
// exports.
module;
#include "config.h"
export module Cfg:Limits;
export constexpr int max_size = §(macro)MAX_SIZE;
