/*
 * Offline test of the go-to mod (src/mod_goto.c) without the game: A* over a fake map
 * memory (around walls, never through blocked tiles, no corner cutting, no route when
 * walled in or too far) and the travel ticks (legs, arrival, stuck and replan, pause,
 * cancel). build.ps1 -SelfTest builds and runs it.
 */
#include "../src/mod_goto.c"

Engine E;
Shared S;
int map_travel;

#define W 400
static unsigned char vis[W][W], blk[W][W];
static float heroE, heroN;
static int at(int t) { return t + W / 2; }
static int inside(int tx, int ty) { return at(tx) >= 0 && at(tx) < W && at(ty) >= 0 && at(ty) < W; }
int map_tile_of(float v) { float t = v / MAP_TILE; int i = (int)t; return (float)i > t ? i - 1 : i; }
int map_tile(int layer, int tx, int ty, int *v, int *b, int *s)
{
    (void)layer;
    *s = 0;
    if (!inside(tx, ty)) { *v = *b = 0; return 0; }
    *v = vis[at(tx)][at(ty)]; *b = blk[at(tx)][at(ty)];
    return *v || *b;
}
void map_block(int layer, int tx, int ty) { (void)layer; if (inside(tx, ty) && blk[at(tx)][at(ty)] < 255) blk[at(tx)][at(ty)]++; }
int map_hero(int *l, float *e, float *n) { *l = 0; *e = heroE; *n = heroN; return 1; }
int map_radar_xy(const DrawApi *d, float e, float n, float *x, float *y) { (void)d; (void)e; (void)n; (void)x; (void)y; return 0; }
int map_radar_ground(const DrawApi *d, float x, float y, float *e, float *n, int *l) { (void)d; *e = x; *n = y; *l = 0; return 1; }
void logmsg(const char *fmt, ...) { (void)fmt; }
float mk_sqrt(float v) { float x = v > 1.0f ? v : 1.0f; for (int i = 0; i < 40; i++) x = 0.5f * (x + v / x); return x; }
static int orders; static float lastE, lastN;
int mk_order_move_offset(unsigned r, void *h, float e, float n) { (void)r; (void)h; orders++; lastE = e; lastN = n; return 1; }

static int fails;
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); fails++; } } while (0)

static int route_ok(int n, int sx, int sy, int gx, int gy)
{
    if (n < 1 || pathX[0] != sx || pathY[0] != sy || pathX[n - 1] != gx || pathY[n - 1] != gy) return 0;
    for (int i = 0; i < n; i++) {
        if (i + 1 < n && (abs(pathX[i + 1] - pathX[i]) > 1 || abs(pathY[i + 1] - pathY[i]) > 1)) return 0;
        if (i > 0 && i < n - 1 && blk[at(pathX[i])][at(pathY[i])] >= 2) return 0;
    }
    return 1;
}

int main(void)
{
    int n;
    /* open ground: straight line */
    n = plan(0, 0, 0, 20, 0);
    CHECK(n == 21 && route_ok(n, 0, 0, 20, 0));

    /* a wall at x = 10 from y = -30 to 30: goes round it, never through */
    for (int y = -30; y <= 30; y++) blk[at(10)][at(y)] = 2;
    n = plan(0, 0, 0, 20, 0);
    CHECK(n > 21 && route_ok(n, 0, 0, 20, 0));
    {
        int maxY = 0;
        for (int i = 0; i < n; i++) if (abs(pathY[i]) > maxY) maxY = abs(pathY[i]);
        CHECK(maxY >= 31);
    }

    /* walked tiles are preferred over unknown ones */
    memset(blk, 0, sizeof blk);
    for (int x = -2; x <= 22; x++) vis[at(x)][at(5)] = 3;    /* a walked road 5 tiles north */
    for (int x = 3; x <= 17; x++) for (int y = -4; y <= 4; y++) vis[at(x)][at(y)] = 0;
    n = plan(0, 0, 0, 20, 0);
    CHECK(route_ok(n, 0, 0, 20, 0));
    {
        int onRoad = 0;
        for (int i = 0; i < n; i++) if (pathY[i] == 5) onRoad++;
        CHECK(onRoad >= 10);
    }
    memset(vis, 0, sizeof vis);

    /* no corner cutting between two diagonal blocks */
    blk[at(1)][at(0)] = 2; blk[at(0)][at(1)] = 2;
    n = plan(0, 0, 0, 1, 1);
    CHECK(n >= 4 && route_ok(n, 0, 0, 1, 1));
    memset(blk, 0, sizeof blk);

    /* walled in: no route; too far: no route */
    for (int i = -3; i <= 3; i++) blk[at(i)][at(3)] = blk[at(i)][at(-3)] = blk[at(3)][at(i)] = blk[at(-3)][at(i)] = 2;
    CHECK(plan(0, 0, 0, 20, 20) == 0);
    memset(blk, 0, sizeof blk);
    CHECK(plan(0, 0, 0, 600, 0) == 0);

    /* travel: click 30 m east, legs of 12 m, arrival */
    {
        Ctx c;
        unsigned char hero[0x400];
        memset(&c, 0, sizeof c);
        c.hero = hero; c.dt = 0.1f; c.heroRef = 1;
        set_goal(30.2f * MAP_TILE, 0.5f * MAP_TILE, 0);
        tick(&c);
        CHECK(G.active && map_travel && orders == 1);
        CHECK(lastE > 11.0f * MAP_TILE && lastE < 13.5f * MAP_TILE);
        heroE += lastE; heroN += lastN;                      /* the hero walks the leg */
        tick(&c); tick(&c);
        CHECK(orders == 2 && lastE > 11.0f * MAP_TILE);
        heroE += lastE; heroN += lastN;
        tick(&c); tick(&c);
        CHECK(orders == 3);
        heroE += lastE; heroN += lastN;
        tick(&c);
        CHECK(!G.active && !map_travel && G.arrived == 1);

        /* pause during a fight: no orders, route kept */
        heroE = heroN = 0;
        set_goal(20.5f * MAP_TILE, 0.5f * MAP_TILE, 0);
        S.threat = 1;
        orders = 0;
        tick(&c);
        CHECK(G.active && orders == 0 && G.np > 0);
        S.threat = 0;
        tick(&c);
        CHECK(orders == 1);

        /* stuck: the hero never moves -> blocks ahead and plans again, then gives up */
        C.maxReplans = 2;
        for (int i = 0; i < 400 && G.active; i++) tick(&c);
        CHECK(!G.active && G.failed);
        CHECK(blk[at(1)][at(0)] >= 1 || blk[at(2)][at(0)] >= 1);

        /* the player takes over: cancelled */
        memset(blk, 0, sizeof blk);
        set_goal(10.5f * MAP_TILE, 0.5f * MAP_TILE, 0);
        tick(&c);
        CHECK(G.active);
        reset();
        CHECK(!G.active && !map_travel);

        /* right click cancels */
        set_goal(10.5f * MAP_TILE, 0.5f * MAP_TILE, 0);
        tick(&c);
        G.pendCancel = 1;
        tick(&c);
        CHECK(!G.active);
    }

    printf("gototest: %s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
