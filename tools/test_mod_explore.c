/*
 * Offline test of the exploration choice (src/mod_explore.c) without the game: a fake
 * map memory, then checks that it heads for unvisited ground, never crosses a tile
 * blocked twice, respects the leash and marks blocks when stuck.
 * build.ps1 -SelfTest builds and runs it.
 */
#include "../src/mod_explore.c"
#include <string.h>

Engine E;
Shared S;
int map_travel;

/* fake map: 200x200 tiles centred on 0 */
#define W 200
static unsigned char vis[W][W], blk[W][W];
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
void map_block(int layer, int tx, int ty) { (void)layer; if (inside(tx, ty)) blk[at(tx)][at(ty)]++; }
int map_hero(int *l, float *e, float *n) { *l = 0; *e = 0; *n = 0; return 1; }
int map_radar_xy(const DrawApi *d, float e, float n, float *x, float *y) { (void)d; (void)e; (void)n; (void)x; (void)y; return 0; }
void logmsg(const char *fmt, ...) { (void)fmt; }
float rnd_range(float lo, float hi) { (void)hi; return lo; }
float mk_sqrt(float v) { float x = v > 1.0f ? v : 1.0f; for (int i = 0; i < 30; i++) x = 0.5f * (x + v / x); return x; }
int mk_hostiles_within(unsigned char *h, float r) { (void)h; (void)r; return 0; }
static int orders; static float lastE, lastN;
int mk_order_move_offset(unsigned r, void *h, float e, float n) { (void)r; (void)h; orders++; lastE = e; lastN = n; return 1; }

static int fails;
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); fails++; } } while (0)

static void fill_visits(int x0, int x1, int y0, int y1, int v)
{
    for (int x = x0; x <= x1; x++) for (int y = y0; y <= y1; y++) if (inside(x, y)) vis[at(x)][at(y)] = (unsigned char)v;
}

int main(void)
{
    float te, tn, ue, un, dist;

    /* everything walked except the east: goes east */
    fill_visits(-30, 30, -30, 30, 20);
    fill_visits(3, 30, -30, 30, 0);
    CHECK(choose(0, 0, 0, &te, &tn, &ue, &un, &dist));
    CHECK(ue > 0.9f && un > -0.4f && un < 0.4f);

    /* a wall two tiles east blocked twice: never crossed, takes the next cheapest way (north) */
    for (int y = -30; y <= 30; y++) blk[at(2)][at(y)] = 2;
    fill_visits(-30, 30, 3, 30, 0);
    fill_visits(-30, 1, 3, 30, 0);
    CHECK(choose(0, 0, 0, &te, &tn, &ue, &un, &dist));
    CHECK(ue < 0.5f && un > 0.9f);
    {   /* no candidate line goes through a tile blocked twice */
        int crossed = 0;
        for (int m = 1; m <= (int)dist; m++)
            if (blk[at(map_tile_of(ue * (float)m * MAP_TILE))][at(map_tile_of(un * (float)m * MAP_TILE))] >= 2) crossed = 1;
        CHECK(!crossed);
    }

    /* boxed in on every side: no step */
    memset(blk, 0, sizeof blk);
    for (int i = -3; i <= 3; i++) { blk[at(i)][at(3)] = blk[at(i)][at(-3)] = blk[at(3)][at(i)] = blk[at(-3)][at(i)] = 2; }
    CHECK(!choose(0, 0, 0, &te, &tn, &ue, &un, &dist));
    memset(blk, 0, sizeof blk);

    /* leash: with a 7 m leash only the 6 m ring is allowed */
    C.leash = 7; X.anchored = 1; X.ae = 0; X.an = 0;
    CHECK(choose(0, 0, 0, &te, &tn, &ue, &un, &dist));
    CHECK(dist == 6.0f);
    C.leash = 0;

    /* momentum: on even unvisited ground it keeps the heading */
    memset(vis, 0, sizeof vis);
    X.he = 0.0f; X.hn = -1.0f;
    CHECK(choose(0, 0, 0, &te, &tn, &ue, &un, &dist));
    CHECK(un < -0.9f);

    /* the tick: orders a step, then marks blocks when the hero does not get closer */
    {
        Ctx c;
        unsigned char hero[0x400];
        memset(&c, 0, sizeof c);
        memset(hero, 0, sizeof hero);
        c.hero = hero; c.dt = 0.1f; c.heroRef = 1;
        C.enabled = 1; S.autoOk = 1; S.sinceHurt = 10.0f;
        tick(&c);
        CHECK(orders == 1 && X.moving);
        CHECK(lastN < -5.0f * MAP_TILE);
        for (int i = 0; i < 30 && X.moving; i++) tick(&c);   /* hero never moves (map_hero stays at 0,0) */
        CHECK(!X.moving && X.stucks == 1);
        CHECK(blk[at(0)][at(-2)] == 1 || blk[at(0)][at(-1)] == 1);   /* the tile 1.5 m ahead */

        S.threat = 1;                                        /* a fight: no orders */
        X.cooldown = 0.0f;
        tick(&c);
        CHECK(orders == 1 && !X.moving);
        S.threat = 0;

        S.autoOk = 0;                                        /* automation off: nothing */
        X.cooldown = 0.0f;
        tick(&c);
        CHECK(orders == 1);
    }

    printf("exploretest: %s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
