# embebido: ESP32 de voz de Alivia

Firmware (Arduino, ESP32 clásico) del nodo de voz de Alivia: un micrófono INMP441 y un parlante con amplificador
MAX98357A en **un solo ESP32**. Envía el audio del micrófono al **edge de voz** (repo `alivia-control-edge-service`) por
WebSocket y reproduce por el mismo enlace la voz de respuesta. El contrato está en
[`docs/protocolo-websocket.md`](docs/protocolo-websocket.md) (el mismo archivo existe en el edge).

## Hardware y cableado

| INMP441 | ESP32 | | MAX98357A | ESP32 |
|---|---|---|---|---|
| VDD | 3V3 (nunca 5 V) | | BCLK | GPIO 26 |
| GND | GND | | LRC (WS) | GPIO 25 |
| L/R | GND | | DIN | GPIO 22 |
| SCK | GPIO 27 | | VIN | 5 V (pin VIN) |
| WS | GPIO 14 | | GND | GND |
| SD | GPIO 2 | | SD, GAIN | sin conectar |

Los pines del micrófono no cambian. El parlante usa el segundo puerto I2S del ESP32 clásico (el micrófono usa el
primero). Todos los pines están en [`AliviaMicrophone/MicrophoneConfig.h`](AliviaMicrophone/MicrophoneConfig.h).
GPIO 2 (SD del micrófono) es un pin de arranque: si falla la carga del firmware, desconecta SD mientras flasheas.

### Alimentación del amplificador

El MAX98357A consume **picos de corriente** al reproducir (cientos de mA con el volumen alto) y eso hace caer la
tensión de una fuente débil: se oye como ruido y puede reiniciar el ESP32.

- Aliméntalo a **5 V** desde el pin `VIN` de la placa (la tensión de USB), no desde 3V3: a 3,3 V entrega menos potencia
  y el regulador de 3V3 del ESP32 también debe sostener los picos de WiFi.
- Usa una fuente USB de **1 A o más** y un cable corto y grueso. Un puerto USB de PC viejo puede quedarse corto.
- Pon un capacitor de **470 µF a 1000 µF** entre VIN y GND del amplificador, lo más cerca posible de él.
- `SPEAKER_VOLUME_PERCENT` (por defecto 60) limita el nivel de salida; bájalo si ves reinicios o ruido.

### Separación entre micrófono y parlante

No hay cancelación de eco: el micrófono oirá al parlante. El edge ignora el micrófono mientras suena una respuesta,
pero conviene ayudarle desde lo físico:

- Separa micrófono y parlante al menos **10 a 15 cm**, con el orificio del micrófono mirando **lejos** del parlante.
- No los pongas en la misma cavidad de la carcasa; aísla el micrófono con espuma o goma de las vibraciones.
- Mantén el cableado del micrófono (SCK, WS, SD) lejos del cable del amplificador.
- La ganancia del micrófono se ajusta con `MICROPHONE_GAIN_DB` o en vivo con `+` y `-`.

## Librerías y compilación

- Placa: **ESP32 Dev Module**, core `esp32:esp32` de Espressif **3.x** (probado con 3.3.8).
- Librerías: **ArduinoJson 7.x** y **WebSockets 2.7.x** (Markus Sattler / `arduinoWebSockets`).
  La capa de scheduler tipo ModestIoT ya está en la carpeta del sketch.

```bash
arduino-cli lib install ArduinoJson WebSockets
cp AliviaMicrophone/Secrets.example.h AliviaMicrophone/Secrets.h     # y edítalo
arduino-cli compile --fqbn esp32:esp32:esp32 AliviaMicrophone
arduino-cli upload  --fqbn esp32:esp32:esp32 -p /dev/cu.usbserial-XXXX AliviaMicrophone
```

Resultado de la compilación con core 3.3.8: 1 151 883 bytes de flash (87 % de la partición `default`, 1 310 720 bytes)
y 54 480 bytes de RAM estática (16 %). Si añades funciones y te quedas sin flash, elige el esquema de particiones
`Huge APP` en el IDE (`--board-options PartitionScheme=huge_app`).

