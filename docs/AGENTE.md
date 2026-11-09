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
| 5 | Exploración por pasos hacia lo menos visto, comprobando que el destino se puede pisar | Movimiento: `cEventMoveTo` a un desplazamiento (ya en el núcleo). Comprobar si se puede pisar: navegación **Kynapse** (`cAIManager::getCollisionWrapper` → `iKynapseCollision`, `getLayers` → `CAiMeshLayer`) | **Por hacer.** Falta la prueba de «se puede pisar» con Kynapse. Mientras tanto: paso hacia lo menos visto, y si el héroe no avanza, esa casilla se marca bloqueada (como en Sacred Gold). |
| 6 | Memoria del mapa: archivo de texto, por capa, bloques de 32x32, 7 bytes por casilla | Rejilla propia en unidades del suelo (por ejemplo 1 casilla = 1 m = 42,5 unidades) y capa de `cWorldPosition::getLayer` | **Por hacer.** Formato y tamaño como en Sacred Gold para poder compartir el código. |
| 7 | Radar redondo orientado como la pantalla: terreno, rastro, enemigos, botín, hallazgos y enciclopedia | Dibujado con ImGui en el menú; la orientación sale de la cámara (`cCameraController` en `s2render`) | **Por hacer.** Al principio orientado al norte; el giro con la cámara después. |
| 8 | Ir a cualquier punto: clic en el mapa del menú, A* por sectores y memoria, por tramos | Sacred 2 ya busca su propio camino (`cEventMoveTo` → `CreatureAIPath`); para lejos, A* sobre la memoria del mapa por tramos | **Por hacer**, después de 6 y 7. |
| 9 | Todo se elige desde el menú (Inicio) | Tabla `Setting[]` por mod: el menú y el `.ini` se generan solos | **Hecho** para todo lo de arriba. |

## Orden propuesto

Primero lo más directo en Sacred 2:

1. Probar en partida lo hecho ([PRUEBAS.md](PRUEBAS.md)), sobre todo:
   - la orden de clic (recogida);
   - el eje vertical (retirada);
   - el volcado con Fin junto a cofres, postes y portales.
2. **Memoria del mapa** y **radar** (piezas 6 y 7): solo leen posiciones y no necesitan más REA.
3. **Exploración** (pieza 5), primero con «marcar bloqueado si no avanza» y después con la prueba de Kynapse.
4. **Ir a cualquier punto** (pieza 8).
5. **Equipo** (pieza 4), después de la ingeniería inversa del inventario.
