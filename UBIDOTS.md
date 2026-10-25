# Beer Monitor: Wi-Fi hibrido e Ubidots

## Arquitetura

```text
iSpindel (ESP8266, STA) -- HTTP/JSON via Wi-Fi --> ESP32 (SoftAP)
                                               |
                                          ESP32 (STA)
                                               |
                                      Wi-Fi externo 2,4 GHz
                                               |
                                        MQTT -> Ubidots
```

O ESP32 usa `WIFI_MODE_APSTA`: cria uma rede WPA2 local para um iSpindel
(maximo de um cliente associado) e conecta a interface STA ao roteador.
O iSpindel nao precisa acessar a internet. Nao ha NAT/compartilhamento de
internet; o proprio ESP32 publica as leituras por MQTT. DS18B20 local,
tela, RTC e reles continuam usando as tarefas existentes.

AP e STA compartilham um unico radio e o mesmo canal. O SoftAP inicia no
canal 6 e acompanha automaticamente o canal do roteador ao associar.
Nao ha alternancia manual de canal para publicar na nuvem. Uma busca ou
mudanca de canal pode interromper brevemente o enlace local; o iSpindel
busca o SSID ao acordar e pode refazer a associacao durante o envio.
Na queda do roteador, o AP local permanece ativo; nao forcamos retorno
ao canal 6 enquanto houver clientes locais. A rede externa e tentada
novamente a cada 30 s, independentemente de MQTT estar habilitado.

## Configurar o ESP32

Em `idf.py menuconfig`, abra **Beer Monitor - Wi-Fi and iSpindel**:

- **Local Wi-Fi SSID**: `BeerMonitor` por padrao.
- **Local Wi-Fi password**: `beer-monitor` por padrao (senha de exemplo;
  substitua na configuracao e use a mesma senha no portal do iSpindel).
- **Local AP IPv4 address**: `192.168.50.1`, mascara `/24`. Escolha uma
  sub-rede diferente da usada pelo roteador externo. O DHCP atribui o IP
  do iSpindel; o endereco do ESP32 e obtido como gateway DHCP.
- **Maximum age of an iSpindel reading**: 1800 s por padrao. Escolha um
  valor maior que o intervalo de envio do iSpindel, com margem para falhas.
  Esse limite vale tanto para a tela quanto para o envio a nuvem.
- **Connect to an external Wi-Fi network**: habilite e preencha o SSID e
  senha da rede externa de 2,4 GHz, diferentes da rede local do ESP32.
- Em redes com usuario/senha, habilite **Use WPA2-Enterprise** e preencha
  usuario, senha do usuario e identidade EAP opcional (vazia = usuario).
  Autenticacao por portal cativo e EAP-TLS nao estao implementadas.
  A configuracao Enterprise existente nao inclui CA do servidor RADIUS.

Em **Beer Monitor - Ubidots STEM**, preencha o token, device label e
intervalo MQTT (300 s por padrao). SSID/senhas/token existentes no
`sdkconfig` local sao preservados; as opcoes do AP recebem os padroes
acima na reconfiguracao. Credenciais nao sao impressas nos logs.

Para funcionar somente localmente, desabilite **Connect to an external
Wi-Fi network**. Desabilitar somente MQTT mantem a conexao externa.
Recompile sempre antes de gravar; binarios antigos de diagnostico em
`build/local_canal6` ainda usam a arquitetura anterior.

```powershell
idf.py build
idf.py -p COM6 flash monitor
```

Ajuste a porta serial ao seu ESP32. Nenhuma placa e gravada pela compilacao.

## Hora do RTC

O ESP32 inicia SNTP em `pool.ntp.org` depois de obter IP na rede externa.
Ao receber uma sincronizacao, grava a hora de Brasilia no DS3232M e a
tela passa a mostrar esse horario. O fuso fica em `main/RtcConfig.h`:
`RTC_TIMEZONE="BRT3"` (formato POSIX, UTC-03:00 sem horario de verao).
O cliente repete a sincronizacao conforme `CONFIG_LWIP_SNTP_UPDATE_DELAY`
(uma hora por padrao) e e reiniciado quando a conexao Wi-Fi retorna.

Procure `RTC sincronizado por NTP (Brasilia): ...` no monitor. Sem acesso
ao servidor NTP, o modulo continua fornecendo a hora que ja possui; uma
hora valida nao e substituida pela hora da compilacao. Se o RTC perdeu a
hora, a compilacao ainda serve como ajuste aproximado ate chegar o NTP.
As notificacoes de rede nao acessam I2C: toda leitura e gravacao ocorre
na tarefa do RTC, com nova tentativa quando uma gravacao falha.

