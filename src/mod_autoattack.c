/*
 * Mod: auto attack.
 *
 * Weighs the enemies around the hero (attacking it, closing in, toughness, distance)
 * and gives the engine's own attack order (sEventAttack, as s2logicai does), with
 * natural uneven timing. Sticks to the enemy being fought, stops the swing when no
 * enemy is left, never re-issues during a combat-art animation.
 */
#include "modkit.h"
#include <string.h>

typedef void *(__thiscall *Fn_EventAttackCtor)(void *self, unsigned attacker, unsigned target);
typedef unsigned char (__thiscall *Fn_AddAttacker)(void *creature, unsigned attacker, float seconds);
static Fn_EventAttackCtor attackCtor;
static Fn_Dtor attackDtor;
static Fn_AddAttacker addAttacker;

/* Ranged weapons (bows, crossbows, guns): the AI casts the weapon's special spell instead
 * (s2logicai FUN_10005a01). All optional: without them the melee attack order is used. */
typedef unsigned char (__thiscall *Fn_IsLongrange)(void *creature, unsigned target, unsigned *weapon,
                                                   unsigned char flag, const void *targetPos);
typedef int (__thiscall *Fn_MapWeaponSpell)(const void *magicMgr, unsigned weapon);
typedef int (__thiscall *Fn_QueryStatus)(void *creature, int cmd, int a, void *b, int c);
typedef void *(__thiscall *Fn_CastCtor)(void *self, unsigned caster, unsigned char slot);
typedef void (__thiscall *Fn_SetRef)(void *ev, unsigned ref);
static Fn_IsLongrange isLongrange;
static Fn_MapWeaponSpell mapWeaponSpell;
static void ***magicMgr;
static Fn_CastCtor castCtor;
static Fn_Dtor castDtor;
static Fn_SetRef castSetTarget;
#define QS_SPECIAL_SPELL_SLOT 0xe4
#define SPECIAL_SPELL_LIMIT   0xb

static struct {
    int enabled, seek, preferTough, ranged;
    float range, stick, reissueMin, reissueMax;
    int lookMin, lookMax, reactMin, reactMax, reactThreatMin, reactThreatMax;
    int retreat; float retreatPct, retreatDist;
} C = { 1, 1, 1, 1, 350.0f, 1.35f, 1.6f, 2.6f, 180, 340, 220, 480, 90, 220, 1, 25.0f, 340.0f };

static const Setting settings[] = {
    { "Enabled", "Autoataque", "Ataca solo a los enemigos cercanos o a los que te atacan.", ST_BOOL, &C.enabled, 0, 1 },
    { "Range", "Radio (unidades)", "Distancia maxima a un enemigo (42.5 = 1 m). La IA del juego usa 100 y 350.", ST_FLOAT, &C.range, 50, 1200 },
    { "Seek", "Buscar combate", "1 = va tambien a por cualquier enemigo dentro del radio. 0 = solo responde a quien te ataca.", ST_BOOL, &C.seek, 0, 1 },
    { "Retreat", "Retirada", "Con poca vida, sin pociones de curacion y enemigos cerca, se aleja de ellos.", ST_BOOL, &C.retreat, 0, 1 },
    { "RetreatPct", "Retirada por debajo de (%)", "Porcentaje de vida al que empieza la retirada.", ST_PERCENT, &C.retreatPct, 5, 60 },
    { "RetreatDist", "Distancia de retirada", "Cuanto se aleja cada vez (42.5 = 1 m).", ST_FLOAT, &C.retreatDist, 100, 1000 },
    { "PreferTough", "Priorizar enemigos duros", "Los enemigos con mucha vida (jefes, campeones) que te atacan van antes.", ST_BOOL, &C.preferTough, 0, 1 },
    { "Ranged", "Disparar con arma a distancia", "Con arco, ballesta o arma de fuego dispara como lo hace el juego (si no, ataque normal).", ST_BOOL, &C.ranged, 0, 1 },
    { "StickFactor", "Fidelidad al objetivo", "Se queda con el enemigo actual salvo que otro puntue este factor mejor.", ST_FLOAT, &C.stick, 1.0f, 3.0f },
    { "LookMinMs", "Mirar cada (min ms)", "Cada cuanto evalua la situacion (minimo).", ST_INT, &C.lookMin, 50, 2000 },
    { "LookMaxMs", "Mirar cada (max ms)", "Cada cuanto evalua la situacion (maximo).", ST_INT, &C.lookMax, 50, 2000 },
    { "ReactMinMs", "Reaccion (min ms)", "Retraso antes de cambiar de objetivo sin amenaza.", ST_INT, &C.reactMin, 0, 3000 },
    { "ReactMaxMs", "Reaccion (max ms)", "Retraso antes de cambiar de objetivo sin amenaza (maximo).", ST_INT, &C.reactMax, 0, 3000 },
    { "ReactThreatMinMs", "Reaccion con amenaza (min ms)", "Retraso cuando algo te ataca o se acerca.", ST_INT, &C.reactThreatMin, 0, 3000 },
    { "ReactThreatMaxMs", "Reaccion con amenaza (max ms)", "Retraso cuando algo te ataca o se acerca (maximo).", ST_INT, &C.reactThreatMax, 0, 3000 },
    { "ReissueMin", "Repetir orden (min s)", "Repite la orden sobre el mismo objetivo a intervalos irregulares.", ST_FLOAT, &C.reissueMin, 0.3f, 10.0f },
    { "ReissueMax", "Repetir orden (max s)", "Maximo del intervalo de repeticion.", ST_FLOAT, &C.reissueMax, 0.3f, 10.0f },
};