### Credenciales: `Secrets.h`

`AliviaMicrophone/Secrets.h` contiene el WiFi, el host del edge y el token. Está en `.gitignore` y **no se sube**; en el
repo solo vive `Secrets.example.h`. Sin `Secrets.h` el sketch no compila. El token debe ser igual a `ALIVIA_WS_TOKEN` del
edge. El puerto y la ruta (`/ws`) están en `MicrophoneConfig.h`.

## Cómo funciona

```
INMP441 ──I2S──> captura (core 1) ──cola de 25 frames──> MicStream (core 0) ──ws /ws──> edge de voz
parlante <──I2S── Playback (core 0) <──jitter buffer 500 ms── EdgeLink (core 0) <──ws /ws── edge de voz
```

- **EdgeLink** conecta WiFi, abre el WebSocket, envía el `hello`, espera `ready` y reconecta con backoff de 1 s a 10 s.
  Pings cada 10 s; tres sin respuesta cierran el enlace. Al caer, el parlante corta el sonido y vacía sus buffers.
- **AudioStreamer** agrupa 2 frames de 20 ms (1280 bytes) por mensaje. Sin enlace descarta el audio y lo cuenta: nunca
  lo acumula. Si la cola de 25 frames se llena, el micrófono descarta el frame más viejo (cuenta en `cola del mic`).
- **Max98357Speaker** (actuador al estilo ModestIoT) guarda el audio en un jitter buffer, empieza a sonar tras 200 ms de
  prefill, rellena con silencio si el audio se retrasa (underrun) y envía `playback_done` cuando la última muestra ya
  sonó. `audio_cancel` corta el sonido; lo que ya está en el DMA (hasta ~90 ms) termina de sonar.
- La telemetría (`voice_activity`, `audio_level`) sale por Serial **y** por el WebSocket cuando hay enlace.

## Probar el enlace

1. En el PC: `cp .env.example .env` en el repo del edge, pon el mismo `ALIVIA_WS_TOKEN`, y arranca `python -m alivia_edge`.
2. Abre el monitor serie a 115200 baudios y escribe `w`. Debes ver `estado LISTO` y la IP del edge.
3. Escribe `t`: el parlante emite un tono de 440 Hz. Si no suena, revisa el cableado del MAX98357A.
4. Di "oye Alivia, abre la puerta": el edge debe preguntar "¿Quieres que abra la puerta?" por el parlante.
5. Sin hardware: `python -m scripts.fake_esp32 --token <token> --wav ...` en el repo del edge hace de ESP32.

| Comando serie | Acción |
|---|---|
| `h` | ayuda |
| `s` | estado del micrófono, pines y heap |
| `v` | vúmetro en vivo |
| `j` | JSON de telemetría por Serial |
| `+` / `-` | ganancia del micrófono ±3 dB |
| `w` | estado del enlace: WiFi, RSSI, conexión, bytes enviados y recibidos, descartes, underruns, heap |
| `t` | tono de prueba por el parlante |
| `r` | prueba de 3 s analizada en el ESP32 |
| `R` | graba y envía un WAV por Serial (para `tools/grabar_wav.py`, que no está en este repo) |

## Memoria: las grabaciones `r` y `R` pueden salir más cortas

Con WiFi activo hay menos RAM libre, y `r`/`R` reservan el bloque libre más grande (menos 16 KB de margen) hasta 3 s.
El enlace añade la pila de WiFi, la cola del micrófono (25 frames = 16 KB), el jitter buffer del parlante (16 KB) y las
pilas de tres tareas nuevas, así que el bloque disponible baja y la grabación se acorta: es esperado, no un fallo.
**No lo he medido en la placa** (solo compilé el firmware): arranca el ESP32 y escribe `s` y `w` para ver `heap libre` y
`bloque mayor`, y compáralo con `r`. Si necesitas 3 s completos, baja `SPEAKER_BUFFER_MS` o `MIC_QUEUE_FRAMES`.
