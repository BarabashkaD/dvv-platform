// A tiny run-time library for the DynamicLibrary tests.
#if defined(_WIN32)
#define DVV_TEST_EXPORT __declspec(dllexport)
#else
#define DVV_TEST_EXPORT __attribute__((visibility("default")))
#endif

extern "C" DVV_TEST_EXPORT int dvvTestValue() {
    return 42;
}
