/*
 * Safe Evidence - Relógio (LilyGo T-Watch-2020 V3) - RE01
 *
 * Mostrador comum de smartwatch (hora, data, bateria); nada na tela indica o
 * Safe Evidence. O relógio procura a câmera por BLE ao ligar e reconecta
 * sozinho; conectado, mostra só um ícone de Bluetooth discreto.
 *
 * Botão lateral (lido pelo chip de energia AXP202):
 *   - segurar ~1,5 s: inicia/encerra a gravação, sem luz, vibração ou som;
 *   - toque curto: liga/desliga a tela; dois toques curtos: desliga o relógio.
 * Tocar na tela só acende a tela, nunca grava.
 *
 * Sem conexão, o pedido fica guardado e é enviado ao conectar. A cada
 * conexão o relógio reenvia o estado desejado, então a câmera volta a
 * gravar sozinha se reiniciar no meio de uma gravação.
 *
 * NimBLE-Arduino 2.x: o relógio precisa dos papéis CENTRAL e OBSERVER
 * habilitados no nimconfig.h da biblioteca; a câmera precisa deles
 * desabilitados. Ajuste o arquivo antes de compilar cada um.
 */

#define LILYGO_WATCH_2020_V3  // precisa vir antes do include da biblioteca
#include <LilyGoWatch.h>
#include <NimBLEDevice.h>
#include <Preferences.h>

// Iguais aos do firmware da câmera
#define UUID_SERVICO "12345678-1234-1234-1234-1234567890ab"
#define UUID_CARACTERISTICA "abcd1234-ab12-cd34-ef56-1234567890ab"

#define COMANDO_PARAR 0x00
#define COMANDO_INICIAR 0x01

static const unsigned long JANELA_DUPLO_TOQUE_MS = 600;
static const unsigned long TEMPO_TELA_LIGADA_MS = 8000;
static const unsigned long INTERVALO_LEITURA_TOQUE_MS = 50;
static const unsigned long INTERVALO_REDESENHO_MS = 500;
static const unsigned long ESPERA_ENVIO_ANTES_DE_DESLIGAR_MS = 1500;
static const unsigned long PAUSA_DO_LACO_MS = 20;

// A espera entre buscas pela câmera dobra até o máximo, para poupar bateria.
static const uint32_t ESPERA_MINIMA_BUSCA_MS = 2000;
static const uint32_t ESPERA_MAXIMA_BUSCA_MS = 20000;
static const uint32_t DURACAO_BUSCA_MS = 3000;
static const uint32_t TIMEOUT_CONEXAO_MS = 5000;
static const uint32_t ESPERA_POR_COMANDO_MS = 1000;
static const uint32_t PILHA_TAREFA_BLE = 8192;
static const int NUCLEO_TAREFA_BLE = 0;

// Segundos entre compilar e o relógio começar a rodar (~20 s a 921600 baud,
// ~80 s a 115200). Hora atrasada: aumente; adiantada: diminua.
static const int AJUSTE_TEMPO_DE_GRAVACAO_S = 20;

#define COR_FUNDO TFT_BLACK
#define COR_HORA TFT_WHITE
#define COR_SECUNDARIA 0x8C51
#define COR_ICONE 0x6B4D

#define CENTRO_X 120
#define POSICAO_HORA_Y 105
#define POSICAO_DATA_Y 160
#define FONTE_HORA 7
#define FONTE_DATA 4
#define FONTE_BATERIA 2

TTGOClass *relogio;
TFT_eSPI *tela;

volatile bool chipDeEnergiaInterrompeu = false;
// O último pedido da usuária; a câmera é mantida nesse estado sempre que conectada.
volatile bool gravacaoDesejada = false;
volatile bool conectadoNaCamera = false;
// Fila de uma posição: vale sempre o último comando do botão.
QueueHandle_t filaDeComandos;

