# Teste da tela MAR2406

Implementacao para ESP32 classico e MAR2406 com controlador **ILI9341**,
barramento paralelo de 8 bits e imagem horizontal de 320 x 240.
Nao usa touch nem cartao SD. Nao depende de Arduino ou LVGL.

## Ligacao sugerida

Conecte com as placas desligadas. Os nomes abaixo sao os sinais LCD do
shield, e nao os numeros de pino do Arduino.

| Sinal da tela | ESP32 |
| --- | --- |
| LCD_D0 | GPIO13 |
| LCD_D1 | GPIO14 |
| LCD_D2 | GPIO18 |
| LCD_D3 | GPIO19 |
| LCD_D4 | GPIO21 |
| LCD_D5 | GPIO22 |
| LCD_D6 | GPIO23 |
| LCD_D7 | GPIO25 |
| LCD_WR | GPIO26 |
| LCD_RS | GPIO27 |
| LCD_CS | GPIO15 |
| LCD_RST | GPIO33 |
| LCD_RD | 3V3 (mantido alto; somente escrita) |
| GND | GND comum |
| 5V | Alimentacao de 5 V |
| 3V3 da tela | 3V3 do ESP32 |

O GPIO4 continua reservado ao DS18B20. O manual da MAR2406 mostra os
pinos 5V e 3V3 ligados nas tabelas de conexao. Os sinais do ESP32 usam
3,3 V; ligue 5V da tela somente ao 5V da placa, nunca a um GPIO.
Deixe os pinos SD sem conexao. Confira os rotulos da sua placa antes de ligar.
GPIO15 e um pino de strapping do ESP32. LCD_CS deve permanecer alto durante
o boot; se o modulo o mantiver baixo, as mensagens iniciais de boot podem
desaparecer da serial.

## Executar

1. Confira a pinagem em `main/Mar2406Config.h` e mantenha `TEST_ENABLED = true`.
2. No terminal ESP-IDF, compile com `idf.py build`.
3. Com o ESP32 conectado, grave usando `idf.py -p COMx flash monitor`,
   substituindo `COMx` pela porta correta.
4. A tela deve apresentar BEER MONITOR, MAR2406 - TESTE, DADOS SIMULADOS,
   temperatura de 20.50 C, densidade de 1.048 SG, bateria de 3.85 V e status
   TESTE ATIVO. O contador deve aumentar continuamente.
5. Confira cores e legibilidade: titulo ciano, aviso amarelo, status verde
   e contador magenta. A limpeza inicial pode levar alguns segundos.
6. Confira no monitor serial que recepcao/ACK e sensor local seguem operando.

Os valores sao ficticios e independentes da telemetria recebida. O contador
avanca a cada ciclo (escrita na tela mais espera de um segundo); nao e um relogio.
O log de envio nao confirma funcionamento fisico: o barramento nao faz leitura.

## Organizacao

- `Mar2406Config.h`: pinagem e habilitacao do teste.
- `Mar2406Display.h/.cpp`: inicializacao ILI9341, escrita paralela e fonte local.
- `DisplayTest.h/.cpp`: tarefa de demonstracao com prioridade 1.
- `main.cpp`: apenas include e chamada `display_test::start()` adicionados.
- `main/CMakeLists.txt`: registro dos fontes e dependencias locais do ESP-IDF.

Para desabilitar, altere `TEST_ENABLED` para `false` e recompile; a tarefa
nao sera criada e os GPIOs da tela nao serao configurados pelo teste.
O driver usa pausas entre caracteres/linhas e nao desabilita interrupcoes.
A fonte atende letras sem acentos, numeros, espaco, ponto, dois-pontos e hifen.

Referencia: https://www.lcdwiki.com/2.4inch_Arduino_Display