#define MAX_TRACK 32
static struct {
    unsigned target, pending;
    float lookIn, reactIn, reissueIn;
    int threat, wasActive, attacks;
    struct { unsigned ref; float dist; } seen[MAX_TRACK];
    int nSeen;
} A;

static int resolve(void)
{
    int ok = 1;
    ok &= mk_resolve(E.logic, &attackCtor, "??0sEventAttack@@QAE@II@Z", 1);
    ok &= mk_resolve(E.logic, &attackDtor, "??1sEventAttack@@UAE@XZ", 1);
    ok &= mk_resolve(E.logic, &addAttacker, "?addAttacker@cEntityCreature@@QAEEIM@Z", 1);
    if (ok) {
        mk_resolve(E.logic, &isLongrange, "?isUsingLongrangeWeapon@cEntityCreature@@QBE_NIPAI_NPBVcWorldPosition@@@Z", 0);
        mk_resolve(E.logic, &mapWeaponSpell, "?mapLongRangeWeaponToSpell@cMagicMgr@@QBE?AW4eSpecialSpells@@I@Z", 0);
        mk_resolve(E.logic, &magicMgr, "?_instance@?$tSingleton@VcMagicMgr@@@Orc@@0PAVcMagicMgr@@A", 0);
        mk_resolve(E.logic, &castCtor, "??0cEventCastSpell@@QAE@IE@Z", 0);
        mk_resolve(E.logic, &castDtor, "??1cEventCastSpell@@UAE@XZ", 0);
        mk_resolve(E.logic, &castSetTarget, "?setTargetRef@cEventCastSpell@@QAEXI@Z", 0);
    }
    return ok;
}

/* Ranged weapon: cast the weapon's special spell like the AI. Returns 0 to fall back to melee. */
static int order_ranged(unsigned hero, unsigned target)
{
    unsigned char ev[0x200] __attribute__((aligned(16)));
    void *h = mk_entity(hero), *t = mk_entity(target), *mm;
    unsigned weapon = 0;
    int special, slot = 0;
    if (!isLongrange || !mapWeaponSpell || !magicMgr || !castCtor || !castDtor || !castSetTarget) return 0;
    if (!h || !t || !(mm = *magicMgr)) return 0;
    if (!isLongrange(h, target, &weapon, 1, E.getPosition(t))) return 0;
    special = mapWeaponSpell(mm, weapon);
    if (special < 0 || special >= SPECIAL_SPELL_LIMIT) return 0; /* the AI attacks normally then */
    (*(Fn_QueryStatus *)*(void **)h)(h, QS_SPECIAL_SPELL_SLOT, special, &slot, 0);
    memset(ev, 0, sizeof ev);
    castCtor(ev, hero, (unsigned char)slot);
    castSetTarget(ev, target);
    mk_push(ev, castDtor);
    return 1;
}

