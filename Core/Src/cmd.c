/*
 * Comandos pelo Bluetooth (HC-05 na USART2, 9600 baud).
 *  - Recepcao: DMA1 canal 6 em modo circular. O loop so le ate onde o DMA ja escreveu.
 *  - Envio: DMA1 canal 7. O texto vai para um anel e e enviado em segundo plano.
 */
#include "cmd.h"
#include "audio.h"
#include "rtttl.h"
#include "midi.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#define RX_TAM    512
#define TX_TAM    512
#define LINHA_MAX RTTTL_MAX

static UART_HandleTypeDef *uart;
static uint8_t rx_buf[RX_TAM];
static uint16_t rx_lido;

static uint8_t tx_buf[TX_TAM];
static volatile uint16_t tx_ini, tx_fim, tx_enviando;

static char linha[LINHA_MAX];
static uint16_t linha_len;
static uint8_t linha_estourou;

static char meme[100];
static uint32_t meme_ate, meme_ver;

static const char *NOMES_CROM[12] = {"Do", "Do#", "Re", "Re#", "Mi", "Fa", "Fa#", "Sol", "Sol#", "La", "La#", "Si"};

/* ---------- Envio (DMA) ---------- */
static void tx_iniciar(void) {
  if (tx_enviando || tx_ini == tx_fim) return;
  uint16_t n = (tx_fim > tx_ini) ? (uint16_t)(tx_fim - tx_ini) : (uint16_t)(TX_TAM - tx_ini);
  if (HAL_UART_Transmit_DMA(uart, &tx_buf[tx_ini], n) == HAL_OK) tx_enviando = n;
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart) {
  if (huart != uart) return;
  tx_ini = (uint16_t)((tx_ini + tx_enviando) % TX_TAM);
  tx_enviando = 0;
}

void cmd_print(const char *s) {
  while (*s) {
    uint16_t prox = (uint16_t)((tx_fim + 1) % TX_TAM);
    uint32_t t0 = HAL_GetTick();
    while (prox == tx_ini) {                 /* anel cheio: espera esvaziar */
      tx_iniciar();
      if (HAL_GetTick() - t0 > 1500) return;
    }
    tx_buf[tx_fim] = (uint8_t)*s++;
    tx_fim = prox;
  }
  tx_iniciar();
}

void cmd_println(const char *s) { cmd_print(s); cmd_print("\r\n"); }

static void printf_ln(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#include <stdarg.h>
static void printf_ln(const char *fmt, ...) {
  char b[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof(b), fmt, ap);
  va_end(ap);
  cmd_println(b);
}

/* ---------- Recepcao (DMA circular) ---------- */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart) {
  if (huart != uart) return;
  /* erro de recepcao (ex.: overrun): o HAL para o DMA; recomeca do zero */
  HAL_UART_Receive_DMA(uart, rx_buf, RX_TAM);
  rx_lido = 0;
  tx_enviando = 0;
}

void cmd_init(UART_HandleTypeDef *huart) {
  uart = huart;
  rx_lido = 0;
  HAL_UART_Receive_DMA(uart, rx_buf, RX_TAM);
}

/* ---------- Notas ---------- */
static void nome_nota(int midi, char *out, size_t tam) {
  int o = midi / 12 - 1;
  snprintf(out, tam, "%s%d", NOMES_CROM[((midi % 12) + 12) % 12], o);
}

