# Sacred 2 Gold ModKit: especificación adaptada

Esta es la lista de mods del ModKit de Sacred Gold, reescrita para cómo se juega Sacred 2. Cada punto distingue entre lo **comprobado** (en los archivos del juego o con REA) y lo **por comprobar** en el juego. El detalle técnico está en [ENGINE.md](ENGINE.md) y las pruebas pendientes en [PRUEBAS.md](PRUEBAS.md).

## Qué cambia en Sacred 2 (comprobado en `options.txt` y en las DLL)

| | Sacred Gold | Sacred 2 Gold |
|---|---|---|
| Cámara | Fija, isométrica | En 3D, con giro y zoom libres (`viewleft`/`viewright`, rueda); hay un modo para mazmorras |
| Movimiento | Clic | Clic **o WASD** (`walkfwd` = W, `walkbwd` = S, `viewleft/right` = A/D) |
| Combate | Clic + artes con maná | Clic, o `forced_attack` (Mayúsculas). **Las artes de combate van en las teclas 1–5** y los **buffs en 6–8**; tienen tiempo de regeneración, no consumen maná. |
| Pociones | Clic en el cinturón | **Barra espaciadora = `heal`**: bebe la poción de curación sola. **6 ranuras de pociones con teclas R T Y U I O**. Opción `potionorder`. |
| Botín | Clic en cada objeto | Clic, o **Q = `autocollect`**: recoge todo lo que hay en un radio de 160 (unos 3,8 m), filtrado por rareza (`autoCollectRareSelf/Other`) |
| Puertas | Clic | `autoDoors = 1`: se abren solas |
| Equipo | Manual | `autoequip = 1` |
| Monturas | No | Caballo y monturas especiales (`mountslot` = 0); se combate a caballo |
| Acompañantes | No | Mascota especial (`call_specialpet` = X) y esbirros con modo y alcance de ataque (`toggle_minion_attackmode/range`) |
| Peligro del enemigo | No | `showDangerclass = 1`: el juego clasifica a cada enemigo por peligro |
| Armas | Una | 5 juegos de armas (F1–F5) |
| Teclas ocupadas por el juego | — | F1–F5 armas, F7 guardar, F8 teleporte a casa, **F9–F12 orbes**, **F11 también ocultar interfaz**, Espacio curar, Q recoger, X mascota |

**Teclas del ModKit en Sacred 2.** F10 y F11 están ocupadas, así que las del ModKit cambian:

| Tecla | Uso |
|---|---|
| **Insert** | Automatización activada/desactivada: autoataque, artes y recogida (en Sacred Gold era F11) |
| **Inicio** | Menú del ModKit (como en Sacred Gold) |
| **Fin** | Volcado de diagnóstico |

## Los mods, adaptados

### 1. Ultrawide 32:9 (5120x1440)
- **En Sacred Gold:** hacía falta un proxy de render para el ultrawide, sin tirones y con más sectores cargados.
- **En Sacred 2:** el juego ya acepta 5120x1440 de forma nativa (`options.txt`, `width`/`height`). El trabajo real es:
  - Ajustar la cámara 3D. **Hecho** en `optionsCustom.txt`: zoom hasta 47 m y ángulo de 20° a 60°.
  - Dejar la niebla de distancia, que con 32:9 se nota más en los bordes. **Hecho**: `render.fog = 0`.
- **Por comprobar:**
  - Si el HUD se estira o queda centrado.
  - Si el FOV horizontal es correcto.
  - Si los bordes cargan tarde (`streaming`, `visDetail`, `miniClipDist`).
  - En mazmorras, que la cámara no atraviese techos.

### 2. Ruta automática
- **En Sacred Gold:** un clic lejano planifica la ruta con A* y la recorre por tramos.
- **En Sacred 2:** el motor ya trae navegación (`cNaviManager`, rutas de IA en `cEventMoveTo`) y movimiento por WASD.
- **Plan:** probar primero un clic largo, porque puede que ya llegue solo. Solo si se atasca: **seguir al héroe hacia un punto del minimapa o del mapa del mundo** (`worldmap`), pidiendo la ruta a la navegación del juego. Nada de un A* propio.
- **Respetar a WASD:** cualquier tecla de movimiento cancela la ruta, igual que un clic.

