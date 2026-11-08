#define STEP(n) total += n;
#define STEPS4(n) STEP(n) STEP(n + 1) STEP(n + 2) STEP(n + 3)
#define STEPS16(n) STEPS4(n) STEPS4(n + 4) STEPS4(n + 8) STEPS4(n + 12)
#define STEPS64(n) STEPS16(n) STEPS16(n + 16) STEPS16(n + 32) STEPS16(n + 48)
#define STEPS256(n) STEPS64(n) STEPS64(n + 64) STEPS64(n + 128) STEPS64(n + 192)
#define STEPS1024(n) STEPS256(n) STEPS256(n + 256) STEPS256(n + 512) STEPS256(n + 768)
#define SUM(steps)                                                                                 \
    [] {                                                                                           \
        int total = 0;                                                                             \
        steps return total;                                                                        \
    }()

int lines = §(lines)SUM(STEPS64(0));
int bytes = §(bytes)SUM(STEPS1024(0));
