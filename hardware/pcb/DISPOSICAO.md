# Disposição proposta para a PCB

![Frente, verso e perfil da montagem](disposicao_pcb.png)

[Abrir o desenho vetorial](disposicao_pcb.svg).

## Decisão de montagem

Para a primeira versão, a proposta é uma **placa base para módulos**, com a
MAR2406 voltada ao usuário e ESP32, RTC e interfaces no verso. Isso mantém os
módulos substituíveis e aproveita o firmware existente. A tela deve ter apoio
mecânico próprio; os conectores não devem sustentar sozinhos a montagem.

O contorno de **100 × 80 mm** é uma hipótese inicial de placa base. As medidas
de referência abaixo permitem estudar o espaço disponível, mas não comprovam
o encaixe da unidade real: faltam conferir offsets, conectores, caixa e alturas.
Os corpos desenhados são envelopes de planejamento, não footprints liberados.

## Módulos fotografados e medidas pesquisadas

As fotos fornecidas em 03/11/2026 permitem identificar os itens abaixo. Elas
não foram usadas para deduzir medidas exatas por perspectiva.

| Item | Identificação visível | Medida adotada no estudo | Evidência e limite |
|---|---|---|---|
| ESP32 | ESP32 DEVKIT V1; duas fileiras de 15 pinos; micro-USB; EN/BOOT | 52 × 28 mm; 2 × 15 contatos de passo 2,54 mm; fileiras a 25,4 mm como referência | A Einstronic informa 52 × 28 × 14 mm para sua versão DOIT. A biblioteca Eagle local usa 25,4 mm entre fileiras e passo 2,54 mm. Ainda não há conferência dimensional da unidade fotografada. |
| RTC | HW-084; CI marcado DS3231M; EEPROM marcada 24C32N; bateria sob a placa | 38 × 22 mm; reservar altura preliminar de 14 mm | A Handsontec documenta 38 × 22 × 14 mm para módulo DS3231 com EEPROM. É referência de uma placa da mesma família, não desenho mecânico certificado do HW-084 fotografado. |
| TFT | Shield vermelho “2.4 TFT LCD SHIELD”, com headers em lados opostos e slot SD | 72,20 × 52,70 mm, provisoriamente | Dimensão da MAR2406 documentada pela LCDWiki, usada pelo firmware. A inscrição e a foto do verso, sozinhas, não confirmam o controlador nem a revisão mecânica exata. |
| Relés | Dois BESTEP JQC3F-03VDC-C; marcação 10 A; seleção High/Low | Reservar aproximadamente 51 × 40 × 20 mm no conjunto externo | Ofertas de módulos BESTEP semelhantes variam entre 49 × 37,5 × 17 e 50,5 × 38,5 × 18,5 mm; a reserva é arredondada. Não usar essas medidas para furar a caixa sem conferir. |
| DS18B20 | Previsto pelo firmware; não identificado nessas fotos | Conector externo de três vias | A pinagem depende do sensor/cabo real, não da cor dos fios. |

