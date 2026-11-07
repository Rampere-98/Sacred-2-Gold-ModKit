/*
 * Mod: combat arts with a strategy.
 *
 * Reads the hero's slotted combat arts (cEntityCreature::getMagicSlots, 0x80-byte
 * cMagicSlotData entries) and casts them the way the game's AI does (s2logicai
 * FUN_10005ec0): cEventCastSpell(hero, slot) + setTargetRef/setTargetPos/setCombo.
 *   - Healing arts below a health threshold.
 *   - Buffs kept active (only when cMagicMgr::isBuffActive can tell; never toggled off).
 *   - Area arts when enough enemies stand inside their effect range.
 *   - Long-cooldown arts saved for tough enemies (bosses, champions) or emergencies.
 *   - A short lock after each cast so the auto-attack does not cut the animation.
 */
#include "modkit.h"
#include <string.h>

typedef void *(__thiscall *Fn_GetMagicSlots)(void *creature);            /* -> stlport vector& */
typedef void *(__thiscall *Fn_CastCtor)(void *self, unsigned caster, unsigned char slot);
typedef void (__thiscall *Fn_SetRef)(void *ev, unsigned ref);
typedef void (__thiscall *Fn_SetPos)(void *ev, const void *wpos);
typedef void (__thiscall *Fn_SetBool)(void *ev, unsigned char b);
typedef float (__thiscall *Fn_SlotF)(const void *slot);
typedef unsigned char (__thiscall *Fn_SlotB)(const void *slot);
typedef unsigned char (__thiscall *Fn_SlotBB)(const void *slot, unsigned char b);
typedef const void *(__thiscall *Fn_SlotEntry)(const void *slot);
typedef const char *(__thiscall *Fn_SlotName)(const void *slot);
typedef unsigned char (__thiscall *Fn_BuffActive)(const void *magicMgr, unsigned char slot, unsigned *ref, unsigned a, unsigned b);
typedef unsigned (__thiscall *Fn_CarrierRef)(const void *entity);

static struct {
    Fn_GetMagicSlots getMagicSlots;
    Fn_CastCtor castCtor; Fn_Dtor castDtor;
    Fn_SetRef setTargetRef; Fn_SetPos setTargetPos; Fn_SetBool setCombo;
    Fn_SlotF curCooldown, fullCooldown, effectRange;
    Fn_SlotB isBuff, isCombo, isForm;
    Fn_SlotBB isAttack;
    Fn_SlotEntry entry;
    Fn_SlotName name;
    unsigned char (__thiscall *isHeal)(const void *entry);
    float (__thiscall *fightDist)(const void *entry);
    Fn_BuffActive buffActive;           /* optional */
    void ***magicMgr;                   /* optional, with buffActive */
    Fn_CarrierRef carrierRef;           /* optional */
} F;

static struct {
    int enabled, useBuffs, aoeMin, mounted, allowedMask;
    float healBelow, toughFactor, longCd, maxRange, gapMin, gapMax, lock;
} C = { 1, 1, 3, 0, 0x7fffffff, 45.0f, 1.5f, 20.0f, 450.0f, 0.5f, 1.1f, 0.9f };

