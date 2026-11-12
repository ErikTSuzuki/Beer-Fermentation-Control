# Cerveja Monitor 🍺

Monitor e controlador de fermentação com **ESP32, ESP-IDF e FreeRTOS**. Mede a
temperatura com um DS18B20 local, recebe a telemetria de um iSpindel e comanda
aquecimento e refrigeração por histerese. O operador configura receitas pelo
navegador e acompanha os dados na tela ou no painel do servidor MQTT.

## Funcionamento

```text
DS18B20 ──────────────> controle local ──────> comandos de aquecer / resfriar
                              ↑                         ↓
iSpindel ── Wi-Fi/HTTP ──> ESP32 + receitas      interface / potência externa
                              │
                 ┌────────────┼─────────────┐
              TFT + RTC   navegador     Wi-Fi externo
                          no AP local        └── MQTT → Mosquitto → painel e InfluxDB
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

## Hardware e pinagem atual

Base: ESP32 clássico, DS18B20 alimentado em 3,3 V, display paralelo de 8 bits
configurado como MAR2406/ILI9341 e interface para dois comandos de relé.
O protótipo usa um ESP32 DEVKIT V1 de 30 pinos e um módulo RTC HW-084.
Confira a revisão dos módulos antes de montar.

### Pinos utilizados no projeto

| Pino do ESP32 | Conexão no projeto |
|---|---|
| GPIO4 | DS18B20 — DQ; pull-up externo de 4,7 kΩ para 3V3 |
| GPIO5 | RTC — SDA; pull-up para 3V3 |
| GPIO15 | RTC — SCL; pull-up para 3V3 |
| GPIO32 | Comando do aquecimento — ativo em HIGH |
| GPIO33 | Comando da refrigeração — ativo em HIGH |
| GPIO13 | TFT — LCD_D0 |
| GPIO14 | TFT — LCD_D1 |
| GPIO16 | TFT — LCD_D2 |
| GPIO17 | TFT — LCD_D3 |
| GPIO18 | TFT — LCD_D4 |
| GPIO19 | TFT — LCD_D5 |
| GPIO21 | TFT — LCD_D6 |
| GPIO22 | TFT — LCD_D7 |
| GPIO23 | TFT — LCD_WR |
| GPIO25 | TFT — LCD_RS |
| GPIO26 | TFT — LCD_CS |
| GPIO27 | TFT — LCD_RST |
| 3V3 | Alimentação do DS18B20 e do RTC, pull-ups e TFT LCD_RD mantido alto |
| VIN / 5 V | Alimentação de 5 V da TFT, conforme a revisão do shield e o caminho de alimentação do DevKit |
| GND | Referência comum dos circuitos de baixa tensão |

A pinagem foi conferida em [Mar2406Config.h](main/Mar2406Config.h),
[RtcConfig.h](main/RtcConfig.h), [RelayConfig.h](main/RelayConfig.h) e
[LocalTemperature.c](main/LocalTemperature.c). Neste firmware, o I²C do RTC
usa **GPIO5/15**, e os GPIO21/22 pertencem à TFT. Confira a tensão de
alimentação da tela para a revisão do módulo. GPIOs operam em 3,3 V.
GPIO5 e GPIO15 participam da configuração de partida do ESP32.

Os GPIO32 e GPIO33 enviam comandos à interface de potência. Confira o nível
de acionamento e o desligamento durante a partida em bancada antes de conectar
as cargas.

## Preparar, compilar e gravar

Use um ambiente ESP-IDF configurado para o ESP32 clássico. O manifesto declara
ESP-IDF `>=5.0.0`, e o projeto foi compilado com ESP-IDF 6.1.
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
| Beer Monitor - MQTT broker | URI, identificador, senha e intervalo de telemetria |

Troque a senha de exemplo do AP e mantenha o `sdkconfig` com credenciais
fora do Git. Consulte [MQTT.md](MQTT.md) para ligar o ESP32 ao broker.

## Usar a interface local

1. Conecte ao AP do ESP32, chamado `BeerMonitor` por padrão.
2. Abra o portal oferecido pelo aparelho ou acesse **http://192.168.50.1/**.
3. Entre com a senha local; se nenhuma senha específica foi configurada, use
   a senha do AP.
4. Cadastre as etapas e selecione **Salvar e iniciar receita**.

O endereço pode mudar conforme a configuração do AP. Para substituir uma
receita em execução, cancele-a pela interface primeiro e aguarde a confirmação.
Ao terminar a última etapa, o firmware mantém sua temperatura alvo.
O editor no painel do servidor usa o mesmo formato de receita.
Consulte [RECEITAS.md](RECEITAS.md).

## Organização e documentação

| Caminho | Conteúdo |
|---|---|
| [main/](main/) | Firmware C, configuração dos GPIOs e página web embarcada |
| [recipes/](recipes/) | Receita de exemplo e esquema JSON |
| [RECEITAS.md](RECEITAS.md) | Contrato JSON, execução e persistência das receitas |
| [MQTT.md](MQTT.md) | Wi-Fi, integração com iSpindel, RTC e broker |
| [SERVIDOR.md](SERVIDOR.md) | Fluxos do dashboard, implantação na VM, acesso HTTPS e verificação |

Após compilar o firmware, execute `python3 -B tests/test_mqtt_commands.py` com
GCC no Linux para verificar a validação dos comandos MQTT sem usar uma placa.
