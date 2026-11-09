/*
 * Mod: map memory and radar.
 *
 * Remembers the ground the hero has walked, in the same layout as the Sacred Gold
 * ModKit (so exploration and A* code can be shared): per layer, chunks of 32x32
 * tiles, 7 bytes per tile (visits, blocked, r, g, b, samples, scan). A tile is 1 m
 * = 42.5 world units on the ground plane; the layer comes from
 * cWorldPosition::getLayer. Hostile kinds are also remembered where they were seen
 * (deduplicated per kind), for the radar and later for exploration.
 *
 * Saved to system/Plugin_ModKit-map.txt at most every 60 s and only when something
 * changed: written to a temporary file first, then renamed over the old one.
 *
 * The radar is a north-up circle drawn through the menu mod's DrawApi: walked
 * ground, trail, enemies (live and remembered), loot and objects around the hero.
 * Read-only towards the game: it only reads positions and the near list.
 */
#include "modkit.h"
#include "mod_map.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TILE        MAP_TILE   /* world units per tile (1 m) */
#define CHUNK_BITS  5
#define CHUNK_SIDE  (1 << CHUNK_BITS)
#define CHUNK_CELLS (CHUNK_SIDE * CHUNK_SIDE)
#define HCAP        8192       /* hash slots (power of two), > MaxChunks / 0.7 */
#define MAX_SIGHT   6000
#define SIGHT_PER_TYPE 48
#define SIGHT_DEDUP 6          /* tiles */
#define TRAIL       128
#define MAX_DOTS    96

static struct {
    int enabled, maxChunks, radar, radarRadius, radarRange, radarSightings;
    float radarX, radarY;
} C = { 1, 3000, 1, 110, 40, 1, 91.0f, 42.0f };

static const Setting settings[] = {
    { "Enabled", "Recordar el mapa", "Guarda por donde has pasado (casillas de 1 m) y donde viste cada tipo de enemigo.", ST_BOOL, &C.enabled, 0, 1 },
    { "MaxChunks", "Bloques en memoria", "Bloques de 32x32 m que se recuerdan (unos 7 KB cada uno). Al llenarse deja de anadir zonas nuevas.", ST_INT, &C.maxChunks, 200, 5000 },
    { "Radar", "Radar", "Radar redondo con el norte arriba: suelo pisado, rastro, enemigos, botin y objetos. Necesita el menu.", ST_BOOL, &C.radar, 0, 1 },
    { "RadarRadius", "Tamano del radar (px)", "Radio del radar en pixeles a 1080p (se escala con la resolucion).", ST_INT, &C.radarRadius, 50, 300 },
    { "RadarRange", "Alcance del radar (m)", "Metros desde el heroe hasta el borde del radar.", ST_INT, &C.radarRange, 10, 120 },
    { "RadarX", "Radar: posicion X (%)", "Centro del radar en % del ancho de la pantalla.", ST_FLOAT, &C.radarX, 0.0f, 100.0f },
    { "RadarY", "Radar: posicion Y (%)", "Centro del radar en % del alto de la pantalla.", ST_FLOAT, &C.radarY, 0.0f, 100.0f },
    { "RadarSightings", "Radar: enemigos recordados", "Marca en el radar donde se vieron enemigos antes, aunque ya no esten.", ST_BOOL, &C.radarSightings, 0, 1 },
};

/* ------------------------------------------------------------------ storage */
typedef struct { unsigned char visits, blocked, r, g, b, samples, scan; } Cell;   /* 7 bytes, as in Sacred Gold */
typedef struct { unsigned long long key; Cell c[CHUNK_CELLS]; } Chunk;
typedef struct { unsigned type; int layer, tx, ty; } Sight;

static Chunk *H[HCAP];
static int nChunks, full, dirty;
static Sight sights[MAX_SIGHT];
static int nSights;
static float saveIn = 60.0f;

static char (__thiscall *getLayer)(const void *wpos);

static int fdiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
static int tile_of(float v) { float t = v / TILE; int i = (int)t; return (float)i > t ? i - 1 : i; }

static unsigned long long chunk_key(int layer, int tx, int ty)
{
    int cx = fdiv(tx, CHUNK_SIDE), cy = fdiv(ty, CHUNK_SIDE);
    return ((unsigned long long)(unsigned char)layer << 48) | ((unsigned long long)(unsigned short)cx << 16) |
           (unsigned short)cy;
}

