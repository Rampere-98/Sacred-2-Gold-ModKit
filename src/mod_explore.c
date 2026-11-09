/*
 * Mod: exploration (the agent walks where it has been least).
 *
 * Port of the Sacred Gold agent's exploration on top of the map memory
 * (mod_map.c): each step looks at 16 directions at 6, 10 and 14 m and walks to the
 * cheapest spot, with the Sacred Gold cost
 *     log2(1 + visits of the 3x3 around it) + 1.5 x blocks, +2 per unwalkable tile
 * plus a little for blocks on the way, for turning away from the current heading
 * and a small random part. Walking is the game's own move order (cEventMoveTo to
 * an offset), so the game finds the path. When the hero stops getting closer, the
 * spot ahead and the destination are marked blocked in the map memory, as in
 * Sacred Gold; a line through a tile blocked twice is never taken.
 *
 * Off by default. Only with the automation on (Insert) and no fight: no target,
 * nobody closing in, not hurt for 3 s, nobody hostile within SafeRadius, no other
 * mod walking the hero (loot, retreat), enough health.
 */
#include "modkit.h"
#include "mod_map.h"
#include <stdio.h>

#define NDIR 16
static const float DIRS[NDIR][2] = {   /* unit vectors (east, north), 22.5 degrees apart */
    { 1.0f, 0.0f }, { 0.9239f, 0.3827f }, { 0.7071f, 0.7071f }, { 0.3827f, 0.9239f },
    { 0.0f, 1.0f }, { -0.3827f, 0.9239f }, { -0.7071f, 0.7071f }, { -0.9239f, 0.3827f },
    { -1.0f, 0.0f }, { -0.9239f, -0.3827f }, { -0.7071f, -0.7071f }, { -0.3827f, -0.9239f },
    { 0.0f, -1.0f }, { 0.3827f, -0.9239f }, { 0.7071f, -0.7071f }, { 0.9239f, -0.3827f },
};
static const int RINGS[] = { 6, 10, 14 };   /* metres */
#define NRINGS (int)(sizeof RINGS / sizeof RINGS[0])
#define ARRIVE   1.5f   /* metres */
#define NO_GAIN  2.0f   /* seconds without getting 0.5 m closer = stuck */

static struct {
    int enabled, leash;
    float safeRadius, minHealth, turn, pause;
} C = { 0, 0, 600.0f, 50.0f, 1.0f, 0.2f };

static const Setting settings[] = {
    { "Enabled", "Explorar", "Con la automatizacion (Insert) y sin combate, el heroe camina hacia lo que menos ha pisado. Necesita la memoria del mapa.", ST_BOOL, &C.enabled, 0, 1 },
    { "SafeRadius", "Radio sin enemigos", "No da un paso si hay un enemigo vivo dentro de este radio (42.5 = 1 m).", ST_FLOAT, &C.safeRadius, 0, 1500 },
    { "MinHealth", "Vida minima (%)", "Por debajo de este % de vida espera a recuperarse antes de seguir.", ST_PERCENT, &C.minHealth, 0, 95 },
    { "Leash", "Radio maximo (m)", "No se aleja mas de estos metros de donde empezo a explorar (0 = sin limite).", ST_INT, &C.leash, 0, 2000 },
    { "Turn", "Preferir seguir recto", "Cuanto cuesta cambiar de direccion (0 = nada, 3 = casi siempre recto).", ST_FLOAT, &C.turn, 0, 3 },
    { "Pause", "Pausa entre pasos (s)", "Espera entre un paso y el siguiente.", ST_FLOAT, &C.pause, 0, 3 },
};

static struct {
    int moving, layer, anchored, steps, stucks;
    float te, tn;          /* destination (ground units) */
    float de, dn;          /* direction of the current step */
    float he, hn;          /* heading of the last step */
    float ae, an;          /* where exploring started (leash) */
    float bestD, noGain, timer, cooldown;
    const char *state;
} X;

static float lg2(float x)   /* log2 for x >= 1, piecewise linear between powers of two */
{
    int e = 0;
    while (x >= 2.0f) { x *= 0.5f; e++; }
    return (float)e + (x - 1.0f);
}

/* Sacred Gold exploration cost of a destination tile. */
static float spot_cost(int layer, int tx, int ty)
{
    int area = 0, pen = 0, v, b, s;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            map_tile(layer, tx + dx, ty + dy, &v, &b, &s);
            area += v;
            if (s == 2) pen += 2;
        }
    map_tile(layer, tx, ty, &v, &b, &s);
    return lg2(1.0f + (float)area) + 1.5f * (float)b + (float)pen;
}

/* Cheapest reachable-looking spot around (e, n); returns 0 when every way is blocked. */
static int choose(int layer, float e, float n, float *oe, float *on, float *ode, float *odn, float *odist)
{
    float best = 1e9f;
    int found = 0;
    for (int r = 0; r < NRINGS; r++)
        for (int k = 0; k < NDIR; k++) {
            float dist = (float)RINGS[r], ue = DIRS[k][0], un = DIRS[k][1];
            float te = e + ue * dist * MAP_TILE, tn = n + un * dist * MAP_TILE, line = 0.0f, cost;
            int ok = 1, v, b, s;
            for (int m = 1; m <= RINGS[r]; m++) {
                map_tile(layer, map_tile_of(e + ue * (float)m * MAP_TILE), map_tile_of(n + un * (float)m * MAP_TILE),
                         &v, &b, &s);
                if (b >= 2 || s == 2) { ok = 0; break; }
                line += 0.75f * (float)b;
            }
            if (!ok) continue;
            if (C.leash > 0 && X.anchored) {
                float le = (te - X.ae) / MAP_TILE, ln = (tn - X.an) / MAP_TILE;
                if (le * le + ln * ln > (float)C.leash * (float)C.leash) continue;
            }
            cost = spot_cost(layer, map_tile_of(te), map_tile_of(tn)) + line +
                   C.turn * 0.5f * (1.0f - (ue * X.he + un * X.hn)) - 0.03f * dist + rnd_range(0.0f, 0.25f);
            if (cost < best) {
                best = cost; found = 1;
                *oe = te; *on = tn; *ode = ue; *odn = un; *odist = dist;
            }
        }
    return found;
}

