#define ALPHA_WIDE 1
#define ALPHA_TMP 1
#include <beta/beta.h>
#include <alpha/local.h>
#include <alpha/anchor.h>
#include <alpha/limits.h>
#include <fakecstdio>
#undef ALPHA_TMP
int main() {
    fassert(1);
    return beta::wrap(ALPHA_VERSION).v + alpha_local() + fake_stdout + FAKE_EOF +
           fake_puts("x");
}
