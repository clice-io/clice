#include "user.h"
#include "widget.h"
#include "unused.h"

void replace(User& user) {
    user.widget.reset(nullptr);
}
