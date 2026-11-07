#ifdef __SYCL_DEVICE_ONLY__
#error "the device job"
#endif

int main() {
    return SYCL_LANGUAGE_VERSION == 202012L ? 0 : 1;
}
