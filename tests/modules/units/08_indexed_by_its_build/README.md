# A module unit compiled once

A module unit is compiled once, into the module file its importers read, and that compile indexes it too

The background index takes the index the build of the module file wrote for a compile of its own, and a file importing the module completes the names the module exports from it, as it does the names of its precompiled preamble.
