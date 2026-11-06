#pragma once
import lib;
template <class C, class P> int remove_bad(C& c, P p) { return lib::erase_if(c, p); }