static const Setting settings[] = {
    { "Enabled", "Artes de combate automaticas", "Lanza tus artes de combate con estrategia durante el combate.", ST_BOOL, &C.enabled, 0, 1 },
    { "UseBuffs", "Mantener buffs activos", "Relanza los buffs que se apaguen (solo si el juego puede decir si estan activos; nunca los apaga).", ST_BOOL, &C.useBuffs, 0, 1 },
    { "HealBelowPct", "Curar por debajo de (%)", "Usa artes de curacion con menos de este % de vida.", ST_PERCENT, &C.healBelow, 0, 95 },
    { "AoeMinEnemies", "Area con al menos N enemigos", "Las artes de area solo con este numero de enemigos dentro de su alcance.", ST_INT, &C.aoeMin, 1, 10 },
    { "ToughFactor", "Enemigo duro = vida x", "Un enemigo es duro (jefe, campeon) si su vida maxima supera la tuya por este factor.", ST_FLOAT, &C.toughFactor, 1.0f, 10.0f },
    { "LongCooldown", "Arte fuerte = regeneracion de (s)", "Las artes con regeneracion igual o mayor se guardan para enemigos duros, grupos o emergencias.", ST_FLOAT, &C.longCd, 1.0f, 120.0f },
    { "MaxRange", "Alcance maximo", "No lanza artes a enemigos mas lejos que esto (42.5 = 1 m).", ST_FLOAT, &C.maxRange, 50, 1500 },
    { "GapMin", "Pausa entre artes (min s)", "Pausa minima entre dos artes.", ST_FLOAT, &C.gapMin, 0.1f, 5.0f },
    { "GapMax", "Pausa entre artes (max s)", "Pausa maxima entre dos artes.", ST_FLOAT, &C.gapMax, 0.1f, 5.0f },
    { "CastLock", "Bloqueo tras lanzar (s)", "Durante este tiempo el autoataque no repite su orden (no corta la animacion).", ST_FLOAT, &C.lock, 0.0f, 3.0f },
    { "CastWhileMounted", "Lanzar a caballo", "Lanzar artes tambien montado.", ST_BOOL, &C.mounted, 0, 1 },
    { "AllowedSlots", "Ranuras permitidas (mascara)", "Bit 0 = primera ranura de arte, bit 1 = segunda... 2147483647 = todas.", ST_INT, &C.allowedMask, 0, 2147483647.0f },
};

#define SLOT_SIZE 0x80
#define MAX_SLOTS 32

#define BUFF_RECAST_GUARD 15.0f   /* never re-cast the same buff sooner: a wrong "inactive" must not toggle it off */

static struct {
    float thinkIn, gap;
    float buffGuard[MAX_SLOTS];
    int casts, ready;
    char last[48];
    char state[96];
} R;

static int resolve(void)
{
    int ok = 1;
    HMODULE l = E.logic;
#define REQ(v, n) ok &= mk_resolve(l, &F.v, (n), 1)
    REQ(getMagicSlots, "?getMagicSlots@cEntityCreature@@QAEAAV?$vector@VcMagicSlotData@@V?$allocator@VcMagicSlotData@@@stlp_std@@@stlp_std@@XZ");
    REQ(castCtor, "??0cEventCastSpell@@QAE@IE@Z");
    REQ(castDtor, "??1cEventCastSpell@@UAE@XZ");
    REQ(setTargetRef, "?setTargetRef@cEventCastSpell@@QAEXI@Z");
    REQ(setTargetPos, "?setTargetPos@cEventCastSpell@@QAEXABVcWorldPosition@@@Z");
    REQ(setCombo, "?setCombo@cEventCastSpell@@QAEX_N@Z");
    REQ(curCooldown, "?getCurrentCoolDown@cMagicSlotData@@QBEMXZ");
    REQ(fullCooldown, "?getCoolDown@cMagicSlotData@@QBEMXZ");
    REQ(effectRange, "?getEffectRange@cMagicSlotData@@QBEMXZ");
    REQ(isBuff, "?isBuff@cMagicSlotData@@QBE_NXZ");
    REQ(isCombo, "?isCombo@cMagicSlotData@@QBE_NXZ");
    REQ(isForm, "?isFormSpell@cMagicSlotData@@QBE_NXZ");
    REQ(isAttack, "?isAttackSpell@cMagicSlotData@@QBE_N_N@Z");
    REQ(entry, "?getMagicEntry@cMagicSlotData@@QBEPBVcMagicEntry@@XZ");
    REQ(name, "?getSpellName@cMagicSlotData@@QBEPBDXZ");
    REQ(isHeal, "?isHealSpell@cMagicEntry@@QBE_NXZ");
    REQ(fightDist, "?getFightDist@cMagicEntry@@QBEMXZ");
#undef REQ
    if (mk_resolve(l, &F.buffActive, "?isBuffActive@cMagicMgr@@QBE_NEAAIII@Z", 0))
        mk_resolve(l, &F.magicMgr, "?_instance@?$tSingleton@VcMagicMgr@@@Orc@@0PAVcMagicMgr@@A", 0);
    mk_resolve(E.core, &F.carrierRef, "?getCarrierRef@cEntity@@QBEIXZ", 0);
    return ok;
}

