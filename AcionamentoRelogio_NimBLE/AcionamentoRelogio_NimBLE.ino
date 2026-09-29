/*
  Safe Evidence — Relógio (LilyGo T-Watch-2020 V3)
  ------------------------------------------------------------------------------
  Requisito atendido: RE01 — Botão de acionamento e interrupção.

  O que o relógio faz:
    - Mostra um mostrador comum de smartwatch: hora, data e bateria. Nada
      na tela indica o Safe Evidence.
    - Ao ligar, já procura a câmera e conecta via BLE em segundo plano. Se
      a conexão cair, reconecta sozinho. Assim o primeiro "apertar e
      segurar" já grava, sem esperar a busca.
    - Quando está conectado à câmera, aparece um ícone de Bluetooth pequeno
      e cinza no canto da tela, igual ao de qualquer smartwatch pareado com
      um celular. É a única sinalização, e ela é discreta de propósito.

  Botão lateral:
    - Apertar e SEGURAR (~1,5 s, "toque longo" do AXP202): inicia a gravação;
      repetir encerra (toggle). Nenhuma luz, vibração, som ou mudança na
      tela ao gravar (discrição exigida pelo RE01).
    - Toque CURTO: liga/desliga a tela (como um relógio normal). A tela
      também apaga sozinha após TELA_TEMPO_LIGADA_MS.

  Tela sensível ao toque:
    - Tocar na tela apagada acende a tela (igual a um smartwatch comum).
    - Tocar com a tela acesa mantém ela acesa por mais TELA_TEMPO_LIGADA_MS.
    - O toque na tela NUNCA inicia/encerra gravação (só o botão faz isso),
      pra não gravar sem querer com o relógio encostando em algo.
    - Dois toques curtos seguidos (< DUPLO_TOQUE_JANELA_MS): desliga o relógio.

  Se o botão for apertado sem conexão com a câmera, o comando fica
  guardado e é enviado assim que conectar. E sempre que (re)conecta, o
  relógio reenvia o estado atual (gravando ou não), então se a câmera
  reiniciar no meio de uma gravação, ela volta a gravar sozinha.

  Hardware:
    No T-Watch-2020 o botão lateral NÃO é um GPIO comum: ele está ligado ao
    chip de energia AXP202, que distingue toque curto e longo e avisa o
    ESP32 pelo pino AXP202_INT.

  Biblioteca BLE: NimBLE-Arduino 2.x, com os papéis CENTRAL e OBSERVER
  habilitados no nimconfig.h (o relógio é quem procura e conecta na câmera).
  Ver a nota no final do arquivo.

  Horário (Brasília):
    Sempre que um firmware NOVO é gravado, o relógio acerta o RTC com o
    horário do computador no momento da compilação (+ AJUSTE_GRAVACAO_S,
    o tempo que leva pra compilar e enviar). O computador precisa estar no
    fuso de Brasília (UTC-3, sem horário de verão desde 2019).
    Depois disso o RTC continua contando sozinho, mesmo com o relógio
    desligado pelo botão. Se a hora ficar alguns segundos atrasada, aumente
    AJUSTE_GRAVACAO_S; se ficar adiantada, diminua.
*/

// Define o modelo do relógio ANTES de incluir a biblioteca — obrigatório.
#define LILYGO_WATCH_2020_V3
#include <LilyGoWatch.h>

#include <NimBLEDevice.h>
#include <Preferences.h>

// UUIDs do serviço/característica BLE — IDÊNTICOS aos do firmware da ESP32-CAM.
#define BLE_SERVICE_UUID        "12345678-1234-1234-1234-1234567890ab"
#define BLE_CHARACTERISTIC_UUID "abcd1234-ab12-cd34-ef56-1234567890ab"

#define CMD_PARAR   0x00
#define CMD_INICIAR 0x01

// --- Ajustes ---------------------------------------------------------------
static const unsigned long DUPLO_TOQUE_JANELA_MS = 600;
static const unsigned long TELA_TEMPO_LIGADA_MS = 8000;
// Espera entre tentativas de achar a câmera: começa curta e vai
// aumentando até o máximo, pra não gastar bateria procurando sem parar.
static const uint32_t BUSCA_ESPERA_MIN_MS = 2000;
static const uint32_t BUSCA_ESPERA_MAX_MS = 20000;
static const uint32_t BUSCA_DURACAO_MS = 3000;
// Segundos entre a compilação e o relógio começar a rodar (compilar +
// enviar). Com Upload Speed 921600 fica perto de 20 s; com 115200, ~80 s.
static const int AJUSTE_GRAVACAO_S = 20;
// Intervalo entre leituras da tela de toque.
static const unsigned long TOQUE_INTERVALO_MS = 50;

