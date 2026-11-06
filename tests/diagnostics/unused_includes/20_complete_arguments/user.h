#pragma once
#include "holder.h"
struct Widget;
struct User {
    Holder<Widget> widget;
};
