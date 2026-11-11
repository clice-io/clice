// A space after `module` or `import` lists the modules; a colon lists the
// partitions of the unit's own module, only its interface partitions in an
// interface, and none after a module name or outside a named module
// (plain.cpp).
module §(declaration)lib;
import :§(colon);
import §(space);
import lib:§(qualified);
