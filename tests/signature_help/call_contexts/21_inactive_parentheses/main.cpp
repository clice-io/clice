// A parenthesis an inactive branch writes in a call's arguments closes
// nothing.

int pick(int first, int second);

int chosen = pick(1,
#if 0
                  2)
#else
                  §(active)3);
#endif
