/*
 * Mod: go to a point (click on the big map).
 *
 * With the menu open the map memory shows a big map; a left click on it sets a
 * destination, a right click cancels. The route is an A* over the map memory, as in
 * the Sacred Gold agent: a walked tile costs 10, an unknown one 22, a tile blocked
 * once 15 more; tiles marked unwalkable or blocked twice are never used, and
 * diagonals do not cut corners. The hero walks the route in legs of a few metres
 * with the game's own move order (cEventMoveTo to an offset), so the game finds
 * the path inside each leg. When the hero stops getting closer the way ahead is
 * marked blocked and the route is planned again.
 *
 * It pauses during fights, while another mod walks the hero (loot, retreat) and
 * while the menu is open; any click or W/S of the player cancels it.
 */
#include "modkit.h"
#include "mod_map.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXSIDE  512      /* A* window side in tiles */
#define MAXPATH  8192
#define ARRIVE   1.5f     /* metres */
#define NO_GAIN  2.5f     /* seconds without getting 0.5 m closer = stuck */

static struct {
    int enabled, leg, maxReplans;
} C = { 1, 12, 8 };

static const Setting settings[] = {
    { "Enabled", "Ir a un punto", "Con el menu abierto, clic izquierdo en el mapa grande = el heroe va alli; clic derecho = cancelar.", ST_BOOL, &C.enabled, 0, 1 },
    { "Leg", "Tramo (m)", "Metros de cada tramo del camino; el juego busca el paso dentro de cada tramo.", ST_INT, &C.leg, 4, 30 },
    { "MaxReplans", "Replanificaciones", "Veces que rehace el camino al quedarse atascado antes de rendirse.", ST_INT, &C.maxReplans, 1, 30 },
};

static short pathX[MAXPATH], pathY[MAXPATH];

static struct {
    int active, layer, np, idx, moving, replans, arrived, failed;
    float ge, gn;                 /* goal (ground units) */
    float le, ln, de, dn;         /* current leg target and its direction */
    float bestD, noGain, timer;
    volatile int pend, pendCancel, pendLayer;
    volatile float pe, pn;
    const char *state;
} G;

/* ------------------------------------------------------------------ A* */
static int tile_cost(int layer, int tx, int ty)   /* 0 = never */
{
    int v, b, s;
    map_tile(layer, tx, ty, &v, &b, &s);
    if (b >= 2 || s == 2) return 0;
    return (v > 0 || s == 1 ? 10 : 22) + (b == 1 ? 15 : 0);
}

static unsigned octile(int dx, int dy)
{
    dx = abs(dx); dy = abs(dy);
    return (unsigned)(dx > dy ? 10 * dx + 4 * dy : 10 * dy + 4 * dx);
}

typedef struct { unsigned f; int i; } HeapItem;

static void heap_push(HeapItem *h, int *n, unsigned f, int i)
{
    int k = (*n)++;
    while (k > 0) {
        int p = (k - 1) / 2;
        if (h[p].f <= f) break;
        h[k] = h[p]; k = p;
    }
    h[k].f = f; h[k].i = i;
}

static HeapItem heap_pop(HeapItem *h, int *n)
{
    HeapItem top = h[0], last = h[--(*n)];
    int k = 0;
    for (;;) {
        int c = 2 * k + 1;
        if (c >= *n) break;
        if (c + 1 < *n && h[c + 1].f < h[c].f) c++;
        if (h[c].f >= last.f) break;
        h[k] = h[c]; k = c;
    }
    if (*n > 0) h[k] = last;
    return top;
}