static unsigned hslot(unsigned long long k)
{
    k ^= k >> 33; k *= 0xff51afd7ed558ccdULL; k ^= k >> 33;
    return (unsigned)k & (HCAP - 1);
}

static Chunk *chunk(unsigned long long key, int create)
{
    unsigned i = hslot(key);
    for (int probe = 0; probe < HCAP; probe++, i = (i + 1) & (HCAP - 1)) {
        Chunk *ch = H[i];
        if (!ch) break;
        if (ch->key == key) return ch;
    }
    if (!create) return NULL;
    if (nChunks >= C.maxChunks || nChunks >= HCAP * 7 / 10) {
        if (!full) logmsg("map: memory full (%d chunks), new ground is not remembered", nChunks);
        full = 1;
        return NULL;
    }
    {
        Chunk *ch = (Chunk *)calloc(1, sizeof(Chunk));
        if (!ch) return NULL;
        ch->key = key;
        H[i] = ch;           /* published after it is initialised: the radar may read from the render thread */
        nChunks++;
        return ch;
    }
}

static Cell *cell(int layer, int tx, int ty, int create)
{
    Chunk *ch = chunk(chunk_key(layer, tx, ty), create);
    return ch ? &ch->c[((ty & (CHUNK_SIDE - 1)) << CHUNK_BITS) | (tx & (CHUNK_SIDE - 1))] : NULL;
}

static int known(const Cell *c) { return c->visits | c->blocked | c->samples | c->scan | c->r | c->g | c->b; }

/* ------------------------------------------------------------------ file */
static void path(char *p, size_t n, const char *ext) { snprintf(p, n, "%sPlugin_ModKit-map.%s", mk_dir(), ext); }