// Cores (RGB565)
#define COR_FUNDO      TFT_BLACK
#define COR_HORA       TFT_WHITE
#define COR_SECUNDARIA 0x8C51   // cinza claro
#define COR_ICONE      0x6B4D   // cinza discreto

TTGOClass *ttgo;
TFT_eSPI *tft;

// --- Estado ------------------------------------------------------------------

// Setado pela interrupção do AXP202 (botão). volatile: alterado numa ISR.
volatile bool pmuInterrompeu = false;

// Gravação: o que a USUÁRIA pediu por último (o relógio garante que a
// câmera fique nesse estado sempre que estiver conectada).
volatile bool gravacaoDesejada = false;

// Conexão com a câmera (atualizada pela tarefa de BLE, lida pela tela).
volatile bool bleConectado = false;

// Fila de 1 posição: último comando pedido pelo botão, pra tarefa de BLE enviar.
QueueHandle_t filaComando;

static unsigned long momentoUltimoToqueCurto = 0;
static bool telaLigada = true;
static unsigned long telaLigadaDesde = 0;

// ISR: só marca a flag; o tratamento é no loop().
void IRAM_ATTR aoInterromperPMU() {
  pmuInterrompeu = true;
}

// ==================== BLE (roda numa tarefa própria) ====================
//
// Tudo que é BLE fica nesta tarefa, no núcleo 0. O loop() (tela e botão)
// roda no núcleo 1 e nunca espera pela busca/conexão — por isso o botão e
// o relógio continuam respondendo enquanto a câmera é procurada.

static NimBLEClient *bleClient = nullptr;
static NimBLERemoteCharacteristic *bleCaracteristica = nullptr;

class CallbacksCliente : public NimBLEClientCallbacks {
  void onDisconnect(NimBLEClient *cliente, int motivo) override {
    bleConectado = false;
    bleCaracteristica = nullptr;
    Serial.printf("[BLE] Desconectado da camera (motivo %d). Vou reconectar.\n", motivo);
  }
};

// Procura a câmera pelo UUID do serviço e conecta. true se ficou pronta.
bool bleProcurarEConectar() {
  NimBLEScan *scan = NimBLEDevice::getScan();
  scan->setActiveScan(true);
  NimBLEScanResults resultados = scan->getResults(BUSCA_DURACAO_MS, false);

  const NimBLEAdvertisedDevice *camera = nullptr;
  for (int i = 0; i < resultados.getCount(); i++) {
    const NimBLEAdvertisedDevice *d = resultados.getDevice(i);
    if (d->isAdvertisingService(NimBLEUUID(BLE_SERVICE_UUID))) {
      camera = d;
      break;
    }
  }
  if (camera == nullptr) {
    scan->clearResults();
    return false;
  }

  if (bleClient == nullptr) {
    bleClient = NimBLEDevice::createClient();
    bleClient->setClientCallbacks(new CallbacksCliente(), true);
    bleClient->setConnectTimeout(5000);
  }

  bool ok = bleClient->connect(camera);
  scan->clearResults();  // "camera" aponta pros resultados: só limpa depois de conectar
  if (!ok) return false;

  NimBLERemoteService *servico = bleClient->getService(BLE_SERVICE_UUID);
  NimBLERemoteCharacteristic *carac = servico ? servico->getCharacteristic(BLE_CHARACTERISTIC_UUID) : nullptr;
  if (carac == nullptr) {
    bleClient->disconnect();
    return false;
  }

  bleCaracteristica = carac;
  return true;
}

bool bleEnviar(uint8_t comando) {
  if (!bleConectado || bleCaracteristica == nullptr) return false;
  bool ok = bleCaracteristica->writeValue(&comando, 1, true);
  Serial.printf("[BLE] Comando %s %s.\n",
                comando == CMD_INICIAR ? "INICIAR" : "PARAR",
                ok ? "enviado" : "FALHOU");
  return ok;
}

