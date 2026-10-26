# Configurar localmente e acompanhar pelo Ubidots

O ESP32 serve uma pagina de configuracao no proprio Wi-Fi local, sem precisar
de aplicativo instalado ou acesso a internet. O controle e a receita continuam
em tarefas FreeRTOS independentes do navegador.

## Uso pelo operador

1. Conecte o celular ou computador ao Wi-Fi local do ESP32 (`BeerMonitor`
   por padrao). Se o celular avisar que essa rede nao tem internet, mantenha
   a conexao enquanto configura.
2. Aguarde a pagina de entrada da rede ou toque no aviso **Entrar na rede**.
   Se o aparelho nao oferecer a pagina, abra **http://192.168.50.1/** no
   navegador. Esse e o endereco padrao; se a instalacao alterou `BEER_AP_IP`,
   use o endereco configurado.
3. Entre com a senha de acesso local. Sem senha propria configurada, ela e
   a mesma senha usada para conectar ao Wi-Fi local do ESP32.
4. Preencha o nome da receita e suas etapas: nome, temperatura alvo, maximo
   de dias e, opcionalmente, densidade limite. A unidade pode ser SG ou Plato.
5. Confira a curva e clique em **Salvar e iniciar receita**. Aguarde a mensagem
   confirmando que a receita esta salva e em execucao.
6. Feche a pagina e acompanhe o lote no seu painel do Ubidots. O ESP32 precisa
   estar conectado a rede externa e configurado para publicar na conta.

A pagina mostra temperatura medida, alvo, densidade, comandos dos reles,
etapa e dias decorridos/maximos. As leituras locais se atualizam a cada dois
segundos. Perder a conexao ao navegador nao interrompe a receita. Durante
a perda de conexao, os valores anteriores sao identificados como antigos e
os botoes de comando ficam indisponiveis.

## Abertura ao conectar: portal cativo

O portal cativo vem habilitado. O DHCP do AP anuncia o proprio ESP32 como
DNS para seus clientes. Uma tarefa FreeRTOS `captive_dns`, com prioridade 1,
responde consultas IPv4 apontando para o IP local. As consultas de verificacao
HTTP dos aparelhos recebem um redirecionamento para o endereco canonico da
pagina de receitas. A senha do Wi-Fi e a senha da interface continuam valendo.

Isso permite que Android, iOS e computadores reconhecam uma pagina de entrada.
A abertura automatica depende do sistema, configuracoes de DNS/VPN e do
historico da rede naquele aparelho: alguns abrem uma janela, outros exibem
um aviso e outros exigem abrir o endereco manualmente. A implementacao usa
DNS e verificacoes HTTP, sem anunciar uma API CAPPORT via DHCP 114.

A rede local continua sem compartilhamento de internet. O aparelho pode
pedir que o usuario mantenha a conexao mesmo assim. Em alguns sistemas,
cancelar a janela de entrada pode desconectar o Wi-Fi; escolha a opcao de
continuar sem internet quando ela existir. Para importar/exportar arquivos
ou usar recursos que a janela de entrada limitar, abra o endereco direto
no Chrome/Safari ou no navegador habitual.

O portal nao intercepta HTTPS nem apresenta certificados de outros sites.
As respostas DNS ficam restritas ao IP e a interface do AP, com TTL zero;
o resolvedor da interface STA usado pelo Ubidots/NTP permanece separado.
O iSpindel continua usando `POST /ispindel` diretamente no IP do gateway,
sem login de navegador ou redirecionamento. A tarefa de controle termico
tem prioridade maior que DNS e HTTP.

Uma falha ao iniciar o DNS gera aviso no log e preserva o acesso direto por
IP. Para desativar esse comportamento, use **Offer recipe page when a device
joins the local Wi-Fi** em **Beer Monitor - Local recipe interface** no
`menuconfig`. A configuracao por IP funciona com a opcao ligada ou desligada.

