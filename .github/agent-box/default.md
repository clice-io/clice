Build as CI does, so its Bazel cache hits: pixi run build RelWithDebInfo
(user.bazelrc in ~/src points Bazel at the cache CI's main run saved);
the programs land in build/RelWithDebInfo/bin/bin. Windows programs
build on a linux box, also from CI's cache, with
-- --platforms=@xclang//platforms:x86_64-w64-mingw32 after the type.
