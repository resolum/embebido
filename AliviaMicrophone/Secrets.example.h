#ifndef SECRETS_H
#define SECRETS_H

/**
 * @file Secrets.example.h
 * @brief Plantilla de las credenciales. Copia este archivo como Secrets.h y rellena los valores.
 *
 * Secrets.h esta en .gitignore: nunca se sube al repositorio. Si falta, el sketch no compila.
 */

#include <stdint.h>

static constexpr const char* WIFI_SSID     = "nombre-de-tu-red";
static constexpr const char* WIFI_PASSWORD = "clave-de-tu-red";

// IP o nombre de la PC donde corre el edge de voz (python -m alivia_edge).
static constexpr const char* EDGE_HOST = "192.168.1.50";

// Mismo valor que ALIVIA_WS_TOKEN en el .env del edge.
static constexpr const char* EDGE_TOKEN = "replace-with-a-random-secret";

#endif // SECRETS_H
