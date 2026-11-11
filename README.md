# Safe Evidence — Relógio (LilyGo T-Watch-2020 V3)

Firmware do relógio que aciona a gravação da ESP32-CAM por Bluetooth (BLE).
Por fora é um relógio comum: nada na tela indica o Safe Evidence.

## Como usar

| Ação no botão lateral | O que faz |
| --- | --- |
| **Segurar** (~1,5 s) | inicia a gravação; segurar de novo encerra. Sem luz, som, vibração ou mudança na tela (RE01) |
| Toque curto | liga/desliga a tela (ela também apaga sozinha depois de 8 s) |
| Dois toques curtos seguidos | desliga o relógio (se estiver gravando, encerra antes) |
| Tocar na tela | acende a tela (nunca inicia gravação) |

- **Conexão com a câmera:** ao ligar, o relógio procura a câmera e conecta
  sozinho, e reconecta se a conexão cair. Um ícone pequeno de Bluetooth, cinza,
  no canto superior esquerdo indica que está conectado.
- **Comando sem conexão:** se o botão for apertado antes de conectar, o
  comando fica guardado e é enviado assim que conectar.
- **Câmera reiniciada no meio da gravação:** quando reconecta, o relógio manda
  de novo o estado atual e a gravação continua.

## Gravar o firmware

1. **Arduino IDE** com o pacote de placas **esp32 (Espressif) 2.0.17**.
2. Bibliotecas (Library Manager):
   - **TTGO TWatch Library** (LilyGo)
   - **NimBLE-Arduino 2.x** (h2zero)
3. **Placa:** `TTGO T-Watch` (ou `ESP32 Dev Module` com PSRAM habilitada).
4. **`nimconfig.h`:** o relógio precisa dos papéis **CENTRAL** e **OBSERVER**
   do BLE habilitados; a câmera precisa deles desabilitados. O arquivo fica em
   `Documentos/Arduino/libraries/NimBLE-Arduino/src/nimconfig.h`. Antes de
   compilar o **relógio**, deixe **comentadas** as linhas que desabilitam
   CENTRAL e OBSERVER (padrão da biblioteca). Detalhes no fim do `.ino`.
5. Conecte o relógio no USB, escolha a porta em **Tools → Port** e clique em
   **Upload**. Se der `Failed to connect to ESP32`, feche o Serial Monitor,
   confira a porta e baixe o **Upload Speed** para 115200.

## Horário

Na primeira vez que liga depois de gravar um firmware novo, o relógio acerta
a hora com o relógio do computador que compilou (que deve estar no fuso de
Brasília). Depois a hora continua contando sozinha. Se ficar alguns segundos
errada, ajuste `AJUSTE_GRAVACAO_S` no código (tempo entre compilar e o
relógio ligar) e grave de novo.

## Compatibilidade com a câmera

Os UUIDs do BLE (`BLE_SERVICE_UUID` e `BLE_CHARACTERISTIC_UUID`) têm que ser
**iguais** aos do firmware da ESP32-CAM. Comandos: `0x01` inicia, `0x00`
encerra.

## Testando

Abra o **Serial Monitor** (115200). Aparecem as mensagens de busca, conexão
e comandos, por exemplo `[BLE] Conectado a camera.` e
`[BLE] Comando INICIAR enviado.` São só para testes: nada disso aparece na
tela do relógio.
