# `auto` parameters and return types

The `auto` of an abbreviated template's parameter reaches the type of its
only instantiation, and an `auto` return type the type returned

A parameter whose template is instantiated with several types reaches
none of them. The leading `auto` of a trailing return type reaches the
type after the arrow.
