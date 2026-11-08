# Protocolo WebSocket edge ↔ ESP32

Fuente de verdad del enlace entre el **edge de voz** (servidor, Python) y el **ESP32** (cliente, firmware).
Este archivo es idéntico en `alivia-control-edge-service` y en `embebido`: si cambia en uno, se actualiza en el otro.

- Endpoint: `ws://<host>:<puerto>/ws` (ruta configurable con `ALIVIA_WS_PATH`).
- Un solo endpoint bidireccional. El sentido de cada mensaje binario define su uso.
- Mensajes de texto: JSON UTF-8. Mensajes binarios: PCM int16 little-endian, mono.

## 1. Saludo (hello)

Primer mensaje que envía el ESP32 tras conectar (el edge espera 5 s como máximo):

```json
{"type":"hello","deviceId":"alivia-mic-01","capabilities":["microphone","speaker"],
 "micSampleRate":16000,"playbackSampleRate":16000,"bits":16,"channels":1,
 "frameSamples":320,"token":"..."}
```

El edge valida el token, `micSampleRate` = 16000, `bits` = 16, `channels` = 1 y responde:

- `{"type":"ready"}`: el enlace queda activo.
- `{"type":"error","reason":"..."}` y cierra la conexión. Razones: `bad_hello`, `unauthorized`, `unsupported_format`, `hello_timeout`.

Si el mismo `deviceId` se conecta de nuevo, el edge cierra la conexión anterior.

## 2. Micrófono (ESP32 → edge)

Mensajes binarios con 1 a 4 frames de 640 bytes (320 muestras = 20 ms cada uno). Valor recomendado: 2 frames
(40 ms, 1280 bytes). Streaming continuo: Vosk necesita escuchar siempre. Si no hay conexión, el ESP32 descarta el audio.

## 3. Parlante (edge → ESP32)

Secuencia por frase:

1. `{"type":"audio_begin","utteranceId":"<id>","sampleRate":16000,"priority":"normal"|"emergency"}`
2. Mensajes binarios PCM int16 LE a `playbackSampleRate` (el edge remuestrea lo que genera Piper).
3. `{"type":"audio_end","utteranceId":"<id>"}`

El edge envía un prefill de ~300 ms y luego mantiene el adelanto sobre el tiempo real acotado a ese valor, así que
el jitter buffer del ESP32 (~200-400 ms) no se desborda. El ESP32 empieza a sonar al tener el prefill o al recibir `audio_end`.

- `{"type":"audio_cancel","utteranceId":"<id>"}` (edge → ESP32): vacía el buffer y corta la reproducción.
- `{"type":"playback_done","utteranceId":"<id>"}` (ESP32 → edge): al terminar de sonar. El edge abre la ventana de
  confirmación a partir de este mensaje. Si no llega, el edge lo da por hecho tras la duración del audio + un margen.
- Ante un underrun el ESP32 rellena con silencio.

### Eco

No hay cancelación de eco. Mientras suena una respuesta (desde `audio_begin` hasta `playback_done` más una cola de
~300 ms) el edge descarta el audio del micrófono para palabra de activación y comandos. La detección de emergencia
sigue activa con una gramática propia.

## 4. Telemetría (ESP32 → edge)

JSON de texto con el mismo formato que sale por Serial con el comando `j` (`AudioTelemetry.h`):

```json
{"type":"voice_activity","deviceId":"alivia-mic-01","active":true,"levelDbfs":-31.2,"uptimeMs":120345}
{"type":"audio_level","deviceId":"alivia-mic-01","status":"OK","rmsDbfs":-38.1,"peakDbfs":-18.5,"peak":3900,
 "noiseFloorDbfs":-55.0,"voice":true,"clipped":0,"frames":50,"dropped":0,"sampleRate":16000,"slot":0,
 "gainDb":24.0,"windowMs":1000}
```

El edge solo la registra; no condiciona el pipeline.

## 5. Keepalive y reconexión

- Ping/pong WebSocket cada ~10 s. El edge da por caída la conexión tras 3 intervalos sin respuesta.
- El ESP32 reconecta con backoff exponencial de 1 s hasta 10 s.
- Al caer la conexión, el ESP32 corta la reproducción y vacía los buffers.