/* aceita C5, c#4, Bb3, do5, re#4, sol3, la, si4 ... (sem oitava = 4) */
static int ler_nota(const char *s) {
  static const struct { const char *pt; int semi; } PT[] = {
    {"sol", 7}, {"do", 0}, {"re", 2}, {"mi", 4}, {"fa", 5}, {"la", 9}, {"si", 11}
  };
  while (*s == ' ') s++;
  char a[8];
  int n = 0;
  while (s[n] && n < 7 && s[n] != ' ') { a[n] = (char)tolower((uint8_t)s[n]); n++; }
  a[n] = 0;
  if (n == 0) return -1;
  int semi = -1, pos = 0;
  for (unsigned k = 0; k < sizeof(PT) / sizeof(PT[0]); k++) {
    size_t l = strlen(PT[k].pt);
    if (!strncmp(a, PT[k].pt, l)) { semi = PT[k].semi; pos = (int)l; break; }
  }
  if (semi < 0) {
    static const int8_t LET[7] = {9, 11, 0, 2, 4, 5, 7};   /* a b c d e f g */
    if (a[0] >= 'a' && a[0] <= 'g') { semi = LET[a[0] - 'a']; pos = 1; }
    else if (a[0] == 'h') { semi = 11; pos = 1; }
    else return -1;
  }
  if (a[pos] == '#') { semi++; pos++; }
  else if (a[pos] == 'b' && isdigit((uint8_t)a[pos + 1])) { semi--; pos++; }
  int oit = 4;
  if (isdigit((uint8_t)a[pos])) oit = a[pos] - '0';
  return 12 * (oit + 1) + semi;
}

/* ---------- Ajuda ---------- */
static void listar_timbres(void) {
  char b[220];
  int n = snprintf(b, sizeof(b), "Timbres:");
  for (int i = 0; i < NTIMBRES; i++) n += snprintf(b + n, sizeof(b) - (size_t)n, " %d=%s", i, audio_nome_timbre(i));
  cmd_println(b);
}

static void listar_teclas(void) {
  char b[160], nm[12];
  int n = snprintf(b, sizeof(b), "Teclas:");
  for (int i = 0; i < NTECLAS; i++) {
    nome_nota(audio_get_tecla_nota(i), nm, sizeof(nm));
    n += snprintf(b + n, sizeof(b) - (size_t)n, " %d=%s", i + 1, nm);
  }
  cmd_println(b);
}

static void mostrar_inst(int t) {
  Inst *I = audio_inst(t);
  printf_ln("%s: onda=%d ataque=%dms decai=%d sustenta=%d%% solta=%d vibrato=%d brilho=%d",
            I->nome, I->tabela, I->ataque_ms, I->decai, I->sustenta, I->solta, I->vibrato, I->brilho);
}

static void ajuda(void) {
  cmd_println("--- Mini teclado STM32 ---");
  cmd_println("Musica RTTTL: cole em UMA linha (nome:d=4,o=5,b=120:notas)");
  cmd_println("storms | tocar (repete a ultima) | stop");
  cmd_println("bpm 140 | bpm auto | volume 0-100 | oitava 2-7 | estereo on/off");
  cmd_println("timbre piano (ou numero) | tecla 8 C5 | tecla (lista)");
  cmd_println("inst piano | inst piano ataque 30 (ataque decai sustenta solta vibrato brilho onda)");
  cmd_println("harm piano 100,50,30,10 (ate 12 harmonicos 0-100)");
  cmd_println("tela 5 seu texto aqui | cpu | reiniciar");
  cmd_println("MIDI: use o enviar_midi.py no computador");
  listar_timbres();
}

/* ---------- Comandos ---------- */
static int comeca(const char *s, const char *p) {
  size_t n = strlen(p);
  return !strncmp(s, p, n) && (s[n] == 0 || s[n] == ' ');
}

static const char *arg(const char *s, const char *cmd) {
  s += strlen(cmd);
  while (*s == ' ') s++;
  return s;
}

static void info_musica_rtttl(void) {
  if (rtttl_get_bpm_forcado() > 0)
    printf_ln("Tocando: %s | b=%d (forcado %d) | %d notas", rtttl_nome(), rtttl_bpm(), rtttl_get_bpm_forcado(), rtttl_total_notas());
  else
    printf_ln("Tocando: %s | b=%d | %d notas", rtttl_nome(), rtttl_bpm(), rtttl_total_notas());
}

