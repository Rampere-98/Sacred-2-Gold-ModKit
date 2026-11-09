/*
 * Offline test of the encyclopedia (src/mod_learn.c) without the game: names saved and
 * loaded (UTF-8, spaces), old files without names, compass and menu lines.
 * build.ps1 -SelfTest builds and runs it.
 */
#include "../src/mod_learn.c"
#include <stdarg.h>

Engine E;
Shared S;
static char dir[MAX_PATH];
void logmsg(const char *fmt, ...) { (void)fmt; }
const char *mk_dir(void) { return dir; }
int mk_resolve(HMODULE m, void *var, const char *name, int required) { (void)m; (void)var; (void)name; (void)required; return 0; }
void *mk_entity(unsigned r) { (void)r; return NULL; }
int mk_alive(void *e) { (void)e; return 0; }
int mk_type_is(Fn_TypeIs fn, void *e) { (void)fn; (void)e; return 0; }
float mk_health(void *e, int w) { (void)e; (void)w; return 0; }
Vec3 mk_pos(void *e) { Vec3 v = { 0, 0, 0 }; (void)e; return v; }
void mk_ground(Vec3 v, float *e, float *n) { *e = v.x; *n = v.z; }
float mk_sqrt(float v) { float x = v > 1.0f ? v : 1.0f; for (int i = 0; i < 40; i++) x = 0.5f * (x + v / x); return x; }

static int fails;
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); fails++; } } while (0)

int main(void)
{
    char p[MAX_PATH], line[200];
    FILE *f;
    GetTempPathA(sizeof dir, dir);
    strcat(dir, "mk_learntest\\");
    CreateDirectoryA(dir, NULL);
    path(p, sizeof p);

    /* an old file (no names) plus a name line */
    f = fopen(p, "w");
    fprintf(f, "# header\n7 30.0 10.0 2 5 400 425 -850 300\n9 0 0 0 1 100 0 0 300\n");
    fprintf(f, "N 7 Orco guerrero \xc3\xa1\xc3\xb1\r\n");
    fclose(f);
    load();
    CHECK(nK == 2);
    CHECK(strcmp(kind(7, 0)->name, "Orco guerrero \xc3\xa1\xc3\xb1") == 0);
    CHECK(kind(9, 0)->name[0] == 0);

    /* round trip */
    snprintf(kind(9, 0)->name, NAME_LEN, "Lobo de las nieves");
    dirty = 1;
    save();
    nK = 0; heroMax = 0;
    load();
    CHECK(nK == 2 && strcmp(kind(9, 0)->name, "Lobo de las nieves") == 0);
    CHECK(strcmp(kind(7, 0)->name, "Orco guerrero \xc3\xa1\xc3\xb1") == 0);
    CHECK(kind(7, 0)->kills == 2 && kind(7, 0)->seen == 5);

    /* compass */
    CHECK(!strcmp(compass(10, 0), "E") && !strcmp(compass(0, 10), "N") && !strcmp(compass(-10, 0), "O"));
    CHECK(!strcmp(compass(0, -10), "S") && !strcmp(compass(10, 10), "NE") && !strcmp(compass(-10, -10), "SO"));
    CHECK(!strcmp(compass(10, 3), "E") && !strcmp(compass(-3, 10), "N"));

    /* menu lines: name, most dangerous first, distance and direction from the hero */
    haveHero = 1; heroE = 0; heroN = 0;
    CHECK(info(0, line, sizeof line));
    CHECK(info(1, line, sizeof line) && strstr(line, "Orco guerrero") && strstr(line, "%/s"));
    CHECK(strstr(line, "a 22 m al SE") != NULL);
    CHECK(info(2, line, sizeof line) && strstr(line, "Lobo de las nieves") && strstr(line, "sin aprender"));
    CHECK(!info(3, line, sizeof line));

    DeleteFileA(p);
    printf("learntest: %s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
