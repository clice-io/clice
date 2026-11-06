#pragma once
#if defined(USE_VULKAN)
inline const char* backend() { return "vk"; }
#elif defined(USE_METAL)
inline const char* backend() { return "mt"; }
#endif