static void cmd_inst(const char *a) {
  char nome[16];
  int n = 0;
  while (a[n] && a[n] != ' ' && n < 15) { nome[n] = a[n]; n++; }
  nome[n] = 0;
  int t = audio_procurar_timbre(nome);
  if (t < 0) { cmd_println("Instrumento nao encontrado."); listar_timbres(); return; }
  const char *p = a + n;
  while (*p == ' ') p++;
  if (!*p) { mostrar_inst(t); return; }

  char par[12];
  int k = 0;
  while (p[k] && p[k] != ' ' && k < 11) { par[k] = (char)tolower((uint8_t)p[k]); k++; }
  par[k] = 0;
  const char *vs = p + k;
  while (*vs == ' ') vs++;
  if (!isdigit((uint8_t)*vs)) { cmd_println("Falta o valor. Ex.: inst piano ataque 30"); return; }
  long v = atol(vs);
  Inst *I = audio_inst(t);

  if      (!strcmp(par, "ataque"))   I->ataque_ms = (uint16_t)(v < 1 ? 1 : v > 3000 ? 3000 : v);
  else if (!strcmp(par, "decai"))    I->decai = (uint8_t)(v > 20 ? 20 : v);
  else if (!strcmp(par, "sustenta")) I->sustenta = (uint8_t)(v > 100 ? 100 : v);
  else if (!strcmp(par, "solta"))    I->solta = (uint8_t)(v < 1 ? 1 : v > 20 ? 20 : v);
  else if (!strcmp(par, "vibrato"))  I->vibrato = (uint8_t)(v > 20 ? 20 : v);
  else if (!strcmp(par, "brilho"))   I->brilho = (uint16_t)(v < 8 ? 8 : v > 256 ? 256 : v);
  else if (!strcmp(par, "onda"))     I->tabela = (uint8_t)(v >= NTIMBRES ? NTIMBRES - 1 : v);
  else { cmd_println("Parametros: ataque decai sustenta solta vibrato brilho onda"); return; }
  audio_inst_atualizar(t);
  mostrar_inst(t);
}

static void cmd_harm(const char *a) {
  char nome[16];
  int n = 0;
  while (a[n] && a[n] != ' ' && n < 15) { nome[n] = a[n]; n++; }
  nome[n] = 0;
  int t = audio_procurar_timbre(nome);
  if (t < 0) { cmd_println("Instrumento nao encontrado."); return; }
  const char *p = a + n;
  uint8_t h[NHARM_MAX];
  int nh = 0;
  while (*p && nh < NHARM_MAX) {
    while (*p == ' ' || *p == ',') p++;
    if (!isdigit((uint8_t)*p)) break;
    long v = strtol(p, (char **)&p, 10);
    h[nh++] = (uint8_t)(v > 100 ? 100 : v);
  }
  if (nh == 0) { cmd_println("Ex.: harm flauta 100,30,10"); return; }
  audio_inst_harmonicos(t, h, nh);
  printf_ln("%s: nova forma de onda com %d harmonicos", audio_nome_timbre(t), nh);
}