Referências de medidas:
[ESP32 — Einstronic](https://einstronic.com/product/esp32-devkit-v1/),
[RTC — Handsontec, página 4 do PDF](https://www.handsontec.com/dataspecs/module/Module%20Collection.pdf#page=4),
[TFT — LCDWiki](https://www.lcdwiki.com/2.4inch_Arduino_Display),
[relés — RoboticsDNA](https://roboticsdna.in/product/bestep-3v-2-channels-relay-low-level-module-trigger-with-optocoupler/)
e [relés — Elecbee](https://www.elecbee.com/en/product-detail/3pcs-2-channel-3v-relay-module-low-level-trigger-optocoupler-isolation-for-auduino_28918).

O RTC deve ser representado como uma placa alongada, girada 90° na lateral do
verso. A bateria fica voltada para a PCB base na orientação mostrada: prever
espaçadores e acesso para removê-la. A EEPROM e os pinos 32K/SQW não precisam
de conexão para o uso atual do relógio. No header de seis vias, a foto mostra
`32K / SQW / SCL / SDA / VCC / GND`; o header de quatro vias na outra ponta
expõe `SCL / SDA / VCC / GND`. Confira a orientação ao desenhar o footprint.

O nome DS3232M nos comentários/logs do firmware diverge da marcação **DS3231M**
fotografada. O código acessa o endereço I²C `0x68`, calendário `0x00…0x06`,
controle `0x0E` e status `0x0F`. Esses endereços e os bits usados também estão
documentados no [datasheet do DS3231M](https://www.analog.com/media/en/technical-documentation/data-sheets/DS3231M.pdf).
Isso sugere compatibilidade para as operações atuais; não equivale a teste
físico nem afirma intercambiabilidade de todos os recursos. O firmware não
foi alterado nesta revisão da disposição.

Frente e verso são vistos de fora. O verso foi espelhado horizontalmente em
relação à frente: a borda esquerda da vista traseira corresponde à borda direita
da vista frontal. O topo continua sendo o mesmo. Linhas pontilhadas indicam a
projeção da tela na face oposta; não são trilhas elétricas.

## Posição dos conjuntos

| Conjunto | Posição sugerida | Motivo |
|---|---|---|
| TFT MAR2406 | Frente, abaixo da faixa reservada ao rádio | Permite janela no painel e evita sobrepor a tela à antena |
| ESP32 DevKit V1, 30 pinos | Verso, com antena junto à borda superior e preferencialmente além do contorno | Reduz obstrução pelo display e facilita substituir o módulo |
| Conexão da TFT | Verso, perto do ESP32 e alinhada aos conectores reais da tela | Mantém curto o barramento paralelo; pode exigir adaptador ou chicote curto |
| RTC HW-084 / DS3231M | Lateral do verso, girado 90°, com folga para a bateria sob o módulo | Aproveita a placa alongada; requer retirada do módulo para manutenção da bateria |
| DS18B20 | Conector na borda, com pull-up de 4,7 kΩ próximo | Facilita levar o cabo para a câmara e identificar 3V3, GND e DQ |
| Alimentação | Borda inferior/lateral, com proteção e desacoplamento próximos | Evita atravessar a placa com o cabo de entrada |
| Interface dos comandos | Próxima ao conector para a etapa externa | Mantém o caminho das saídas curto e afasta a comutação do sensor |
| USB do DevKit | Corredor livre para encaixe do plugue e acesso pela caixa | Permite gravar e depurar sem desmontar a tela |
| Fixação | Próxima aos cantos, ajustada à caixa | Sustenta a PCB; apoio da tela deve respeitar seus pontos reais de montagem |

O desenho não representa posições físicas de pinos do DevKit ou do shield.
Use os nomes dos sinais do firmware e confira a ordem de cada conector antes
de criar o esquemático. Não suponha que os headers da MAR2406 formem uma única
barra com passo uniforme; confirme os offsets do shield Arduino na placa real.
Para este shield, a primeira montagem pode usar adaptador/chicote curto para
os grupos de dados e de controle/alimentação. O bloco J_LCD no desenho
representa essa interface lógica; não é uma cópia de seu header físico.
Os headers em ângulo vistos no RTC também podem exigir chicote ou adaptação
para montar o módulo paralelo à base; o desenho não comprova encaixe direto.
Para sustentar a TFT, use apoio/moldura compatível com a placa real: não há
furos de montagem da tela confirmados nas fotos.

## Antena e tela em faces opostas

A face metálica da tela, sua placa, a bateria do RTC, cabos e a caixa também
influenciam o rádio. Por isso, colocar a antena diretamente atrás da tela é uma
má escolha inicial. Reserve uma faixa no topo e mantenha a região da antena
livre nas duas faces e no conjunto mecânico.

A Espressif recomenda preferir a antena do módulo fora da placa base. Quando
isso não for possível, seu guia pede ao menos **15 mm ao redor da área da
antena**, sem cobre, trilhas ou componentes. O afastamento exato deve seguir o
módulo escolhido; essa recomendação não é uma distância de isolamento de rede
elétrica. O desenho marca uma reserva conceitual, cujo tamanho final depende
do DevKit real e de sua posição.

Fonte: [Espressif — posicionamento de módulos e antena](https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32/pcb-layout-design.html#general-principles-of-pcb-layout-for-modules-positioning-a-module-on-a-base-board).

## Alimentação, sinais e comandos

- Use fonte externa de baixa tensão; dimensione 5 V e 3,3 V pela corrente real
  do conjunto, inclusive tela e eventuais módulos. Não pressuponha que o
  regulador do DevKit possa alimentar tudo.
- Defina um caminho de alimentação que impeça retorno entre a fonte externa
  e o USB de programação. A solução depende do esquema do DevKit escolhido;
  não una duas fontes de 5 V sem verificar esse caminho.
- A PCB proposta contém somente alimentação de baixa tensão e comandos.
  Relés de potência, contatores, fusíveis e bornes de 127/220 V ficam em
  conjunto separado. O conector de comandos não transporta a corrente do motor.
- Não ligue bobinas diretamente aos GPIOs. Se houver bobina na interface,
  dimensione driver e supressão; se for módulo pronto, confirme alimentação,
  corrente e compatibilidade de entrada com 3,3 V.
- Os relés fotografados estão marcados `03VDC`, correspondendo à bobina de
  3 V, não a 5 V. Essa inscrição não determina sozinha o esquema de alimentação
  do módulo completo. Confira VCC, corrente e seleção High/Low antes de
  conectá-lo; a foto não demonstra a posição funcional dos seletores.
- Preserve GPIO32/33 e a polaridade ativa em LOW do firmware. Projete a
  interface para permanecer desligada durante reset, programação e ausência
  do ESP32. A validação inclui alimentação do módulo de comando com o ESP32
  desligado; não basta confiar na inicialização por software.
- Mantenha retorno de alimentação da interface de comandos longe do caminho
  do sensor. Use referência de GND contínua para os sinais de baixa tensão,
  respeitando a reserva de antena; evite cortes sob o barramento da tela.
- Mantenha o barramento paralelo da TFT curto, com retorno de GND próximo,
  sem roteá-lo na área de antena. Valores de resistores série, proteção e
  desacoplamento dependem do esquema e da verificação elétrica.
- O RTC usa GPIO5/15, ambos com função de configuração de partida. Verifique
  os pull-ups existentes no módulo, sua tensão e o comportamento no reset.
  Sinais I²C do ESP32 devem permanecer em 3,3 V.
- Preveja pontos de teste para GND, 5 V, 3,3 V, DQ e os dois comandos,
  além de acesso a BOOT/EN. Se o módulo RTC tiver circuito de carga, confirme
  que a bateria escolhida é compatível.

A MAR2406 documentada usa ILI9341 e interface paralela de 8 bits. Revise o
esquema da revisão física antes de definir sua alimentação; as etiquetas 5V e
3V3 não autorizam aplicar 5 V aos GPIOs.
Fonte: [LCDWiki — MAR2406](https://www.lcdwiki.com/2.4inch_Arduino_Display).
A pinagem utilizada está em [TESTE_MAR2406.md](../../TESTE_MAR2406.md).

## Perfil mecânico

Na direção frente → fundo: janela da caixa, módulo TFT, espaço para os
conectores/isolamento mecânico, PCB base, módulos do verso e tampa traseira.
Defina espaçadores pela maior altura real sob a tela, incluindo soldas e
conectores; não adote uma altura padrão sem medir. Reserve espaço para o
plugue USB e para tirar a bateria do RTC. Partes metálicas da tela não podem
encostar em terminais ou soldas da outra montagem.

## Antes de fabricar

1. Confirmar o DevKit exato: largura entre headers, quantidade de pinos,
   orientação da antena, USB, regulador e circuito de alimentação.
2. Medir a MAR2406 real: contorno, offsets dos headers, área visível, pontos
   de apoio e maior altura na face inferior. Fazer gabarito em escala 1:1.
3. Escolher RTC, conectores, interface dos comandos, fonte e caixa; conferir
   tensões, correntes, polaridade e espaço de manutenção.
4. Criar o esquemático e a lista de conexões; revisar partida e saídas desligadas.
5. Transferir os footprints conferidos para o layout e verificar interferências
   dos dois lados, incluindo a projeção da tela e a área livre da antena.
6. Rotear a placa, aplicar as regras da fabricante e executar ERC/DRC.
7. Validar o protótipo de baixa tensão, resets e Wi-Fi com tela e caixa montadas.
   A etapa de potência exige dimensionamento e revisão próprios.

**Estado desta entrega:** proposta visual de disposição. Não há esquemático
elétrico completo, roteamento, ERC/DRC, Gerbers ou comprovação de encaixe e
funcionamento físico associados a esta figura.
