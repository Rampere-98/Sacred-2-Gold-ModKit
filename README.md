# Sacred 2 Gold ModKit

*English summary: a native Orc plugin for **Sacred 2 Gold** (Steam) that adds auto attack (melee and ranged), combat
arts used with a strategy, auto loot, auto potions and an in-game ImGui menu, plus camera, ultrawide 32:9 and no-fog
settings. It drives the game's own engine events (found with REA/Ghidra), never simulated clicks, and patches no game
file. `main` is the full ModKit; every mod also lives on its own `mod/*` branch. No game files are included.*

Hermano del [Sacred Gold ModKit](https://github.com/Rampere-98/Sacred-Gold-ModKit), adaptado a cómo se juega Sacred 2.
Cómo se adaptó cada mod: [docs/ESPECIFICACION.md](docs/ESPECIFICACION.md). Qué falta del agente de Sacred Gold y cómo se hará en Sacred 2: [docs/AGENTE.md](docs/AGENTE.md).

> **Estado:** el autoataque está probado en el juego. El resto compila y supera la autoprueba sin el juego, pero
> **aún no se ha probado en partida**. Lista de pruebas: [docs/PRUEBAS.md](docs/PRUEBAS.md).

## Qué hace

| Mod | Rama | Qué hace |
|---|---|---|
| Autoataque | [`mod/auto-attack`](../../tree/mod/auto-attack) | Ataca con la orden de ataque del propio motor: solo a quien te ataca o, con «Buscar combate», a cualquier enemigo cercano. Elige primero a quien te hiere o se acerca, a los jefes y a los tipos que más pegan; se queda con el enemigo que combate y para al acabar. Con arco, ballesta o arma de fuego dispara como lo hace el juego. Con poca vida y sin pociones, **se retira** lejos de los enemigos. |
| Aprendizaje | [`mod/combat-learn`](../../tree/mod/combat-learn) | Aprende cuánto pega cada tipo de enemigo y mantiene una **enciclopedia** de los vistos y muertos, con su sitio, que se guarda entre partidas (`Plugin_ModKit-foes.txt`). |
| Artes de combate | [`mod/combat-arts`](../../tree/mod/combat-arts) | Lanza tus artes con estrategia: curación bajo un umbral de vida, buffs siempre activos (sin apagarlos nunca), artes de área solo con grupos y artes fuertes reservadas para jefes o emergencias. No corta animaciones. |
| Recogida | [`mod/auto-loot`](../../tree/mod/auto-loot) | Recogida **propia**, nunca la tecla del juego: toma uno a uno lo que hay alrededor con la orden de un clic. Opcionalmente abre cofres y recipientes, cada uno una sola vez. Solo actúa sin combate y nunca te estorba; lo inalcanzable se reintenta una vez y luego se deja. |
| Pociones | [`mod/auto-potion`](../../tree/mod/auto-potion) | Bebe cuando falta tanta vida como cura una poción: lo aprende midiendo y no confunde la regeneración. Tiene ranura de reserva, umbral de emergencia y poción de mejora opcional contra jefes. |
| Memoria del mapa y radar | [`mod/map-memory`](../../tree/mod/map-memory) | Recuerda por dónde has pasado (casillas de 1 m, por capa) y dónde viste cada tipo de enemigo, con el mismo formato que el Sacred Gold ModKit (`Plugin_ModKit-map.txt`). Un **radar** redondo, con el norte arriba, muestra el suelo pisado, tu rastro, los enemigos (vivos y recordados), el botín y los objetos. |
| Exploración | [`mod/explore`](../../tree/mod/explore) | Desactivada por defecto. Con la automatización y sin combate, el héroe camina por pasos hacia lo que menos ha pisado (el coste del agente de Sacred Gold sobre la memoria del mapa). Si deja de avanzar, marca ese camino como bloqueado y cambia de rumbo. Respeta vida mínima, enemigos cerca y un radio máximo opcional. Va en la misma rama que la memoria del mapa, de la que depende. |
| Ir a un punto | [`mod/goto`](../../tree/mod/goto) | Con el menú abierto, el radar se convierte en un **mapa grande** en el centro: clic izquierdo y el héroe va allí, clic derecho cancela. El camino es un A* sobre la memoria del mapa (prefiere lo pisado y evita lo bloqueado) que se recorre por tramos con la orden de mover del juego; si se atasca, rehace el camino. Se para en los combates y lo cancela cualquier clic tuyo. |
| Menú e indicador | [`mod/menu`](../../tree/mod/menu) | **Inicio** abre un menú ImGui con todas las opciones: cambios al momento y botón «Guardar». Un indicador en pantalla muestra ON/OFF y lo que hace cada mod. Ofrece a los demás mods una API de dibujo 2D con el ratón y los clics fuera del menú (el radar y el mapa grande la usan). |
| Cámara, ultrawide y niebla | [`mod/camera-ultrawide`](../../tree/mod/camera-ultrawide) | `optionsCustom.txt`: zoom hasta 47 m, ángulo de 20° a 60° para 32:9, sin giro automático y sin niebla de distancia. |

## Teclas

| Tecla | Acción |
|---|---|
| **Insert** | Automatización ON/OFF: autoataque, artes, recogida y exploración (pitido agudo = ON). Las pociones siguen siempre. |
| **Inicio** | Menú del ModKit |
| **Fin** | Volcado de diagnóstico a `system/Plugin_ModKit.log` |
| Clic, **W** o **S** | Tomas el control: la automatización espera 0,75 s y nunca da una orden encima de la tuya. |

F9–F12 no se usan porque son los orbes del juego (F11 además oculta la interfaz). Todas las teclas se cambian en el menú.

## Instalar

Requisitos: [Zig 0.13](https://ziglang.org/download/) para compilar y PowerShell 5.1 o superior.

```powershell
git clone https://github.com/Rampere-98/Sacred-2-Gold-ModKit
cd Sacred-2-Gold-ModKit
.\scripts\fetch-imgui.ps1        # Dear ImGui 1.91.9 for the menu (hash pinned)
.\build.ps1 -SelfTest            # build\Plugin_ModKit.dll + offline load test
.\scripts\install.ps1 -Camera    # finds the game via Steam; or -GameDir "<path>"
```

`install.ps1` copia la DLL a `system/` y registra `Plugin=Plugin_ModKit` en `system/plugins.cfg`. Hace copia de
seguridad (`*.bak-<fecha>`) de todo lo que sustituye. `-Camera` instala además la cámara y quita la niebla en tu
perfil (`%LOCALAPPDATA%\Ascaron Entertainment\Sacred 2\optionsCustom.txt`).

Al arrancar por primera vez se crea `system/Plugin_ModKit.ini`, con comentarios; se edita en el menú o a mano.

Desinstalar: `.\scripts\uninstall.ps1 -GameDir "<ruta>" [-Camera]`.

## Cómo funciona

- **Plugin Orc nativo** (`dllStartPlugin`), sin proxy de DLL.
- **Las llamadas al juego** se hacen por el nombre exportado de sus funciones C++ (`GetProcAddress`). Si una no
  existe, solo se desactiva el mod que la necesita, y el log lo dice.
- **Órdenes al héroe:** son eventos del motor (`sEventAttack`, `cEventCastSpell`, `cEventAutoCollect`,
  `cEventItemTransfer`, `cEventMoveTo`), creados igual que los crea la IA del juego.
- **Ritmo natural:** mira la situación cada 180–340 ms y reacciona tras 220–480 ms (90–220 ms con amenaza). Repite
  las órdenes a intervalos irregulares.

Detalle técnico y de dónde sale cada función: [docs/ENGINE.md](docs/ENGINE.md).

## Estructura y ramas

```text
src/modkit.h        core interface: engine ABI, Setting tables, Module, shared state
src/core.c          plugin entry, frame listener, config (ini + menu), input, helpers
src/mod_*.c/.cpp    one file per mod, registered with MK_MODULE(name)
build.ps1           generates the module registry from the mod files present and builds
tools/selftest.c    loads the DLL in a 32-bit process without the game
tools/test_<mod>.c  offline unit test of a mod (build.ps1 -SelfTest runs it when the mod is present)
scripts/            install, uninstall, fetch-imgui
config/             optionsCustom.txt (camera, ultrawide, no fog)
```

- **`main` es el ModKit completo.** `core` es el núcleo compartido. Cada mod se desarrolla en su rama `mod/*`
  (el núcleo más solo ese mod), y `main` las fusiona todas.
- **Cada rama compila por sí sola**: `build.ps1` monta el registro con los archivos `mod_*` presentes.
- **Para un mod nuevo:** crea `src/mod_<nombre>.c` con su tabla `Setting[]` y `MK_MODULE(mod_<nombre>)`. El menú y
  el `.ini` lo recogen solos.

## Licencia

MIT (ver [LICENSE](LICENSE)). [Dear ImGui](https://github.com/ocornut/imgui) es MIT y se descarga aparte.
Sacred 2 es de sus propietarios; este repositorio no contiene ningún archivo del juego.