Referencias: [deteccao de portal no Android](https://developer.android.com/about/versions/11/features/captive-portal)
e [redes cativas no iPhone/iPad](https://support.apple.com/en-gb/102554).

O editor permite adicionar, remover e reordenar ate 16 etapas, importar um
JSON existente, visualizar o JSON gerado e exportar uma copia. Nenhum arquivo
e enviado para servicos externos pela pagina. A receita configurada so entra
em uso ao clicar em **Salvar e iniciar**; alterar campos ou importar um arquivo
nao modifica a receita em andamento.

Para substituir uma receita em andamento, use **Cancelar receita e desligar**
e confirme na propria pagina. Aguarde a confirmacao do cancelamento e entao
inicie a nova receita. Isso reinicia a contagem pela primeira etapa. Uma
receita concluida pode ser substituida sem cancelar antes.

O editor recupera a ultima receita iniciada no ESP32, inclusive apos cancelar
ou concluir. A receita/progresso sao persistidos no inicio, nas transicoes e
a cada 15 minutos, conforme [RECEITAS.md](RECEITAS.md). Edicoes que ainda nao
foram iniciadas existem apenas naquela pagina; exporte o JSON se quiser
guarda-las antes de iniciar um lote.

## Preparacao na instalacao

Compile e grave este firmware uma vez para adicionar a interface. Depois,
as receitas sao configuradas pelo navegador sem recompilar nem reiniciar.

Configure em `menuconfig`:

- **Beer Monitor - Wi-Fi and iSpindel**: rede local, senha e endereco AP;
  rede externa para o envio de dados. O AP aceita um iSpindel e ate tres
  dispositivos de configuracao simultaneos.
- **Beer Monitor - Local recipe interface**: senha propria opcional de
  acesso ao painel (8 a 63 bytes UTF-8). Campo vazio usa a senha do AP.
- **Beer Monitor - Ubidots STEM**: token, device label e intervalo de envio.

As credenciais da rede externa e do Ubidots continuam sendo configuradas na
instalacao; esta pagina edita as receitas. O login local nao salva a senha
em armazenamento do navegador e nao a envia ao Ubidots. A senha e mantida
apenas em memoria enquanto a pagina esta aberta. **Sair** limpa esse acesso.

O texto sobre envio ao Ubidots indica configuracao e conexao ao Wi-Fi externo;
nao e uma confirmacao de armazenamento de cada leitura na nuvem. Consulte
os timestamps do painel e os logs de MQTT para verificar a recepcao.

## Monitoramento remoto

O firmware publica ate dez variaveis no intervalo configurado, de 300 segundos
por padrao. As novas variaveis sao:

| Variavel | Uso no painel |
|---|---|
| `etapa-fermentacao` | Numero da etapa atual, com zero para receita inativa |
| `dias-etapa` | Dias decorridos na etapa |
| `dias-max-etapa` | Prazo maximo de dias da etapa |

O contexto de `etapa-fermentacao` contem `receita`, `etapa`, `total_etapas`,
`estado`, `motivo`, `densidade_disponivel`, `armazenamento_ok`, `erro` e o
`densidade_limite` quando configurado. Os estados da receita sao: 0 inativa,
1 em execucao, 2 concluida e 3 falha. Os motivos sao: 0 nenhum, 1 inicio,
2 densidade, 3 prazo e 4 cancelamento.

No painel Ubidots da conta configurada, use `temperatura-local` e
`temperatura-alvo` em um grafico, `densidade` em outro, e as novas variaveis
para etapa e duracao. `estado-controle` continua indicando aquecimento,
refrigeracao, bloqueios e falhas. Nao foi criado um dashboard na conta nem
adicionado comando remoto: a configuracao desta entrega e local.

Sem conexao externa, o painel remoto fica com as ultimas leituras recebidas;
os timestamps indicam sua idade. A receita segue localmente, e a publicacao
retoma quando ha conexao. Nao ha historico offline armazenado para reenviar.
Dez variaveis a cada cinco minutos correspondem a aproximadamente 2880 pontos
por dia. Confira a franquia da conta antes de reduzir o intervalo.

## Integracao HTTP

O servidor do iSpindel continua na porta 80, com `POST /ispindel`. A interface
usa rotas separadas e nao altera esse protocolo:

| Rota | Operacao |
|---|---|
| `GET /` | Pagina embarcada, sem dependencias externas |
| `GET /api/status` | Estado recente de controle, sensores, receita e limites do editor |
| `GET /api/recipe` | Ultima receita iniciada ou `null` |
| `POST /api/recipe` | Valida e enfileira o inicio de uma receita JSON |
| `POST /api/recipe/cancel` | Enfileira cancelamento e desligamento dos comandos |

As APIs exigem `X-Local-Key`, contendo a senha UTF-8 codificada em hexadecimal.
Essa codificacao permite caracteres com acentos no header; nao e criptografia.
O transporte HTTP depende da rede local WPA2 protegida. O servidor verifica
que a origem e um cliente associado ao AP com IP atribuido por seu DHCP,
confere o `Host` contra o endereco AP, e os comandos exigem `Origin` identico
a origem local. Nao ha permissao CORS para outras paginas. A interface nao
e disponibilizada pela interface Wi-Fi externa nem pela nuvem.

Com o portal habilitado, navegacoes GET/HEAD desconhecidas de clientes
locais sao redirecionadas para o IP do AP, incluindo as verificacoes de
conectividade dos sistemas. Um GET de `/` com Host diferente tambem recebe
apenas esse redirecionamento: a pagina e a API nao sao servidas sob dominios
de terceiros. Rotas `/api/` desconhecidas e comandos POST nao recebem
redirecionamento cativo. Autenticacao e verificacao de origem das APIs
permanecem ativas.

Respostas de comando usam `202 Accepted`: o pedido foi enfileirado, nao ha
confirmacao sincrona da execucao. A pagina consulta o estado para confirmar
inicio/cancelamento e persistencia; nao repete comandos automaticamente se
uma resposta se perder. Requisicoes invalidas usam 400/401/403/409/413/415;
indisponibilidade momentanea usa 503. Corpos incompletos ou lentos expiram.

O servidor HTTP tem prioridade 2, abaixo do controlador (4) e do sensor local
(3). Os comandos nao executam controle nem persistencia dentro do handler:
sao copiados para a fila da tarefa de receitas. O tamanho do JSON permanece
limitado a 4096 bytes e a leitura de seu corpo tem um prazo total de tres
segundos. A interface nao inicia o exemplo automaticamente.

## Validacao

- `tests/test_local_web_ui.py`: handlers reais, autenticacao, origem/Host,
  comandos, JSON, dados vencidos, redirecionamentos cativos e falhas de alocacao.
- `tests/test_captive_portal.py`: parser DNS real, pacotes invalidos, limites
  de memoria, tarefas/UDP, vinculacao ao AP, configuracao DHCP e recuperacao
  de falhas, sem abrir sockets reais no computador.
- `tests/test_control_telemetry.py`: payload MQTT real, nomes com caracteres
  especiais, estado de receita e limites do buffer.
- `tests/test_fermentation_recipe.py`: tarefa, retomada, cancelamento e copia
  segura da receita para a interface.
- `tests/preview_local_ui.py`: previa em `127.0.0.1:8765`, com dados de teste
  somente em memoria e senha `local-test-password`. Nao controla uma placa.

Na placa, valide o acesso simultaneo do iSpindel e celular, inicio e
cancelamento, operacao sem navegador/internet, retomada apos reiniciar e
recepcao das novas variaveis no Ubidots. Os testes de computador nao substituem
essa verificacao de rede, cargas e equipamentos reais.
Confira tambem a oferta da pagina de entrada no celular, tanto com o roteador
externo ligado quanto desligado. Se o aparelho guardou uma concessao DHCP
anterior, desconecte e reconecte ao AP apos atualizar o firmware.
