# Precompiled headers

The units of a target with precompiled headers share its PCH, built by clice from the build's command

CMake gives every unit of such a target the precompiled header it builds from `cmake_pch.hxx.cxx`. The file on disk may come from another compiler, so clice builds its own from the same command, once, function bodies and all, for the files open and the index alike: that build is the compile of `cmake_pch.hxx.cxx` and indexes it. Editing one of the target's headers rebuilds it and reaches every unit.
