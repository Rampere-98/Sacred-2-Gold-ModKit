/*
 * Mod: combat learning and enemy encyclopedia.
 *
 * Learns how hard each enemy kind hits: the health the hero loses while enemies
 * attack it is shared among the attackers' types, over the seconds each spends
 * attacking (danger = share of the hero's maximum health per second). Also keeps an
 * encyclopedia of the kinds met: how many seen and killed, their maximum health and
 * where the last one was, with the game's own (localised) name of the kind: the same
 * lookup as cLokaMgr::getName(WString&, ref) (queryStatus 4 -> text category 3, else
 * queryStatus 6 -> category 2). Kept in system/Plugin_ModKit-foes.txt between sessions.
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
#define NAME_LEN  48
typedef struct {
    unsigned type;
    float dmg, secs, maxHp, east, north;
    int kills, seen;
    char name[NAME_LEN];      /* UTF-8, empty until the game names it */
} Kind;

/* cLokaMgr (s2logic): localised texts by category and id. */
typedef unsigned (__thiscall *Fn_Query)(void *ent, unsigned cmd, unsigned a, unsigned b, unsigned c);
static void **lokaInstance;
static unsigned char (__thiscall *lokaExists)(void *loka, int category, unsigned id);
static const wchar_t *(__thiscall *lokaGetText)(void *loka, int category, unsigned id);
static float heroE, heroN;
static int haveHero;
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

/* The game's name of a creature, as its tooltip shows it; 0 if unknown. */
static int name_of(void *ent, char *out, int n)
{
    static const struct { unsigned cmd; int category; } Q[] = { { 4, 3 }, { 6, 2 } };
    void *loka;
    if (!ent || !lokaInstance || !lokaGetText || !lokaExists || !(loka = *lokaInstance)) return 0;
    for (int i = 0; i < 2; i++) {
        unsigned id = ((Fn_Query)(*(void ***)ent)[0])(ent, Q[i].cmd, 0, 0, 0);   /* vtable slot 0 = queryStatus */
        const wchar_t *w;
        if (!id || !lokaExists(loka, Q[i].category, id)) continue;
        w = lokaGetText(loka, Q[i].category, id);
        if (w && w[0] && WideCharToMultiByte(CP_UTF8, 0, w, -1, out, n, NULL, NULL) > 1) {
            out[n - 1] = 0;
            for (char *c = out; *c; c++) if (*c == '\n' || *c == '\r' || *c == '\t') *c = ' ';
            return 1;
        }
    }
    return 0;
}

static int resolve(void)
{
    mk_resolve(E.logic, &lokaInstance, "?_instance@?$tSingleton@VcLokaMgr@@@Orc@@0PAVcLokaMgr@@A", 0);
    mk_resolve(E.logic, &lokaExists, "?exists@cLokaMgr@@QBE_NW4eCategory@1@I@Z", 0);
    mk_resolve(E.logic, &lokaGetText, "?getText@cLokaMgr@@QBEPB_WW4eCategory@1@I@Z", 0);
    if (!lokaInstance || !lokaExists || !lokaGetText) logmsg("learn: enemy names unavailable (cLokaMgr exports missing)");
    return 1;
}

/* Eight-point compass from (dx, dy) on the ground plane (north up). */
static const char *compass(float dx, float dy)
{
    static const char *P[] = { "E", "NE", "N", "NO", "O", "SO", "S", "SE" };
    float ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
    if (ay <= ax * 0.4142f) return dx >= 0 ? P[0] : P[4];
    if (ax <= ay * 0.4142f) return dy >= 0 ? P[2] : P[6];
    return dy >= 0 ? (dx >= 0 ? P[1] : P[3]) : (dx >= 0 ? P[7] : P[5]);
}

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
    for (int i = 0; i < nK; i++) if (K[i].name[0]) fprintf(f, "N %u %s\n", K[i].type, K[i].name);
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
        if (line[0] == 'N' && line[1] == ' ') {   /* N type name (after the kinds) */
            unsigned type;
            int at = 0;
            if (sscanf(line + 2, "%u %n", &type, &at) >= 1 && at > 0) {
                Kind *kn = kind(type, 1);
                char *nm = line + 2 + at, *e = nm + strlen(nm);
                while (e > nm && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = 0;
                if (kn) snprintf(kn->name, sizeof kn->name, "%s", nm);
            }
            continue;
        }
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
    if (S.upAxis >= 0) { mk_ground(mk_pos(c->hero), &heroE, &heroN); haveHero = 1; }
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
        if (!k->name[0] && name_of(ent, k->name, sizeof k->name)) dirty = 1;
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
        if (!k->name[0]) name_of(ent, k->name, sizeof k->name);
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
        char who[NAME_LEN + 16], hits[32], where[48];
        if (k->name[0]) snprintf(who, sizeof who, "%s", k->name);
        else snprintf(who, sizeof who, "tipo %u", k->type);
        if (k->secs >= 3.0f) snprintf(hits, sizeof hits, "%.1f %%/s", danger_of_kind(k));
        else snprintf(hits, sizeof hits, "peligro sin aprender");
        if (haveHero && (k->east != 0.0f || k->north != 0.0f)) {
            float dx = (k->east - heroE) / 42.5f, dy = (k->north - heroN) / 42.5f;
            snprintf(where, sizeof where, ", ultimo a %.0f m al %s", mk_sqrt(dx * dx + dy * dy), compass(dx, dy));
        } else where[0] = 0;
        snprintf(buf, (size_t)n, "  %s: %s, %d muertos, %d vistos, vida %.0f%s", who, hits, k->kills, k->seen, k->maxHp, where);
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
    init, resolve, 0, tick, 0, dump, status, stop, 0, info,
};