/* Route inside a window of `margin` tiles around start and goal into pathX/pathY; tiles, 0 if none. */
static int plan_window(int layer, int sx, int sy, int gx, int gy, int margin)
{
    static const int DX[8] = { 1, -1, 0, 0, 1, 1, -1, -1 }, DY[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };
    int x0 = (sx < gx ? sx : gx) - margin, y0 = (sy < gy ? sy : gy) - margin;
    int w = abs(gx - sx) + 2 * margin + 1, h = abs(gy - sy) + 2 * margin + 1, nodes, cap, nh = 0, n = 0, found = 0;
    unsigned *g;
    unsigned char *from;          /* 0 = unseen, 1..8 = direction index + 1, bit 7 = closed */
    HeapItem *heap;
    if (w > MAXSIDE || h > MAXSIDE) return 0;
    nodes = w * h;
    cap = nodes * 2 + 16;
    g = (unsigned *)malloc(sizeof(unsigned) * (size_t)nodes);
    from = (unsigned char *)calloc((size_t)nodes, 1);
    heap = (HeapItem *)malloc(sizeof(HeapItem) * (size_t)cap);
    if (!g || !from || !heap) { free(g); free(from); free(heap); return 0; }
    memset(g, 0xff, sizeof(unsigned) * (size_t)nodes);
    {
        int s = (sy - y0) * w + (sx - x0);
        g[s] = 0;
        from[s] = 9;              /* start: own marker */
        heap_push(heap, &nh, octile(gx - sx, gy - sy), s);
    }
    while (nh > 0) {
        HeapItem it = heap_pop(heap, &nh);
        int cx = it.i % w, cy = it.i / w;
        if (from[it.i] & 0x80) continue;
        from[it.i] |= 0x80;
        if (cx + x0 == gx && cy + y0 == gy) { found = 1; break; }
        for (int k = 0; k < 8; k++) {
            int nx = cx + DX[k], ny = cy + DY[k], ni, cost;
            unsigned ng;
            if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
            ni = ny * w + nx;
            if (from[ni] & 0x80) continue;
            cost = (nx + x0 == gx && ny + y0 == gy) ? 10 : tile_cost(layer, nx + x0, ny + y0);
            if (!cost) continue;
            if (k >= 4) {             /* diagonal: both sides must be walkable */
                if (!tile_cost(layer, cx + x0 + DX[k], cy + y0) || !tile_cost(layer, cx + x0, cy + y0 + DY[k])) continue;
                cost = cost * 14 / 10;
            }
            ng = g[it.i] + (unsigned)cost;
            if (ng >= g[ni]) continue;
            g[ni] = ng;
            from[ni] = (unsigned char)(k + 1);
            if (nh < cap) heap_push(heap, &nh, ng + octile(gx - (nx + x0), gy - (ny + y0)), ni);
        }
    }
    if (found) {                  /* walk back from the goal, then reverse */
        int i = (gy - y0) * w + (gx - x0);
        while (n < MAXPATH) {
            int d = from[i] & 0x7f;
            pathX[n] = (short)(i % w + x0); pathY[n] = (short)(i / w + y0); n++;
            if (d == 9) break;
            i -= DY[d - 1] * w + DX[d - 1];
        }
        if (n >= MAXPATH && (from[i] & 0x7f) != 9) n = 0;   /* too long */
        for (int a = 0, b = n - 1; a < b; a++, b--) {
            short t = pathX[a]; pathX[a] = pathX[b]; pathX[b] = t;
            t = pathY[a]; pathY[a] = pathY[b]; pathY[b] = t;
        }
    }
    free(g); free(from); free(heap);
    return n;
}

/* A small window first (fast); if walls close it, a wider one to go round them. */
static int plan(int layer, int sx, int sy, int gx, int gy)
{
    int n = plan_window(layer, sx, sy, gx, gy, 24);
    return n ? n : plan_window(layer, sx, sy, gx, gy, 96);
}

/* ------------------------------------------------------------------ travel */
static float centre(int t) { return ((float)t + 0.5f) * MAP_TILE; }

static void finish(const char *why, int failed)
{
    G.active = G.moving = 0;
    G.np = 0;
    G.failed = failed;
    G.state = why;
    map_travel = 0;
    logmsg("goto: %s", why);
}

static int replan(int layer, float e, float n)
{
    int np;
    G.np = 0;                     /* the radar stops drawing the old route */
    np = plan(layer, map_tile_of(e), map_tile_of(n), map_tile_of(G.ge), map_tile_of(G.gn));
    if (!np) return 0;
    G.idx = 0;
    G.np = np;
    logmsg("goto: route of %d tiles", np);
    return 1;
}

/* Called from the radar (render thread): handed over to the tick. */
static void set_goal(float e, float n, int layer) { G.pe = e; G.pn = n; G.pendLayer = layer; G.pend = 1; }

static void reset(void) { if (G.active) finish("Ir a un punto: cancelado (tomaste el control)", 0); }

/* The path node closest to the hero ahead of the current one, so detours by loot or fights do not walk back. */
static void resync(int tx, int ty)
{
    int best = G.idx, bd = 1 << 30;
    int hi = G.idx + 60 < G.np ? G.idx + 60 : G.np;
    for (int i = G.idx > 5 ? G.idx - 5 : 0; i < hi; i++) {
        int dx = pathX[i] - tx, dy = pathY[i] - ty, d = dx * dx + dy * dy;
        if (d <= bd) { bd = d; best = i; }
    }
    G.idx = best;
}

