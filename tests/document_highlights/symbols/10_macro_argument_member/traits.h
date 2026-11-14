#pragma once

/// Calls a member function named Member, as fatal's FATAL_CALL_TRAITS
/// does: the member's name is a macro argument spelled in the file using
/// the macro.
#define CALL_TRAITS(Name, Member)                                                                  \
    struct Name {                                                                                  \
        struct member_function {                                                                   \
            template <class T>                                                                     \
            static auto call(T&& subject) -> decltype(static_cast<T&&>(subject).Member()) {        \
                return static_cast<T&&>(subject).Member();                                         \
            }                                                                                      \
        };                                                                                         \
    }