Teste local: `python tests/test_rtc_clock.py`. Ele cobre o fuso, viradas
de data/ano, funcionamento offline, reconexao e repeticao de gravacoes.

## Configurar e compilar o iSpindel

O firmware neste workspace esta em `firmware_spidnel/iSpindel`.

1. Grave o novo firmware e abra o portal de configuracao por duplo reset.
2. Selecione a rede **local do ESP32** (`BeerMonitor` por padrao) e informe
   sua senha. Substitua as credenciais antigas do roteador, caso existam.
   O firmware usa o SSID/senha salvos no portal; nao usa o campo Server/API
   para escolher o destinatario deste enlace.
3. Ajuste **Update Interval (s)**. Foi removida a sobrescrita de bancada de
   10 s: agora vale o intervalo salvo (padrao original: 900 s).
4. Salve a configuracao e saia do portal. No primeiro ciclo apos salvar,
   o fluxo original usa 1 s de sono para teste; os demais usam o valor salvo.

Sem SSID salvo, `pio/src/BrewWifi.h` possui os mesmos padroes de SSID/senha
do AP do ESP32. Esses valores sao apenas reserva para configuracao vazia;
eles nao substituem uma rede antiga ja salva no portal. Calibracao e
polinomio de densidade continuam no portal. A unidade continua `unknown`
ate ajustar `brewDensityUnit` conforme o polinomio (SG ou Plato).

O PlatformIO e as dependencias ja estao em `.pio/`. No PowerShell, a partir
da pasta do iSpindel:

```powershell
$env:PYTHONPATH = Join-Path $PWD '.pio/tooling'
$env:PLATFORMIO_CORE_DIR = Join-Path $PWD '.pio/core'
$env:PLATFORMIO_BUILD_FLAGS = '-I.pio/build-support'
python -m platformio run -e d1_mini_standalone
```

O resultado e `.pio/build/d1_mini_standalone/firmware.bin`.
A integracao AWS legada inclui `secrets.h`; o ambiente local existente
possui um cabecalho com certificados vazios em `.pio/build-support`.
O fluxo ativo Wi-Fi/HTTP nao usa AWS. Em outro ambiente, esse cabecalho
e as dependencias devem ser preparados antes da compilacao.

## Protocolo e recuperacao

O transporte ativo usa **HTTP sobre TCP**, porta `80`, com
`POST /ispindel` e `Content-Type: application/json`. O iSpindel usa o IP do
gateway recebido por DHCP: por padrao, `http://192.168.50.1/ispindel`.
Recompile e grave **ambos** os dispositivos. Firmwares antigos ESP-NOW ou
UDP nao sao compativeis com esse receptor HTTP/JSON.

Exemplo de corpo da requisicao (nomes dos campos diferenciam maiusculas):

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

`gravity_unit`: `0` = desconhecida, `1` = SG, `2` = Plato. Os identificadores
sao inteiros positivos de 32 bits; `boot_id` muda a cada despertar e
`sequence` identifica a leitura dentro da mesma execucao. A versao e `1`.

Resposta de sucesso, enviada somente depois de aceitar a leitura:

```json
{"accepted":true,"version":1,"sensor_id":1193046,"boot_id":123,"sequence":1}
```

O iSpindel exige HTTP `200` e um JSON com `accepted: true`, versao e os
mesmos identificadores. Receber apenas uma resposta TCP ou HTTP 200 nao
confirma a leitura. A confirmacao e local ao ESP32, nao ao Ubidots.

O ESP32 aceita corpos de ate 511 bytes, rejeita JSON malformado, campos
obrigatorios ausentes, tipos incorretos, identificadores fora da faixa,
valores nao finitos e leituras fora dos limites dos sensores. Campos
repetidos e objetos aninhados sao rejeitados. O primeiro MAC de sensor
valido fica selecionado ate reiniciar. Conexoes destinadas ao IP externo
sao fechadas antes de ler HTTP; o remetente precisa estar associado e
possuir um IP atribuido pelo DHCP do AP local. O servidor nao usa TLS;
a rede local utiliza WPA2.

Respostas de erro incluem `accepted: false`: `400` para dados invalidos,
`403` para origem nao autorizada, `409` para outro sensor ou sequencia
reutilizada com valores diferentes, `413` para tamanho invalido, `415`
para outro Content-Type e `408` para corpo incompleto/tempo excedido.

