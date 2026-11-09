/*
 * Sacred 2 Gold ModKit - shared core interface.
 *
 * The ModKit is one Orc plugin (Plugin_ModKit.dll) made of a core and independent
 * mod modules (src/mod_*.c / .cpp). Each module registers itself with MK_MODULE();
 * build.ps1 generates the module registry from whatever mod files are present, so
 * every mod branch builds on its own and main builds them all.
 *
 * Engine entry points are resolved by exported mangled name (GetProcAddress); no
 * game binary is patched. See docs/ENGINE.md for where each one comes from.
 */
#ifndef MODKIT_H
#define MODKIT_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MK_VERSION "1.0.0"

/* ------------------------------------------------------------------ engine ABI */
typedef struct { float x, y, z; } Vec3;

/* sEntityNear (s2core cEntity::getNearListElement): 0x24 bytes, fields filled per flag. */
typedef struct {
    unsigned ref;           /* +0x00 */
    float dist;             /* +0x04 */
    unsigned visType;       /* +0x08 (flag 4) */
    unsigned char hostile;  /* +0x0c (flag 2) */
    unsigned char b0d;      /* +0x0d (flag 0x40) */
    unsigned char b0e;
    unsigned char notAlive; /* +0x0f (flag 0x100) */
    unsigned char rest[0x24 - 0x10];
} EntityNear;
#define NEAR_HOSTILE 0x002
#define NEAR_ALIVE   0x100

typedef unsigned (__thiscall *Fn_GetHeroRef)(void *objMgr, unsigned idx);
typedef void *(__thiscall *Fn_GetDynamic)(void *objMgr, unsigned ref);
typedef const EntityNear *(__thiscall *Fn_GetNearListElement)(void *entity, unsigned idx, unsigned flags);
typedef unsigned (__thiscall *Fn_GetU32)(void *entity);
typedef void *(__thiscall *Fn_GetPosition)(void *entity);
typedef Vec3 *(__thiscall *Fn_GetAbsPosition)(void *wpos, Vec3 *out);
typedef unsigned char (__thiscall *Fn_TypeIs)(void *typeMgr, unsigned type);
typedef void (__thiscall *Fn_PushEvent)(void *kernel, void *event, float delay);
typedef void (__thiscall *Fn_Dtor)(void *self);
typedef void *(__thiscall *Fn_EventMoveToCtor)(void *self, unsigned entity, const void *wpos);
typedef float (__thiscall *Fn_HealthGet)(void *creature, int which);

/* Core engine exports every module may use (resolved before any module tick). */
typedef struct {
    void ***objMgrInstance, ***kernelInstance, ***typeMgrInstance;
    Fn_GetHeroRef getHeroRef;
    Fn_GetDynamic getDynamic;
    Fn_GetNearListElement getNearListElement;
    Fn_GetU32 getSyncFlags, getType, getFamily, getSubFamily, getFlags;
    Fn_GetPosition getPosition;
    Fn_GetAbsPosition getAbsPosition;
    Fn_TypeIs isItem, isCreature, isClickable;
    Fn_PushEvent pushEvent;
    Fn_EventMoveToCtor eventMoveToCtor; Fn_Dtor eventMoveToDtor;
    Fn_HealthGet healthGet;            /* optional */
    /* cWorldPosition helpers (s2core) */
    void *(__thiscall *wposCopy)(void *self, const void *other);
    void (__thiscall *wposDtor)(void *self);
    void (__thiscall *wposAdd)(void *self, float x, float y, float z);
    float (__thiscall *wposDist2D)(const void *self, const void *other);
    void (__thiscall *moveSetTarget)(void *ev, unsigned ref);   /* cEventMoveTo::setTargetRef */
    HMODULE core, logic, render;
} Engine;
extern Engine E;

#define SYNCFLAG_DEAD  0x2   /* cEntityCreature::query_isDead */
#define HEALTH_CURRENT 0
#define HEALTH_MAX     1
#define CREATURE_ATTACKERS_BEGIN 0x3b0  /* vector<{uint ref; float t;}> */
#define CREATURE_ATTACKERS_END   0x3b4

/* ------------------------------------------------------------------ settings */
typedef enum { ST_BOOL, ST_INT, ST_FLOAT, ST_KEY, ST_PERCENT } SetType;
typedef struct {
    const char *key;     /* ini key */
    const char *label;   /* menu label (Spanish, shown to the player) */
    const char *help;    /* menu tooltip / ini comment */
    SetType type;
    void *ptr;           /* int* (BOOL/INT/KEY) or float* (FLOAT/PERCENT) */
    float min, max;
} Setting;

