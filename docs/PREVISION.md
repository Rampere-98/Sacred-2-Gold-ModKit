# Previsión de combates

Diseño, todavía **sin implementar**, de una lógica que estime antes de cada pelea:
- la probabilidad de ganar con la build actual;
- la vida que quedará;
- las pociones que se gastarán;
- a quién conviene matar primero.

Estado a 2026-11-12: solo documento. Las reglas del dueño ([AGENTE.md](AGENTE.md)) valen igual: solo se **leen**
datos, nunca se escriben estadísticas.

## 1. La pelea como una carrera

- **Tiempo en matar (TTK)** = vida del enemigo ÷ el daño por segundo que le hace el héroe.
- **Tiempo en morir (TTD)** = vida efectiva del héroe ÷ el daño por segundo que recibe.
- **Vida efectiva** = vida actual + pociones × curación por poción + regeneración + artes de curación. Las pociones
  están limitadas por su enfriamiento (`PotionHealCooldown` en `balance.txt`).

Se gana si caen todos los enemigos antes de agotar la vida efectiva. La enciclopedia (`mod/combat-learn`) ya mide
el % de vida por segundo que quita cada tipo; falta la otra mitad, **el daño que el héroe hace a cada tipo**. Se mide
leyendo cómo baja la vida del enemigo (`queryStatus 0x58`) mientras es el objetivo.

## 2. El azar: Monte Carlo

Por tipo de enemigo se guarda cómo se reparte cada medida, no solo la media: el daño por golpe, el tiempo entre
golpes y la variación del daño propio. Antes de una pelea se simula unas 200 veces con esos datos, lo que cuesta
menos de un fotograma. Salen la probabilidad de ganar, la vida esperada al final y las pociones gastadas, cada una
con su margen.

## 3. Orden de objetivos: la regla de Smith

Contra un grupo, el daño total recibido es mínimo si se ataca primero a quien tenga el mayor cociente
**daño por segundo que hace ÷ tiempo que se tarda en matarlo**. Es la regla de Smith de planificación, exacta cuando
el daño de cada enemigo es estable, y mejor que elegir «al más fuerte» o «al más cercano».

**Invocadores:** el peso de un invocador es su propio daño más el de sus invocaciones, contado como invocaciones por
segundo × daño de cada una × lo que duraría la pelea. Con ese peso, la regla lo pone el primero sin un caso especial.

## 4. La build

- **Huella de build:** nivel, vida máxima y, cuando se sepan leer, arma, armadura y resistencias. Cada medida se
  guarda junto a su huella.
- **Al cambiar de build**, las medidas viejas pesan cada vez menos, pero no se borran.
- **Confianza:** con pocas peleas la previsión sale con un margen ancho («entre 40 y 80 %») que se estrecha con cada
  combate.

## 5. Comprobar que acierta (calibración)

Antes de cada pelea se apunta la previsión y, al acabar, el resultado. El menú muestra si está calibrada: de las
peleas con «70 %», ¿se ganaron unas 7 de cada 10?

## 6. Usos en el agente

- **Objetivo:** elegido por la regla de Smith, en lugar de las prioridades fijas.
- **Retirada:** cuando la probabilidad de ganar baje de un umbral, no con un % de vida fijo.
- **Artes fuertes y pociones:** se usan cuando cambian el resultado.
- **Exploración e ir a un punto:** evitan las zonas con enemigos de baja probabilidad de victoria.

## 7. Datos del juego que sirven de base (comprobado en disco)

Sacred 2 trae en texto plano, en `scripts/`, mucho de lo que en Sacred Gold está dentro del exe. La previsión puede
arrancar con un cálculo a partir de estos datos y afinarlo con lo medido en combate.

