/*
 * Mod: auto potions.
 *
 * Healing: drinks from the configured belt slot when the health missing is at least
 * what one potion heals. It learns that amount by measuring the rise right after
 * drinking (regeneration-sized rises are ignored), falls back to a backup slot when
 * the first is empty, and always drinks below an emergency threshold.
 * Buff potions (optional): drinks a belt slot when a tough enemy (boss/champion)
 * attacks, with its own long cooldown.
 * Potions keep working with the automation switched off (safety first).
 */
#include "modkit.h"
#include <string.h>

typedef void *(__thiscall *Fn_ItemTransferCtor)(void *self, unsigned creature, unsigned item, int context,
                                                int invType, unsigned char flag);
typedef void *(__thiscall *Fn_GetInventory)(void *creature, int invType);
typedef unsigned char (__thiscall *Fn_GetPotionSlot)(void *inv, unsigned char slot, unsigned *bp, unsigned *item, int *count);
static Fn_ItemTransferCtor transferCtor;
static Fn_Dtor transferDtor;
static Fn_GetInventory getInventory;
static Fn_GetPotionSlot getPotionSlot;

#define ITEM_CTX_USE 0xc
#define INV_MAIN     2

static struct {
    int enabled, slot, backupSlot, buffSlot;
    float guessPct, regenPct, cooldown, emergencyPct, buffCooldown, buffToughFactor;
} C = { 0, 0, -1, -1, 30.0f, 3.0f, 2.0f, 25.0f, 90.0f, 1.5f };

static const Setting settings[] = {
    { "Enabled", "Pociones automaticas", "Bebe sola cuando falta tanta vida como cura una pocion. SIN PROBAR en el juego.", ST_BOOL, &C.enabled, 0, 1 },
    { "Slot", "Ranura de curacion (0-5)", "Ranura del cinturon de pociones con las de curacion (0 = la primera).", ST_INT, &C.slot, 0, 5 },
    { "BackupSlot", "Ranura de reserva (-1 = no)", "Si la principal se vacia, bebe de esta.", ST_INT, &C.backupSlot, -1, 5 },
    { "GuessHealPct", "Cura supuesta (%)", "Lo que se supone que cura una pocion hasta que lo aprende midiendo.", ST_PERCENT, &C.guessPct, 5, 100 },
    { "RegenIgnorePct", "Ignorar subidas menores (%)", "Subidas de vida menores que esto son regeneracion, no pocion.", ST_PERCENT, &C.regenPct, 0, 20 },
    { "EmergencyPct", "Beber siempre bajo (%)", "Por debajo de este % de vida bebe siempre (0 = nunca).", ST_PERCENT, &C.emergencyPct, 0, 90 },
    { "Cooldown", "Espera entre pociones (s)", "Tiempo minimo entre dos pociones (el juego tiene ademas su propio enfriamiento).", ST_FLOAT, &C.cooldown, 0.5f, 30.0f },
    { "BuffSlot", "Ranura de pocion de mejora (-1 = no)", "Bebe esta ranura cuando te ataca un enemigo duro (jefe, campeon).", ST_INT, &C.buffSlot, -1, 5 },
    { "BuffCooldown", "Espera de la mejora (s)", "Tiempo minimo entre dos pociones de mejora.", ST_FLOAT, &C.buffCooldown, 5.0f, 600.0f },
    { "ToughFactor", "Enemigo duro = vida x", "Un enemigo es duro si su vida maxima supera la tuya por este factor.", ST_FLOAT, &C.buffToughFactor, 1.0f, 10.0f },
};

static struct {
    float cooldown, buffCooldown, watch, learned, before;
    int learning, drunk;
} P;

static int resolve(void)
{
    int ok = 1;
    ok &= mk_resolve(E.logic, &transferCtor, "??0cEventItemTransfer@@QAE@IIW4eItemTransferContext@@W4eInventoryType@@_N@Z", 1);
    ok &= mk_resolve(E.logic, &transferDtor, "??1cEventItemTransfer@@UAE@XZ", 1);
    ok &= mk_resolve(E.logic, &getInventory, "?getInventory@cEntityCreature@@QBEPAViInventory@@W4eInventoryType@@@Z", 1);
    ok &= mk_resolve(E.logic, &getPotionSlot, "?getPotionSlot@cItemInventory@@QBE_NEAAI0AAH@Z", 1);
    ok &= E.healthGet != NULL;
    return ok;
}