### 3. Autoataque sin ratón (Insert)
- **En Sacred Gold:** se da la orden de clic del juego (atacar, recoger, abrir), con el ratón libre, y un clic tuyo lo pausa.
- **En Sacred 2:** se usa la **orden de ataque del motor** (`sEventAttack`), la misma que usa la IA del juego. **Comprobado en el juego:** ataca solo.
- **Adaptaciones:**
  - **Parar al acabar.** En Sacred 2 la orden de ataque no termina sola, así que se manda una orden de parada al morir el último enemigo (sin probar).
  - **Pausa también con WASD**, no solo con un clic.
  - **Armas a distancia:** en Sacred 2 el arco y la ballesta disparan como un hechizo. La IA del juego usa `cEventCastSpell` para esas armas, y el mod debe hacer lo mismo.
  - **A caballo:** el ataque montado lo hace la montura (`carrier`). Hay que comprobar que la orden funciona montado o enviarla a la montura.
  - **Ni la mascota ni los esbirros cuentan como objetivo**: solo los enemigos hostiles.
  - Nunca durante diálogos, comercio, cinemáticas ni con el mapa abierto.

### 4. Recogida automática
- **En Sacred Gold:** se camina al objeto y se recoge (oro incluido), se abren cofres y recipientes, y nunca se tocan postes ni placas.
- **En Sacred 2:** el juego **ya tiene recogida por radio** (Q, `cEventAutoCollect`) con filtro de rareza. El mod no la sustituye, la **automatiza**:
  - Sin amenaza, camina hasta el objeto más cercano y lanza la recogida nativa en modo silencioso (radio 50) **una sola vez**; luego espera.
  - **La rareza mínima es la del propio juego** (`autoCollectRareSelf/Other`), para que el mod no recoja lo que el jugador ha decidido ignorar.
  - **Oro:** por comprobar si en Sacred 2 se recoge solo al pasar por encima. Si es así, no hace falta caminar hacia él.
  - **Inventario lleno:** dejar de recoger y avisar con un pitido, en vez de ir y volver sin parar.
  - **Cofres, sarcófagos y recipientes:** por identificar con el volcado (tecla Fin). Se abrirán con `cEventInteract`.
  - **Nunca tocar:** postes, placas, piedras de reaparición, santuarios ni NPC. Solo objetos de tipo `isItem` hasta que el volcado diga otra cosa.

### 5. Decisión según la situación, con movimientos naturales
- **Igual que en Sacred Gold:**
  - Primero los enemigos que te hieren o se acercan; el botín, solo sin amenaza (salvo lo que está a tus pies).
  - Se queda con el enemigo que está combatiendo.
  - Mira la situación cada 180–340 ms y reacciona tras 220–480 ms (90–220 ms con amenaza); repite la orden a intervalos irregulares.
- **Propio de Sacred 2:**
  - **Clase de peligro y nivel del enemigo** (el juego ya los calcula, `getAdjustedLevel`/`showDangerclass`): los jefes y los más peligrosos que te atacan van antes. Los que no te atacan y son peligrosos no se provocan si `OnlyWhenAttacked=1`.
  - **Distancia en 3D:** las cuestas y los puentes cuentan. Un enemigo a otra altura o tras un muro no es una amenaza inmediata.
  - **Esbirros y mascota:** si ya están luchando contra un enemigo, el héroe puede preferir otro.

### 6. Pociones automáticas
- **En Sacred Gold:** bebe cuando falta tanta vida como cura una poción, lo aprende midiendo y no confunde la regeneración con una poción.
- **En Sacred 2:**
  - **Hay una acción `heal` (Espacio)** que elige la poción de curación con el orden `potionorder` del juego. El mod debe usar **esa misma lógica** y no una ranura fija. Hasta encontrarla con REA, usa la ranura configurada.
  - **Las pociones de curación tienen enfriamiento propio** (`getPotionHealCooldown`) y la vida se regenera sola, con más retraso en combate (`LifeRegenerationDelayCombat`). Hay que respetar el enfriamiento del juego y no beber mientras la regeneración aún cubre lo que falta.
  - **Resto de ranuras (R–O):** pociones de buffs (velocidad, daño, resistencias). Son opcionales y se configuran por ranura, por ejemplo «bebe la ranura 3 al empezar contra un jefe».
  - **Montado:** la vida de la montura es aparte. Por comprobar si conviene curarla también.

