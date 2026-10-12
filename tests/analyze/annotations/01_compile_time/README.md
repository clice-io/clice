# Weighing by compile time

An annotation file weighs each translation unit by what rebuilding it costs, here seconds of compile time

core/fast.h is included by app/main.cpp alone, but partitioned into modules
every source imports core, directly or through util's interface: an edit to
it rebuilds three units instead of one.
