# Dashboard, broker e implantação na VM

O código do servidor fica na pasta `IOT/server`, fora do repositório do
firmware. Na VM Ubuntu 24.04, os arquivos ficam em `/opt/fermentacao/app`.
O Docker Compose sobe Mosquitto, InfluxDB e o backend Node.js. O Caddy roda
como serviço do Ubuntu e fornece HTTPS no IP público da VM.

## Como o dashboard se comunica

O navegador usa HTML, CSS e JavaScript. O JavaScript chama a API do
**backend Node.js por HTTP(S)**. O backend usa Fastify para servir o painel
e a API, e usa MQTT.js para se conectar ao Mosquitto. O InfluxDB fica
na rede interna dos contêineres.

```text
Telemetria: iSpindel -- HTTP/JSON --> ESP32 -- MQTT por WSS --> Caddy
            Caddy --> Mosquitto -- MQTT --> Node.js --> InfluxDB
            Node.js -- SSE/HTTPS via Caddy --> navegador

Receitas:   navegador -- HTTPS POST via Caddy --> Node.js -- MQTT via Mosquitto --> ESP32
Retorno:    navegador <-- HTTPS/JSON via Caddy -- Node.js <-- MQTT via Mosquitto -- ESP32
```

### Telemetria e histórico

1. O ESP32 publica a telemetria no tópico
   `cervejaria/v1/fermentadores/fermentador01/telemetria` com QoS 1.
2. O Node.js assina `cervejaria/v1/fermentadores/+/telemetria` com a conta
   `coletor`, valida a mensagem e grava a leitura no bucket `telemetria`
   do InfluxDB, na organização `cervejaria`. O bucket da VM foi criado com
   retenção de 90 dias.
3. O JavaScript consulta `GET /api/history?device=fermentador01&hours=24`
   para montar os gráficos. Ele mantém uma conexão HTTP aberta com
   `EventSource` em `GET /api/events?device=fermentador01`. O backend envia
   um evento `reading` por SSE após a gravação, e a página atualiza os valores
   e consulta novamente o histórico.

O teste de terminal publica com a conta `simulador` no tópico de
`simulador01`. As leituras desse dispositivo podem ser consultadas pela API
de histórico. A lista de fermentadores do dashboard exibe os dispositivos
de produção.

### Início e cancelamento de receitas

1. O backend assina `cervejaria/v1/fermentadores/fermentador01/estado/receita`
   com a conta MQTT `painel`. O ESP32 publica nesse tópico a sessão MQTT,
   a revisão, a última receita, o estado e os limites de temperatura.
2. O JavaScript consulta `GET /api/recipe?device=fermentador01` a cada dois
   segundos enquanto o fermentador está selecionado. Para iniciar ou
   cancelar, envia `POST /api/recipe?device=fermentador01` com JSON. A ação
   `start` inclui a receita. As duas ações levam a sessão e a revisão atuais.
3. O Node.js confere a origem HTTPS (`PANEL_ORIGIN`), a sessão e a revisão.
   Ele gera um identificador para o pedido e publica em
   `cervejaria/v1/fermentadores/fermentador01/comandos/receita` com QoS 1,
   sem retenção e com expiração MQTT de cinco segundos.
4. O firmware confere a validade do pedido, encaminha-o à tarefa de
   receitas e publica o resultado com o mesmo identificador no tópico
   de estado. No cancelamento, confirma também os dois relés desligados.
5. O backend correlaciona a resposta MQTT com o pedido HTTP. A página
   mostra se a operação foi aplicada, rejeitada ou ficou sem confirmação.
   O campo `storage_ok` informa se a alteração foi salva no ESP32.

