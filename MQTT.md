# Wi-Fi e broker MQTT

O iSpindel envia leituras HTTP/JSON ao ESP32 pela rede local `BeerMonitor`.
Pela rede do roteador, o ESP32 publica a telemetria no Mosquitto e recebe
comandos do painel. O backend grava as leituras no InfluxDB.

## Configurar o ESP32

Em `idf.py menuconfig`, abra **Beer Monitor - Wi-Fi and iSpindel**.
Configure a rede própria do ESP32, usada pelo iSpindel e pela página local,
e habilite a conexão do ESP32 à rede Wi-Fi de 2,4 GHz que alcança o broker.
O AP usa `192.168.50.1/24` por padrão. Use uma sub-rede diferente da do
roteador. AP e STA compartilham um canal e o AP acompanha o roteador.

Em **Beer Monitor - MQTT broker**, configure a URI, o dispositivo, a senha
e o intervalo de publicação. Na instalação local, use
`mqtt://IP_DO_PC:18883` e o dispositivo `fermentador01`. O identificador
também é o usuário MQTT. A senha local fica no campo
`MQTT_DEVICE_PASSWORD` de `.runtime/credentials.json` na pasta do servidor.
O intervalo padrão é de 15 segundos. O firmware aceita `mqtts://` e `wss://` com
certificados confiáveis pelo bundle do ESP-IDF.

Na instalação do servidor fora deste repositório, inicie o broker pela
interface de rede local com `python3 -B local.py start --mqtt-lan IP_DO_PC`.
O painel abre em <http://127.0.0.1:8090> no próprio PC. O servidor também
tem uma configuração Docker Compose. Consulte o README da instalação do
servidor e o [guia da VM](SERVIDOR.md) para a implantação em contêineres.

## MQTT remoto pelo Caddy

Configure a URI do ESP32 como `wss://IP_PUBLICO/mqtt` e use a
`MQTT_DEVICE_PASSWORD` da VM. O ESP32 usa a porta HTTPS `443` e valida o
certificado do IP público com o conjunto de autoridades do ESP-IDF.
Consulte [MQTT pela porta 443](SERVIDOR.md#mqtt-pela-porta-443) para configurar
o listener WebSocket e o encaminhamento pelo Caddy.

## Tópicos

| Tópico sob `cervejaria/v1/fermentadores/fermentador01` | Fluxo |
|---|---|
| `/telemetria` | ESP32 → coletor → InfluxDB e painel |
| `/comandos/receita` | Backend do painel → ESP32 |
| `/estado/receita` | ESP32 → backend do painel |

A telemetria inclui temperaturas, inclinação, bateria, densidade e estado
do controlador e da receita. Usa QoS 1 sem retenção. O ESP32 envia
horário UTC quando o relógio do sistema está válido. O coletor usa a
hora de chegada como alternativa.

O ESP32 informa o estado, os limites térmicos e a última receita. O comando
leva um identificador, a sessão MQTT, uma validade curta e a revisão da
receita. O painel só confirma a operação após a resposta do ESP32.
No cancelamento, o firmware também verifica o estado dos dois relés.
O painel informa falhas de gravação e falta de confirmação. O formato
da receita é o mesmo da [interface local](README.md#usar-a-interface-local)
e está descrito em [RECEITAS.md](RECEITAS.md).

## Comunicação com o iSpindel

No portal do iSpindel, configure o SSID e a senha do AP do ESP32. O
transmissor usa como destino o gateway recebido por DHCP e publica em
`POST /ispindel`, na porta `80`, com `Content-Type: application/json`.

```json
{
  "version": 1,
  "sensor_id": 1193046,
  "boot_id": 123,
  "sequence": 1,
  "gravity_unit": 0,
  "temperature_c": 20.5,
  "tilt_degrees": 45,
  "gravity": 1.05,
  "battery_v": 4.1
}
```

A unidade de densidade usa `0` para desconhecida, `1` para SG e `2` para
Plato. O receptor devolve HTTP 200 com `accepted: true`, a versão e os
mesmos `sensor_id`, `boot_id` e `sequence`. Uma retransmissão idêntica
recebe a confirmação sem duplicar nem renovar a leitura. Ajuste a
validade máxima no ESP32 para superar o intervalo do iSpindel com margem.

## Relógio e verificação

O ESP32 sincroniza o relógio do sistema por SNTP quando o roteador permite
o acesso ao servidor de hora e grava a hora de Brasília no RTC. As opções
estão em [RtcConfig.h](main/RtcConfig.h). O controle térmico e a contagem
da receita continuam no ESP32 durante quedas da rede.

No monitor serial, confira a associação Wi-Fi e a mensagem
`MQTT conectado`. No painel, selecione o fermentador e confira o horário
da última leitura. Para verificar os comandos em bancada, inicie uma
receita pelo painel, confirme a primeira etapa no ESP32 e cancele pelo
painel para conferir o desligamento dos relés.
