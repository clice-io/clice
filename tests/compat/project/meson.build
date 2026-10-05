project('compat', 'c', 'cpp')

add_project_arguments('-DCOMPAT_DEFINE=1', language: ['c', 'cpp'])
executable('compat', 'src/main.cpp', 'src/util.c', include_directories: 'include')