static unsigned long ultimoToqueCurtoMs = 0;
static bool telaLigada = true;
static unsigned long telaLigadaDesdeMs = 0;

void IRAM_ATTR aoInterromperChipDeEnergia() {
  chipDeEnergiaInterrompeu = true;
}

// ==================== BLE (tarefa própria, núcleo 0) ====================
// O loop() (tela e botão) roda no núcleo 1 e nunca espera pela busca da câmera.

static NimBLEClient *clienteBle = nullptr;
static NimBLERemoteCharacteristic *caracteristicaDaCamera = nullptr;

class CallbacksDoCliente : public NimBLEClientCallbacks {
  void onDisconnect(NimBLEClient *cliente, int motivo) override {
    conectadoNaCamera = false;
    caracteristicaDaCamera = nullptr;
    Serial.printf("[BLE] Desconectado da camera (motivo %d). Vou reconectar.\n", motivo);
  }
};

const NimBLEAdvertisedDevice *procurarCamera(NimBLEScan *busca) {
  NimBLEScanResults encontrados = busca->getResults(DURACAO_BUSCA_MS, false);
  for (int i = 0; i < encontrados.getCount(); i++) {
    const NimBLEAdvertisedDevice *dispositivo = encontrados.getDevice(i);
    if (dispositivo->isAdvertisingService(NimBLEUUID(UUID_SERVICO))) return dispositivo;
  }
  return nullptr;
}

bool conectarNaCamera() {
  NimBLEScan *busca = NimBLEDevice::getScan();
  busca->setActiveScan(true);
  const NimBLEAdvertisedDevice *camera = procurarCamera(busca);
  if (camera == nullptr) {
    busca->clearResults();
    return false;
  }

  if (clienteBle == nullptr) {
    clienteBle = NimBLEDevice::createClient();
    clienteBle->setClientCallbacks(new CallbacksDoCliente(), true);
    clienteBle->setConnectTimeout(TIMEOUT_CONEXAO_MS);
  }

  bool conectou = clienteBle->connect(camera);
  busca->clearResults();  // "camera" aponta para os resultados: só limpa depois de conectar
  if (!conectou) return false;

  NimBLERemoteService *servico = clienteBle->getService(UUID_SERVICO);
  NimBLERemoteCharacteristic *caracteristica = servico ? servico->getCharacteristic(UUID_CARACTERISTICA) : nullptr;
  if (caracteristica == nullptr) {
    clienteBle->disconnect();
    return false;
  }

  caracteristicaDaCamera = caracteristica;
  return true;
}

bool enviarComando(uint8_t comando) {
  if (!conectadoNaCamera || caracteristicaDaCamera == nullptr) return false;
  bool enviado = caracteristicaDaCamera->writeValue(&comando, 1, true);
  Serial.printf("[BLE] Comando %s %s.\n", comando == COMANDO_INICIAR ? "INICIAR" : "PARAR",
                enviado ? "enviado" : "FALHOU");
  return enviado;
}

// A câmera ignora INICIAR se já grava e PARAR se já parou, então reenviar é seguro.
void sincronizarEstadoComCamera() {
  enviarComando(gravacaoDesejada ? COMANDO_INICIAR : COMANDO_PARAR);
  xQueueReset(filaDeComandos);
}

// Espera antes da próxima busca, mas acorda na hora se o botão for apertado.
uint32_t aguardarProximaBusca(uint32_t esperaMs) {
  uint8_t comando;
  if (xQueuePeek(filaDeComandos, &comando, pdMS_TO_TICKS(esperaMs)) == pdTRUE) return ESPERA_MINIMA_BUSCA_MS;
  return min(esperaMs * 2, ESPERA_MAXIMA_BUSCA_MS);
}

bool cameraConectada() {
  return conectadoNaCamera && clienteBle != nullptr && clienteBle->isConnected();
}

