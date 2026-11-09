/*
 * Mod: combat learning and enemy encyclopedia.
 *
 * Learns how hard each enemy kind hits: the health the hero loses while enemies
 * attack it is shared among the attackers' types, over the seconds each spends
 * attacking (danger = share of the hero's maximum health per second). Also keeps an
 * encyclopedia of the kinds met: how many seen and killed, their maximum health and
 * where the last one was. Kept in system/Plugin_ModKit-foes.txt between sessions.
 * The auto attack goes first for the kinds learned to hit hardest (S.danger).
 * Read-only towards the game: it only watches health and the attacker list.
 */
#include "modkit.h"
#include <stdio.h>
#include <string.h>

static struct { int enabled, lines; float minDanger; } C = { 1, 12, 0.0f };

static const Setting settings[] = {
    { "Enabled", "Aprender de los combates", "Mide cuanto pega cada tipo de enemigo y lo recuerda entre partidas.", ST_BOOL, &C.enabled, 0, 1 },
    { "MenuLines", "Enemigos en el menu", "Cuantos tipos de enemigo muestra la enciclopedia del menu (los mas peligrosos primero).", ST_INT, &C.lines, 0, 40 },
    { "MinDanger", "Ocultar peligro menor de (%/s)", "La enciclopedia oculta los tipos que quitan menos de este % de vida por segundo.", ST_FLOAT, &C.minDanger, 0.0f, 20.0f },
};

#define MAX_KINDS 512
typedef struct {
    unsigned type;
    float dmg, secs, maxHp, east, north;
    int kills, seen;
} Kind;
static Kind K[MAX_KINDS];
static int nK, dirty;
static float saveIn = 60.0f, lastHp = -1.0f, heroMax;

#define MAX_REFS 256
static struct { unsigned ref; unsigned type; float age; } recent[MAX_REFS];   /* attacked the hero lately */
static int nRecent;
static unsigned seenRefs[MAX_REFS];
static int seenPos;

static Kind *kind(unsigned type, int create)
{
    for (int i = 0; i < nK; i++) if (K[i].type == type) return &K[i];
    if (!create || nK >= MAX_KINDS) return NULL;
    memset(&K[nK], 0, sizeof K[nK]);
    K[nK].type = type;
    return &K[nK++];
}

static float danger_of_kind(const Kind *k)
{
    return k && k->secs >= 3.0f && heroMax > 0.0f ? 100.0f * k->dmg / k->secs / heroMax : 0.0f; /* % per second */
}

/* For the auto attack: 0 unknown, ~0.01 = 1 % of the hero's health a second. */
static float danger(unsigned type) { return danger_of_kind(kind(type, 0)) / 100.0f; }

static void path(char *p, size_t n) { snprintf(p, n, "%sPlugin_ModKit-foes.txt", mk_dir()); }

static void save(void)
{
    char p[MAX_PATH];
    FILE *f;
    if (!dirty) return;
    path(p, sizeof p);
    f = fopen(p, "w");
    if (!f) return;
    fprintf(f, "# Sacred 2 Gold ModKit - enemy kinds learned: type dmg secs kills seen maxHp east north heroMaxHp\n");
    for (int i = 0; i < nK; i++)
        fprintf(f, "%u %.1f %.1f %d %d %.0f %.0f %.0f %.0f\n", K[i].type, K[i].dmg, K[i].secs, K[i].kills, K[i].seen,
                K[i].maxHp, K[i].east, K[i].north, heroMax);
    fclose(f);
    dirty = 0;
}

static void load(void)
{
    char p[MAX_PATH], line[256];
    FILE *f;
    path(p, sizeof p);
    f = fopen(p, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f) && nK < MAX_KINDS) {
        Kind k;
        float hm = 0;
        memset(&k, 0, sizeof k);
        if (line[0] == '#') continue;
        if (sscanf(line, "%u %f %f %d %d %f %f %f %f", &k.type, &k.dmg, &k.secs, &k.kills, &k.seen,
                   &k.maxHp, &k.east, &k.north, &hm) >= 5) {
            K[nK++] = k;
            if (hm > heroMax) heroMax = hm;
        }
    }
    fclose(f);
    logmsg("learn: %d enemy kinds loaded", nK);
}

static void init(void) { load(); S.danger = danger; }

static void remember_attacker(unsigned ref, unsigned type)
{
    for (int i = 0; i < nRecent; i++) if (recent[i].ref == ref) { recent[i].age = 0; return; }
    if (nRecent < MAX_REFS) { recent[nRecent].ref = ref; recent[nRecent].type = type; recent[nRecent].age = 0; nRecent++; }
}

static int seen_before(unsigned ref)
{
    for (int i = 0; i < MAX_REFS; i++) if (seenRefs[i] == ref) return 1;
    seenRefs[seenPos] = ref;
    seenPos = (seenPos + 1) % MAX_REFS;
    return 0;
}

