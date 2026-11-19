# Cerveja Monitor 🍺

Monitor e controlador de fermentação com **ESP32, ESP-IDF e FreeRTOS**. Mede a
temperatura com um DS18B20 local, recebe a telemetria de um iSpindel e comanda
aquecimento e refrigeração por histerese. O operador configura receitas pelo
navegador, acompanha os dados na tela e pode enviá-los ao Ubidots.

## Funcionamento

```text
DS18B20 ──────────────> controle local ──────> comandos de aquecer / resfriar
                              ↑                         ↓
iSpindel ── Wi-Fi/HTTP ──> ESP32 + receitas      interface / potência externa
                              │
                 ┌────────────┼─────────────┐
              TFT + RTC   navegador     Wi-Fi externo
                          no AP local        └── MQTT → Ubidots
```

- **Controle independente da internet:** usa o DS18B20 local como realimentação.
  A temperatura do iSpindel é exibida e publicada, mas não comanda os relés.
- **Proteções no firmware:** desliga os comandos quando o sensor falha ou a
  amostra vence, impede aquecimento e refrigeração simultâneos e aplica tempos
  de espera para o compressor e para a troca de modo.
- **Receitas:** até 16 etapas com temperatura alvo, prazo máximo e limite opcional
  de densidade; receita e progresso são persistidos em NVS. Uma receita ativa
  salva pode retomar após reiniciar.
- **Interface local:** rede Wi-Fi própria, portal cativo e editor de receitas
  no navegador. Não precisa de aplicativo instalado.
- **Monitoramento:** TFT MAR2406/ILI9341, RTC e telemetria MQTT opcional.
  O enlace atual com o iSpindel usa HTTP/JSON por Wi-Fi.

## Hardware e pinagem do firmware

O protótipo usa um **ESP32 DEVKIT V1 de 30 pinos**, um DS18B20 e um display TFT
MAR2406/ILI9341 com barramento paralelo de 8 bits. O módulo RTC HW-084 do
protótipo tem um CI marcado DS3231M e uma EEPROM 24C32N. O firmware identifica
a tarefa de leitura do RTC como `rtc_ds3232m`. Use os números dos GPIOs indicados
na tabela para identificar os pinos da placa.

| Periférico | Sinal | Pino do ESP32 | Ligação ou função |
|---|---|---|---|
| DS18B20 | `DQ` | **GPIO13** | Dados 1-Wire, com resistor de 4,7 kΩ entre `DQ` e `3V3` |
| RTC | `SDA` | GPIO5 | Dados I²C, com pull-up para `3V3` |
| RTC | `SCL` | GPIO15 | Clock I²C, com pull-up para `3V3` |
| Relé de aquecimento (J4) | `IN` | **GPIO26** | Aciona em `HIGH` |
| Relé de resfriamento (J5) | `IN` | **GPIO25** | Aciona em `HIGH` |
| TFT | `LCD_D0` | GPIO22 | Bit 0 do barramento paralelo |
| TFT | `LCD_D1` | GPIO23 | Bit 1 do barramento paralelo |
| TFT | `LCD_D2` | GPIO4 | Bit 2 do barramento paralelo |
| TFT | `LCD_D3` | GPIO16 | Bit 3 do barramento paralelo |
| TFT | `LCD_D4` | GPIO17 | Bit 4 do barramento paralelo |
| TFT | `LCD_D5` | GPIO18 | Bit 5 do barramento paralelo |
| TFT | `LCD_D6` | GPIO19 | Bit 6 do barramento paralelo |
| TFT | `LCD_D7` | GPIO21 | Bit 7 do barramento paralelo |
| TFT | `LCD_WR` | GPIO14 | Pulso de escrita |
| TFT | `LCD_RS` | GPIO32 | Seleção entre comando e dados |
| TFT | `LCD_CS` | GPIO33 | Seleção da tela |
| TFT | `LCD_RST` | GPIO27 | Reset da tela |
| TFT | `LCD_RD` | `3V3` | Mantido em nível alto para operação somente de escrita |

As definições estão em [LocalTemperature.c](main/LocalTemperature.c),
[RtcConfig.h](main/RtcConfig.h), [RelayConfig.h](main/RelayConfig.h) e
[Mar2406Config.h](main/Mar2406Config.h).

