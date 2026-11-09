# El agente de Sacred Gold en Sacred 2

Lista de lo que hace el agente del [Sacred Gold ModKit](https://github.com/Rampere-98/Sacred-Gold-ModKit) (rama
`modkit`) y cómo se traslada a Sacred 2.

**Las reglas del dueño valen igual:**
- El agente nunca depende del ratón ni de las teclas del jugador.
- No se toca nada que proteja el juego: nada de escribir oro ni estadísticas.
- La memoria debe ser compacta, porque el juego es de 32 bits.
- Nunca se guarda sobre la partida del jugador.

En Sacred 2 todo pasa por **eventos del motor**, igual que la IA del juego. No hace falta simular clics ni pulsar
teclas.

Estado a 2026-11-09:

| # | Pieza (Sacred Gold) | En Sacred 2 | Estado |
|---|---|---|---|
| 1 | Autoataque con criterio de jugador: solo quien te ataca o «buscar combate», reacción, foco, retirada | `sEventAttack` + lista de atacantes; `Seek`; retirada con `cEventMoveTo` a un desplazamiento | **Hecho** (sin probar en partida). La retirada usa el eje vertical aprendido en el juego. |
| 1b | Artes y consumibles (Tactics) | `cEventCastSpell` + `cMagicSlotData` (enfriamiento, buff, cura, área) | **Hecho**: curación, buffs sin apagarlos nunca, área con grupos, artes fuertes para jefes. Las pociones de mejora son opcionales. |
| 2 | Aprende de los combates: peligro por tipo y enciclopedia con sitio | Vida perdida repartida entre los atacantes; `getType`; posición en el suelo | **Hecho** (`mod/combat-learn`). Faltan los nombres de los tipos: el juego solo da su id. |
| 3 | Recogida propia, uno a uno con la orden de un clic; cofres y escondites; tras el combate o siempre | `cEventMoveTo` + `setTargetRef(objeto)`, que es lo que manda un clic (el servidor pasa el destino con `queryStatus(0x184)`) | **Hecho para objetos.** Los cofres se pueden activar, pero son experimentales hasta identificarlos con el volcado (Fin). |
| 4 | Equipo: puntuar piezas, equipar lo mejor, vender y comprar | Pendiente de REA en `s2logic`: inventario (`getInventory`, `cItemInventory`) y `cEventItemTransfer` con los contextos de equipar y vender | **Por hacer.** Primero ingeniería inversa de equipar, vender y comprar, y de las estadísticas de los objetos. Mover objetos por el servidor es legítimo; no se escriben valores. |
| 5 | Exploración por pasos hacia lo menos visto, comprobando que el destino se puede pisar | Movimiento: `cEventMoveTo` a un desplazamiento (ya en el núcleo). Comprobar si se puede pisar: navegación **Kynapse** (`cAIManager::getCollisionWrapper` → `iKynapseCollision`, `getLayers` → `CAiMeshLayer`) | **Hecho sin Kynapse** (`mod/explore`, sin probar en partida): pasos de 6, 10 o 14 m en 16 direcciones hacia lo menos pisado, con el coste de Sacred Gold; si el héroe no avanza, se marca bloqueado. Falta la prueba de «se puede pisar» con Kynapse para descartar destinos antes de ir. |
| 6 | Memoria del mapa: archivo de texto, por capa, bloques de 32x32, 7 bytes por casilla | Rejilla propia en unidades del suelo (por ejemplo 1 casilla = 1 m = 42,5 unidades) y capa de `cWorldPosition::getLayer` | **Hecho** (`mod/map-memory`, sin probar en partida): visitas por casilla y capa, avistamientos de enemigos, guardado atómico cada 60 s en `Plugin_ModKit-map.txt`. Faltan el color del suelo, los hallazgos (`L`) y los nombres (`N`). |
| 7 | Radar redondo orientado como la pantalla: terreno, rastro, enemigos, botín, hallazgos y enciclopedia | Dibujado con ImGui en el menú; la orientación sale de la cámara (`cCameraController` en `s2render`) | **Hecho, orientado al norte** (en `mod/map-memory`, dibujado con la API 2D del menú). Falta girarlo con la cámara. |
| 8 | Ir a cualquier punto: clic en el mapa del menú, A* por sectores y memoria, por tramos | Sacred 2 ya busca su propio camino (`cEventMoveTo` → `CreatureAIPath`); para lejos, A* sobre la memoria del mapa por tramos | **Por hacer**, después de 6 y 7. |
| 9 | Todo se elige desde el menú (Inicio) | Tabla `Setting[]` por mod: el menú y el `.ini` se generan solos | **Hecho** para todo lo de arriba. |

