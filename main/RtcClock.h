#ifndef RTC_CLOCK_H
#define RTC_CLOCK_H

#include <stdbool.h>
#include <stddef.h>

// DS3232M. Inicializar uma vez antes das tarefas que consultam o horario.
void rtc_clock_start(void);
// Copia o horario local do RTC (AAAA-MM-DD HH:MM:SS), sem acessar I2C.
// Retorna false e escreve "RTC INDISPONIVEL" se a leitura nao for confiavel.
bool rtc_clock_timestamp(char *buffer, size_t size);

#endif