void cmd_processar(char *s) {
  while (*s == ' ' || *s == '\t') s++;
  size_t n = strlen(s);
  while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
  if (n == 0) return;

  /* MIDI (vem do script do computador) */
  if (!strncmp(s, "MIDI:", 5) || !strncmp(s, "MIDI+", 5)) {
    bool novo = (s[4] == ':');
    if (novo) { rtttl_parar(); cmd_println("Recebendo MIDI..."); }
    midi_linha(s);
    if (!midi_carregando() && midi_tem_guardada()) {
      printf_ln("MIDI %s: %u eventos, %lu s%s", midi_nome(), (unsigned)midi_eventos(),
                (unsigned long)(midi_duracao_ms() / 1000), midi_estourou() ? " (cortada: memoria cheia)" : "");
      midi_tocar();
    }
    return;
  }

  char cmd[16];
  size_t k = 0;
  for (; s[k] && k < sizeof(cmd) - 1; k++) cmd[k] = (char)tolower((uint8_t)s[k]);
  cmd[k] = 0;
  bool e_musica = (strchr(s, ':') != NULL);

  if (e_musica) {
    midi_parar();
    if (rtttl_iniciar(s, true)) info_musica_rtttl();
    else cmd_println("Musica invalida ou longa demais (formato nome:d=4,o=5,b=120:notas).");
  } else if (comeca(cmd, "help") || comeca(cmd, "ajuda") || comeca(cmd, "?")) {
    ajuda();
  } else if (comeca(cmd, "stop") || comeca(cmd, "parar")) {
    rtttl_parar(); midi_parar();
    cmd_println("Parado.");
  } else if (comeca(cmd, "storms")) {
    midi_parar();
    rtttl_iniciar(rtttl_storms(), false);
    info_musica_rtttl();
  } else if (comeca(cmd, "tocar")) {
    if (midi_tem_guardada()) { rtttl_parar(); midi_tocar(); printf_ln("Tocando MIDI: %s", midi_nome()); }
    else { rtttl_iniciar(rtttl_guardada(), false); info_musica_rtttl(); }
  } else if (comeca(cmd, "bpm")) {
    const char *a = arg(cmd, "bpm");
    if (!strcmp(a, "auto") || !strcmp(a, "0")) { rtttl_set_bpm_forcado(0); cmd_println("BPM: o da musica"); }
    else if (isdigit((uint8_t)*a)) {
      int v = atoi(a);
      v = v < 20 ? 20 : v > 400 ? 400 : v;
      rtttl_set_bpm_forcado(v);
      printf_ln("BPM forcado: %d (vale para RTTTL)", v);
    } else printf_ln("BPM da musica: %d | forcado: %d", rtttl_bpm(), rtttl_get_bpm_forcado());
  } else if (comeca(cmd, "volume")) {
    const char *a = arg(s, "volume");
    if (isdigit((uint8_t)*a)) audio_set_volume_pct(atoi(a));
    printf_ln("Volume: %d%%", audio_get_volume_pct());
  } else if (comeca(cmd, "oitava")) {
    const char *a = arg(s, "oitava");
    if (isdigit((uint8_t)*a)) audio_set_oitava(atoi(a));
    printf_ln("Oitava: %d", audio_get_oitava());
  } else if (comeca(cmd, "estereo")) {
    const char *a = arg(cmd, "estereo");
    if (!strncmp(a, "on", 2) || *a == '1') audio_set_estereo(true);
    else if (!strncmp(a, "off", 3) || *a == '0') audio_set_estereo(false);
    printf_ln("Estereo: %s", audio_get_estereo() ? "ligado" : "desligado (mono nos dois lados)");
  } else if (comeca(cmd, "timbre")) {
    int t = audio_procurar_timbre(arg(s, "timbre"));
    if (t >= 0) { audio_set_timbre(t); printf_ln("Timbre: %s", audio_nome_timbre(t)); }
    else { cmd_println("Timbre nao encontrado."); listar_timbres(); }
  } else if (comeca(cmd, "tecla")) {
    const char *a = arg(s, "tecla");
    if (!*a) { listar_teclas(); return; }
    char *e;
    long i = strtol(a, &e, 10);
    int midi = ler_nota(e);
    if (i < 1 || i > NTECLAS || midi < 0) { cmd_println("Ex.: tecla 8 C5  (ou: tecla 8 do5)"); return; }
    audio_set_tecla_nota((int)i - 1, midi);
    char nm[12];
    nome_nota(audio_get_tecla_nota((int)i - 1), nm, sizeof(nm));
    printf_ln("Tecla %ld = %s", i, nm);
  } else if (comeca(cmd, "inst")) {
    cmd_inst(arg(s, "inst"));
  } else if (comeca(cmd, "harm")) {
    cmd_harm(arg(s, "harm"));
  } else if (comeca(cmd, "tela")) {
    const char *a = arg(s, "tela");
    char *e;
    long seg = strtol(a, &e, 10);
    if (e == a) seg = 5;
    if (seg < 1) seg = 1;
    if (seg > 600) seg = 600;
    while (*e == ' ') e++;
    strncpy(meme, e, sizeof(meme) - 1);
    meme[sizeof(meme) - 1] = 0;
    meme_ate = HAL_GetTick() + (uint32_t)seg * 1000UL;
    meme_ver++;
    printf_ln("Tela por %ld s", seg);
  } else if (comeca(cmd, "cpu")) {
    printf_ln("CPU audio: media %d%% | pico %d%% | vozes tocando: %d", audio_cpu_pct(), audio_cpu_pico_pct(), audio_vozes_ativas());
  } else if (comeca(cmd, "reiniciar")) {
    cmd_println("Reiniciando...");
    HAL_Delay(200);
    NVIC_SystemReset();
  } else {
    cmd_println("Comando nao reconhecido (digite help).");
  }
}