## Orden propuesto

Primero lo más directo en Sacred 2:

1. Probar en partida lo hecho ([PRUEBAS.md](PRUEBAS.md)), sobre todo:
   - la orden de clic (recogida);
   - el eje vertical (retirada);
   - el volcado con Fin junto a cofres, postes y portales.
2. ~~**Memoria del mapa** y **radar** (piezas 6 y 7)~~: hechos; falta probarlos en partida.
3. ~~**Exploración** (pieza 5) con «marcar bloqueado si no avanza»~~: hecha; falta probarla en partida y, después, la prueba de Kynapse.
4. **Ir a cualquier punto** (pieza 8).
5. **Equipo** (pieza 4), después de la ingeniería inversa del inventario.

## Memoria del mapa: formato de Sacred Gold (para compartir el código)

Lo facilitó la sesión de Sacred Gold (`src/game/map_memory.cpp`, rama `modkit` de sacred-bild).

**En memoria:**
- `unordered_map<uint64_t, unique_ptr<Chunk>>`.
- Clave de chunk: `(layer << 48) | (u16(cx) << 16) | u16(cy)`, con `cx = tx >> 5`.
- Cada chunk es `Cell[1024]` (32x32 casillas), con índice `((ty & 31) << 5) | (tx & 31)`.
- `Cell`, 7 bytes:

  | Campo | Significado |
  |---|---|
  | `visits` | veces pisada (satura en 255) |
  | `blocked` | veces que no se pudo pisar (satura) |
  | `r`, `g`, `b` | color medio del suelo |
  | `samples` | muestras de color (0 = sin color) |
  | `scan` | 0 sin escanear, 1 se puede pisar, 2 no se puede pisar |

  Una casilla es conocida si algún campo es distinto de 0.

**Archivo** (texto, sin cabecera, una línea por registro, los vacíos no se escriben):

| Registro | Línea |
|---|---|
| Casilla | `layer tx ty visits blocked RRGGBB samples scan`. Una línea de 7 columnas se lee con scan 0. |
| Hallazgo | `L kind layer tx ty`: kind 0 mercader, 1 entrada, 2 portal |
| Nombre de tipo | `N type nombre_sin_espacios` |
| Avistamiento | `E type layer tx ty`, deduplicado a 6 casillas por tipo; como mucho 48 por tipo y 6000 en total |

Se guarda como mucho cada 60 s y solo si hay cambios: primero en un archivo temporal, que luego se renombra.

**Uso:**
- **Exploración:** `cost = log2(1 + visitas del 3x3) + 1.5 × bloqueos`, sumando 2 por cada casilla de scan 2. Va hacia el coste más bajo.
- **Ruta A\*:**
  - Una casilla pisada o de scan 1 cuesta 10, y una desconocida 22.
  - No se pisan las de scan 2 ni las que tienen `blocked >= 2`.
  - La ventana llega hasta 1100 casillas.
- **Escaneo:** prueba de casilla a 96 casillas alrededor del héroe, como mucho 2500 por fotograma.

**Adaptación a Sacred 2:**
- Una casilla será 1 m, que son 42,5 unidades, en el plano del suelo.
- La capa sale de `cWorldPosition::getLayer`.
- El escaneo usará la navegación de Kynapse en lugar de la prueba de casilla.