void tarefaBle(void *parametro) {
  uint32_t esperaMs = ESPERA_MINIMA_BUSCA_MS;

  for (;;) {
    if (!cameraConectada()) {
      conectadoNaCamera = false;
      Serial.println("[BLE] Procurando a camera...");
      if (conectarNaCamera()) {
        conectadoNaCamera = true;
        esperaMs = ESPERA_MINIMA_BUSCA_MS;
        Serial.println("[BLE] Conectado a camera.");
        sincronizarEstadoComCamera();
      } else {
        Serial.printf("[BLE] Camera nao encontrada. Nova tentativa em %lu s.\n", (unsigned long)(esperaMs / 1000));
        esperaMs = aguardarProximaBusca(esperaMs);
      }
      continue;
    }

    uint8_t comando;
    if (xQueueReceive(filaDeComandos, &comando, pdMS_TO_TICKS(ESPERA_POR_COMANDO_MS)) == pdTRUE) {
      // Se o envio falhar, a reconexão reenvia o estado desejado: o pedido não se perde.
      if (!enviarComando(comando)) conectadoNaCamera = false;
    }
  }
}

// ==================== MOSTRADOR ====================

static const char *DIAS_DA_SEMANA[] = {"dom", "seg", "ter", "qua", "qui", "sex", "sab"};
static const char *MESES[] = {"jan", "fev", "mar", "abr", "mai", "jun", "jul", "ago", "set", "out", "nov", "dez"};

