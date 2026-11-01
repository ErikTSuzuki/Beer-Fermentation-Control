# Receitas de fermentacao: dias e densidade

A tarefa `fermentation_recipe` escolhe a temperatura alvo de cada etapa e a
envia ao controlador termico existente. A regra de saida de cada etapa e:

```text
densidade <= limite, confirmada por novas leituras
                       OU
tempo de operacao na etapa >= max_days
```

O prazo maximo sempre funciona: densidade ausente, congelada, acima do limite,
fora da faixa, com unidade desconhecida/diferente ou leitura vencida nao impede
o avanco pelo tempo. Nao ha uma condicao adicional exigindo que a densidade
esteja estagnada: `max_days` e o limite absoluto daquela etapa, mesmo que ela
continue variando. Este contrato substitui o exemplo anterior que combinava
tempo minimo e densidade com AND.

## Tarefa e protecoes

- Uma tarefa FreeRTOS com prioridade 2, pilha de 8192 bytes e periodo de um
  segundo usa `vTaskDelayUntil`. Ela nao escreve nos GPIOs dos reles.
- `FermentationRecipeLogic.c` decide as transicoes; `FermentationRecipeJson.c`
  valida a receita; `FermentationRecipe.c` integra filas, iSpindel e NVS.
- O controlador de temperatura continua separado, com prioridade 4 e periodo
  de 500 ms. Toda mudanca de alvo passa por `temperature_control_set_target`,
  preservando histerese, intertravamento e bloqueio do compressor.
- A receita avanca no maximo uma etapa por ciclo. Uma demora longa da tarefa
  nao pula varias etapas sem aplicar suas temperaturas.
- A contagem comeca na entrada da etapa. Inclui o tempo para chegar ao alvo,
  periodos sem internet e falhas dos sensores enquanto o ESP32 permanece
  ligado. Falha do DS18B20 continua desligando os reles pelo controlador,
  independentemente do avanco da receita.
- Os prazos usam um relogio monotono de 64 bits. Um dia corresponde a 86400
  segundos; ajustes do RTC/NTP e a volta dos ticks do RTOS nao alteram os dias.
- Ao finalizar a ultima etapa, a receita passa a concluida e mantem seu ultimo
  alvo. Nao reinicia automaticamente nem desliga a refrigeracao.

## Contrato JSON v1

O arquivo completo esta em [recipes/exemplo.json](recipes/exemplo.json) e o
contrato para o futuro editor em [recipes/recipe.schema.json](recipes/recipe.schema.json).
Os numeros de temperatura/densidade deste exemplo ilustram o mecanismo;
nao sao uma recomendacao de processo para uma levedura especifica.

```json
{
  "version": 1,
  "name": "Exemplo 20-12-3 C",
  "gravity_unit": "sg",
  "gravity_confirm_samples": 3,
  "on_complete": "hold",
  "steps": [
    {"name": "Fermentacao inicial", "target_c": 20, "max_days": 5, "gravity_limit": 1.020},
    {"name": "Etapa intermediaria", "target_c": 12, "max_days": 5, "gravity_limit": 1.010},
    {"name": "Manutencao final", "target_c": 3, "max_days": 10, "gravity_limit": null}
  ]
}
```

| Campo | Regra |
|---|---|
| `version` | Obrigatorio, valor 1 |
| `name` | Nome da receita ou da etapa; obrigatorio, 1 a 48 bytes UTF-8 |
| `gravity_unit` | Obrigatorio: `sg` ou `plato`, para todos os limites da receita |
| `gravity_confirm_samples` | Opcional, inteiro de 1 a 10; padrao 3 |
| `on_complete` | Opcional, somente `hold`; padrao `hold` |
| `steps` | Lista ordenada com 1 a 16 etapas |
| `target_c` | Obrigatorio, temperatura alvo em graus Celsius |
| `max_days` | Obrigatorio, maior que zero e ate 365 dias por etapa; duracao minima de um segundo |
| `gravity_limit` | Opcional; omitido ou `null` desabilita o criterio de densidade na etapa |

`max_days` aceita fracoes: `0.5` representa 12 horas. O prazo comeca de novo
em cada etapa. Se a primeira etapa terminar em tres dias por densidade, os
cinco dias maximos da segunda contam a partir daquele momento, nao do quinto
dia originalmente previsto. Sem avanco por densidade, o exemplo executa
20 C por cinco dias, 12 C pelos cinco seguintes e 3 C por mais dez dias.
Depois continua mantendo 3 C.

Limites de densidade aceitos: SG de 0.8 a 1.5; Plato de -10 a 50. O firmware
nao converte silenciosamente entre unidades. A unidade enviada pelo iSpindel
precisa coincidir com a receita. `unknown` faz a etapa depender apenas do
prazo enquanto nao houver unidade correta.

O parser rejeita campos desconhecidos/repetidos, tipos incorretos, conteudo
extra depois do JSON, NULs, profundidade excessiva e arquivos maiores que
4096 bytes. A receita inteira e validada antes de aceitar qualquer mudanca.
Todos os alvos devem caber nos limites e diferenciais configurados no
controlador termico; um alvo invalido em uma etapa futura rejeita a receita
inteira, sem mudar a receita atual.