O iSpindel aguarda ate 15 s por associacao/DHCP e faz ate tres requisicoes
por associacao, com timeout de 2 s nas operacoes de rede. Em falhas de
conexao, timeout, resposta incorreta ou erro transitorio, repete o mesmo
JSON e pode refazer uma vez a associacao com busca do SSID. Erros de
dados/configuracao (`400`, `403`, `409`, `413`, `415`, `422`) encerram as
tentativas daquele ciclo. Depois do sucesso ou do limite de tentativas,
desconecta e volta ao sono configurado. Nao existe fila persistente:
uma leitura sem confirmacao pode ser perdida.

Uma retransmissao identica recebe novamente HTTP 200, sem duplicar o
contador/log nem renovar a idade da amostra. Isso cobre a perda da resposta
apos o ESP32 aceitar a leitura. As estruturas antigas sao usadas apenas
internamente pela tela/nuvem; nenhum pacote binario e enviado neste enlace.
Nao e necessario fixar MAC ou canal no transmissor.

## Publicacao MQTT

- Broker: `industrial.api.ubidots.com`, porta `1883`, TCP sem TLS.
- Topico: `/v1.6/devices/<device-label>`.
- Usuario MQTT: token do Ubidots; senha vazia; MQTT 3.1.1, QoS 0,
  `retain=false`, identificador unico baseado no MAC do ESP32.
- Variaveis: `temperatura-local`, `temperatura-ispindel`, `inclinacao`,
  `densidade` e `bateria`. A densidade inclui unidade e sensor no contexto.
- Leitura local com mais de 5 s e omitida; iSpindel usa o limite ajustavel
  acima. Nao ha historico offline; cada publicacao usa as ultimas amostras
  ainda validas. A mesma amostra pode aparecer em mais de uma publicacao
  quando o sono do iSpindel for maior que o intervalo MQTT.
- Falhas de publicacao tambem aguardam o intervalo. QoS 0 confirma envio
  ao transporte local, sem confirmacao de armazenamento pelo broker.

## Teste nas placas

1. Confira `SoftAP ... ativo` e `Recepcao HTTP/JSON ativa` no ESP32.
2. Confira `Wi-Fi conectado` e `Leitura confirmada pelo ESP32 via HTTP/JSON` no iSpindel; a tela
   deve exibir os mesmos valores de temperatura, inclinacao e bateria.
3. Confira IP externo, `MQTT conectado ao Ubidots` e os pontos no dashboard.
4. Desligue o roteador: o iSpindel deve continuar enviando ao AP local.
5. Religue o roteador em outro canal (por exemplo 1 -> 11). Aguarde a
   reconexao do ESP32 e o proximo ciclo do iSpindel, que deve reencontrar
   o SSID sem alterar configuracoes de canal.
6. Desligue o ESP32 durante um ciclo: o iSpindel deve encerrar as tentativas
   e dormir; religue e confira o retorno no proximo ciclo.
7. Verifique que a tela/nuvem respeitam o prazo de validade configurado.

Os dois firmwares foram compilados no ambiente local (ESP-IDF 6.0 e
Arduino ESP8266 3.1.2). Os testes executam o codigo real do transmissor
com Wi-Fi/HTTP simulados e o decodificador real do receptor com cJSON.
Passaram 38 cenarios do transmissor, 59 do decodificador JSON e 19 do
handler HTTP. Cobrem confirmacoes corretas/incorretas, status HTTP, limites de tentativas,
reconexao, tipos/limites dos campos, corpo TCP fragmentado, resposta perdida,
retransmissao sem duplicar/renovar leituras e compatibilidade entre o JSON
produzido pelo iSpindel e o decodificador do ESP32.
Para repetir, na pasta `beer_monitor`, com compiladores C/C++ locais:

```powershell
python tests/test_hybrid_sender.py C:/MinGW/bin/g++.exe
python tests/test_http_json.py C:/MinGW/bin/gcc.exe
python tests/test_http_receiver.py C:/MinGW/bin/gcc.exe
```

Os arquivos gerados pelo teste ficam em `build/hybrid-tests`.

Compilacao e testes no computador nao comprovam alcance, consumo de
bateria ou convivencia do radio. Esses pontos exigem os testes acima.

## Referencias

- [Espressif: coexistencia AP/STA e canal compartilhado](https://docs.espressif.com/projects/esp-idf/en/v5.4/esp32/api-guides/wifi.html)
- [ESP8266: associacao, DHCP e gateway](https://arduino-esp8266.readthedocs.io/en/latest/esp8266wifi/station-class.html)
- [Ubidots: broker e autenticacao MQTT](https://help.ubidots.com/en/articles/9350714-ubidots-mqtt-broker)

- [Espressif: servidor HTTP](https://docs.espressif.com/projects/esp-idf/en/v6.0/esp32/api-reference/protocols/esp_http_server.html)
- [Componente cJSON](https://components.espressif.com/components/espressif/cjson/versions/1.7.19/readme)