// Algoritmo de Sakamoto (0 = domingo).
int diaDaSemana(int ano, int mes, int dia) {
  static const int deslocamentoDoMes[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (mes < 3) ano -= 1;
  return (ano + ano / 4 - ano / 100 + ano / 400 + deslocamentoDoMes[mes - 1] + dia) % 7;
}

// O mesmo símbolo de "pareado com o celular" dos smartwatches: não chama atenção.
void desenharIconeBluetooth(int x, int y, uint16_t cor) {
  tela->drawLine(x, y - 7, x, y + 7, cor);
  tela->drawLine(x, y - 7, x + 4, y - 3, cor);
  tela->drawLine(x + 4, y - 3, x - 4, y + 4, cor);
  tela->drawLine(x, y + 7, x + 4, y + 3, cor);
  tela->drawLine(x + 4, y + 3, x - 4, y - 4, cor);
}

void desenharHoraEData(const RTC_Date &agora) {
  char hora[6];
  snprintf(hora, sizeof(hora), "%02d:%02d", agora.hour, agora.minute);
  tela->setTextDatum(MC_DATUM);
  tela->setTextColor(COR_HORA, COR_FUNDO);
  tela->drawString(hora, CENTRO_X, POSICAO_HORA_Y, FONTE_HORA);

  char data[24];
  int indiceDoDia = diaDaSemana(agora.year, agora.month, agora.day);
  snprintf(data, sizeof(data), "  %s, %02d %s  ", DIAS_DA_SEMANA[indiceDoDia], agora.day, MESES[(agora.month + 11) % 12]);
  tela->setTextColor(COR_SECUNDARIA, COR_FUNDO);
  tela->drawString(data, CENTRO_X, POSICAO_DATA_Y, FONTE_DATA);
}

void desenharBateria(int percentual) {
  char texto[8];
  snprintf(texto, sizeof(texto), " %d%% ", percentual);
  tela->setTextDatum(TR_DATUM);
  tela->setTextColor(COR_SECUNDARIA, COR_FUNDO);
  tela->drawString(texto, 232, 8, FONTE_BATERIA);
}

void desenharIconeDeConexao(bool conectado) {
  tela->fillRect(4, 2, 20, 20, COR_FUNDO);
  if (conectado) desenharIconeBluetooth(14, 12, COR_ICONE);
}

// Só redesenha o que mudou, para a tela não piscar.
void desenharMostrador(bool redesenharTudo) {
  static int ultimoMinuto = -1;
  static int ultimaBateria = -1;
  static bool ultimaConexao = false;

  RTC_Date agora = relogio->rtc->getDateTime();
  int bateria = relogio->power->getBattPercentage();
  bool conectado = conectadoNaCamera;

  if (redesenharTudo) {
    tela->fillScreen(COR_FUNDO);
    ultimoMinuto = -1;
    ultimaBateria = -1;
    ultimaConexao = !conectado;
  }
  if (agora.minute != ultimoMinuto) {
    ultimoMinuto = agora.minute;
    desenharHoraEData(agora);
  }
  if (bateria != ultimaBateria) {
    ultimaBateria = bateria;
    desenharBateria(bateria);
  }
  if (conectado != ultimaConexao) {
    ultimaConexao = conectado;
    desenharIconeDeConexao(conectado);
  }
}

void ligarTela() {
  relogio->displayWakeup();
  relogio->openBL();
  telaLigada = true;
  telaLigadaDesdeMs = millis();
  desenharMostrador(true);
}

void desligarTela() {
  relogio->closeBL();
  relogio->displaySleep();
  telaLigada = false;
}

// Reage só quando o dedo encosta, para um toque longo não acender e apagar várias vezes.
void tratarToqueNaTela() {
  static unsigned long ultimaLeituraMs = 0;
  static bool dedoNaTela = false;
  if (millis() - ultimaLeituraMs < INTERVALO_LEITURA_TOQUE_MS) return;
  ultimaLeituraMs = millis();

  int16_t x, y;
  bool tocandoAgora = relogio->getTouch(x, y);
  if (tocandoAgora && !dedoNaTela) {
    if (telaLigada) telaLigadaDesdeMs = millis();
    else ligarTela();
  }
  dedoNaTela = tocandoAgora;
}

// ==================== HORÁRIO ====================

static int mesPeloNome(const char *nome) {
  static const char *nomes = "JanFebMarAprMayJunJulAugSepOctNovDec";
  for (int i = 0; i < 12; i++) {
    if (strncmp(nome, nomes + i * 3, 3) == 0) return i + 1;
  }
  return 1;
}

static bool anoBissexto(int ano) {
  return (ano % 4 == 0 && ano % 100 != 0) || ano % 400 == 0;
}

static int diasNoMes(int ano, int mes) {
  static const int dias[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return (mes == 2 && anoBissexto(ano)) ? 29 : dias[mes - 1];
}

// Acerta o RTC com o horário da compilação (Brasília, UTC-3) só na primeira vez que
// um firmware novo liga; depois o RTC conta sozinho, mesmo com o relógio desligado.
// O rtc->check() da biblioteca não serve: não corrige hora errada no mesmo dia.
void acertarHorarioSeFirmwareNovo() {
  const char *idFirmware = __DATE__ " " __TIME__;

  Preferences preferencias;
  preferencias.begin("relogio", false);
  if (preferencias.getString("firmware", "") == idFirmware) {
    preferencias.end();
    return;
  }

  // __DATE__ = "Sep 23 2026", __TIME__ = "14:05:33"
  const char *data = __DATE__;
  const char *horario = __TIME__;
  int mes = mesPeloNome(data);
  int dia = atoi(data + 4);
  int ano = atoi(data + 7);
  int hora = atoi(horario);
  int minuto = atoi(horario + 3);
  int segundo = atoi(horario + 6) + AJUSTE_TEMPO_DE_GRAVACAO_S;

  minuto += segundo / 60;
  segundo %= 60;
  hora += minuto / 60;
  minuto %= 60;
  if (hora >= 24) {
    hora -= 24;
    if (++dia > diasNoMes(ano, mes)) {
      dia = 1;
      if (++mes > 12) {
        mes = 1;
        ano++;
      }
    }
  }

  relogio->rtc->setDateTime(ano, mes, dia, hora, minuto, segundo);
  preferencias.putString("firmware", idFirmware);
  preferencias.end();
  Serial.printf("[Relogio] Horario acertado: %02d/%02d/%04d %02d:%02d:%02d\n", dia, mes, ano, hora, minuto, segundo);
}

// ==================== BOTÃO ====================

// Sem retorno visual, tátil ou sonoro (RE01); o Serial é só para testes.
void alternarGravacao() {
  gravacaoDesejada = !gravacaoDesejada;
  uint8_t comando = gravacaoDesejada ? COMANDO_INICIAR : COMANDO_PARAR;
  xQueueOverwrite(filaDeComandos, &comando);
  Serial.println(gravacaoDesejada ? "[Safe Evidence] Gravacao pedida" : "[Safe Evidence] Encerramento pedido");
}

void desligarRelogio() {
  Serial.println("[Safe Evidence] Clique duplo - desligando...");
  Serial.flush();
  if (gravacaoDesejada) {
    alternarGravacao();
    delay(ESPERA_ENVIO_ANTES_DE_DESLIGAR_MS);
  }
  relogio->power->shutdown();
}

void tratarToqueCurto(unsigned long agoraMs) {
  if (agoraMs - ultimoToqueCurtoMs <= JANELA_DUPLO_TOQUE_MS) {
    desligarRelogio();
    return;
  }
  ultimoToqueCurtoMs = agoraMs;
  if (telaLigada) desligarTela();
  else ligarTela();
}

// O pino INT fica baixo enquanto houver qualquer interrupção pendente no AXP202
// (botão, carregador, USB); conferir o nível evita que uma delas trave o botão.
void tratarBotao() {
  if (!chipDeEnergiaInterrompeu && digitalRead(AXP202_INT) == HIGH) return;
  chipDeEnergiaInterrompeu = false;

  relogio->power->readIRQ();
  if (relogio->power->isPEKLongPressIRQ()) {
    alternarGravacao();
  } else if (relogio->power->isPEKShortPressIRQ()) {
    tratarToqueCurto(millis());
  }
  relogio->power->clearIRQ();
}

// ==================== SETUP / LOOP ====================

void configurarChipDeEnergia() {
  relogio->power->adc1Enable(AXP202_BATT_VOL_ADC1 | AXP202_BATT_CUR_ADC1 | AXP202_VBUS_VOL_ADC1 | AXP202_VBUS_CUR_ADC1,
                             true);
  relogio->power->enableIRQ(AXP202_PEK_SHORTPRESS_IRQ | AXP202_PEK_LONGPRESS_IRQ, true);
  relogio->power->clearIRQ();
  pinMode(AXP202_INT, INPUT);
  attachInterrupt(AXP202_INT, aoInterromperChipDeEnergia, FALLING);
}

void setup() {
  Serial.begin(115200);
  relogio = TTGOClass::getWatch();
  relogio->begin();
  tela = relogio->tft;

  acertarHorarioSeFirmwareNovo();
  configurarChipDeEnergia();
  ligarTela();

  // Nome vazio: o relógio não anuncia nada, só procura a câmera.
  NimBLEDevice::init("");
  filaDeComandos = xQueueCreate(1, sizeof(uint8_t));
  xTaskCreatePinnedToCore(tarefaBle, "ble", PILHA_TAREFA_BLE, nullptr, 1, nullptr, NUCLEO_TAREFA_BLE);

  Serial.println("[Safe Evidence] Pronto.");
}

void loop() {
  tratarBotao();
  tratarToqueNaTela();

  if (telaLigada) {
    static unsigned long ultimoDesenhoMs = 0;
    if (millis() - telaLigadaDesdeMs > TEMPO_TELA_LIGADA_MS) {
      desligarTela();
    } else if (millis() - ultimoDesenhoMs > INTERVALO_REDESENHO_MS) {
      ultimoDesenhoMs = millis();
      desenharMostrador(false);
    }
  }

  delay(PAUSA_DO_LACO_MS);
}