static void stop_walking(const char *why) { X.moving = 0; X.state = why; }

static void reset(void) { stop_walking("Explorar: en pausa"); }

/* The hero stopped getting closer: the way ahead and the destination are blocked. */
static void stuck(float e, float n)
{
    map_block(X.layer, map_tile_of(e + X.de * 1.5f * MAP_TILE), map_tile_of(n + X.dn * 1.5f * MAP_TILE));
    map_block(X.layer, map_tile_of(X.te), map_tile_of(X.tn));
    X.stucks++;
    logmsg("explore: stuck heading (%.2f, %.2f), marked blocked", X.de, X.dn);
    stop_walking("Explorar: bloqueado, cambio de rumbo");
    X.cooldown = 0.3f;
}

static void tick(const Ctx *c)
{
    int layer, danger;
    float e, n;
    if (!C.enabled) { X.anchored = 0; stop_walking("Explorar: desactivado"); return; }
    if (!S.autoOk) { stop_walking("Explorar: en pausa"); return; }
    if (!map_hero(&layer, &e, &n)) { stop_walking("Explorar: esperando a la memoria del mapa"); return; }
    if (!X.anchored) { X.ae = e; X.an = n; X.anchored = 1; }

    danger = S.target || S.threat || S.busy || S.castLock > 0.0f || S.sinceHurt < 3.0f ||
             (C.safeRadius > 0.0f && mk_hostiles_within(c->hero, C.safeRadius) > 0);
    if (danger) {
        stop_walking("Explorar: esperando (combate u otra tarea)");
        X.cooldown = rnd_range(0.5f, 1.0f);
        return;
    }
    if (c->maxHp > 0.0f && c->hp < c->maxHp * C.minHealth / 100.0f) { stop_walking("Explorar: recuperando vida"); return; }

    if (X.moving) {
        float dE = (X.te - e) / MAP_TILE, dN = (X.tn - n) / MAP_TILE, d = mk_sqrt(dE * dE + dN * dN);
        X.timer -= c->dt;
        if (layer != X.layer) { stop_walking("Explorar: cambio de capa"); return; }
        if (d < ARRIVE) {
            X.steps++;
            stop_walking("Explorar: paso hecho");
            X.cooldown = C.pause + rnd_range(0.0f, 0.15f);
            return;
        }
        if (d < X.bestD - 0.5f) { X.bestD = d; X.noGain = 0.0f; }
        else X.noGain += c->dt;
        if (X.noGain > NO_GAIN || X.timer <= 0.0f) stuck(e, n);
        return;
    }

    X.cooldown -= c->dt;
    if (X.cooldown > 0.0f) return;
    {
        float te, tn, ue, un, dist;
        if (!choose(layer, e, n, &te, &tn, &ue, &un, &dist)) {
            X.he = -X.he; X.hn = -X.hn;        /* boxed in: try the other way next time */
            stop_walking("Explorar: sin salida");
            X.cooldown = 2.0f;
            return;
        }
        if (!mk_order_move_offset(c->heroRef, c->hero, te - e, tn - n)) { X.cooldown = 1.0f; return; }
        X.moving = 1; X.layer = layer; X.te = te; X.tn = tn; X.de = ue; X.dn = un;
        X.he = ue; X.hn = un;
        X.bestD = dist; X.noGain = 0.0f; X.timer = 3.0f + dist * 0.6f;
        X.state = "Explorar: caminando";
    }
}

static void overlay(const DrawApi *d)
{
    float x, y, s;
    unsigned col = MK_RGBA(90, 230, 230, 240);
    if (!C.enabled || !X.moving || !map_radar_xy(d, X.te, X.tn, &x, &y)) return;
    s = 3.5f * d->scale;
    d->line(x - s, y - s, x + s, y + s, col, 1.5f * d->scale);
    d->line(x - s, y + s, x + s, y - s, col, 1.5f * d->scale);
}

static int info(int line, char *buf, int n)
{
    if (line != 0) return 0;
    snprintf(buf, (size_t)n, "Pasos: %d, bloqueos marcados: %d. En el radar, la X turquesa es el destino.", X.steps, X.stucks);
    return 1;
}

static const char *status(void)
{
    if (!C.enabled) return "Explorar: desactivado";
    return X.state ? X.state : "Explorar: listo";
}

static void dump(const Ctx *c)
{
    (void)c;
    logmsg("explore: moving=%d layer=%d dest=(%.0f, %.0f) heading=(%.2f, %.2f) steps=%d stuck=%d %s",
           X.moving, X.layer, X.te, X.tn, X.he, X.hn, X.steps, X.stucks, status());
}

MK_MODULE(mod_explore) = {
    "Exploracion", "Explore", settings, sizeof settings / sizeof settings[0], 60,
    0, 0, 0, tick, reset, dump, status, 0, 0, info, overlay,
};
