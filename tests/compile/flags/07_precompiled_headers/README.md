# Precompiled headers

The units of a target with precompiled headers share its PCH, built by clice from the build's command

CMake gives every unit of such a target the precompiled header it builds from `cmake_pch.hxx.cxx`. The file on disk may come from another compiler, so clice builds its own from the same command, twice: once for the files open, skipping the function bodies as their own preambles do, and once for the index, keeping them. Editing one of the target's headers rebuilds both and reaches every unit.
