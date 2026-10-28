struct alignas(§(alignas)16) Vector {
    float x, y;
};

[[deprecated("use §(message)other")]] int old_api();

int counter __attribute__((§(gnu)aligned(8)));

[[using gnu: §(using)hot]] void warm();

int ignored __attribute__((unknown_name, §(second)aligned(8)));

#define UNUSED maybe_unused
[[§(macro)UNUSED]] int spare;

#pragma clang attribute push(__attribute__((cold)), apply_to = function)
void §(pragma)rarely();
#pragma clang attribute pop

[[deprecated]] void retired();
void §(inherited)retired() {}

#define CXX(x) [[x]]
CXX(nodiscard) int §(argument)make();

int §(alias)plain __attribute((unused));

int aligned_by __attribute__((aligned([] {
    [[maybe_unused]] int §(inner)eight = 8;
    return eight;
}())));