| Fichero | Qué aporta |
|---|---|
| `server/balance.txt` | Vida del héroe (`LifeBase` 130, `LifeStep` 19, `LifeAttribut` 25), daño y crítico (`AdjustDamageFactor`, `AdjustCriticalDamageFactor` 1200), resistencia a hechizos (`SpellResistFactor` 700), regeneración (`balanceLifeRegenerationTime`, factores de armadura), enfriamiento de pociones (`PotionHealCooldown`) y alcance de aggro (`RangeAggro` 400, `RangeAggroBoss` 750) |
| `server/balance.txt` (tablas de 5 dificultades) | `LifeFactorDiff`, `DamageFactorDiff`, `AttackFactorDiff`, `DefenseFactorDiff`, `Enemy_armor`, `Enemy_weapondamage`, `Enemy_spelldamage` |
| `server/balance.txt` (tablas `*MT`) | Multiplicadores de rango (normal, campeón, jefe): `DamageFactorMT`, `HitFactorMT`, `LifeQuotientMT`, `SpellAttackFactorMT`, `SpellDefenseFactorMT` |
| `server/creatures.txt` | Cada enemigo con su `dangerclass`, `rank`, comportamiento (`Enemy_mage`…), versión de élite y lista de hechizos |
| `shared/creatureinfo.txt` | Velocidades, distancias de combate y `dangerClass` por tipo |
| `shared/spells.txt` | Daño, enfriamiento y clase de cada hechizo |

**Invocadores detectables sin luchar:**
- `spells.txt` tiene 24 hechizos con `spellClass = "cSpellSpawnEnemy"`, y en `creatures.txt` hay 48 referencias a
  hechizos `enemy_spawn_*` o `enemy_summon_*`.
- Ejemplo: `Demon_WL_summoner` (`Enemy_mage`) lanza `enemy_spawn_fireskeleton`, que tiene un enfriamiento de 10 s y
  los tokens `et_summon_officer` y `et_summon_mylevel` (invocaciones del nivel del invocador).
- Para el agente: una tabla `id de criatura → invoca`, generada una vez desde estos ficheros. Así se evita leer los
  scripts en el juego.

**Lo que no está en los ficheros:**
- La fórmula exacta de impacto contra defensa y de daño contra armadura y resistencia. Necesita ingeniería inversa
  de `s2logic.dll`.
- Las estadísticas vivas del héroe y del enemigo: `sCreatureStats` y `getStats`, que ya están localizadas.

## 8. Orden de trabajo

1. **Medir** (solo observa, no cambia el comportamiento):
   - el daño del héroe a cada tipo;
   - el reparto de los golpes recibidos;
   - la huella de build.
2. **Prever:**
   - la simulación con los enemigos presentes;
   - un registro de previsión y resultado;
   - la calibración en el menú.
3. **Decidir:**
   - el objetivo por la regla de Smith;
   - la retirada por probabilidad de ganar;
   - evitar las zonas peligrosas.
4. **Invocadores:**
   - la tabla estática sacada de `creatures.txt` y `spells.txt`;
   - confirmarla en partida, viendo aparecer enemigos nuevos junto al invocador.
5. **Estadísticas del juego:** ingeniería inversa de atributos, nivel, resistencias y la fórmula de daño, para
   ajustar el modelo a los golpes observados y prever contra enemigos nunca vistos.

## 9. Sacred 2 frente a Sacred Gold

Lo comparten:
- el estudio (Ascaron) y el mundo (Ancaria);
- las artes de combate con runas y enfriamiento;
- el cinturón de pociones y la regeneración;
- cinco dificultades;
- campeones y jefes;
- las familias de enemigos.

Cambian:
- **Motor:** Sacred Gold es 2D isométrico en un solo exe; Sacred 2 es 3D, con el motor Orc repartido en DLLs que
  exportan nombres C++.
- **Datos:** dentro del exe en Sacred Gold; en scripts Lua de texto en Sacred 2.
- **Mods:** proxy `ddraw.dll` en Sacred Gold; plugins oficiales (`plugins.cfg`) en Sacred 2.
- **Daño:** Sacred 2 añade hielo y la T-Energía.

Para la previsión, la diferencia clave es esta: en Sacred 2 las tablas de balance y de criaturas se leen
directamente.
