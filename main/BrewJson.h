#ifndef BREW_JSON_H
#define BREW_JSON_H

#include <stddef.h>
#include "BrewProtocol.h"

// Aceita somente uma leitura JSON completa, com identificadores e valores validos.
bool brew_json_parse(const char *body, size_t length, BrewTelemetry *packet);

#endif
