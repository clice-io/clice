#pragma once

#define bail(...) std::unexpected(std::format(__VA_ARGS__))
