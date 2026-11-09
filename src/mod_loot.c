/*
 * Mod: auto loot (the agent's own collect).
 *
 * Takes what lies around one item at a time with the order a click on it gives
 * (cEventMoveTo + setTargetRef: the hero walks there and picks it up), never the
 * game's collect key. Containers (chests, barrels...) can be opened the same way
 * (opt-in until the End dump has told them apart from fixtures). Only when no fight
 * is on: nobody attacking the hero, not hurt in the last 3 s, no enemy within
 * SafeRadius, and never in the player's way (after their last click, hero standing).
 * Loot it cannot reach is tried again later and then left; used containers are
 * never gone for again. The game's own radius collect stays available as an option.
 */
#include "modkit.h"
#include <string.h>

typedef void *(__thiscall *Fn_AutoCollectCtor)(void *self, unsigned hero, unsigned char rareSelf,
                                               unsigned char rareOther, unsigned char silent);
static Fn_AutoCollectCtor collectCtor;
static Fn_Dtor collectDtor;

#define SILENT_RADIUS 50.0f   /* filterItemsInRange, silent */

static struct {
    int enabled, always, containers, gameCollect, rareSelf, rareOther, tries, inCombat;
    float range, timeout, safeRadius, playerWait;
} C = { 1, 0, 0, 0, 0, 0, 2, 1, 600.0f, 4.0f, 450.0f, 2.5f };

static const Setting settings[] = {
    { "Enabled", "El agente recoge el botin", "Recoge uno a uno lo que hay alrededor con la orden de un clic (nunca la tecla de recoger del juego).", ST_BOOL, &C.enabled, 0, 1 },
    { "Always", "Tambien sin la automatizacion", "1 = recoge siempre que no haya combate, aunque la automatizacion (Insert) este apagada.", ST_BOOL, &C.always, 0, 1 },
    { "Containers", "Abrir cofres y recipientes", "Abre cofres, barriles y demas (EXPERIMENTAL: puede tocar algo que no es un cofre; cada cosa solo una vez).", ST_BOOL, &C.containers, 0, 1 },
    { "Range", "Radio de busqueda", "Distancia maxima al botin (42.5 = 1 m).", ST_FLOAT, &C.range, 60, 1500 },
    { "SafeRadius", "Radio sin enemigos", "No va a por botin si hay un enemigo vivo dentro de este radio.", ST_FLOAT, &C.safeRadius, 0, 1500 },
    { "AtFeetInCombat", "Recoger a tus pies en combate", "Lo que esta pegado a ti se recoge aunque haya combate.", ST_BOOL, &C.inCombat, 0, 1 },
    { "PlayerWait", "Espera tras tu clic (s)", "Sin la automatizacion, no empieza hasta estos segundos tras tu ultimo clic.", ST_FLOAT, &C.playerWait, 0.5f, 10.0f },
    { "WalkTimeout", "Tiempo para llegar (s)", "Si no lo coge en este tiempo, lo deja para luego.", ST_FLOAT, &C.timeout, 0.5f, 15.0f },
    { "Tries", "Intentos por objeto", "Tras estos intentos fallidos lo ignora.", ST_INT, &C.tries, 1, 10 },
    { "GameCollect", "Usar la recogida del juego", "0 = la del agente (uno a uno). 1 = la de radio del juego, con su filtro de rareza.", ST_BOOL, &C.gameCollect, 0, 1 },
    { "RareSelf", "Rareza minima (recogida del juego)", "Solo con GameCollect: 0-3 como autoCollectRareSelf.", ST_INT, &C.rareSelf, 0, 3 },
    { "RareOther", "Rareza minima otros (recogida del juego)", "Solo con GameCollect: 0-3 como autoCollectRareOther.", ST_INT, &C.rareOther, 0, 3 },
};

