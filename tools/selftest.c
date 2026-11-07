/* Offline self-test: loads Plugin_ModKit.dll in a 32-bit process (no game needed),
 * checks the plugin exports and prints the registered modules and their settings. */
#include <windows.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    char buf[2048] = { 0 };
    HMODULE m;
    int (*selftest)(char *, int);
    if (argc < 2) { fprintf(stderr, "usage: selftest Plugin_ModKit.dll\n"); return 2; }
    m = LoadLibraryA(argv[1]);
    if (!m) { fprintf(stderr, "LoadLibrary failed: %lu\n", GetLastError()); return 3; }
    if (!GetProcAddress(m, "dllStartPlugin") || !GetProcAddress(m, "dllStopPlugin")) {
        fprintf(stderr, "plugin entry points missing\n");
        return 4;
    }
    *(FARPROC *)&selftest = GetProcAddress(m, "mk_selftest");
    if (!selftest) { fprintf(stderr, "mk_selftest missing\n"); return 5; }
    printf("modules: %d -> %s\n", selftest(buf, sizeof buf), buf);
    FreeLibrary(m);
    return 0;
}
