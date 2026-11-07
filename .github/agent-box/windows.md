This box builds nothing: build the Windows programs on a linux box
(pixi run build RelWithDebInfo -- --platforms=@xclang//platforms:x86_64-w64-mingw32,
aarch64-w64-mingw32 for windows-arm), bring them over with
box cp <linux> <this> build/RelWithDebInfo/bin/bin build/RelWithDebInfo/bin/lib build/RelWithDebInfo/bin/clice.gsym --exclude '*.runfiles'
and run the suites with the test-run environment, as CI does:
pixi run -e test-run unit-test RelWithDebInfo.