void tarefaBLE(void *param) {
  uint32_t espera = BUSCA_ESPERA_MIN_MS;

  for (;;) {
    // --- 1. Sem conexão: procura a câmera ---
    if (!bleConectado || bleClient == nullptr || !bleClient->isConnected()) {
      bleConectado = false;
      Serial.println("[BLE] Procurando a camera...");
      if (bleProcurarEConectar()) {
        bleConectado = true;
        espera = BUSCA_ESPERA_MIN_MS;
        Serial.println("[BLE] Conectado a camera.");
        // Sincroniza: garante que a câmera está no estado que a usuária
        // pediu (a câmera ignora INICIAR se já grava e PARAR se já parou).
        bleEnviar(gravacaoDesejada ? CMD_INICIAR : CMD_PARAR);
        xQueueReset(filaComando);  // o estado atual já foi enviado
      } else {
        Serial.printf("[BLE] Camera nao encontrada. Nova tentativa em %lu s.\n",
                      (unsigned long)(espera / 1000));
        // Espera, mas acorda na hora se o botão for apertado
        uint8_t cmd;
        if (xQueuePeek(filaComando, &cmd, pdMS_TO_TICKS(espera)) == pdTRUE) {
          espera = BUSCA_ESPERA_MIN_MS;  // usuária quer gravar: procura já
        } else {
          espera = min(espera * 2, BUSCA_ESPERA_MAX_MS);
        }
      }
      continue;
    }

    // --- 2. Conectado: espera comandos do botão ---
    uint8_t cmd;
    if (xQueueReceive(filaComando, &cmd, pdMS_TO_TICKS(1000)) == pdTRUE) {
      if (!bleEnviar(cmd)) {
        // Falhou: a conexão provavelmente caiu. Na reconexão o estado
        // desejado é reenviado, então o comando não se perde.
        bleConectado = false;
      }
    }
  }
}

// ==================== TELA (mostrador) ====================

static const char *DIAS[] = {"dom", "seg", "ter", "qua", "qui", "sex", "sab"};
static const char *MESES[] = {"jan", "fev", "mar", "abr", "mai", "jun",
                              "jul", "ago", "set", "out", "nov", "dez"};

// Dia da semana (0 = domingo), algoritmo de Sakamoto.
int diaDaSemana(int a, int m, int d) {
  static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (m < 3) a -= 1;
  return (a + a / 4 - a / 100 + a / 400 + t[m - 1] + d) % 7;
}

// Ícone de Bluetooth pequeno (o mesmo símbolo que smartwatches usam para
// "pareado com o celular") — por isso não chama atenção.
void desenharIconeBT(int x, int y, uint16_t cor) {
  tft->drawLine(x, y - 7, x, y + 7, cor);
  tft->drawLine(x, y - 7, x + 4, y - 3, cor);
  tft->drawLine(x + 4, y - 3, x - 4, y + 4, cor);
  tft->drawLine(x, y + 7, x + 4, y + 3, cor);
  tft->drawLine(x + 4, y + 3, x - 4, y - 4, cor);
}

void desenharMostrador(bool completo) {
  static int ultimoMinuto = -1, ultimaBateria = -1;
  static bool ultimaConexao = false;

  RTC_Date agora = ttgo->rtc->getDateTime();
  int bateria = ttgo->power->getBattPercentage();
  bool conectado = bleConectado;

  if (completo) {
    tft->fillScreen(COR_FUNDO);
    ultimoMinuto = ultimaBateria = -1;
    ultimaConexao = !conectado;
  }

  // Hora e data (só redesenha quando o minuto muda)
  if (agora.minute != ultimoMinuto) {
    ultimoMinuto = agora.minute;
    char hora[6];
    snprintf(hora, sizeof(hora), "%02d:%02d", agora.hour, agora.minute);
    tft->setTextDatum(MC_DATUM);
    tft->setTextColor(COR_HORA, COR_FUNDO);
    tft->drawString(hora, 120, 105, 7);  // fonte 7: estilo display digital

    char data[24];
    int ds = diaDaSemana(agora.year, agora.month, agora.day);
    snprintf(data, sizeof(data), "  %s, %02d %s  ", DIAS[ds], agora.day, MESES[(agora.month + 11) % 12]);
    tft->setTextColor(COR_SECUNDARIA, COR_FUNDO);
    tft->drawString(data, 120, 160, 4);
  }

  // Bateria (canto superior direito)
  if (bateria != ultimaBateria) {
    ultimaBateria = bateria;
    char txt[8];
    snprintf(txt, sizeof(txt), " %d%% ", bateria);
    tft->setTextDatum(TR_DATUM);
    tft->setTextColor(COR_SECUNDARIA, COR_FUNDO);
    tft->drawString(txt, 232, 8, 2);
  }

  // Ícone de conexão (canto superior esquerdo)
  if (conectado != ultimaConexao) {
    ultimaConexao = conectado;
    tft->fillRect(4, 2, 20, 20, COR_FUNDO);
    if (conectado) desenharIconeBT(14, 12, COR_ICONE);
  }
}