- **DS18B20 e RTC.** Ligue `VDD` do sensor e `VCC` do RTC a `3V3` e conecte
  os respectivos `GND` ao terra comum. O RTC usa o endereço I²C `0x68`.
  GPIO5 e GPIO15 também participam da configuração de partida do ESP32.
- **Relés.** `RELAY_ACTIVE_LOW` está definido como `false`. O firmware usa
  `HIGH` para acionar e `LOW` para desligar as duas saídas. A alimentação do
  módulo depende do modelo. As entradas `IN` precisam aceitar o comando de 3,3 V.
- **TFT.** Use GND comum e siga a alimentação indicada para a revisão do shield.
  Se o módulo usar 5 V na alimentação, mantenha os sinais ligados ao ESP32 em 3,3 V.

A etapa de potência deve ser dimensionada para o compressor e montada com
proteção, isolamento e aterramento adequados.

## Preparar, compilar e gravar

Use um ambiente ESP-IDF configurado para o ESP32 clássico. O manifesto declara
ESP-IDF `>=5.0.0`, e o arquivo `dependencies.lock` registra uma resolução com
`6.0.0`.
As dependências incluem OneWire, DS18B20, cJSON e MQTT, conforme
[main/idf_component.yml](main/idf_component.yml).

No terminal ESP-IDF, a partir da raiz do repositório:

```powershell
idf.py set-target esp32
idf.py menuconfig
idf.py build
idf.py -p COMx flash monitor
```

Substitua `COMx` pela porta do dispositivo. Em uma instalação já configurada,
preserve o `sdkconfig` antes de executar `set-target`, que pode redefinir a
configuração. O arquivo [sdkconfig.defaults](sdkconfig.defaults) seleciona a
tabela de partições de [partitions.csv](partitions.csv).

Configure em `menuconfig`:

| Menu | Configuração |
|---|---|
| Beer Monitor - Wi-Fi and iSpindel | AP local, senha, rede externa opcional e validade da telemetria |
| Beer Monitor - Temperature control | Alvo, histerese, limites e tempos de proteção |
| Beer Monitor - Local recipe interface | Senha da página local e portal cativo |
| Beer Monitor - Fermentation recipe | Opção de iniciar a receita de exemplo; desabilitada por padrão |
| Beer Monitor - Ubidots STEM | Token, identificação do dispositivo e intervalo de envio |

Troque a senha de exemplo do AP. Não publique senhas, tokens ou um `sdkconfig`
com credenciais. A publicação atual usa MQTT/TCP sem TLS, conforme
[UbidotsTelemetry.c](main/UbidotsTelemetry.c).

## Usar a interface local

1. Conecte ao AP do ESP32, chamado `BeerMonitor` por padrão.
2. Abra o portal oferecido pelo aparelho ou acesse **http://192.168.50.1/**.
3. Entre com a senha local; se nenhuma senha específica foi configurada, use
   a senha do AP.
4. Cadastre as etapas e selecione **Salvar e iniciar receita**.

O endereço pode mudar conforme a configuração do AP. Para substituir uma
receita em execução, cancele-a pela interface primeiro. Ao terminar a última
etapa, o firmware mantém sua temperatura alvo. Consulte [RECEITAS.md](RECEITAS.md).

## Proposta de PCB

A proposta posiciona a tela na frente, a eletrônica no verso e os conectores
nas bordas. A antena do ESP32 fica junto à borda superior, fora da projeção da
tela. Dimensões, módulos e conectores ainda precisam de conferência mecânica.

## Organização e documentação

| Caminho | Conteúdo |
|---|---|
| [main/](main/) | Firmware C, configuração dos GPIOs e página web embarcada |
| [recipes/](recipes/) | Receita de exemplo e esquema JSON |
| [TemperatureControl.c](main/TemperatureControl.c) | Controle térmico e acionamento dos relés |
| [RECEITAS.md](RECEITAS.md) | Contrato JSON e execução das receitas |
| [LocalWebUi.c](main/LocalWebUi.c) | Interface de receitas e API local |
| [Kconfig.projbuild](main/Kconfig.projbuild) | Opções do projeto no `menuconfig` |

## Validação em bancada

Antes de conectar cargas, confira a polaridade real dos comandos, falha do
sensor, partida, reset e bloqueio do compressor em bancada.