#define MAX_MEM 128
typedef struct { unsigned ref; int tries; float retryIn; } Mem;
static struct {
    unsigned target; int isContainer;
    float timer, cooldown;
    Mem mem[MAX_MEM]; int nMem;            /* failed tries per ref */
    unsigned used[MAX_MEM]; int nUsed;     /* containers used once: never again */
    int picked, opened;
    const char *state;
} L;

static int resolve(void)
{
    mk_resolve(E.logic, &collectCtor, "??0cEventAutoCollect@@QAE@IEE_N@Z", 0);
    mk_resolve(E.logic, &collectDtor, "??1cEventAutoCollect@@UAE@XZ", 0);
    if (!E.moveSetTarget && !(collectCtor && collectDtor)) return 0;
    return 1;
}

static Mem *mem_of(unsigned ref, int create)
{
    for (int i = 0; i < L.nMem; i++) if (L.mem[i].ref == ref) return &L.mem[i];
    if (!create) return NULL;
    if (L.nMem < MAX_MEM) { L.mem[L.nMem].ref = ref; L.mem[L.nMem].tries = 0; L.mem[L.nMem].retryIn = 0; return &L.mem[L.nMem++]; }
    L.mem[rnd() % MAX_MEM].ref = ref;
    return mem_of(ref, 0);
}

static int was_used(unsigned ref)
{
    for (int i = 0; i < L.nUsed; i++) if (L.used[i] == ref) return 1;
    return 0;
}

static void mark_used(unsigned ref)
{
    if (L.nUsed < MAX_MEM) L.used[L.nUsed++] = ref;
    else L.used[rnd() % MAX_MEM] = ref;
}

static int skip(unsigned ref)
{
    Mem *m = mem_of(ref, 0);
    if (was_used(ref)) return 1;
    return m && (m->tries >= C.tries || m->retryIn > 0.0f);
}

static void order_game_collect(unsigned hero)
{
    unsigned char ev[0x80] __attribute__((aligned(16)));
    memset(ev, 0, sizeof ev);
    collectCtor(ev, hero, (unsigned char)C.rareSelf, (unsigned char)C.rareOther, 1 /* silent */);
    mk_push(ev, collectDtor);
}

/* Nearest loot: items, and (opt-in) clickable non-creature, non-item things = containers. */
static unsigned nearest_loot(unsigned char *hero, float maxDist, int *container, float *distOut)
{
    unsigned best = 0;
    float bestDist = maxDist;
    *container = 0;
    for (unsigned i = 0; i < 256; i++) {
        const EntityNear *e = E.getNearListElement(hero, i, 0);
        void *ent;
        int item, cont;
        if (!e) break;
        if (e->dist > bestDist || skip(e->ref)) continue;
        ent = mk_entity(e->ref);
        if (!ent) continue;
        item = mk_type_is(E.isItem, ent);
        cont = !item && C.containers && mk_type_is(E.isClickable, ent) && !mk_type_is(E.isCreature, ent);
        if (!item && !cont) continue;
        if (item && mk_dist2d(hero, ent) < 1.0f) continue;  /* carried by the hero, not on the ground */
        best = e->ref; bestDist = e->dist; *container = cont;
    }
    *distOut = bestDist;
    return best;
}

static int attacked(unsigned char *hero)
{
    unsigned *it = *(unsigned **)(hero + CREATURE_ATTACKERS_BEGIN);
    unsigned *end = *(unsigned **)(hero + CREATURE_ATTACKERS_END);
    return it && end && end > it && (end - it) <= 2 * 16;
}

static void finish(void) { L.target = 0; S.busy = 0; }

/* A picked-up item may live on as an entity (in the backpack): it leaves the near list. */
static int in_near(unsigned char *hero, unsigned ref)
{
    for (unsigned i = 0; i < 256; i++) {
        const EntityNear *e = E.getNearListElement(hero, i, 0);
        if (!e) break;
        if (e->ref == ref) return 1;
    }
    return 0;
}

