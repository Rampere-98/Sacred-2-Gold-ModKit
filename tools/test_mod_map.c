/*
 * Offline test of the map memory (src/mod_map.c) without the game: tile maths,
 * chunk keys, visits, sightings and a save/load round trip.
 * Build: zig cc -target x86-windows-gnu -Isrc tools/test_mod_map.c -o build/test_mod_map.exe (build.ps1 -SelfTest runs it)
 */
#include "../src/mod_map.c"
#include <stdarg.h>
#include <assert.h>

Engine E;
Shared S;
static char dir[MAX_PATH];
void logmsg(const char *fmt, ...) { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); putchar('\n'); }
const char *mk_dir(void) { return dir; }
int mk_resolve(HMODULE m, void *var, const char *name, int required) { (void)m; (void)var; (void)name; (void)required; return 0; }
/* the live part (tick) is not exercised here: stubs only to link */
Vec3 mk_pos(void *e) { Vec3 v = { 0, 0, 0 }; (void)e; return v; }
void mk_ground(Vec3 v, float *e, float *n) { *e = v.x; *n = v.z; }
void *mk_entity(unsigned r) { (void)r; return NULL; }
int mk_type_is(Fn_TypeIs fn, void *e) { (void)fn; (void)e; return 0; }
int mk_alive(void *e) { (void)e; return 0; }
float mk_dist2d(void *a, void *b) { (void)a; (void)b; return 0; }

static int fails;
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); fails++; } } while (0)

int main(void)
{
    char p[MAX_PATH];
    GetTempPathA(sizeof dir, dir);
    strcat(dir, "mk_maptest\\");
    CreateDirectoryA(dir, NULL);
    path(p, sizeof p, "txt");
    DeleteFileA(p);

    CHECK(tile_of(0.0f) == 0);
    CHECK(tile_of(42.4f) == 0);
    CHECK(tile_of(42.5f) == 1);
    CHECK(tile_of(-0.1f) == -1);
    CHECK(tile_of(-42.5f) == -1);
    CHECK(tile_of(-42.6f) == -2);
    CHECK(fdiv(-1, 32) == -1 && fdiv(-32, 32) == -1 && fdiv(-33, 32) == -2 && fdiv(31, 32) == 0);
    CHECK(chunk_key(0, -1, 0) != chunk_key(0, 0, 0));
    CHECK(chunk_key(0, 0, 0) == chunk_key(0, 31, 31));
    CHECK(chunk_key(1, 5, 5) != chunk_key(0, 5, 5));

    {   /* cells: same chunk shares storage, neighbours differ */
        Cell *a = cell(0, -1, -1, 1), *b = cell(0, -32, -32, 1), *c = cell(0, -33, 0, 1);
        CHECK(a && b && c && a != b);
        CHECK(nChunks == 2);
        a->visits = 3; c->blocked = 2; c->scan = 2;
        cell(2, 100, -7, 1)->visits = 255;
        cell(2, 100, -7, 1)->r = 0xAB;
    }
    add_sighting(77, 0, 10, 10);
    add_sighting(77, 0, 12, 14);        /* within 6 tiles: deduplicated */
    add_sighting(77, 0, 30, 10);
    add_sighting(78, 0, 10, 10);
    CHECK(nSights == 3);
    for (int i = 0; i < 60; i++) add_sighting(90, 0, i * 10, 0);
    CHECK(nSights == 3 + SIGHT_PER_TYPE);

    dirty = 1;
    C.maxChunks = 3000;
    save();
    CHECK(dirty == 0);
    stop();                              /* frees everything (and saves nothing new) */
    nSights = 0;
    CHECK(nChunks == 0 && cell(0, -1, -1, 0) == NULL);
    load();
    CHECK(nChunks == 3);
    CHECK(cell(0, -1, -1, 0) && cell(0, -1, -1, 0)->visits == 3);
    CHECK(cell(0, -33, 0, 0) && cell(0, -33, 0, 0)->blocked == 2 && cell(0, -33, 0, 0)->scan == 2);
    CHECK(cell(2, 100, -7, 0) && cell(2, 100, -7, 0)->visits == 255 && cell(2, 100, -7, 0)->r == 0xAB);
    CHECK(cell(0, -32, -32, 0) && !known(cell(0, -32, -32, 0)));
    CHECK(nSights == 3 + SIGHT_PER_TYPE);

    {   /* a Sacred Gold 7-column line is read with scan 0 */
        FILE *f = fopen(p, "a");
        fprintf(f, "0 500 500 4 1 102030 2\nL 0 0 1 2\nN 5 Lobo\n");
        fclose(f);
        stop(); nSights = 0;
        load();
        CHECK(cell(0, 500, 500, 0) && cell(0, 500, 500, 0)->visits == 4 && cell(0, 500, 500, 0)->scan == 0 &&
              cell(0, 500, 500, 0)->g == 0x20);
    }

    {   /* memory cap */
        stop(); nSights = 0; full = 0;
        C.maxChunks = 200;
        for (int i = 0; i < 300; i++) cell(0, i * 32, 0, 1);
        CHECK(nChunks == 200 && full);
    }
    DeleteFileA(p);
    printf(fails ? "maptest: %d FAILED\n" : "maptest: all passed\n", fails);
    return fails != 0;
}
