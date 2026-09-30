# Monitor de sensores na tela MAR2406

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
| LCD_D2 | GPIO16 |
| LCD_D3 | GPIO17 |
| LCD_D4 | GPIO18 |
| LCD_D5 | GPIO19 |
| LCD_D6 | GPIO21 |
| LCD_D7 | GPIO22 |
| LCD_WR | GPIO23 |
| LCD_RS | GPIO25 |
| LCD_CS | GPIO26 |
| LCD_RST | GPIO27 |
| LCD_RD | 3V3 (mantido alto; somente escrita) |
| GND | GND comum |
| 5V | Alimentacao de 5 V |
| 3V3 da tela | 3V3 do ESP32 |

O GPIO4 continua reservado ao DS18B20. O manual da MAR2406 mostra os
pinos 5V e 3V3 ligados nas tabelas de conexao. Os sinais do ESP32 usam
3,3 V; ligue 5V da tela somente ao 5V da placa, nunca a um GPIO.
Deixe os pinos SD sem conexao. Confira os rotulos da sua placa antes de ligar.

## Executar

1. Confira a pinagem em `main/Mar2406Config.h` e mantenha `TEST_ENABLED = true`.
2. No terminal ESP-IDF, compile com `idf.py build`.
3. Com o ESP32 conectado, grave usando `idf.py -p COMx flash monitor`,
   substituindo `COMx` pela porta correta.
4. A tela deve apresentar BEER MONITOR e duas secoes: DS18B20 LOCAL com sua
   temperatura, e ISPINDEL com temperatura, densidade, bateria e inclinacao.
5. Sem leitura valida, a secao correspondente mostra ERRO: NAO ENCONTRADO
   em vermelho. A limpeza inicial pode levar alguns segundos.
6. Confira no monitor serial que recepcao/ACK e sensor local seguem operando.

Os valores sao reais. O DS18B20 fica indisponivel quando a leitura falha ou
fica mais de 5 segundos sem atualizacao. O iSpindel fica indisponivel enquanto
nao houver pacote valido ou apos 35 segundos sem recepcao valida do sensor
selecionado. Os valores antigos do iSpindel sao substituidos por tracos.
As leituras voltam automaticamente quando os sensores retornam. O intervalo
de envio do iSpindel deve ser menor que esse limite para evitar avisos entre
envios. A tarefa espera 500 ms entre verificacoes e redesenha so linhas alteradas.
O log de inicializacao nao confirma funcionamento fisico: o LCD nao e lido.

## Organizacao

- `Mar2406Config.h`: pinagem e habilitacao do teste.
- `Mar2406Display.h/.cpp`: inicializacao ILI9341, escrita paralela e fonte local.
- `DisplayTest.h/.cpp`: tarefa de monitoramento real com prioridade 1.
- `IspindelReceiver.h/.cpp`: disponibiliza copia da ultima telemetria validada
  em uma fila separada, sem consumir a fila de recepcao nem desenhar no ACK.
- `main.cpp`: apenas include e chamada `display_test::start()` adicionados.
- `main/CMakeLists.txt`: registro dos fontes e dependencias locais do ESP-IDF.

Para desabilitar, altere `TEST_ENABLED` para `false` e recompile; a tarefa
nao sera criada e os GPIOs da tela nao serao configurados pelo teste.
O driver usa pausas entre caracteres/linhas e nao desabilita interrupcoes.
A fonte atende letras sem acentos, numeros, espaco, ponto, dois-pontos e hifen.

Referencia: https://www.lcdwiki.com/2.4inch_Arduino_Display