static int slot_item(void *hero, int slot, unsigned *item, int *count)
{
    unsigned bp = 0;
    void *inv;
    if (slot < 0 || slot > 5) return 0;
    inv = getInventory(hero, INV_MAIN);   /* the game casts this straight to cItemInventory* */
    if (!inv) return 0;
    *item = 0; *count = 0;
    if (!getPotionSlot(inv, (unsigned char)slot, &bp, item, count)) return 0;
    return *item != 0 && *count > 0;
}

static void use_item(unsigned hero, unsigned item)
{
    unsigned char ev[0x200] __attribute__((aligned(16)));
    memset(ev, 0, sizeof ev);
    transferCtor(ev, hero, item, ITEM_CTX_USE, INV_MAIN, 0);
    mk_push(ev, transferDtor);
    P.drunk++;
}

static int tough_attacker(const Ctx *c)
{
    for (unsigned i = 0; i < 256; i++) {
        const EntityNear *e = E.getNearListElement(c->hero, i, NEAR_HOSTILE | NEAR_ALIVE);
        if (!e) break;
        if (!e->hostile || e->notAlive || !mk_is_attacking_hero(c->hero, e->ref)) continue;
        if (mk_health(mk_entity(e->ref), HEALTH_MAX) > c->maxHp * C.buffToughFactor) return 1;
    }
    return 0;
}

static void tick(const Ctx *c)
{
    float missing, need;
    unsigned item;
    int count, slot;
    if (!C.enabled || c->maxHp <= 0.0f || S.menuOpen) return;
    P.cooldown -= c->dt;
    P.buffCooldown -= c->dt;

    if (P.learning) {
        float rise = c->hp - P.before;
        P.watch -= c->dt;
        if (rise > c->maxHp * C.regenPct / 100.0f) {
            P.learned = P.learned > 0.0f ? 0.7f * P.learned + 0.3f * rise : rise;
            P.learning = 0;
            logmsg("potion: heals about %.0f of %.0f", P.learned, c->maxHp);
        } else if (P.watch <= 0.0f) {
            P.learning = 0;
        }
    }

    if (C.buffSlot >= 0 && P.buffCooldown <= 0.0f && tough_attacker(c) && slot_item(c->hero, C.buffSlot, &item, &count)) {
        use_item(c->heroRef, item);
        logmsg("potion: buff slot %d (tough enemy)", C.buffSlot);
        P.buffCooldown = C.buffCooldown;
        P.cooldown = C.cooldown > P.cooldown ? C.cooldown : P.cooldown;
        return;
    }

    if (P.cooldown > 0.0f || P.learning) return;
    missing = c->maxHp - c->hp;
    need = P.learned > 0.0f ? P.learned : c->maxHp * C.guessPct / 100.0f;
    if (missing < need && c->hp > c->maxHp * C.emergencyPct / 100.0f) return;
    slot = C.slot;
    if (!slot_item(c->hero, slot, &item, &count)) {
        slot = C.backupSlot;
        if (!slot_item(c->hero, slot, &item, &count)) return;
    }
    use_item(c->heroRef, item);
    logmsg("potion: drink slot %d (hp %.0f/%.0f, %d left)", slot, c->hp, c->maxHp, count - 1);
    P.before = c->hp; P.watch = 1.5f; P.learning = 1;
    P.cooldown = C.cooldown;
}

static const char *status(void)
{
    static char buf[64];
    if (!C.enabled) return "Pociones: desactivadas";
    if (P.learned > 0.0f) { wsprintfA(buf, "Pociones: curan ~%d", (int)P.learned); return buf; }
    return "Pociones: listas (aprendiendo)";
}

static void dump(const Ctx *c)
{
    unsigned item; int count;
    for (int s = 0; s < 6; s++)
        if (slot_item(c->hero, s, &item, &count)) logmsg("potion slot %d: item %08x x%d", s, item, count);
    logmsg("potion: learned=%.0f drunk=%d", P.learned, P.drunk);
}

MK_MODULE(mod_potions) = {
    "Pociones", "Potion", settings, sizeof settings / sizeof settings[0], 10,
    0, resolve, 0, tick, 0, dump, status, 0, 0,
};
