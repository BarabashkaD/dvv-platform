// A run-time library that needs test_dependency at load time.
#if defined(_WIN32)
#define DVV_TEST_EXPORT __declspec(dllexport)
#define DVV_TEST_IMPORT __declspec(dllimport)
#else
#define DVV_TEST_EXPORT __attribute__((visibility("default")))
#define DVV_TEST_IMPORT
#endif

extern "C" DVV_TEST_IMPORT int dvvTestDependencyValue();

extern "C" DVV_TEST_EXPORT int dvvTestDependentValue() {
    return dvvTestDependencyValue() + 2;
}