/* stlport vector<cMagicSlotData>: {begin, end, capacity}; element size 0x80 (AI code: >> 7). */
static int slots_of(unsigned char *hero, unsigned char **begin)
{
    unsigned char **v = (unsigned char **)F.getMagicSlots(hero);
    int n;
    if (!v || !v[0] || v[1] < v[0]) return 0;
    if ((v[1] - v[0]) % SLOT_SIZE) return 0;
    n = (int)((v[1] - v[0]) / SLOT_SIZE);
    *begin = v[0];
    return n > MAX_SLOTS ? MAX_SLOTS : n;
}

/* 1 active, 0 inactive, -1 unknown (export missing): buffs are then never cast. */
static int buff_active(int slot)
{
    unsigned ref = 0;
    void *mm;
    if (!F.buffActive || !F.magicMgr || !(mm = *F.magicMgr)) return -1;
    /* Same arguments as the AI (s2logicai FUN_1008735d): (slot, &ref, caster 0 = any, exclude 0).
     * It walks every active spell, so another creature's spell in the same slot index can
     * report "active" (we then skip a cast); it never reports a live buff as inactive. */
    return F.buffActive(mm, (unsigned char)slot, &ref, 0, 0) ? 1 : 0;
}

static void cast(const Ctx *c, int slot, const unsigned char *data, unsigned target)
{
    unsigned char ev[0x200] __attribute__((aligned(16)));   /* AI keeps it in 92 bytes */
    void *t = mk_entity(target);
    const char *nm = F.name(data);
    memset(ev, 0, sizeof ev);
    F.castCtor(ev, c->heroRef, (unsigned char)slot);
    F.setTargetRef(ev, target);
    if (t) F.setTargetPos(ev, E.getPosition(t));
    F.setCombo(ev, F.isCombo(data) ? 1 : 0);
    mk_push(ev, F.castDtor);
    R.casts++;
    lstrcpynA(R.last, nm ? nm : "?", sizeof R.last);
    S.castLock = C.lock;
    R.gap = rnd_range(C.gapMin, C.gapMax > C.gapMin ? C.gapMax : C.gapMin);
    logmsg("art: slot %d '%s' -> %08x", slot, R.last, target);
}

static unsigned nearest_enemy(const Ctx *c)
{
    unsigned best = 0;
    float bd = C.maxRange;
    for (unsigned i = 0; i < 256; i++) {
        const EntityNear *e = E.getNearListElement(c->hero, i, NEAR_HOSTILE | NEAR_ALIVE);
        if (!e) break;
        if (e->hostile && !e->notAlive && e->dist < bd && mk_alive(mk_entity(e->ref))) { bd = e->dist; best = e->ref; }
    }
    return best;
}

static void reset(void) { R.thinkIn = 0.3f; }

