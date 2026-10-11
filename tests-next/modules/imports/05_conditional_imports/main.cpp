// Imports under preprocessor conditions, as a platform switch writes them:
// only the module the command selects is built.
#ifdef USE_FAST
import fast;
#else
import slow;
#endif
#if 0
import missing;
#endif

int main() {
    return speed();
}
