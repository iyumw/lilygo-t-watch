# Safe Evidence — Relógio (LilyGo T-Watch-2020 V3)

Firmware do relógio que aciona a gravação da ESP32-CAM por Bluetooth Low
Energy (BLE). Por fora é um relógio comum: o mostrador tem só hora, data e
bateria, e nada na tela indica o Safe Evidence.

O sketch fica em `AcionamentoRelogio_NimBLE/AcionamentoRelogio_NimBLE.ino`.

## Como usar

| Ação | O que faz |
| --- | --- |
| **Segurar** o botão lateral (~1,5 s) | inicia a gravação; segurar de novo encerra. Sem luz, som, vibração ou mudança na tela (RE01) |
| Toque curto no botão | liga ou apaga a tela (ela também apaga sozinha depois de 8 s) |
| Dois toques curtos seguidos (em até 600 ms) | desliga o relógio; se estiver gravando, encerra a gravação antes |
| Tocar na tela | acende a tela; nunca inicia nem encerra gravação |

- **Conexão com a câmera:** ao ligar, o relógio procura a câmera e conecta
  sozinho, e reconecta se a conexão cair. Quando não acha a câmera, espaça as
  buscas de 2 s até 20 s para poupar bateria; apertar o botão faz buscar na
  hora. Um ícone pequeno de Bluetooth, cinza, no canto superior esquerdo
  indica que está conectado.
- **Comando sem conexão:** se o botão for apertado antes de conectar, o
  pedido fica guardado e é enviado assim que conectar.
- **Câmera reiniciada no meio da gravação:** a cada conexão o relógio reenvia
  o estado desejado (gravando ou não), e a gravação continua.
- **Botão no AXP202:** no T-Watch-2020 o botão lateral não é um GPIO comum.
  Ele está ligado ao chip de energia AXP202, que distingue toque curto e
  longo e avisa o ESP32 pelo pino `AXP202_INT`.
- **BLE em tarefa própria:** a busca e o envio dos comandos rodam no núcleo 0;
  a tela e o botão, no núcleo 1, sem esperar pela busca.

## Gravar o firmware

1. **Arduino IDE 2.x** com o pacote de placas **esp32 (Espressif) 2.0.17**.
2. Bibliotecas (Gerenciador de Bibliotecas):
   - **TTGO TWatch Library** (LilyGo)
   - **NimBLE-Arduino 2.x** (h2zero)
3. **Placa:** `TTGO T-Watch` (ou `ESP32 Dev Module` com PSRAM habilitada).
4. **`nimconfig.h`:** o relógio precisa dos papéis **CENTRAL** e **OBSERVER**
   do BLE habilitados; a câmera precisa deles desabilitados. O arquivo fica em
   `Documentos/Arduino/libraries/NimBLE-Arduino/src/nimconfig.h`. Antes de
   compilar o relógio, deixe **comentadas** as linhas
   `#define CONFIG_BT_NIMBLE_ROLE_CENTRAL 0` e
   `#define CONFIG_BT_NIMBLE_ROLE_OBSERVER 0` (padrão da biblioteca).
5. Conecte o relógio no USB, escolha a porta em **Ferramentas > Porta** e
   clique em **Carregar**. Se der `Failed to connect to ESP32`, feche o Serial
   Monitor, confira a porta e baixe o **Upload Speed** para 115200.

## Horário

Na primeira vez que liga depois de gravar um firmware novo, o relógio acerta
a hora com o horário do computador que compilou, que deve estar no fuso de
Brasília. Depois a hora continua contando sozinha, mesmo com o relógio
desligado pelo botão. Se ficar alguns segundos atrasada, aumente
`AJUSTE_TEMPO_DE_GRAVACAO_S` no código (tempo entre compilar e o relógio
ligar: cerca de 20 s a 921600 baud, 80 s a 115200); se ficar adiantada,
diminua. Depois grave de novo.

## Compatibilidade com a câmera

`UUID_SERVICO` e `UUID_CARACTERISTICA` têm que ser **iguais** aos do firmware
da ESP32-CAM. Comandos: `0x01` inicia e `0x00` encerra. A câmera ignora
INICIAR se já está gravando e PARAR se já parou, então reenviar é seguro.

## Bateria

O firmware não usa os modos de suspensão do ESP32: a autonomia é de horas.
Com a câmera conectada e a tela apagada, o consumo medido foi de cerca de
34 mA (cerca de 11 h com a bateria de 380 mAh). A medição está descrita no
Guia do Usuário (seção 2.3.2).

## Testando

Abra o **Serial Monitor** (115200). Aparecem as mensagens de busca, conexão
e comandos, por exemplo:

- `[BLE] Procurando a camera...`
- `[BLE] Conectado a camera.`
- `[Safe Evidence] Gravacao pedida`
- `[BLE] Comando INICIAR enviado.`
- `[BLE] Comando PARAR enviado.`

São só para testes: nada disso aparece na tela do relógio.