/* ------------------------------------------------------------------ modules */
typedef struct {
    float dt;
    unsigned heroRef;
    unsigned char *hero;
    float hp, maxHp;     /* 0 when health is unavailable */
} Ctx;

typedef struct Module {
    const char *name;        /* menu title */
    const char *section;     /* ini section */
    const Setting *settings;
    int nSettings;
    int order;               /* lower ticks first */
    void (*init)(void);                  /* after config load (exports may not be loaded yet) */
    int  (*resolve)(void);               /* engine loaded: resolve own exports, 0 disables module */
    void (*frame)(float dt);             /* every frame, focus or not */
    void (*tick)(const Ctx *c);          /* every frame with a live hero, focus, automation gate passed per module */
    void (*reset)(void);                 /* player took over / hero died: drop pending orders */
    void (*dump)(const Ctx *c);          /* debug dump key */
    const char *(*status)(void);         /* one-line status for the overlay, may be NULL */
    void (*stop)(void);                  /* plugin unload */
    int disabled;                        /* set by core when resolve fails */
    int (*info)(int line, char *buf, int n); /* extra menu text lines (0 when no more), may be NULL */
} Module;

#define MK_MODULE(var) Module var   /* build.ps1 greps MK_MODULE(name) to build the registry */

/* ------------------------------------------------------------------ shared state */
typedef struct {
    int active;          /* automation ON (toggle key); potions run regardless */
    float sinceInput;    /* seconds since the player clicked or pressed a move key */
    float inputPause;    /* automation waits this long after player input */
    int menuOpen;        /* ImGui menu visible: player input is for the menu */
    int autoOk;          /* active && no recent player input: automation may give orders */
    unsigned target;     /* auto-attack target (0 none) */
    int threat;          /* something attacks the hero or closes in */
    float castLock;      /* seconds left of a combat art animation: do not re-issue attacks */
    int healPotions;     /* healing potions left in the configured slots, -1 unknown (no potion mod) */
    float sinceHurt;     /* seconds since the hero last lost health */
    float standing;      /* seconds the hero has not moved */
    int busy;            /* a module is walking the hero somewhere (loot, retreat, explore): others wait */
    float (*danger)(unsigned type); /* learned danger of an enemy type (0..), NULL without the learn mod */
    int upAxis;          /* vertical axis of absolute positions (0 x, 1 y, 2 z), -1 until learned at runtime */
    int nModules;
    Module **modules;
} Shared;
extern Shared S;

/* ------------------------------------------------------------------ core helpers */
void logmsg(const char *fmt, ...);
const char *mk_dir(void);                 /* plugin directory with trailing backslash */
void mk_save_config(void);                /* write every module's settings to the ini */
void mk_reload_config(void);

unsigned rnd(void);
float rnd_range(float lo, float hi);
float rnd_ms(int lo, int hi);

typedef struct { int vk, down; } Key;
int key_pressed(Key *k);
int game_has_focus(void);
const char *key_name(int vk);

void *mk_entity(unsigned ref);
int mk_alive(void *ent);
Vec3 mk_pos(void *ent);
float mk_dist(void *a, void *b);
float mk_sqrt(float v);
int mk_is_attacking_hero(unsigned char *hero, unsigned ref);
int mk_type_is(Fn_TypeIs fn, void *ent);
int mk_hostiles_within(unsigned char *hero, float radius);
int mk_hostiles_near_entity(unsigned char *hero, void *center, float radius);
float mk_health(void *creature, int which);   /* -1 if unavailable */

void mk_push(void *ev, Fn_Dtor dtor);         /* pushEvent(ev, 0) then destroy the stack copy */
void mk_order_move(unsigned hero, void *towards);
void mk_order_stop(unsigned heroRef, void *hero);
/* The order a click on an object gives: walk to it and use it (pick up, open...). */
int mk_order_interact(unsigned heroRef, unsigned targetRef);
/* Walk to the hero's position shifted by (east, north) in world units on the ground plane. */
int mk_order_move_offset(unsigned heroRef, void *hero, float east, float north);
float mk_dist2d(void *a, void *b);           /* ground-plane distance (game's getAbsDistanceTo2D) */
void mk_ground(Vec3 v, float *east, float *north); /* project an absolute position on the ground plane */

/* resolve helpers: return 1 if found */
int mk_resolve(HMODULE m, void *var, const char *name, int required);

#ifdef __cplusplus
}
#endif
#endif