void cmd_atualizar(void) {
  tx_iniciar();
  uint16_t pos = (uint16_t)(RX_TAM - __HAL_DMA_GET_COUNTER(uart->hdmarx));
  if (pos >= RX_TAM) pos = 0;
  while (rx_lido != pos) {
    char c = (char)rx_buf[rx_lido];
    rx_lido = (uint16_t)((rx_lido + 1) % RX_TAM);
    if (c == '\n' || c == '\r') {
      if (linha_estourou) {
        cmd_println("Linha longa demais: ignorada.");
        linha_estourou = 0;
        linha_len = 0;
      } else if (linha_len > 0) {
        linha[linha_len] = 0;
        linha_len = 0;
        cmd_processar(linha);
      }
    } else if (linha_len < LINHA_MAX - 1) {
      linha[linha_len++] = c;
    } else {
      linha_estourou = 1;
    }
  }
}

/* ---------- Configuracao do HC-05 pelo modo AT ---------- */
static void at_enviar(const char *cmd, char *saida, size_t tam) {
  HAL_UART_Transmit(uart, (uint8_t *)cmd, (uint16_t)strlen(cmd), 300);
  size_t n = strlen(saida);
  uint8_t c;
  while (HAL_UART_Receive(uart, &c, 1, 400) == HAL_OK) {      /* le ate a resposta parar */
    if (c == '\r') continue;
    if (n + 1 < tam) { saida[n++] = (c == '\n') ? ' ' : (char)c; saida[n] = 0; }
  }
}

bool cmd_hc05_115200(char *saida, size_t tam) {
  static const uint32_t BAUDS_AT[2] = {38400, 9600};    /* modo AT completo e modo AT "mini" */
  uint32_t baud_normal = uart->Init.BaudRate;
  bool ok = false;

  HAL_UART_DMAStop(uart);
  tx_enviando = 0;
  tx_ini = tx_fim;

  for (int b = 0; b < 2 && !ok; b++) {
    saida[0] = 0;
    uart->Init.BaudRate = BAUDS_AT[b];
    HAL_UART_Init(uart);
    at_enviar("AT\r\n", saida, tam);
    if (!strstr(saida, "OK")) continue;
    at_enviar("AT+UART=115200,0,0\r\n", saida, tam);
    at_enviar("AT+UART?\r\n", saida, tam);
    ok = (strstr(saida, "115200") != NULL);
  }
  if (!ok && saida[0] == 0) strncpy(saida, "sem resposta", tam - 1);

  uart->Init.BaudRate = baud_normal;                     /* volta a velocidade do CubeMX */
  HAL_UART_Init(uart);
  rx_lido = 0;
  linha_len = 0;
  HAL_UART_Receive_DMA(uart, rx_buf, RX_TAM);
  return ok;
}

const char *cmd_meme_texto(void) { return meme; }
uint32_t cmd_meme_ate(void) { return meme_ate; }
uint32_t cmd_meme_versao(void) { return meme_ver; }
