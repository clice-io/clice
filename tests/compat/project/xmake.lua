target 'compat'
set_kind 'binary'
add_files('src/main.cpp', 'src/util.c')
add_includedirs 'include'
add_defines 'COMPAT_DEFINE=1'
