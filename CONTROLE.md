# Controle de temperatura com FreeRTOS

O ESP32 executa um controlador on-off local. O DS18B20 no GPIO4 fornece a
temperatura de realimentacao; o rele do aquecedor usa GPIO32 e o do
refrigerador usa GPIO33. O teste que alternava os reles a cada cinco segundos
foi removido. O iSpindel continua fornecendo dados para tela e nuvem; sua
temperatura e densidade nao comandam os reles nesta primeira versao.

## Configuracao

Em `idf.py menuconfig`, abra **Beer Monitor - Temperature control**:

| Parametro | Padrao | Comportamento |
|---|---:|---|
| Enable temperature control at boot | Sim | Habilita o controle assim que ha temperatura local valida |
| Initial target temperature | 1800 | 18,00 graus Celsius |
| Heating differential | 50 | Liga em alvo - 0,50 C; desliga ao atingir o alvo |
| Cooling differential | 50 | Liga em alvo + 0,50 C; desliga ao atingir o alvo |
| Minimum compressor off time | 300 s | Vale tambem na primeira partida apos reiniciar |
| Both outputs off before switching | 30 s | Intervalo entre aquecer e resfriar, nos dois sentidos |
| Maximum age of local sample | 5000 ms | Mais antigo que isso desliga ambos os reles |
| Minimum/maximum valid process temperature | -500 / 4000 | -5,00 a 40,00 C; fora da faixa desliga ambos |

Os valores de temperatura na configuracao sao inteiros em centesimos de grau:
1850 significa 18,50 C. O alvo e seus diferenciais precisam caber inteiramente
nos limites minimo/maximo. Combinacoes invalidas impedem a inicializacao,
mantendo os comandos dos reles desligados. Os cinco minutos sao um padrao
ajustavel; confira a exigencia do fabricante do refrigerador.

Com alvo de 18 C, um refrigerador desligado solicita partida em 18,5 C.
Enquanto resfria, permanece ligado em 18,4 C e desliga em 18 C. O aquecedor
segue a regra simetrica, ligando em 17,5 C. Dentro da faixa, um rele que ja
estava desligado continua desligado. Nao ha tempo minimo ligado: atingir o
alvo, desabilitar o controle ou detectar falha pode desligar imediatamente.

Em `main/RelayConfig.h`, a polaridade continua **ativa em LOW**. Confirme essa
polaridade no modulo real. Os estados publicados sao comandos do firmware;
nao existe retorno eletrico para confirmar se os contatos abriram/fecharam.
Posicione o DS18B20 para representar a temperatura do mosto: medir somente o
ar da camara altera a resposta do sistema.

## Tarefas e comunicacao

- `temperature_control`: prioridade 4, pilha de 4096 bytes, periodo de 500 ms
  com `vTaskDelayUntil`. E a unica tarefa que escreve nos GPIOs dos reles.
- `ds18b20_local`: continua com prioridade 3 e periodo de dois segundos.
  A conversao do sensor acontece nessa tarefa, sem bloquear o controlador.
- Temperatura, comandos e estado passam por caixas postais FreeRTOS de uma
  posicao. O controlador consulta a amostra com `xQueuePeek`, sem consumi-la
  da tela/nuvem. Um comando novo substitui o comando pendente anterior.
- Wi-Fi, HTTP, MQTT, display e RTC continuam em suas tarefas. O controle nao
  aguarda conexao ao roteador, publicacao ou resposta de rede.

`TemperatureControlLogic.c` contem a maquina de estados sem dependencias do
hardware. `TemperatureControl.c` executa essa maquina dentro da tarefa RTOS,
valida a idade da amostra, aplica os comandos aos GPIOs e publica o estado.

As protecoes de tempo usam `esp_timer_get_time`, monotono e de 64 bits.
Mudancas no RTC/NTP e a volta do contador de ticks do FreeRTOS nao encurtam
o bloqueio. A idade da temperatura usada no controle tambem usa esse relogio.

## Falhas, partida e mudancas de alvo

- A inicializacao coloca ambos os GPIOs em desligado antes dos servicos de
  rede. Sem amostra valida, nenhum rele liga.
- Dado ausente, erro do sensor, NaN, infinito, temperatura fora da faixa ou
  amostra vencida desliga os dois comandos. O prazo de cinco segundos e
  avaliado a cada 500 ms, sujeito ao escalonamento normal do RTOS.
- O retorno de uma amostra valida restaura o controle automaticamente, sem
  dispensar o bloqueio do compressor ou o intervalo entre modos.