static void reset(void) { if (L.target) finish(); L.state = "Recoger: en pausa"; }

static void take(const Ctx *c, unsigned ref, int container)
{
    if (C.gameCollect && !container && collectCtor) { order_game_collect(c->heroRef); L.cooldown = rnd_range(0.6f, 1.0f); return; }
    if (!mk_order_interact(c->heroRef, ref)) return;
    L.target = ref; L.isContainer = container;
    L.timer = C.timeout;
    S.busy = 1;
    L.state = container ? "Recoger: abriendo un recipiente" : "Recoger: yendo a por un objeto";
    logmsg("loot: %s %08x", container ? "open" : "take", ref);
}

static void tick(const Ctx *c)
{
    float d;
    unsigned it;
    int danger, cont, allowed;
    for (int i = 0; i < L.nMem; i++) if (L.mem[i].retryIn > 0.0f) L.mem[i].retryIn -= c->dt;
    allowed = C.enabled && !S.menuOpen && (S.autoOk || (C.always && S.sinceInput >= C.playerWait));
    if (!allowed) { if (L.target) finish(); return; }
    L.cooldown -= c->dt;
    danger = S.target || S.threat || attacked(c->hero) || S.sinceHurt < 3.0f ||
             (C.safeRadius > 0.0f && mk_hostiles_within(c->hero, C.safeRadius) > 0);

    if (L.target) {
        void *ent = mk_entity(L.target);
        L.timer -= c->dt;
        if (danger) { finish(); L.state = "Recoger: esperando (combate)"; return; }
        if (!ent || !in_near(c->hero, L.target)) {   /* gone: taken */
            if (L.isContainer) { mark_used(L.target); L.opened++; } else L.picked++;
            finish(); L.cooldown = rnd_range(0.25f, 0.5f);
            return;
        }
        if (L.isContainer && mk_dist2d(c->hero, ent) < 70.0f && S.standing > 0.8f) {
            mark_used(L.target); L.opened++;               /* reached and used: never again */
            finish(); L.cooldown = rnd_range(0.5f, 0.9f);
            return;
        }
        if (L.timer <= 0.0f) {
            Mem *m = mem_of(L.target, 1);
            if (m) { m->tries++; m->retryIn = 60.0f; }      /* try once more a minute later */
            if (m && m->tries >= C.tries) logmsg("loot: gave up on %08x", L.target);
            finish(); L.cooldown = rnd_range(0.4f, 0.8f);
        }
        return;
    }

    if (danger) {
        L.state = "Recoger: esperando (combate)";
        if (C.inCombat && L.cooldown <= 0.0f && S.castLock <= 0.0f) {
            it = nearest_loot(c->hero, SILENT_RADIUS + 10.0f, &cont, &d);
            if (it && !cont) take(c, it, 0);              /* at its feet only */
            L.cooldown = rnd_range(0.8f, 1.4f);
        }
        return;
    }
    if (L.cooldown > 0.0f) return;
    if (!S.autoOk && S.standing < 1.0f) return;           /* never in the player's way */
    it = nearest_loot(c->hero, C.range, &cont, &d);
    if (!it) { L.cooldown = rnd_range(0.5f, 0.9f); L.state = "Recoger: nada cerca"; return; }
    take(c, it, cont);
}

static const char *status(void)
{
    if (!C.enabled) return "Recoger: desactivado";
    return L.state ? L.state : "Recoger: listo";
}

static void dump(const Ctx *c)
{
    (void)c;
    logmsg("loot: target=%08x container=%d remembered=%d used=%d picked=%d opened=%d",
           L.target, L.isContainer, L.nMem, L.nUsed, L.picked, L.opened);
}

MK_MODULE(mod_loot) = {
    "Recogida", "Loot", settings, sizeof settings / sizeof settings[0], 40,
    0, resolve, 0, tick, reset, dump, status, 0, 0, 0,
};