/* Mirrors s2logicai FUN_10005a01: stack sEventAttack, pushEvent(ev, 0), target->addAttacker(hero, 2.5). */
static void order_attack(unsigned hero, unsigned target)
{
    unsigned char ev[0x100] __attribute__((aligned(16)));
    void *t = mk_entity(target);
    if (C.ranged && order_ranged(hero, target)) { A.attacks++; return; }
    memset(ev, 0, sizeof ev);
    attackCtor(ev, hero, target);
    mk_push(ev, attackDtor);
    if (t) addAttacker(t, hero, 2.5f);
    A.attacks++;
}

static float last_dist(unsigned ref)
{
    for (int i = 0; i < A.nSeen; i++) if (A.seen[i].ref == ref) return A.seen[i].dist;
    return -1.0f;
}

/* Lower is better: attackers and enemies closing in first, tough attackers before weak ones. */
static float score(const Ctx *c, unsigned ref, void *ent, float dist, int *threat)
{
    float s = dist, prev = last_dist(ref);
    int attacking = mk_is_attacking_hero(c->hero, ref);
    int closing = prev > 0.0f && dist < prev - 8.0f;
    if (attacking) s *= 0.35f;
    else if (closing) s *= 0.6f;
    if (attacking && C.preferTough && c->maxHp > 0.0f) {
        float mhp = mk_health(ent, HEALTH_MAX);
        if (mhp > c->maxHp * 1.5f) s *= 0.7f;          /* champion / boss */
    }
    if (S.danger) s /= 1.0f + 10.0f * S.danger(E.getType(ent));  /* learned: hard hitters first */
    if (attacking || closing || dist < 120.0f) *threat = 1;
    return s;
}

/* ------------------------------------------------------------------ retreat */
static struct { int active; float timer, reissue, startHp; int n; } RT;

/* Walk away from the enemies around: opposite of the sum of directions toward them. */
static int retreat_step(const Ctx *c)
{
    float he, hn, ve = 0.0f, vn = 0.0f, len;
    int n = 0;
    mk_ground(mk_pos(c->hero), &he, &hn);
    for (unsigned i = 0; i < 256; i++) {
        const EntityNear *e = E.getNearListElement(c->hero, i, NEAR_HOSTILE | NEAR_ALIVE);
        void *ent;
        float ee, en, dd;
        if (!e) break;
        if (!e->hostile || e->notAlive || e->dist > 450.0f) continue;
        ent = mk_entity(e->ref);
        if (!mk_alive(ent)) continue;
        mk_ground(mk_pos(ent), &ee, &en);
        dd = mk_sqrt((ee - he) * (ee - he) + (en - hn) * (en - hn));
        if (dd < 1.0f) continue;
        ve += (he - ee) / dd; vn += (hn - en) / dd; n++;
    }
    if (!n) return 0;
    len = mk_sqrt(ve * ve + vn * vn);
    if (len < 0.01f) { ve = 1.0f; vn = 0.0f; len = 1.0f; }    /* surrounded evenly: pick a side */
    return mk_order_move_offset(c->heroRef, c->hero, ve / len * C.retreatDist, vn / len * C.retreatDist);
}

static void retreat_end(const char *why)
{
    if (!RT.active) return;
    RT.active = 0;
    S.busy = 0;
    logmsg("retreat: end (%s)", why);
}

/* Returns 1 while retreating (no attack orders then). */
static int retreat_tick(const Ctx *c)
{
    float pct;
    int close;
    if (!C.retreat || c->maxHp <= 0.0f) { retreat_end("off"); return 0; }
    pct = 100.0f * c->hp / c->maxHp;
    close = mk_hostiles_within(c->hero, 300.0f);
    if (!RT.active) {
        if (pct > C.retreatPct || S.healPotions > 0 || !close) return 0;
        if (!retreat_step(c)) return 0;            /* up axis not learned yet / no helpers */
        RT.active = 1; RT.timer = 12.0f; RT.reissue = rnd_range(1.6f, 2.4f); RT.startHp = c->hp; RT.n++;
        A.target = A.pending = 0;
        S.busy = 1;
        logmsg("retreat: start (hp %.0f%%)", pct);
        return 1;
    }
    RT.timer -= c->dt; RT.reissue -= c->dt;
    if (RT.timer <= 0.0f) { retreat_end("time"); return 0; }
    if (!close) { retreat_end("no enemy close"); return 0; }
    if (S.healPotions > 0) { retreat_end("potion found"); return 0; }
    if (c->hp >= RT.startHp + c->maxHp * 0.15f) { retreat_end("health back"); return 0; }
    if (RT.reissue <= 0.0f) { retreat_step(c); RT.reissue = rnd_range(1.6f, 2.4f); }
    return 1;
}