static void tick(const Ctx *c)
{
    int layer;
    float e, n;
    if (G.pendCancel) { G.pendCancel = 0; G.pend = 0; if (G.active) finish("Ir a un punto: cancelado", 0); }
    if (!C.enabled) { if (G.active) finish("Ir a un punto: desactivado", 0); G.pend = 0; return; }
    if (!map_hero(&layer, &e, &n)) return;
    if (G.pend) {
        G.pend = 0;
        if (G.pendLayer == layer) {
            G.ge = G.pe; G.gn = G.pn; G.layer = layer;
            G.active = 1; G.moving = 0; G.replans = 0; G.arrived = 0; G.failed = 0;
            map_travel = 1;
            logmsg("goto: to (%.0f, %.0f) m from here", (G.ge - e) / MAP_TILE, (G.gn - n) / MAP_TILE);
            if (!replan(layer, e, n)) { finish("Ir a un punto: no hay camino conocido (o esta demasiado lejos)", 1); return; }
            G.state = "Ir a un punto: en camino";
        }
    }
    if (!G.active) return;
    if (layer != G.layer) { finish("Ir a un punto: cambio de capa", 1); return; }

    if (S.menuOpen || S.target || S.threat || S.busy || S.castLock > 0.0f) {   /* pause, keep the route */
        G.moving = 0;
        G.state = S.menuOpen ? "Ir a un punto: cierra el menu para salir" : "Ir a un punto: esperando (combate u otra tarea)";
        return;
    }

    {
        float dge = (G.ge - e) / MAP_TILE, dgn = (G.gn - n) / MAP_TILE;
        if (dge * dge + dgn * dgn < ARRIVE * ARRIVE) { G.arrived++; finish("Ir a un punto: has llegado", 0); return; }
    }

    if (G.moving) {
        float dE = (G.le - e) / MAP_TILE, dN = (G.ln - n) / MAP_TILE, d = mk_sqrt(dE * dE + dN * dN);
        G.timer -= c->dt;
        if (d < 2.0f) { G.moving = 0; return; }            /* next leg */
        if (d < G.bestD - 0.5f) { G.bestD = d; G.noGain = 0.0f; }
        else G.noGain += c->dt;
        if (G.noGain > NO_GAIN || G.timer <= 0.0f) {
            map_block(layer, map_tile_of(e + G.de * 1.5f * MAP_TILE), map_tile_of(n + G.dn * 1.5f * MAP_TILE));
            G.moving = 0;
            if (++G.replans > C.maxReplans) { finish("Ir a un punto: atascado, me rindo", 1); return; }
            logmsg("goto: stuck, planning again (%d)", G.replans);
            if (!replan(layer, e, n)) { finish("Ir a un punto: ya no hay camino", 1); return; }
        }
        return;
    }

    /* next leg: up to Leg tiles further along the route */
    resync(map_tile_of(e), map_tile_of(n));
    {
        int j = G.idx + C.leg < G.np - 1 ? G.idx + C.leg : G.np - 1;
        float dE, dN, d;
        if (j == G.np - 1) { G.le = G.ge; G.ln = G.gn; }     /* the last leg ends on the clicked point */
        else { G.le = centre(pathX[j]); G.ln = centre(pathY[j]); }
        dE = (G.le - e) / MAP_TILE; dN = (G.ln - n) / MAP_TILE;
        d = mk_sqrt(dE * dE + dN * dN);
        if (d < 0.01f) d = 0.01f;
        if (!mk_order_move_offset(c->heroRef, c->hero, G.le - e, G.ln - n)) return;
        G.de = dE / d; G.dn = dN / d;
        G.moving = 1; G.bestD = d; G.noGain = 0.0f; G.timer = 3.0f + d * 0.6f;
        G.state = "Ir a un punto: en camino";
    }
}

/* ------------------------------------------------------------------ radar */
static void overlay(const DrawApi *d)
{
    float gx, gy, px = 0, py = 0;
    int np = G.np, have = 0, step;
    unsigned col = MK_RGBA(120, 255, 160, 230);
    if (!C.enabled) return;
    if (d->click == 1) {
        float e, n;
        int layer;
        if (map_radar_ground(d, d->mx, d->my, &e, &n, &layer)) set_goal(e, n, layer);
    } else if (d->click == 2) G.pendCancel = 1;
    if (!G.active) return;
    step = np > 400 ? np / 400 : 1;
    for (int i = G.idx; i < np; i += step) {             /* route ahead */
        float x, y;
        if (!map_radar_xy(d, centre(pathX[i]), centre(pathY[i]), &x, &y)) { have = 0; continue; }
        if (have) d->line(px, py, x, y, MK_RGBA(120, 255, 160, 150), 1.5f * d->scale);
        px = x; py = y; have = 1;
    }
    if (map_radar_xy(d, G.ge, G.gn, &gx, &gy)) {
        d->circle(gx, gy, 5.0f * d->scale, col, 0);
        d->circle(gx, gy, 2.0f * d->scale, col, 1);
    }
}

static int info(int line, char *buf, int n)
{
    if (line != 0) return 0;
    if (G.active) snprintf(buf, (size_t)n, "Camino de %d m, tramo %d, replanificado %d veces", G.np, G.idx, G.replans);
    else snprintf(buf, (size_t)n, "Abre el menu y haz clic en el mapa grande para ir alli (clic derecho cancela)");
    return 1;
}

static const char *status(void)
{
    if (!C.enabled) return "Ir a un punto: desactivado";
    return G.state ? G.state : "Ir a un punto: sin destino";
}

static void dump(const Ctx *c)
{
    (void)c;
    logmsg("goto: active=%d layer=%d goal=(%.0f, %.0f) route=%d at=%d replans=%d %s",
           G.active, G.layer, G.ge, G.gn, G.np, G.idx, G.replans, status());
}

static void stop(void) { map_travel = 0; }

MK_MODULE(mod_goto) = {
    "Ir a un punto", "GoTo", settings, sizeof settings / sizeof settings[0], 55,
    0, 0, 0, tick, reset, dump, status, stop, 0, info, overlay,
};
