/*
 * test_plugin.c — a tiny DLL the watchdog test loads and then unloads,
 * standing in for a real plugin/addon a target app might load at runtime.
 */
__declspec(dllexport) int plugin_add(int a, int b) {
    volatile int r = a + b;
    return r;
}