O JSON Schema valida a estrutura e as faixas gerais. O editor tambem deve
verificar o tamanho dos nomes em bytes UTF-8, o tamanho total do JSON e as
capacidades termicas configuradas no ESP32. A validacao final permanece no
firmware. O parser nao aceita caracteres NUL mesmo quando escapados.

## Confirmacao por densidade

Por padrao, sao necessarias tres novas leituras consecutivas em/abaixo do
limite. Reconsultar a mesma caixa postal a cada segundo nao conta como novas
medicoes. A identidade considera sensor, boot e sequencia; retransmissoes
identicas nao contam novamente nem renovam a idade no receptor.

Uma leitura acima do limite, uma leitura invalida/vencida, troca de sensor
ou uma lacuna maior que a validade configurada reinicia a confirmacao. A
validade usa `Maximum age of an iSpindel reading`, atualmente 1800 segundos
por padrao. Escolha essa validade considerando o intervalo de envio real.
Na nova etapa, as leituras da etapa anterior nao sao reaproveitadas para
completar a confirmacao. A confirmacao recomeca apos reiniciar o ESP32.

## Progresso e reinicializacao

A receita e o progresso sao gravados no namespace NVS `fermentation`, em
uma estrutura versionada unica. Sao salvos no inicio, nas transicoes, no
cancelamento e a cada 15 minutos de uma receita em andamento. Uma falha de
gravacao fica visivel na tela e em `FermentationRecipeStatus.storageOk`; ha
nova tentativa depois de um minuto. A temporizacao continua no mesmo boot,
mesmo durante falhas de gravacao.

Na partida, o firmware restaura a etapa e solicita seu alvo antes de iniciar
a tarefa que atua nos reles. A protecao completa do compressor continua
valendo. Tempo com o ESP32 desligado nao e contado. Uma queda inesperada
pode perder ate o ultimo intervalo de 15 minutos desde o checkpoint, mais
eventual atraso de escalonamento/gravacao. Falhas de flash tornam a recuperacao
do proximo boot incerta, incluindo a persistencia de um cancelamento.

Dados persistidos invalidos ou alvos incompatíveis com a configuracao atual
colocam a receita em falha e desabilitam o controle; nao reiniciam a etapa 1
nem apagam outros namespaces NVS. Uma nova receita valida pode ser enviada
explicitamente para recuperar esse estado.

## Como iniciar e cancelar

A tarefa existe sempre, mas **nao inicia o exemplo automaticamente por
padrao**. Ha duas formas de iniciar:

1. Em `menuconfig`, abra **Beer Monitor - Fermentation recipe** e habilite
   **Start embedded recipes/exemplo.json when no saved run exists**. Compile
   e grave para incorporar o arquivo. Essa opcao so inicia a receita se ainda
   nao existir registro de execucao no NVS.
2. Outra tarefa pode fornecer o JSON diretamente em tempo de execucao:

```c
#include <string.h>
#include "FermentationRecipe.h"

// json recebido de uma interface, com comprimento conhecido.
esp_err_t accepted = fermentation_recipe_submit_json(json, strlen(json));
// Quando solicitado pelo operador:
esp_err_t cancelled = fermentation_recipe_cancel();

FermentationRecipeStatus status;
bool available = fermentation_recipe_latest(&status);
```

`ESP_OK` confirma enfileiramento. A tarefa processa o comando no proximo
ciclo, e o controlador termico aplica o alvo no seu proximo ciclo. Consulte
o estado para confirmar a execucao. Se a fila estiver ocupada, retorna
`ESP_ERR_TIMEOUT`; os comandos nao sao sobrescritos silenciosamente.

Uma receita em andamento deve ser cancelada antes de outra ser iniciada.
Cancelar solicita os dois reles desligados e persiste o cancelamento; uma
receita concluida pode ser substituida por inicio explicito de outra. Para
controle manual, cancele a receita antes de usar a API direta de temperatura.

Uma execucao salva, mesmo concluida ou cancelada, tem precedencia sobre o
arquivo incorporado. Editar o arquivo ou desligar a opcao de autoinicio nao
substitui um lote salvo. A opcao nao impede a retomada de uma receita ativa.

O editor visual e o upload HTTP local agora estao implementados: veja
[INTERFACE_LOCAL.md](INTERFACE_LOCAL.md). O operador conecta-se ao AP do
ESP32 e configura/inicia a receita pelo navegador, sem recompilar ou
reiniciar. O envio remoto ao Ubidots inclui etapa e dias; nao ha comando MQTT.

A tela e a pagina local mostram etapa, dias decorridos/maximos e conclusao/
falha. Temperatura alvo, etapa e dias sao publicados no Ubidots; a API C e os
logs continuam disponiveis.

## Verificacao

`python3 tests/test_fermentation_recipe.py` compila e executa o parser real,
a logica real e a tarefa real com simulacoes de FreeRTOS, NVS e sensores.
Os testes cobrem os 20 dias do exemplo por tempo simulado, densidade
confirmada/repetida/vencida/ausente/incorreta, limites de JSON, prazo absoluto,
retomada da etapa, cancelamento, falhas de flash e criacao da tarefa, e a
prioridade de execucoes salvas sobre o autoinicio do exemplo.
