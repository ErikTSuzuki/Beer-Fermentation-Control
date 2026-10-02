#pragma once

#include <cstddef>

namespace rtc_clock {

// DS3232M. Inicializar uma vez antes das tarefas que consultam o horario.
void start();
// Copia o horario local do RTC (AAAA-MM-DD HH:MM:SS), sem acessar I2C.
// Retorna false e escreve "RTC INDISPONIVEL" se a leitura nao for confiavel.
bool timestamp(char *buffer, size_t size);

} // namespace rtc_clock