static void tick(const Ctx *c)
{
    unsigned *it, *end;
    unsigned refs[16];
    int n = 0;
    float lost;
    if (!C.enabled || c->maxHp <= 0.0f) return;
    if (c->maxHp > heroMax) heroMax = c->maxHp;
    lost = lastHp >= 0.0f && c->hp < lastHp ? lastHp - c->hp : 0.0f;
    lastHp = c->hp;

    it = *(unsigned **)(c->hero + CREATURE_ATTACKERS_BEGIN);
    end = *(unsigned **)(c->hero + CREATURE_ATTACKERS_END);
    if (it && end && end >= it && (end - it) <= 2 * 16)
        for (; it < end && n < 16; it += 2) if (mk_alive(mk_entity(it[0]))) refs[n++] = it[0];

    for (int i = 0; i < n; i++) {
        void *ent = mk_entity(refs[i]);
        unsigned type = E.getType(ent);
        Kind *k = kind(type, 1);
        if (!k) continue;
        k->secs += c->dt;
        if (lost > 0.0f) k->dmg += lost / (float)n;
        remember_attacker(refs[i], type);
        dirty = 1;
    }

    /* kills: an enemy that attacked the hero lately and is now dead */
    for (int i = 0; i < nRecent; i++) {
        void *ent = mk_entity(recent[i].ref);
        recent[i].age += c->dt;
        if (ent && !mk_alive(ent)) {
            Kind *k = kind(recent[i].type, 1);
            if (k) { k->kills++; dirty = 1; }
            recent[i] = recent[--nRecent]; i--;
        } else if (!ent || recent[i].age > 30.0f) {
            recent[i] = recent[--nRecent]; i--;
        }
    }

    /* encyclopedia: hostile kinds met, with the place of the last one */
    for (unsigned i = 0; i < 256; i++) {
        const EntityNear *e = E.getNearListElement(c->hero, i, NEAR_HOSTILE | NEAR_ALIVE);
        void *ent;
        Kind *k;
        if (!e) break;
        if (!e->hostile || e->notAlive || e->dist > 700.0f || seen_before(e->ref)) continue;
        ent = mk_entity(e->ref);
        if (!ent || !mk_type_is(E.isCreature, ent)) continue;
        k = kind(E.getType(ent), 1);
        if (!k) continue;
        k->seen++;
        {
            float mh = mk_health(ent, HEALTH_MAX);
            if (mh > k->maxHp) k->maxHp = mh;
        }
        mk_ground(mk_pos(ent), &k->east, &k->north);
        dirty = 1;
    }

    saveIn -= c->dt;
    if (saveIn <= 0.0f) { save(); saveIn = 60.0f; }
}

/* Menu lines: most dangerous kinds first. */
static int info(int line, char *buf, int n)
{
    static int order[MAX_KINDS], nOrder;
    if (line == 0) {
        nOrder = 0;
        for (int i = 0; i < nK; i++) if (danger_of_kind(&K[i]) >= C.minDanger) order[nOrder++] = i;
        for (int a = 1; a < nOrder; a++) {      /* insertion sort by danger, descending */
            int v = order[a], b = a - 1;
            while (b >= 0 && danger_of_kind(&K[order[b]]) < danger_of_kind(&K[v])) { order[b + 1] = order[b]; b--; }
            order[b + 1] = v;
        }
        snprintf(buf, (size_t)n, "Enciclopedia: %d tipos de enemigo conocidos", nK);
        return 1;
    }
    if (line > C.lines || line > nOrder) return 0;
    {
        const Kind *k = &K[order[line - 1]];
        float d = danger_of_kind(k);
        if (k->secs >= 3.0f)
            snprintf(buf, (size_t)n, "  tipo %u: %.1f %%/s, %d muertos, %d vistos, vida %.0f, en (%.0f, %.0f)",
                     k->type, d, k->kills, k->seen, k->maxHp, k->east, k->north);
        else
            snprintf(buf, (size_t)n, "  tipo %u: sin aprender, %d muertos, %d vistos, vida %.0f, en (%.0f, %.0f)",
                     k->type, k->kills, k->seen, k->maxHp, k->east, k->north);
    }
    return 1;
}

static const char *status(void)
{
    static char b[64];
    if (!C.enabled) return "Aprender: desactivado";
    snprintf(b, sizeof b, "Aprender: %d tipos de enemigo", nK);
    return b;
}

static void dump(const Ctx *c)
{
    char line[160];
    (void)c;
    for (int i = 0; info(i, line, sizeof line); i++) logmsg("%s", line);
}

static void stop(void) { save(); }

MK_MODULE(mod_learn) = {
    "Aprendizaje y enciclopedia", "Learn", settings, sizeof settings / sizeof settings[0], 5,
    init, 0, 0, tick, 0, dump, status, stop, 0, info,
};
