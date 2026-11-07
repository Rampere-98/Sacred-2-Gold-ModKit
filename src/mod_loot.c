/*
 * Mod: auto loot.
 *
 * With no enemy around, walks to the nearest item on the ground and triggers the
 * game's own radius pick-up (cEventAutoCollect, silent mode, radius 50) once, then
 * waits. The game's rarity filter (autoCollectRareSelf/Other) is applied by the
 * engine itself. Only isItem entities are approached: fixtures, signposts, shrines
 * and NPCs are never touched. Unreachable items are given up after N tries.
 */
#include "modkit.h"
#include <string.h>

typedef void *(__thiscall *Fn_AutoCollectCtor)(void *self, unsigned hero, unsigned char rareSelf,
                                               unsigned char rareOther, unsigned char silent);
static Fn_AutoCollectCtor collectCtor;
static Fn_Dtor collectDtor;

#define SILENT_RADIUS 50.0f   /* filterItemsInRange, silent */

static struct {
    int enabled, rareSelf, rareOther, tries, inCombat;
    float range, timeout, safeRadius;
} C = { 1, 0, 0, 2, 1, 600.0f, 3.0f, 450.0f };

static const Setting settings[] = {
    { "Enabled", "Recogida automatica", "Sin enemigos cerca, camina al objeto mas cercano y lo recoge.", ST_BOOL, &C.enabled, 0, 1 },
    { "Range", "Radio de busqueda", "Distancia maxima a un objeto (42.5 = 1 m).", ST_FLOAT, &C.range, 60, 1500 },
    { "SafeRadius", "Radio sin enemigos", "No va a por objetos si hay un enemigo vivo dentro de este radio.", ST_FLOAT, &C.safeRadius, 0, 1500 },
    { "AtFeetInCombat", "Recoger a tus pies en combate", "Lo que esta a tus pies se recoge aunque haya combate (sin moverse).", ST_BOOL, &C.inCombat, 0, 1 },
    { "RareSelf", "Rareza minima (propios)", "0-3, como autoCollectRareSelf del juego. 0 = todo.", ST_INT, &C.rareSelf, 0, 3 },
    { "RareOther", "Rareza minima (otros)", "0-3, como autoCollectRareOther del juego.", ST_INT, &C.rareOther, 0, 3 },
    { "WalkTimeout", "Tiempo para llegar (s)", "Si no llega en este tiempo, lo intenta otra vez o lo deja.", ST_FLOAT, &C.timeout, 0.5f, 15.0f },
    { "Tries", "Intentos por objeto", "Tras estos intentos fallidos lo ignora.", ST_INT, &C.tries, 1, 10 },
};

#define MAX_BLACK 64
static struct {
    unsigned item;
    float timer, cooldown;
    int tries, nBlack, picked;
    unsigned black[MAX_BLACK];
    const char *state;
} L;

static int resolve(void)
{
    int ok = 1;
    ok &= mk_resolve(E.logic, &collectCtor, "??0cEventAutoCollect@@QAE@IEE_N@Z", 1);
    ok &= mk_resolve(E.logic, &collectDtor, "??1cEventAutoCollect@@UAE@XZ", 1);
    return ok;
}

static int blacklisted(unsigned ref)
{
    for (int i = 0; i < L.nBlack; i++) if (L.black[i] == ref) return 1;
    return 0;
}

static void blacklist(unsigned ref)
{
    if (L.nBlack < MAX_BLACK) L.black[L.nBlack++] = ref;
    else L.black[rnd() % MAX_BLACK] = ref;
}

static void order_collect(unsigned hero)
{
    unsigned char ev[0x80] __attribute__((aligned(16)));
    memset(ev, 0, sizeof ev);
    collectCtor(ev, hero, (unsigned char)C.rareSelf, (unsigned char)C.rareOther, 1 /* silent */);
    mk_push(ev, collectDtor);
    L.picked++;
}

static unsigned nearest_item(unsigned char *hero, float maxDist, float *distOut)
{
    unsigned best = 0;
    float bestDist = maxDist;
    for (unsigned i = 0; i < 256; i++) {
        const EntityNear *e = E.getNearListElement(hero, i, 0);
        void *ent;
        if (!e) break;
        if (e->dist > bestDist || blacklisted(e->ref)) continue;
        ent = mk_entity(e->ref);
        if (!ent || !mk_type_is(E.isItem, ent)) continue;
        best = e->ref; bestDist = e->dist;
    }
    *distOut = bestDist;
    return best;
}

static void reset(void) { L.item = 0; L.state = "Recoger: en pausa"; }

static void tick(const Ctx *c)
{
    float d;
    unsigned it;
    int danger;
    if (!C.enabled || !S.autoOk) return;
    L.cooldown -= c->dt;
    danger = S.target || S.threat || (C.safeRadius > 0.0f && mk_hostiles_within(c->hero, C.safeRadius) > 0);

    if (danger) {
        L.item = 0;
        L.state = "Recoger: esperando (enemigos)";
        if (C.inCombat && L.cooldown <= 0.0f && S.castLock <= 0.0f) {
            /* at its feet only: never walk away from a fight */
            if (nearest_item(c->hero, SILENT_RADIUS - 5.0f, &d)) { order_collect(c->heroRef); }
            L.cooldown = rnd_range(0.8f, 1.4f);
        }
        return;
    }

    if (L.item) {
        void *ent = mk_entity(L.item);
        L.timer -= c->dt;
        if (!ent) { L.item = 0; L.cooldown = rnd_range(0.25f, 0.5f); return; } /* picked up by someone */
        if (mk_dist(c->hero, ent) <= SILENT_RADIUS - 5.0f) {
            order_collect(c->heroRef);  /* once, then wait */
            L.item = 0; L.cooldown = rnd_range(0.6f, 1.0f);
            return;
        }
        if (L.timer <= 0.0f) {
            if (++L.tries >= C.tries) { blacklist(L.item); logmsg("loot: gave up on %08x", L.item); }
            L.item = 0; L.cooldown = rnd_range(0.4f, 0.8f);
        }
        return;
    }
    if (L.cooldown > 0.0f) return;
    it = nearest_item(c->hero, C.range, &d);
    if (!it) { L.cooldown = rnd_range(0.5f, 0.9f); L.state = "Recoger: nada cerca"; return; }
    if (d <= SILENT_RADIUS - 5.0f) {
        order_collect(c->heroRef);
        L.cooldown = rnd_range(0.6f, 1.0f);
        return;
    }
    if (it != L.item) L.tries = 0;
    L.item = it;
    L.timer = C.timeout;
    mk_order_move(c->heroRef, mk_entity(it));   /* once; wait for arrival */
    L.state = "Recoger: yendo a un objeto";
    logmsg("loot: walking to %08x (%.0f)", it, d);
}

static const char *status(void)
{
    if (!C.enabled) return "Recoger: desactivado";
    return L.state ? L.state : "Recoger: listo";
}

static void dump(const Ctx *c)
{
    (void)c;
    logmsg("loot: item=%08x tries=%d ignored=%d pickups=%d", L.item, L.tries, L.nBlack, L.picked);
}

MK_MODULE(mod_loot) = {
    "Recogida", "Loot", settings, sizeof settings / sizeof settings[0], 40,
    0, resolve, 0, tick, reset, dump, status, 0, 0,
};