static void tick(const Ctx *c)
{
    unsigned char *base;
    int n, bestSlot = -1;
    float bestScore = -1.0f, hpPct;
    unsigned target;
    void *t;

    R.gap -= c->dt; R.thinkIn -= c->dt;
    for (int i = 0; i < MAX_SLOTS; i++) if (R.buffGuard[i] > 0.0f) R.buffGuard[i] -= c->dt;
    if (!C.enabled || !S.autoOk || R.gap > 0.0f || S.castLock > 0.0f || R.thinkIn > 0.0f) return;
    R.thinkIn = rnd_range(0.22f, 0.42f);
    if (!C.mounted && F.carrierRef && F.carrierRef(c->hero)) { lstrcpyA(R.state, "Artes: a caballo (en pausa)"); return; }
    n = slots_of(c->hero, &base);
    if (!n) { lstrcpyA(R.state, "Artes: ninguna equipada"); return; }
    hpPct = c->maxHp > 0.0f ? 100.0f * c->hp / c->maxHp : 100.0f;
    target = S.target ? S.target : nearest_enemy(c);
    t = mk_entity(target);
    if (t && (!mk_alive(t) || mk_dist(c->hero, t) > C.maxRange)) { target = 0; t = NULL; }

    R.ready = 0;
    for (int i = 0; i < n; i++) {
        const unsigned char *s = base + i * SLOT_SIZE;
        const void *en;
        float full, aoe, sc;
        int big, heal, buff, nAoe, tough;
        if (!(C.allowedMask & (1 << i))) continue;
        en = F.entry(s);
        if (!en || F.curCooldown(s) > 0.05f || F.isForm(s)) continue;
        R.ready++;
        full = F.fullCooldown(s);
        big = full >= C.longCd;
        heal = F.isHeal(en) != 0;
        buff = F.isBuff(s) != 0;

        if (heal) {                                   /* 1. stay alive */
            if (hpPct < C.healBelow) { sc = 100.0f + (C.healBelow - hpPct); if (sc > bestScore) { bestScore = sc; bestSlot = i; } }
            continue;
        }
        if (buff) {                                   /* 2. keep buffs up, never toggle off */
            if (C.useBuffs && R.buffGuard[i] <= 0.0f && buff_active(i) == 0) { sc = 50.0f; if (sc > bestScore) { bestScore = sc; bestSlot = i; } }
            continue;
        }
        if (!t || !F.isAttack(s, 0)) continue;        /* 3. attack arts need a target */
        aoe = F.effectRange(s);
        nAoe = aoe > 30.0f ? mk_hostiles_near_entity(c->hero, t, aoe) : 1;
        tough = c->maxHp > 0.0f && mk_health(t, HEALTH_MAX) > c->maxHp * C.toughFactor;
        if (big && !(tough || nAoe >= C.aoeMin || hpPct < 50.0f)) continue;  /* save the big ones */
        sc = 10.0f + full * 0.2f;                     /* stronger arts have longer regeneration */
        if (aoe > 30.0f) sc += nAoe >= C.aoeMin ? 15.0f : -6.0f;
        if (tough && big) sc += 10.0f;
        if (sc > bestScore) { bestScore = sc; bestSlot = i; }
    }

    if (bestSlot < 0) {
        wsprintfA(R.state, "Artes: %d listas, esperando", R.ready);
        return;
    }
    {
        const unsigned char *s = base + bestSlot * SLOT_SIZE;
        const void *en = F.entry(s);
        int self = F.isBuff(s) || (en && F.isHeal(en));
        if (F.isBuff(s)) R.buffGuard[bestSlot] = BUFF_RECAST_GUARD;
        cast(c, bestSlot, s, self ? c->heroRef : target);
        wsprintfA(R.state, "Artes: %s", R.last);
    }
}

static const char *status(void)
{
    if (!C.enabled) return "Artes: desactivadas";
    return R.state[0] ? R.state : "Artes: listas";
}

static void dump(const Ctx *c)
{
    unsigned char *base;
    int n = slots_of(c->hero, &base);
    logmsg("arts: %d slot(s), buffActive %s, casts=%d", n, F.buffActive && F.magicMgr ? "available" : "MISSING", R.casts);
    for (int i = 0; i < n; i++) {
        const unsigned char *s = base + i * SLOT_SIZE;
        const void *en = F.entry(s);
        const char *nm = F.name(s);
        if (!en) { logmsg("  [%d] empty", i); continue; }
        logmsg("  [%d] '%s' cd=%.1f/%.1f range=%.0f fight=%.0f buff=%d(active=%d) heal=%d attack=%d combo=%d form=%d",
               i, nm ? nm : "?", F.curCooldown(s), F.fullCooldown(s), F.effectRange(s), F.fightDist(en),
               F.isBuff(s), F.isBuff(s) ? buff_active(i) : -1, F.isHeal(en), F.isAttack(s, 0), F.isCombo(s), F.isForm(s));
    }
}

MK_MODULE(mod_arts) = {
    "Artes de combate", "CombatArts", settings, sizeof settings / sizeof settings[0], 20,
    0, resolve, 0, tick, reset, dump, status, 0, 0,
};
