# Engine notes (Sacred 2 Gold, Steam)

Everything the ModKit calls was found by reverse engineering the game's DLLs with
[REA](https://github.com/morluto/rea) (Ghidra 12.1.4 backend). No game file is patched
and none is included in this repository.

## How the ModKit hooks in

| | |
|---|---|
| Loading | Orc plugin: `system/plugins.cfg` → `Plugin=Plugin_ModKit`. The loader calls `dllStartPlugin` / `dllStopPlugin`. |
| Calls into the game | Mangled C++ exports of `s2core.dll`, `s2logic.dll`, `s2render.dll`, `orcSystem.dll`, resolved with `GetProcAddress`. A missing export disables only the mod that needs it. |
| Per-frame tick | `Orc::Root::addFrameListener`. The plugin clones `??_7FrameListener@Orc@@6B@` and replaces slot 1 (`frameStarted(const FrameEvent&)`, `FrameEvent+4` = seconds). |
| Orders to the hero | Engine events on the stack, `tSingleton<cKernel>::_instance->pushEvent(ev, 0.0f)`, then the destructor. `pushEvent` serialises a copy (same as `s2logicai`). |
| Menu | D3D9 device from `tSingleton<cDX9Advance>::InstancePtr()` → `cDX9Base::getDevice()` / `getHwnd()`. The menu hooks `Present` (vtable 17) and `Reset` (16) in memory and subclasses the window. |
| Executable | `sacred2.exe` is SteamStub-packed, so the analysis targets the DLLs. |

## Entities

| Piece | Symbol / data |
|---|---|
| Local hero | `cObjectMgr::getHeroRef(0)` → `getDynamic(ref)` |
| Query interface | `vtable[0] = queryStatus(cmd, a, b, c)`: `0x58` health_get, `0x6f` isDead, `0x70` alive, `0x72`/`0x78` hostile, `0x9f` useItem, `0xe4` special-spell slot, `0xec` countPotionUsage |
| Near list | `cEntity::getNearListElement(i, flags)`, 0x24-byte entries: `+0` ref, `+4` distance, `+0xc` hostile (flag 2), `+0xf` not alive (flag 0x100). Every dynamic entity around, not only enemies. |
| Attackers of the hero | `cEntityCreature +0x3b0/+0x3b4`: vector of `{ref, float}`, capped at 5 |
| Dead | `cEntity::getSyncFlags() & 2` |
| Health | `cEntityCreature::query_creatureHealth_get(eHealth)`: `0` current, `1` maximum |
| Type checks | `cTypeMgr::isItem / isCreature / isClickable(type)` |
| Mount | `cEntity::getCarrierRef()` (non-zero when riding) |

## Orders

| Order | How |
|---|---|
| Attack | `sEventAttack(attacker, target)` (0x38 bytes), then `target->addAttacker(attacker, 2.5)`. Copied from `s2logicai` `FUN_10005a01`. |
| Ranged weapon | Same function: `isUsingLongrangeWeapon(target, &weapon, true, &targetPos)` → `cMagicMgr::mapLongRangeWeaponToSpell(weapon)`; if `< 0xb`, `queryStatus(0xe4, spell, &slot, 0)` gives the slot and the hero casts `cEventCastSpell(hero, slot)` + `setTargetRef(target)`. Otherwise a normal attack. |
| Stop | `cEventMoveTo(hero, hero's own cWorldPosition)`: an attack order otherwise keeps the hero swinging. |
| Move | `cEventMoveTo(entity, const cWorldPosition&)` (more than 0xd0 bytes) |
| Loot | `cEventAutoCollect(hero, rareSelf, rareOther, silent)`. Server side `filterItemsInRange`: radius 160, 50 when silent; rarity 0-3 = options `autoCollectRareSelf/Other`; each item becomes `cEventItemTransfer(hero, item, 0, 2, false)`. |
| Use item / potion | `cEventItemTransfer(hero, item, 0xc, 2, false)`; the server checks `hasItem`, then `queryStatus(0x9f)`. |
| Potion belt | `getInventory(hero, 2)` used directly as `cItemInventory*`, `getPotionSlot(0..5, &bp, &item, &count)` |
| Combat art | `cEventCastSpell(hero, slot)` + `setTargetRef(target)` + `setTargetPos(target pos)` + `setCombo(bool)`. Copied from `s2logicai` `FUN_10005ec0` (the AI keeps the event in 92 bytes). |

## Combat arts

| | |
|---|---|
| Slots | `cEntityCreature::getMagicSlots()` returns an stlport `vector<cMagicSlotData>` (`{begin, end, cap}`); element size **0x80** (the AI divides with `>> 7`). |
| Per slot | `getMagicEntry`, `getCurrentCoolDown` (0 = ready), `getCoolDown` (full), `getEffectRange`, `isBuff`, `isCombo`, `isFormSpell`, `isAttackSpell(bool)`, `getSpellName` |
| Per entry | `cMagicEntry::isHealSpell`, `getFightDist`, `isMove`, `canUseTargetRef` |
| Buff state | `cMagicMgr::isBuffActive(slot, &ref, 0, 0)`, as the AI calls it in `FUN_1008735d`. |
| Mounted | The AI unmounts first (`cEventUnmount`) when the spell has token `0xe7` and the mount is special. |

## Rendering / config

| | |
|---|---|
| Camera | Table `camera` (`cGameConfigMgr::initCamera`) and `options.cameraConfig*` (integers in metres, `initOptions`). `camera.cameraConfig*` is ignored. |
| Fog | `render.fog` (distance fog). Weather fog (`weatherFog`) lives in `s2logic`. |
| Profile | The game reads `%LOCALAPPDATA%\Ascaron Entertainment\Sacred 2\optionsCustom.txt` on top of `options.txt`. |

## Keys the game already uses

F1-F5 weapon sets, F7 save, F8 home teleport, F9-F12 orbs (F11 also hides the HUD),
Space heal, Q auto-collect, X special pet, 1-5 combat arts, 6-8 buffs, R T Y U I O potions.
So the ModKit uses Insert (automation), Home (menu) and End (dump).

## Open questions

1. Chests and containers: their family/flags, from the End dump in the game, then `cEventInteract`.
2. The game's own healing-potion cooldown (`sCreatureStats::getHealPotionReuseTimeout`): where `sCreatureStats` lives.
3. Whether the window messages the menu swallows are all the game reads (if it uses DirectInput, the mouse also reaches the game while the menu is open).
4. FOV / HUD at 32:9 in `s2render`, only if the game shows problems.