static unsigned pick_enemy(const Ctx *c, float *bestScore, float *curScore)
{
    unsigned best = 0;
    int threat = 0, n = 0;
    struct { unsigned ref; float dist; } now[MAX_TRACK];
    *bestScore = 1e30f; *curScore = 1e30f;

    for (unsigned i = 0; i < 256; i++) {
        const EntityNear *e = E.getNearListElement(c->hero, i, NEAR_HOSTILE | NEAR_ALIVE);
        void *ent;
        float s;
        if (!e) break;
        if (!e->hostile || e->notAlive || e->dist > C.range) continue;
        ent = mk_entity(e->ref);
        if (!mk_alive(ent)) continue;
        if (!C.seek && !mk_is_attacking_hero(c->hero, e->ref)) continue;
        s = score(c, e->ref, ent, e->dist, &threat);
        if (e->ref == A.target) *curScore = s;
        if (s < *bestScore) { *bestScore = s; best = e->ref; }
        if (n < MAX_TRACK) { now[n].ref = e->ref; now[n].dist = e->dist; n++; }
    }
    memcpy(A.seen, now, sizeof(now[0]) * (size_t)n);
    A.nSeen = n;
    A.threat = threat;
    return best;
}

static void reset(void) { A.target = A.pending = 0; S.target = 0; S.threat = 0; retreat_end("player"); }

static void tick(const Ctx *c)
{
    float bestScore, curScore;
    /* automation switched off (key or menu) while fighting: stop the swing once */
    if (A.wasActive && !S.active && A.target) { mk_order_stop(c->heroRef, c->hero); reset(); }
    A.wasActive = S.active;
    if (!C.enabled || !S.autoOk) { retreat_end("paused"); return; }
    if (retreat_tick(c)) { S.target = 0; S.threat = 1; return; }

    A.lookIn -= c->dt; A.reactIn -= c->dt; A.reissueIn -= c->dt;
    if (A.lookIn <= 0.0f) {
        unsigned best = pick_enemy(c, &bestScore, &curScore);
        A.lookIn = rnd_ms(C.lookMin, C.lookMax);
        if (!best) {
            if (A.target) mk_order_stop(c->heroRef, c->hero); /* last enemy gone */
            A.target = A.pending = 0;
        } else if (best != A.target && best != A.pending) {
            int keep = A.target && curScore < 1e29f && bestScore * C.stick > curScore;
            if (!keep) {
                A.pending = best;
                A.reactIn = A.threat ? rnd_ms(C.reactThreatMin, C.reactThreatMax) : rnd_ms(C.reactMin, C.reactMax);
            }
        }
    }

    if (A.pending && A.reactIn <= 0.0f && S.castLock <= 0.0f) {
        if (mk_alive(mk_entity(A.pending))) {
            order_attack(c->heroRef, A.pending);
            logmsg("attack %08x%s", A.pending, A.threat ? " (threat)" : "");
            A.target = A.pending;
            A.reissueIn = rnd_range(C.reissueMin, C.reissueMax);
        }
        A.pending = 0;
    } else if (A.target && A.reissueIn <= 0.0f && S.castLock <= 0.0f && mk_alive(mk_entity(A.target))) {
        order_attack(c->heroRef, A.target);
        A.reissueIn = rnd_range(C.reissueMin, C.reissueMax);
    }
    S.target = A.target ? A.target : A.pending;
    S.threat = A.threat;
}

static const char *status(void)
{
    if (!C.enabled) return "Ataque: desactivado";
    if (RT.active) return "Ataque: retirada (poca vida, sin pociones)";
    if (A.target) return A.threat ? "Ataque: luchando (amenaza)" : "Ataque: luchando";
    return "Ataque: vigilando";
}

static void dump(const Ctx *c)
{
    (void)c;
    logmsg("autoattack: target=%08x pending=%08x threat=%d seen=%d orders=%d retreats=%d seek=%d", A.target, A.pending, A.threat, A.nSeen, A.attacks, RT.n, C.seek);
}

MK_MODULE(mod_autoattack) = {
    "Autoataque", "AutoAttack", settings, sizeof settings / sizeof settings[0], 30,
    0, resolve, 0, tick, reset, dump, status, 0, 0,
};