static void save(void)
{
    char tmp[MAX_PATH], fin[MAX_PATH];
    FILE *f;
    int cells = 0;
    if (!dirty) return;
    path(tmp, sizeof tmp, "tmp");
    path(fin, sizeof fin, "txt");
    f = fopen(tmp, "w");
    if (!f) { logmsg("map: cannot write %s", tmp); return; }
    for (int s = 0; s < HCAP; s++) {
        const Chunk *ch = H[s];
        int layer, cx, cy;
        if (!ch) continue;
        layer = (signed char)(ch->key >> 48);
        cx = (short)(ch->key >> 16);
        cy = (short)ch->key;
        for (int i = 0; i < CHUNK_CELLS; i++) {
            const Cell *c = &ch->c[i];
            if (!known(c)) continue;
            fprintf(f, "%d %d %d %u %u %02X%02X%02X %u %u\n", layer, cx * CHUNK_SIDE + (i & (CHUNK_SIDE - 1)),
                    cy * CHUNK_SIDE + (i >> CHUNK_BITS), c->visits, c->blocked, c->r, c->g, c->b, c->samples, c->scan);
            cells++;
        }
    }
    for (int i = 0; i < nSights; i++)
        fprintf(f, "E %u %d %d %d\n", sights[i].type, sights[i].layer, sights[i].tx, sights[i].ty);
    if (fclose(f) != 0 || !MoveFileExA(tmp, fin, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        logmsg("map: save failed (%lu)", GetLastError());
        return;
    }
    dirty = 0;
    logmsg("map: saved %d tiles in %d chunks, %d sightings", cells, nChunks, nSights);
}

static void load(void)
{
    char p[MAX_PATH], line[160];
    FILE *f;
    int cells = 0;
    path(p, sizeof p, "txt");
    f = fopen(p, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        int layer, tx, ty;
        unsigned v, b, rgb, sm, sc = 0, type;
        if (line[0] == 'E') {
            if (nSights < MAX_SIGHT && sscanf(line + 1, "%u %d %d %d", &type, &layer, &tx, &ty) == 4) {
                Sight *s = &sights[nSights++];
                s->type = type; s->layer = layer; s->tx = tx; s->ty = ty;
            }
            continue;
        }
        if (line[0] == 'L' || line[0] == 'N' || line[0] == '#') continue;   /* landmarks / names: not produced yet */
        if (sscanf(line, "%d %d %d %u %u %x %u %u", &layer, &tx, &ty, &v, &b, &rgb, &sm, &sc) >= 7) {
            Cell *c = cell(layer, tx, ty, 1);
            if (!c) break;
            c->visits = (unsigned char)(v > 255 ? 255 : v);
            c->blocked = (unsigned char)(b > 255 ? 255 : b);
            c->r = (unsigned char)(rgb >> 16); c->g = (unsigned char)(rgb >> 8); c->b = (unsigned char)rgb;
            c->samples = (unsigned char)(sm > 255 ? 255 : sm);
            c->scan = (unsigned char)(sc > 2 ? 0 : sc);
            cells++;
        }
    }
    fclose(f);
    logmsg("map: %d tiles in %d chunks, %d sightings loaded", cells, nChunks, nSights);
}

/* ------------------------------------------------------------------ live state */
static struct { int valid, layer, tx, ty; float e, n; } hero;
static struct { int tx, ty, layer; } trail[TRAIL];
static int trailHead, trailN;
typedef enum { DOT_ENEMY, DOT_FRIEND, DOT_ITEM, DOT_OBJECT } DotKind;
typedef struct { float e, n; DotKind kind; } Dot;
static Dot dots[MAX_DOTS];
static int nDots;
static unsigned seenRefs[256];
static int seenPos;

static int seen_before(unsigned ref)
{
    for (int i = 0; i < 256; i++) if (seenRefs[i] == ref) return 1;
    seenRefs[seenPos] = ref;
    seenPos = (seenPos + 1) & 255;
    return 0;
}

static void add_sighting(unsigned type, int layer, int tx, int ty)
{
    int perType = 0;
    for (int i = 0; i < nSights; i++) {
        const Sight *s = &sights[i];
        if (s->type != type) continue;
        perType++;
        if (s->layer == layer && abs(s->tx - tx) <= SIGHT_DEDUP && abs(s->ty - ty) <= SIGHT_DEDUP) return;
    }
    if (perType >= SIGHT_PER_TYPE || nSights >= MAX_SIGHT) return;
    sights[nSights].type = type; sights[nSights].layer = layer; sights[nSights].tx = tx; sights[nSights].ty = ty;
    nSights++;
    dirty = 1;
}

static int resolve(void)
{
    mk_resolve(E.core, &getLayer, "?getLayer@cWorldPosition@@QBEDXZ", 0);
    return 1;
}

static void init(void) { load(); }

static void tick(const Ctx *c)
{
    static int haveLast, lastLayer, lastTx, lastTy;
    static Dot buf[MAX_DOTS];
    float e, n;
    int layer, tx, ty, nb = 0;
    void *wpos;
    if (!C.enabled || S.upAxis < 0) { hero.valid = 0; return; }  /* tiles lie on the ground plane: wait for the vertical axis */
    wpos = E.getPosition(c->hero);
    if (!wpos) { hero.valid = 0; return; }
    mk_ground(mk_pos(c->hero), &e, &n);
    layer = getLayer ? getLayer(wpos) : 0;
    tx = tile_of(e); ty = tile_of(n);

    if (!haveLast || tx != lastTx || ty != lastTy || layer != lastLayer) {   /* stepped on a new tile */
        Cell *cl = cell(layer, tx, ty, 1);
        if (cl && cl->visits < 255) { cl->visits++; dirty = 1; }
        trail[trailHead].tx = tx; trail[trailHead].ty = ty; trail[trailHead].layer = layer;
        trailHead = (trailHead + 1) % TRAIL;
        if (trailN < TRAIL) trailN++;
        haveLast = 1; lastLayer = layer; lastTx = tx; lastTy = ty;
    }

    /* what is around: dots for the radar, sightings of hostile kinds for the memory */
    for (unsigned i = 0; i < 256; i++) {
        const EntityNear *en = E.getNearListElement(c->hero, i, NEAR_HOSTILE | NEAR_ALIVE);
        void *ent;
        float de, dn;
        DotKind k;
        if (!en) break;
        if (en->ref == c->heroRef || en->dist > (float)C.radarRange * TILE * 1.1f) continue;
        ent = mk_entity(en->ref);
        if (!ent) continue;
        if (mk_type_is(E.isCreature, ent)) {
            if (en->notAlive || !mk_alive(ent)) continue;
            k = en->hostile ? DOT_ENEMY : DOT_FRIEND;
        } else if (mk_type_is(E.isItem, ent)) {
            if (mk_dist2d(c->hero, ent) < 1.0f) continue;          /* carried by the hero */
            k = DOT_ITEM;
        } else if (mk_type_is(E.isClickable, ent)) {
            k = DOT_OBJECT;
        } else continue;
        mk_ground(mk_pos(ent), &de, &dn);
        if (nb < MAX_DOTS) { buf[nb].e = de; buf[nb].n = dn; buf[nb].kind = k; nb++; }
        if (k == DOT_ENEMY && !seen_before(en->ref)) add_sighting(E.getType(ent), layer, tile_of(de), tile_of(dn));
    }
    /* publish for the radar (it may draw from the render thread) */
    hero.layer = layer; hero.tx = tx; hero.ty = ty; hero.e = e; hero.n = n; hero.valid = 1;
    nDots = 0;
    memcpy(dots, buf, sizeof(buf[0]) * (size_t)nb);
    nDots = nb;

    saveIn -= c->dt;
    if (saveIn <= 0.0f) { save(); saveIn = 60.0f; }
}

/* ------------------------------------------------------------------ radar */
static void overlay(const DrawApi *d)
{
    float R, cx, cy, ppt, range, hx, hy;
    int rng, step;
    Chunk *lastCh = NULL;
    unsigned long long lastKey = ~0ULL;
    if (!C.radar || !C.enabled || !hero.valid) return;
    R = (float)C.radarRadius * d->scale;
    cx = d->w * C.radarX / 100.0f;
    cy = d->h * C.radarY / 100.0f;
    rng = C.radarRange;
    range = (float)rng;
    ppt = R / range;                                  /* pixels per tile */
    hx = hero.e / TILE; hy = hero.n / TILE;
    step = ppt >= 2.0f ? 1 : (int)(2.0f / ppt + 0.999f);

    d->circle(cx, cy, R, MK_RGBA(12, 12, 14, 150), 1);

    /* walked ground */
    for (int dy = -rng; dy <= rng; dy += step)
        for (int dx = -rng; dx <= rng; dx += step) {
            int tx = hero.tx + dx, ty = hero.ty + dy;
            unsigned long long key;
            const Cell *c;
            float x, y;
            unsigned col;
            if (dx * dx + dy * dy > rng * rng) continue;
            key = chunk_key(hero.layer, tx, ty);
            if (key != lastKey) { lastCh = chunk(key, 0); lastKey = key; }
            if (!lastCh) continue;
            c = &lastCh->c[((ty & (CHUNK_SIDE - 1)) << CHUNK_BITS) | (tx & (CHUNK_SIDE - 1))];
            if (!known(c)) continue;
            if (c->scan == 2 || c->blocked >= 2) col = MK_RGBA(170, 50, 40, 170);
            else {
                unsigned a = 70 + (c->visits > 20 ? 20 : c->visits) * 6;
                col = MK_RGBA(120, 150, 95, a);
            }
            x = cx + ((float)tx - hx) * ppt;          /* west edge of the tile */
            y = cy - ((float)ty - hy) * ppt;          /* south edge */
            d->rect(x, y - ppt * (float)step, x + ppt * (float)step, y, col, 1);
        }

    /* trail, newest brightest */
    for (int i = 1; i < trailN; i++) {
        int a = (trailHead - i + TRAIL) % TRAIL, b = (trailHead - i - 1 + TRAIL) % TRAIL;
        float ax, ay, bx, by;
        if (trail[a].layer != hero.layer || trail[b].layer != hero.layer) continue;
        ax = (float)trail[a].tx + 0.5f - hx, ay = (float)trail[a].ty + 0.5f - hy;
        bx = (float)trail[b].tx + 0.5f - hx, by = (float)trail[b].ty + 0.5f - hy;
        if (ax * ax + ay * ay > range * range || bx * bx + by * by > range * range) continue;
        if ((ax - bx) * (ax - bx) + (ay - by) * (ay - by) > 9.0f) continue;   /* teleport: no line */
        d->line(cx + ax * ppt, cy - ay * ppt, cx + bx * ppt, cy - by * ppt,
                MK_RGBA(120, 190, 255, 220 - i * 200 / TRAIL), 1.5f * d->scale);
    }

    /* remembered enemies */
    if (C.radarSightings)
        for (int i = 0; i < nSights; i++) {
            const Sight *s = &sights[i];
            float x = (float)s->tx + 0.5f - hx, y = (float)s->ty + 0.5f - hy;
            if (s->layer != hero.layer || x * x + y * y > range * range) continue;
            d->circle(cx + x * ppt, cy - y * ppt, 2.0f * d->scale, MK_RGBA(200, 70, 60, 110), 0);
        }

    /* what is around now */
    for (int i = 0; i < nDots; i++) {
        float x = (dots[i].e - hero.e) / TILE, y = (dots[i].n - hero.n) / TILE;
        static const unsigned col[] = { MK_RGBA(255, 60, 50, 255), MK_RGBA(190, 190, 190, 220),
                                        MK_RGBA(255, 215, 60, 255), MK_RGBA(80, 200, 255, 230) };
        if (x * x + y * y > range * range) continue;
        d->circle(cx + x * ppt, cy - y * ppt, (dots[i].kind == DOT_ENEMY ? 3.5f : 2.5f) * d->scale, col[dots[i].kind], 1);
    }

    d->circle(cx, cy, 3.5f * d->scale, MK_RGBA(255, 255, 255, 255), 1);
    d->circle(cx, cy, R, MK_RGBA(200, 160, 90, 200), 0);
    d->text(cx - 4.0f * d->scale, cy - R - 2.0f - 17.0f * d->scale, MK_RGBA(230, 200, 140, 255), "N");
}

/* ------------------------------------------------------------------ API for other mods (mod_map.h) */
int map_hero(int *layer, float *east, float *north)
{
    if (!hero.valid) return 0;
    *layer = hero.layer; *east = hero.e; *north = hero.n;
    return 1;
}

int map_tile(int layer, int tx, int ty, int *visits, int *blocked, int *scan)
{
    const Cell *c = cell(layer, tx, ty, 0);
    *visits = c ? c->visits : 0; *blocked = c ? c->blocked : 0; *scan = c ? c->scan : 0;
    return c && known(c);
}

void map_block(int layer, int tx, int ty)
{
    Cell *c = cell(layer, tx, ty, 1);
    if (c && c->blocked < 255) { c->blocked++; dirty = 1; }
}

int map_tile_of(float v) { return tile_of(v); }

int map_radar_xy(const DrawApi *d, float east, float north, float *x, float *y)
{
    float R, ppt, dx, dy;
    if (!C.radar || !C.enabled || !hero.valid) return 0;
    R = (float)C.radarRadius * d->scale;
    ppt = R / (float)C.radarRange;
    dx = (east - hero.e) / TILE; dy = (north - hero.n) / TILE;
    if (dx * dx + dy * dy > (float)(C.radarRange * C.radarRange)) return 0;
    *x = d->w * C.radarX / 100.0f + dx * ppt;
    *y = d->h * C.radarY / 100.0f - dy * ppt;
    return 1;
}

/* ------------------------------------------------------------------ menu / log */
static int info(int line, char *buf, int n)
{
    switch (line) {
    case 0:
        snprintf(buf, (size_t)n, "Memoria: %d bloques (%d KB)%s, %d avistamientos", nChunks,
                 nChunks * (int)sizeof(Chunk) / 1024, full ? " LLENA" : "", nSights);
        return 1;
    case 1:
        if (hero.valid) snprintf(buf, (size_t)n, "Heroe en la casilla (%d, %d), capa %d", hero.tx, hero.ty, hero.layer);
        else snprintf(buf, (size_t)n, "Esperando a saber el eje vertical del mundo");
        return 1;
    case 2:
        snprintf(buf, (size_t)n, "Radar: rojo enemigo, gris amigo, amarillo botin, azul objeto, verde suelo pisado");
        return 1;
    }
    return 0;
}

static const char *status(void)
{
    static char b[64];
    if (!C.enabled) return "Mapa: desactivado";
    snprintf(b, sizeof b, "Mapa: %d bloques%s", nChunks, full ? " (lleno)" : "");
    return b;
}

static void dump(const Ctx *c)
{
    char line[160];
    (void)c;
    for (int i = 0; info(i, line, sizeof line); i++) logmsg("map: %s", line);
    logmsg("map: getLayer %s, %d dots around", getLayer ? "available" : "missing (layer 0)", nDots);
}

static void stop(void)
{
    save();
    for (int i = 0; i < HCAP; i++) { free(H[i]); H[i] = NULL; }
    nChunks = 0;
}

MK_MODULE(mod_map) = {
    "Memoria del mapa y radar", "Map", settings, sizeof settings / sizeof settings[0], 50,
    init, resolve, 0, tick, 0, dump, status, stop, 0, info, overlay,
};
