#ifndef RTC_CONFIG_H
#define RTC_CONFIG_H

#include <stdbool.h>
#include "driver/gpio.h"

// GPIO21/22 ja pertencem a TFT. GPIO32/33 ficam livres para futuros reles.
// GPIO5/15 sao strapping: SDA e SCL precisam de pull-ups para 3V3.
#define RTC_SDA GPIO_NUM_5
#define RTC_SCL GPIO_NUM_15
// Somente se o RTC perdeu a hora (OSF) ou possui calendario invalido.
// Hora aproximada, no fuso do computador que compila; nunca reajusta RTC valido.
#define RTC_INITIALIZE_FROM_BUILD_TIME true
// NTP fornece UTC; o modulo e a tela guardam/exibem horario de Brasilia.
// Formato POSIX: BRT3 significa UTC-03:00, sem horario de verao.
#define RTC_SYNC_FROM_NTP true
#define RTC_NTP_SERVER "pool.ntp.org"
#define RTC_TIMEZONE "BRT3"

#endif
