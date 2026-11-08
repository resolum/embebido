# CLAUDE.md: reglas de trabajo de Alivia

Este archivo es idéntico en los dos repos y aplica a ambos:

- **Edge de voz** (Python): `github.com/resolum/alivia-control-edge-service`
- **Firmware ESP32** (Arduino): `github.com/resolum/embebido`

## Ramas

- `main` es la rama estable. **Nunca se toca** directamente.
- `develop` sale de `main`. De `develop` salen las ramas de feature, que ya existen en el remoto (por ejemplo `AP-103-TS-45-...`).
- **Los cambios se hacen en la rama de feature que ya existe** para ese trabajo. No se crean ramas nuevas por cada añadido.
- Si no hay una rama que corresponda al trabajo, se pregunta antes de crear una.
- Los PR van de la rama de feature hacia `develop`.

## Commits

- Siempre en [Conventional Commits](https://www.conventionalcommits.org/): `tipo(ámbito opcional): descripción en imperativo`.
- Tipos usados: `feat`, `fix`, `refactor`, `test`, `docs`, `chore`, `ci`, `build`, `perf`.
- Commits pequeños, uno por cambio lógico. No se commitea ni se hace push sin que se pida.
- Ejemplos: `feat(transport): add websocket server`, `docs: document emergency flow`.

## Documentación

- **Todo añadido nuevo se documenta en `docs/`** dentro del mismo cambio. No se crean ramas ni archivos sueltos para documentar.
- `docs/protocolo-websocket.md` es la fuente de verdad del contrato edge ↔ ESP32 y debe ser **idéntico en ambos repos**. Si cambia en uno, se actualiza en el otro.
- El README de cada repo resume arquitectura, ejecución y pruebas, y enlaza a `docs/`.

## Reglas del edge de voz (Python)

- CI (PR a `main`/`develop`): `python:3.14-slim`, ARM64, 2 CPUs, 1 GB de RAM. Ejecuta `pytest`, `compileall`, `ruff check`, `ruff format --check` y `pip-audit -r requirements.txt`. Todo debe pasar localmente antes de pedir revisión.
- `.github/workflows/jira-subtask-review.yml` no se toca.
- `requirements.txt` es liviano y con versiones fijadas. Lo pesado (`vosk`, `faster-whisper`, `silero-vad`, `piper-tts`, `ollama`, `onnxruntime`/`torch`) va en `requirements-ml.txt`, que el CI no instala.
- Imports perezosos de los módulos pesados, detrás de interfaces. Los tests usan fakes: sin modelos, sin GPU, menos de 1 GB de RAM.
- Ninguna acción sobre dispositivos llega a MQTT sin confirmación explícita. La regla la aplica el código del edge, nunca el LLM.
- La **emergencia nunca pasa por MQTT, Whisper, Qwen ni el edge de actuadores**, y nunca espera confirmación. Es la única excepción a "100 % local".
- Endpoint, token y URLs de emergencia solo por variables de entorno (`.env.example` con placeholders). Nada de secretos en el código.
- Todo umbral, timeout, topic, modelo, ruta, URL y puerto va en configuración.

## Reglas del firmware (ESP32)

- Placa `esp32:esp32:esp32`, core Espressif 3.x (`ESP_I2S.h`), framework ModestIoT (copiado en el repo).
- Pines del micrófono (SCK=27, WS=14, SD=2), ganancia y comandos de Serial existentes (`h s v j + - r R`) **no cambian**.
- `Secrets.h` (WiFi, host del edge, token) está ignorado por git. Solo se versiona `Secrets.example.h`.
- Captura en el core 1; red y reproducción en el core 0. Si no hay enlace, el audio se descarta, no se acumula.
- Verificar la compilación con `arduino-cli` cuando esté disponible, y documentar en `docs/` el uso de heap.

## Criterio general

- Ante la duda sobre una acción de dispositivos, se pregunta; nunca se ejecuta.
- Si algo choca con el código existente o con el CI, se pregunta antes de seguir.
- No se tocan archivos con cambios del usuario sin commitear que no sean parte de la tarea.
