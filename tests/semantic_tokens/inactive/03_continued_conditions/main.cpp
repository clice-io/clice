#define READY 1
#define STEADY 0

#if READY && \
    STEADY
int disabled = 0;
#endif

#if 0 /* a comment
         spanning lines */
int also_disabled = 1;
#endif

int after = 2;