### 7. Menú del ModKit (Inicio)
- **Igual:** superposición ImGui con una sección por mod, cambios en vivo y botón «Guardar» al `.ini`.
- **En Sacred 2:**
  - Se dibuja con el dispositivo D3D9 del juego (`cDX9Advance`), no con un proxy.
  - Mientras el menú está abierto, el juego **no debe recibir** ratón ni teclado; si no, se mueve la cámara 3D o se disparan las artes 1–5.
  - Con F11 (ocultar interfaz), el menú se oculta también.
- **Requiere** descargar Dear ImGui, o reutilizar la del ModKit de Sacred Gold, con tu permiso.

### 8. Artes de combate con estrategia (lo que más cambia en Sacred 2)
En Sacred 2 las artes de combate son **el centro del combate**:
- Cada una tiene **tiempo de regeneración**, alcance y efecto.
- Los **buffs** (6–8) se mantienen activos y **alargan la regeneración de todas las artes**.
- Hay **combos** del maestro de combos, que encadenan varias artes en una sola ranura.
- **No hay maná**: el coste real es el tiempo de regeneración.

Criterios propuestos:

| Situación | Qué hacer |
|---|---|
| Buffs | Mantenerlos siempre activos. Relanzar al caducar, nunca en mitad de una animación. |
| Curas y defensas | Solo por debajo de un umbral de vida o con N atacantes. |
| Área | Solo con 3 o más enemigos dentro de su alcance (`getEffectRange`). |
| Enemigo fácil (clase de peligro baja) | Ataque normal, sin artes de regeneración larga. |
| Jefe o enemigo peligroso | Las artes más fuertes en cuanto estén listas (`getCurrentCoolDown` = 0). |
| Combos | Tratarlos como un arte más, según su regeneración total. |
| A distancia | Las artes de distancia solo si el objetivo está lejos; si está pegado, las de cuerpo a cuerpo. |

- **Orden de lanzamiento:** `cEventCastSpell(héroe, ranura)` + `setTargetRef(enemigo)`, igual que hace la IA del juego.
- **Por resolver con REA:** el tipo de cada arte (área, cura, buff, distancia) a partir de `cMagicEntry`, y si se puede lanzar (`canBeCasted`).
- **Modo de lanzado:** respetar `instantCastSpell`, la opción del juego para lanzar las artes al instante.

## Lecciones de Sacred Gold que se mantienen
- Dar órdenes al motor, no simular clics. En Sacred 2 es aún más fácil: son eventos.
- No repetir la orden de recoger: una vez y esperar.
- Distinguir los objetos fijos con datos del juego. Aquí: tipo, familia y banderas, del volcado con Fin.
- Teclas con `GetAsyncKeyState`, mirando también el bit 0.

## Decisiones tomadas (2026-11-07)
1. Teclas del ModKit: **Insert**, **Inicio** y **Fin** (confirmadas).
2. Menú: Dear ImGui 1.91.9, que `scripts/fetch-imgui.ps1` descarga con el hash fijado.
3. Pociones de mejora: opcionales (`BuffSlot`). Se beben cuando te ataca un enemigo duro; por defecto, desactivadas.

## Estado de implementación
| Mod | Estado |
|---|---|
| 1. Ultrawide y cámara | `config/optionsCustom.txt`. Falta comprobar el HUD y el FOV a 32:9. |
| 2. Ruta automática | No hace falta un A*: el motor ya trae navegación. W/S cancelan la automatización. |
| 3. Autoataque | Hecho, con arma a distancia (`cEventCastSpell` de la IA). El ataque básico está probado. |
| 4. Recogida | Hecho para objetos del suelo. Los cofres esperan al volcado con Fin. |
| 5. Decisión según la situación | Hecho: amenaza, enemigos que se acercan, enemigos duros, fidelidad al objetivo y tiempos naturales. |
| 6. Pociones | Hecho: aprende cuánto cura, ranura de reserva, umbral de emergencia y poción de mejora. Desactivado por defecto. |
| 7. Menú | Hecho: ImGui en `Present`, opciones generadas de cada mod, Guardar y un indicador en pantalla. |
| 8. Artes de combate | Hecho: curación, buffs con `isBuffActive` (nunca los apaga), área con grupos y artes fuertes para jefes. |
