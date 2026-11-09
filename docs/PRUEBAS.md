# Pruebas en el juego

Estado a 2026-11-07:

- **Probado en el juego:** el autoataque básico (ataca solo).
- **Sin probar en partida:** todo lo demás. Compila, y la autoprueba (`build.ps1 -SelfTest`) carga la DLL y lista
  sus 6 módulos sin el juego.

Durante las pruebas, el registro está en `system/Plugin_ModKit.log`. **Fin** vuelca en él lo que hay alrededor,
tus artes de combate y tu cinturón de pociones.

## Arranque
1. En el log aparecen `core engine exports resolved` y `frame listener registered`, sin `REQUIRED export missing`.
2. Ninguna línea `module ... disabled`. Si sale alguna, ese mod no encontró una función del juego.

## Menú e indicador
3. **Inicio** abre el menú y en el log aparece `menu: ImGui ... ready`. Los cambios se aplican al momento y
   **Guardar** los escribe en `Plugin_ModKit.ini`.
4. Con el menú abierto, el ratón y el teclado no mueven ni al héroe ni a la cámara. Si los mueven, el juego lee la
   entrada por DirectInput y hay que bloquearla de otra forma.
5. Al cambiar la resolución o pasar a pantalla completa con Alt+Intro, el menú no desaparece ni el juego se cierre
   (`Reset`).
6. El indicador muestra ModKit ON/OFF. **F11** lo oculta junto con la interfaz.

## Autoataque
7. **Parar:** con Insert ON, al morir el último enemigo el héroe deja de golpear (en el log, `stop`).
8. **Insert OFF** en mitad de un combate también lo para.
9. **Prioridades:** con varios enemigos, va primero a por el que te pega (`attack ... (threat)`) y no salta de
   un objetivo a otro parecido.
10. **Repetición:** con el mismo enemigo, la orden se repite cada 1,6–2,6 s. Comprueba que no corta combos; si
    los corta, sube `ReissueMin/Max`.
11. **Tus clics y W/S** te dan el control durante 0,75 s.
12. **A distancia:** con arco o ballesta, dispara.

## Artes de combate
13. Pulsa **Fin** y busca `arts: N slot(s), buffActive available` seguido de una línea por arte: nombre,
    regeneración, buff, cura, ataque.
14. En combate lanza artes (`art: slot ...`). Las de área solo salen con 3 o más enemigos.
15. **Buffs:** se activan si estaban apagados y **nunca** se apagan. Si alguno parpadea, desactiva
    `UseBuffs` y avísame.
16. Con poca vida usa las artes de curación.
17. El autoataque no corta la animación de un arte (`CastLock`).

## Recogida
18. Sin enemigos, va al objeto más cercano y lo recoge uno a uno con la orden de un clic (`loot: take ...`). La
    tecla de recoger del juego no interviene.
19. Con enemigos en `SafeRadius` no se aleja; lo que tiene a los pies sí lo recoge.
20. Si un objeto es inalcanzable, lo deja tras 2 intentos (`loot: gave up`).
21. Pulsa **Fin** junto a un cofre, un poste y un santuario, y guarda el log: con eso se añaden los cofres.

## Retirada, aprendizaje y enciclopedia
21a. En el log aparece `up axis learned: y` (o `z`): hace falta para la retirada.
21b. Con poca vida, sin pociones y enemigos cerca, se aleja (`retreat: start`) y vuelve a luchar al recuperarse.
21c. Tras unos combates, el menú (Inicio → Aprendizaje) lista los tipos de enemigo con `%/s`, muertos y sitio.
     `system/Plugin_ModKit-foes.txt` sigue ahí en la siguiente partida.
21d. Opción «Abrir cofres» activada: abre cofres y barriles una sola vez. Si toca otra cosa (un poste, un portal),
     guarda el log y avisa.

## Memoria del mapa y radar
21e. Tras `up axis learned`, aparece el radar (a la derecha, a media altura). Al andar se pinta en verde el suelo pisado y una línea azul con tu rastro.
21f. Los puntos encajan con lo que ves: rojo enemigo, gris amigo, amarillo botín, azul objeto. Si el radar sale girado o en espejo respecto a la pantalla, dime hacia dónde: el norte se ajusta.
21g. Pulsa **Fin** y busca `map: ... capa N` y `getLayer available`. Entra en una cueva o mazmorra y vuelve a pulsarlo: dime si la capa cambia.
21h. Al salir del juego, `system/Plugin_ModKit-map.txt` existe y en la siguiente partida el radar recuerda el suelo pisado.
21i. Sin tirones cada 60 s (cuando guarda). Si los hay, dime cuántos bloques marca el menú.

## Pociones
Antes, activa `Pociones automáticas` en el menú.

22. Con pociones de curación en la ranura 0, recibe daño: bebe cuando falta un 30 % de vida
    (`potion: drink ...`).
23. Tras la primera, el log dice `potion: heals about N`. La regeneración no cuenta como poción.
24. Con la ranura vacía, bebe de `BackupSlot`.

## Cámara, ultrawide y niebla
25. El zoom llega a 47 m, el ángulo va de 20° a 60° y no hay niebla de distancia.
26. A 32:9: la interfaz no se estira, el campo de visión está bien y los bordes no cargan tarde.

## General
27. En una sesión larga no hay tirones ni cierres.