void ligarTela() {
  ttgo->displayWakeup();
  ttgo->openBL();
  telaLigada = true;
  telaLigadaDesde = millis();
  desenharMostrador(true);
}

void desligarTela() {
  ttgo->closeBL();
  ttgo->displaySleep();
  telaLigada = false;
}

// ==================== TOQUE NA TELA ====================

// Tocar acende a tela; tocar com ela acesa renova o tempo ligada.
void tratarToque() {
  static unsigned long ultimaLeitura = 0;
  static bool tocando = false;
  if (millis() - ultimaLeitura < TOQUE_INTERVALO_MS) return;
  ultimaLeitura = millis();

  int16_t x, y;
  bool toqueAgora = ttgo->getTouch(x, y);

  // Só reage no INÍCIO do toque (dedo encostou), não enquanto o dedo fica
  // na tela, pra um toque só não acender e apagar várias vezes.
  if (toqueAgora && !tocando) {
    if (telaLigada) telaLigadaDesde = millis();
    else ligarTela();
  }
  tocando = toqueAgora;
}

// ==================== HORÁRIO ====================

static int mesDoNome(const char *m) {
  static const char *nomes = "JanFebMarAprMayJunJulAugSepOctNovDec";
  for (int i = 0; i < 12; i++) {
    if (strncmp(m, nomes + i * 3, 3) == 0) return i + 1;
  }
  return 1;
}

static bool bissexto(int a) { return (a % 4 == 0 && a % 100 != 0) || a % 400 == 0; }

static int diasNoMes(int a, int m) {
  static const int d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return (m == 2 && bissexto(a)) ? 29 : d[m - 1];
}

// Acerta o RTC com o horário da compilação, mas só na PRIMEIRA vez que
// este firmware liga (depois o RTC continua contando sozinho; se o
// acertasse a cada boot, o relógio "voltaria no tempo" ao ser religado).
//
// Não usamos ttgo->rtc->check() porque ele só corrige quando a DATA do
// RTC é anterior à da compilação: uma hora errada no mesmo dia ficava
// errada pra sempre.
void acertarHorarioSeFirmwareNovo() {
  const char *idFirmware = __DATE__ " " __TIME__;  // ex.: "Sep 23 2026 14:05:33"

  Preferences prefs;
  prefs.begin("relogio", false);
  String gravado = prefs.getString("firmware", "");
  if (gravado == idFirmware) {
    prefs.end();
    return;  // mesmo firmware de antes: o RTC já está certo
  }

  // __DATE__ = "Sep 23 2026" (dia com espaço à esquerda se < 10)
  // __TIME__ = "14:05:33"    (horário local do computador que compilou)
  const char *data = __DATE__;
  const char *hora = __TIME__;
  int mes = mesDoNome(data);
  int dia = atoi(data + 4);
  int ano = atoi(data + 7);
  int h = atoi(hora), mi = atoi(hora + 3), se = atoi(hora + 6);

  // Soma o tempo de compilar + enviar, com "vai um" até o ano.
  se += AJUSTE_GRAVACAO_S;
  mi += se / 60; se %= 60;
  h += mi / 60;  mi %= 60;
  if (h >= 24) {
    h -= 24;
    if (++dia > diasNoMes(ano, mes)) {
      dia = 1;
      if (++mes > 12) { mes = 1; ano++; }
    }
  }

  ttgo->rtc->setDateTime(ano, mes, dia, h, mi, se);
  prefs.putString("firmware", idFirmware);
  prefs.end();
  Serial.printf("[Relogio] Horario acertado: %02d/%02d/%04d %02d:%02d:%02d\n",
                dia, mes, ano, h, mi, se);
}

// ==================== BOTÃO ====================

void alternarGravacao() {
  gravacaoDesejada = !gravacaoDesejada;
  uint8_t cmd = gravacaoDesejada ? CMD_INICIAR : CMD_PARAR;
  // Sobrescreve qualquer comando ainda não enviado: vale o último pedido.
  xQueueOverwrite(filaComando, &cmd);
  // Sem nenhum retorno visual/tátil/sonoro (RE01). Só no Serial, pra testes.
  Serial.println(gravacaoDesejada ? "[Safe Evidence] Gravacao pedida" : "[Safe Evidence] Encerramento pedido");
}

