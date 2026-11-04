# Precompiled headers

The units of a target with precompiled headers share one PCH, built by clice from the build's command

CMake gives every unit of such a target the precompiled header it builds from `cmake_pch.hxx.cxx`. The file on disk may come from another compiler, so clice builds its own from the same command: the target's headers are parsed once for every file open and for the index, and editing one of them rebuilds the PCH and reaches every unit.