- O tempo minimo desligado e contado desde a escrita que desliga o cooler.
  Alterar o alvo ou desabilitar/habilitar o controle nao reinicia nem elimina
  esse bloqueio. Antes de ligar novamente, a necessidade termica e reavaliada.
- Ao trocar de aquecimento para refrigeracao ou vice-versa, primeiro desliga,
  depois aguarda o intervalo. Se tambem houver bloqueio do compressor, vale
  o maior tempo restante.
- Erro retornado pela escrita dos GPIOs tenta desligar ambos e trava novas
  partidas ate reiniciar. Isso nao detecta contato de rele colado.
- Falha ao criar filas ou a tarefa impede a partida do controle e mantem os
  comandos desligados.
- Comandos manuais em tempo de execucao nao sao persistidos em flash. Sem
  receita salva, reiniciar usa os valores do `menuconfig`. Com uma receita,
  seu alvo e progresso sao restaurados antes da primeira atuacao, conforme
  [RECEITAS.md](RECEITAS.md).

## Interface para o executor de receitas e controle manual

Depois da inicializacao, outra tarefa pode chamar:

```c
// Unidades em graus Celsius na API C (nao em centesimos).
bool accepted = temperature_control_set_target(20.0f, true);
// Desabilita e solicita ambos desligados no proximo ciclo de controle.
bool stopped = temperature_control_set_target(20.0f, false);

TemperatureControlStatus status;
if (temperature_control_latest(&status)) {
    // Estado, alvo, temperatura validada, comandos dos reles e espera restante.
}
```

`true` significa que o comando foi aceito na caixa postal, nao que um rele
ligou: a aplicacao ocorre no proximo ciclo e continua sujeita as protecoes.
Alvos invalidos retornam `false` sem alterar o comando anterior. Para desligar,
use um alvo valido. O controle nao oferece uma funcao para forcar GPIOs.

O executor de receitas agora e uma tarefa separada, com interpretacao de
JSON, transicoes por densidade OU prazo maximo e persistencia em NVS.
Veja [RECEITAS.md](RECEITAS.md). A pagina e a API HTTP locais para configurar
perfis estao em [INTERFACE_LOCAL.md](INTERFACE_LOCAL.md). Cancele uma receita
ativa antes de alterar o controle manualmente.

## Tela e nuvem

A tela mostra alvo e estado em uma linha acima da temperatura local, ou a
contagem de bloqueio enquanto aguarda. Dados do iSpindel continuam visiveis.

O Ubidots recebe duas novas variaveis: `temperatura-alvo` e `estado-controle`.
O contexto do estado contem habilitacao, validade do sensor, comandos dos
reles e segundos restantes. A publicacao do controle ocorre mesmo que nao
haja amostras validas dos sensores, desde que o estado do controlador seja
recente (ate dois segundos). Se o estado estiver vencido, ele e omitido.
Sem internet, o controle continua; a publicacao espera a reconexao e o
intervalo configurado. Nao foi adicionada notificacao instantanea de alarmes.

| Codigo | Estado |
|---:|---|
| 0 | Desativado |
| 1 | Em espera |
| 2 | Aquecendo |
| 3 | Resfriando |
| 4 | Bloqueio do compressor |
| 5 | Bloqueio da troca de modo |
| 6 | Falha do sensor |
| 7 | Falha da escrita dos reles |
| 8 | Configuracao invalida |

## Validacao

Execute `python3 tests/test_temperature_control.py`. O teste compila o codigo
real da logica e o adaptador RTOS/GPIO com simulacoes de filas, sensor e GPIOs.
Inclui limites da histerese, partida, reentrada apos falhas, troca de alvo,
desabilitacao, polaridades LOW/HIGH, configuracao desabilitada na partida,
falhas de alocacao/criacao da tarefa/escrita e temperaturas vencidas em lotes
de mais de 90 dias. Sequencias variadas de entradas verificam as invariantes
do compressor e da troca de modo independentemente dos nomes dos estados.
Depois execute `python3 tests/test_control_telemetry.py` para conferir o JSON
real de publicacao, incluindo a falha sem sensores, idade do estado e limites
do buffer de envio.

Na bancada, primeiro com os contatos sem cargas, confira a polaridade,
observe os dois pontos da histerese, desconecte o DS18B20 durante cada modo,
confira o bloqueio apos reiniciar e a operacao sem roteador. Depois valide
a resposta termica com o fermentador e os equipamentos reais. Testes no
computador nao comprovam comportamento eletrico ou desempenho termico.
