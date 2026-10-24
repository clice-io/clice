# abseil

Modern template-heavy C++20 with its tests, in three toolchains: clang,
Ubuntu's GCC 13 (libstdc++, and the search paths, macros and defaults GCC's
driver adds) and MSVC's cl (its database read by servers started without
Visual Studio's environment). The perf jobs time a 5,000-line test
(cold and warm starts, edits, every request), multi-file sessions with a
branch switch, fresh servers on sampled files, references and rename of
names used across the library, a 300-edit soak, requests after idleness, a
long session, editing during the index, and the index in memory limits.
