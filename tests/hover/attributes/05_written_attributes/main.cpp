#define NODISCARD [[nodiscard]]
#define DEPRECATED(message) [[deprecated(message)]]
#define PACKED __attribute__((packed))
#define HOT_PATH __attribute__((hot, noinline))
#define FORCE_INLINE static inline __attribute__((always_inline))

namespace written_attributes {
NODISCARD int §(macro_attribute)compute();

DEPRECATED("use run_v2") void §(macro_arguments)run();

struct PACKED §(macro_on_class)Header {
    char tag;
    int size;
};

HOT_PATH void §(macro_group)spin();

[[nodiscard]] __attribute__((cold)) int §(mixed_syntaxes)rarely();

void §(after_signature)fail() __attribute__((noreturn, cold));

int §(after_declarator)counter [[maybe_unused]] = 0;

int §(braced_after_declarator)braced [[maybe_unused]]{0};

template <typename T> [[nodiscard]] T §(after_template_head)make();

struct Empty {};

struct §(member_attributes)Holder {
    [[no_unique_address]] Empty empty;
    alignas(64) int counter;
    NODISCARD int size() const;
    [[deprecated]] virtual void reset() = 0;
};

[[using gnu: noinline, §(using_prefix)hot]] void tuned();

[[using gnu: used, §(using_arguments)aligned(8)]] static int §(using_declaration)aligned_value;

FORCE_INLINE int §(macro_with_specifiers)twice(int x) {
    return x * 2;
}

#pragma clang attribute push(__attribute__((cold)), apply_to = function)
void §(pragma_region)rarely_called();
#pragma clang attribute pop
}