void tratarToqueCurto(unsigned long agora) {
  if (agora - momentoUltimoToqueCurto <= DUPLO_TOQUE_JANELA_MS) {
    Serial.println("[Safe Evidence] Clique duplo - desligando...");
    Serial.flush();
    // Se estava gravando, pede pra câmera parar antes de desligar.
    if (gravacaoDesejada) {
      alternarGravacao();
      delay(1500);  // dá tempo da tarefa de BLE enviar
    }
    ttgo->power->shutdown();
    return;
  }
  momentoUltimoToqueCurto = agora;

  if (telaLigada) desligarTela();
  else ligarTela();
}

void tratarBotao() {
  // O pino INT fica em nível BAIXO enquanto houver QUALQUER interrupção
  // pendente no AXP202 (botão, carregador, USB...). Checar o nível, e não
  // só a flag da ISR, evita que uma interrupção esquecida "trave" o botão.
  if (!pmuInterrompeu && digitalRead(AXP202_INT) == HIGH) return;
  pmuInterrompeu = false;

  ttgo->power->readIRQ();
  if (ttgo->power->isPEKLongPressIRQ()) {
    alternarGravacao();
  } else if (ttgo->power->isPEKShortPressIRQ()) {
    tratarToqueCurto(millis());
  }
  // Limpa SEMPRE (inclusive interrupções que não são do botão).
  ttgo->power->clearIRQ();
}

// ==================== SETUP / LOOP ====================

void setup() {
  Serial.begin(115200);

  ttgo = TTGOClass::getWatch();
  ttgo->begin();
  tft = ttgo->tft;

  // Horário de Brasília: acerta o RTC na primeira vez que este firmware liga.
  acertarHorarioSeFirmwareNovo();

  // Leitura da bateria
  ttgo->power->adc1Enable(AXP202_BATT_VOL_ADC1 | AXP202_BATT_CUR_ADC1 |
                          AXP202_VBUS_VOL_ADC1 | AXP202_VBUS_CUR_ADC1, true);

  // Botão (toque curto e longo) via interrupção do AXP202
  ttgo->power->enableIRQ(AXP202_PEK_SHORTPRESS_IRQ | AXP202_PEK_LONGPRESS_IRQ, true);
  ttgo->power->clearIRQ();
  pinMode(AXP202_INT, INPUT);
  attachInterrupt(AXP202_INT, aoInterromperPMU, FALLING);

  ligarTela();

  // BLE: nome vazio (o relógio não anuncia nada, só procura a câmera)
  NimBLEDevice::init("");
  filaComando = xQueueCreate(1, sizeof(uint8_t));
  xTaskCreatePinnedToCore(tarefaBLE, "ble", 8192, nullptr, 1, nullptr, 0);

  Serial.println("[Safe Evidence] Pronto.");
}

void loop() {
  tratarBotao();
  tratarToque();

  if (telaLigada) {
    if (millis() - telaLigadaDesde > TELA_TEMPO_LIGADA_MS) {
      desligarTela();
    } else {
      static unsigned long ultimoDesenho = 0;
      if (millis() - ultimoDesenho > 500) {
        ultimoDesenho = millis();
        desenharMostrador(false);
      }
    }
  }

  delay(20);
}

/*
  ---------------------------------------------------------------------------
  NOTA: nimconfig.h (relógio x câmera)
  ---------------------------------------------------------------------------
  A NimBLE-Arduino guarda quais "papéis" BLE ficam habilitados num único
  arquivo, nimconfig.h, na pasta da biblioteca instalada:

    Documentos\Arduino\libraries\NimBLE-Arduino\src\nimconfig.h

  A ESP32-CAM pede CENTRAL e OBSERVER DESABILITADOS (economia de IRAM).
  O relógio PRECISA dos dois habilitados (é ele quem procura e conecta).
  A Arduino IDE não aceita uma cópia da biblioteca dentro da pasta do
  sketch, então:
    - antes de compilar o RELÓGIO: linhas de CENTRAL/OBSERVER desabilitados
      COMENTADAS (padrão da biblioteca);
    - antes de compilar a CÂMERA: descomente essas linhas.
  ---------------------------------------------------------------------------
*/