O fluxo usa o mesmo formato JSON e o mesmo executor de receitas da
[interface local](README.md#usar-a-interface-local). Para substituir uma
receita em execução, cancele-a, aguarde a confirmação e inicie a próxima.

| Arquivo | Responsabilidade |
|---|---|
| `IOT/server/public/app.js` | Consultas de histórico e eventos SSE |
| `IOT/server/public/recipe-editor.js` | Editor e chamadas HTTP de receita |
| `IOT/server/src/server.ts` | API, assinatura da telemetria e envio de SSE |
| `IOT/server/src/recipe.ts` | Estado, publicação e confirmação de comandos MQTT |
| `IOT/server/src/database.ts` | Escrita e consultas ao InfluxDB |
| [main/BrokerMqtt.c](main/BrokerMqtt.c) | Cliente MQTT do ESP32, telemetria e comandos |
| [main/FermentationRecipe.c](main/FermentationRecipe.c) | Executor e persistência da receita na NVS |

## Preparar e iniciar a VM

1. Instale [Docker Engine e o plugin Compose](https://docs.docker.com/engine/install/ubuntu/)
   e Python 3 no Ubuntu. Prepare
   `/opt/fermentacao/app` para os arquivos do servidor e
   `/opt/fermentacao/backups` para exportações do banco.
2. Copie os arquivos de `IOT/server` pelo SSH. Com uma chave `.ppk`, use
   [PSCP](https://the.earth.li/~sgtatham/putty/latest/htmldoc/Chapter5.html) no PC.
   Copie o `compose.yaml`, o `Dockerfile`, o `.dockerignore`, os manifestos Node,
   as pastas `src/` e `public/`, `local.py`, `mosquitto.conf` e
   `mosquitto.acl`. O Docker gera o build. As credenciais da VM são geradas
   na própria VM.
3. Na primeira instalação, execute os comandos abaixo. Na VM foi usada a
   imagem `influxdb:2.9.1` com um banco novo, pois a tag
   `influxdb:2.10.0` do Compose original estava indisponível no registro.
   Ajuste a tag no `compose.yaml` antes de subir os serviços.

```bash
cd /opt/fermentacao/app
python3 -B local.py compose-env
sudo docker compose --env-file .runtime/compose.env config --quiet
sudo docker compose --env-file .runtime/compose.env up -d --build
sudo docker compose --env-file .runtime/compose.env ps -a
```

`compose-env` recria o arquivo de ambiente. Faça as edições de
`PANEL_ORIGIN`, `PANEL_USER` e `PANEL_PASSWORD` depois de gerá-lo e
preserve o arquivo entre atualizações. O `mosquitto-init` cria o arquivo
de senhas e normalmente termina com `Exited (0)`. Os volumes
`mqtt-data` e `influx-data` guardam os dados entre recriações. A VM iniciou
com histórico vazio, e o banco local continuou no PC.

| Serviço | Porta e alcance |
|---|---|
| Caddy | TCP `80` para validação e redirecionamento, TCP `443` para HTTPS e WSS |
| Painel Node.js | `127.0.0.1:8090` na VM |
| MQTT por TCP | `mosquitto:1883` na rede Docker, com `127.0.0.1:18883` para teste na VM |
| MQTT por WebSocket | `127.0.0.1:19001` na VM, encaminhada à `9001` do Mosquitto |
| InfluxDB | `influxdb:8086` na rede Docker |

## HTTPS público e contas do painel

O acesso público usa uma versão atual do [Caddy](https://caddyserver.com/docs/install#debian-ubuntu-raspbian),
com configuração em `/etc/caddy/Caddyfile`. O certificado para IP público
é emitido no [perfil shortlived do Let’s Encrypt](https://letsencrypt.org/2026/01/15/6day-and-ip-general-availability)
e renovado pelo Caddy. O firewall do provedor e o da VM precisam permitir
entrada TCP nas portas 80 e 443. A porta 80 é usada pelo desafio HTTP-01.
Na Oracle Cloud, aplique as regras à sub-rede ou ao grupo de segurança
associado à interface da VM.

A configuração de duas contas usa `operador` para visualizar e comandar,
e `teste` para visualizar. Gere cada hash com `caddy hash-password` e
cole a linha completa do hash no Caddyfile. O endereço abaixo é de exemplo
e deve ser trocado pelo IP público nas duas ocorrências.

```caddyfile
{
    default_sni 203.0.113.10
}

https://203.0.113.10 {
    tls {
        issuer acme https://acme-v02.api.letsencrypt.org/directory {
            profile shortlived
            disable_tlsalpn_challenge
        }
    }

    route {
        reverse_proxy /mqtt 127.0.0.1:19001

        basic_auth {
            operador HASH_DO_OPERADOR
            teste HASH_DO_TESTE
        }

        @bloquearEscrita {
            expression `{http.auth.user.id} != "operador"`
            not method GET HEAD
        }
        respond @bloquearEscrita `{"error":"Este usuário só pode visualizar o painel."}` 403

        reverse_proxy 127.0.0.1:8090
    }
}
```

Valide e recarregue o Caddy antes de transferir o login HTTP para ele.

```bash
sudo caddy validate --config /etc/caddy/Caddyfile &&
sudo systemctl reload caddy
```

Na pasta `/opt/fermentacao/app`, execute
`sudo docker compose --env-file .runtime/compose.env port painel 8090`.
Depois que o Caddy recarregar e o comando mostrar `127.0.0.1:8090`, deixe `PANEL_USER` e
`PANEL_PASSWORD` vazios em `.runtime/compose.env` e configure
`PANEL_ORIGIN=https://IP_PUBLICO` com o endereço público real, sem caminho.
O Caddy passa a exigir o login HTTP, e o backend continua verificando a
origem dos pedidos de receita. Para revogar o teste, remova a linha
`teste` de `basic_auth` e recarregue o Caddy.

```bash
cd /opt/fermentacao/app
sudo docker compose --env-file .runtime/compose.env up -d --no-deps painel
```

## MQTT pela porta 443

O WebSocket em `/mqtt` é usado pelo ESP32. Ele tem uma rota própria no
Caddy, antes do login do painel, e autentica no Mosquitto com a conta
`fermentador01`. As regras em `mosquitto.acl` permitem ao ESP publicar
telemetria e estado e receber comandos do próprio dispositivo.

1. No final de `mosquitto.conf`, adicione o listener WebSocket, preservando
   o listener TCP e as regras de autenticação existentes.

   ```conf
   listener 9001
   protocol websockets
   ```

2. Em `compose.yaml`, adicione o mapeamento WebSocket em
   `services.mosquitto.ports`, junto ao mapeamento TCP.

   ```yaml
       ports:
         - "${MQTT_HOST_BIND:-127.0.0.1}:${MQTT_HOST_PORT:-18883}:1883"
         - "127.0.0.1:19001:9001"
   ```

3. Recrie o broker e confira se a porta 9001 do contêiner aponta para
   `127.0.0.1:19001`. O Caddy usa esse endereço como destino de `/mqtt`.

   ```bash
   cd /opt/fermentacao/app
   sudo docker compose --env-file .runtime/compose.env config --quiet &&
   sudo docker compose --env-file .runtime/compose.env up -d --no-deps --force-recreate mosquitto &&
   sudo docker compose --env-file .runtime/compose.env port mosquitto 9001
   ```

4. No PC, em `idf.py menuconfig` → **Beer Monitor - MQTT broker**, configure
   a URI `wss://IP_PUBLICO/mqtt`, o dispositivo `fermentador01` e a
   `MQTT_DEVICE_PASSWORD` da VM. A senha MQTT é separada do login do
   dashboard. Compile e grave o ESP32.

## Verificar e diagnosticar

Na implantação atual, a checagem externa da rota `/mqtt` retornou
`101 Switching Protocols`. O monitor serial e o histórico do painel
permitem confirmar a autenticação e a chegada das leituras do ESP32 real.

| Verificação | Resultado esperado ou próxima checagem |
|---|---|
| Abrir o painel por HTTPS em outra rede | O certificado é aceito, o navegador pede o login e o painel carrega. |
| `GET /api/health` com login | `{"mqtt":true}` indica que o coletor assinou o tópico no Mosquitto. |
| Login `teste` | O histórico abre. Um `POST /api/recipe` devolve `403`. |
| Negociação WebSocket em `/mqtt` | HTTP `101 Switching Protocols` indica que o Caddy alcançou o listener WebSocket. |
| Monitor serial do ESP32 | `MQTT conectado` e assinatura do tópico de comandos confirmada. |
| Painel em `fermentador01` | O horário da última leitura avança e o gráfico recebe os dados do ESP32. |
| Receitas em bancada | O início é confirmado pelo ESP32. O cancelamento confirma ambos os relés desligados. |
| HTTPS inacessível ou desafio HTTP-01 falhando | Confira TCP 80 e 443 no firewall do provedor e da VM. |
| `Sec-WebSocket-Accept not found` e HTTP `502` em `/mqtt` | Rode o comando de porta do passo 3 e confira os logs do Mosquitto. O mapeamento esperado é `127.0.0.1:19001`. |

Logs úteis na VM:

```bash
cd /opt/fermentacao/app
sudo docker compose --env-file .runtime/compose.env logs --tail=60 mosquitto painel
sudo journalctl -u caddy -n 60 --no-pager
```

Para conferir a folga da VM, use `free -h`, `df -h /`,
`sudo docker stats --no-stream` e `sudo docker system df`.

Guarde a chave SSH, o `sdkconfig`, o `.runtime` e as credenciais fora do Git.
