// log.h names Sink through its own forward declaration: sink.h stays
// internal, as detail.h, which only core's source includes.
#pragma once

struct Sink;

struct Logger {
    Sink* sink;
};
