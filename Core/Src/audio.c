/*
 * Motor de audio: sintese por tabela de onda com envelope, vibrato, filtro e panorama estereo.
 * So usa contas com inteiros (o STM32F103 nao tem FPU).
 *
 * Caminho da amostra:  calculo (aqui) -> buffer na RAM -> DMA1 canal 5 -> TIM1->CCR1/CCR2 -> PA8/PA9
 */
#pragma GCC optimize ("O2")   /* o calculo do audio precisa de otimizacao mesmo no build Debug */

#include "audio.h"
#include "input.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

/* ---------- Constantes ---------- */
#define ENV_MAX     16777215UL               /* envelope de 24 bits */
#define LIMIAR_ENV  16384UL
#define LFO_INC     1175867UL                /* vibrato de ~5,5 Hz */
#define INC_MAX     1282763565UL             /* 6 kHz */
#define PWM_CENTRO  256
#define BUF_AMOSTRAS (AUDIO_BLOCO * 2)       /* buffer inteiro = 2 metades */

/* ---------- Tabelas ---------- */
static const int8_t SIN256[256] = {
  0, 3, 6, 9, 12, 16, 19, 22, 25, 28, 31, 34, 37, 40, 43, 46,
  49, 51, 54, 57, 60, 63, 65, 68, 71, 73, 76, 78, 81, 83, 85, 88,
  90, 92, 94, 96, 98, 100, 102, 104, 106, 107, 109, 111, 112, 113, 115, 116,
  117, 118, 120, 121, 122, 122, 123, 124, 125, 125, 126, 126, 126, 127, 127, 127,
  127, 127, 127, 127, 126, 126, 126, 125, 125, 124, 123, 122, 122, 121, 120, 118,
  117, 116, 115, 113, 112, 111, 109, 107, 106, 104, 102, 100, 98, 96, 94, 92,
  90, 88, 85, 83, 81, 78, 76, 73, 71, 68, 65, 63, 60, 57, 54, 51,
  49, 46, 43, 40, 37, 34, 31, 28, 25, 22, 19, 16, 12, 9, 6, 3,
  0, -3, -6, -9, -12, -16, -19, -22, -25, -28, -31, -34, -37, -40, -43, -46,
  -49, -51, -54, -57, -60, -63, -65, -68, -71, -73, -76, -78, -81, -83, -85, -88,
  -90, -92, -94, -96, -98, -100, -102, -104, -106, -107, -109, -111, -112, -113, -115, -116,
  -117, -118, -120, -121, -122, -122, -123, -124, -125, -125, -126, -126, -126, -127, -127, -127,
  -127, -127, -127, -127, -126, -126, -126, -125, -125, -124, -123, -122, -122, -121, -120, -118,
  -117, -116, -115, -113, -112, -111, -109, -107, -106, -104, -102, -100, -98, -96, -94, -92,
  -90, -88, -85, -83, -81, -78, -76, -73, -71, -68, -65, -63, -60, -57, -54, -51,
  -49, -46, -43, -40, -37, -34, -31, -28, -25, -22, -19, -16, -12, -9, -6, -3
};

/* incremento de fase de Do0..Si0 (notas MIDI 12..23) a 20089 Hz; cada oitava acima = 1 bit a esquerda */
static const uint32_t INC0[12] = {
  3495872, 3703748, 3923984, 4157316, 4404523, 4666430,
  4943910, 5237890, 5549351, 5879333, 6228936, 6599328
};

static int8_t TAB[NTIMBRES][256];

/* ---------- Instrumentos (podem ser alterados pelo Bluetooth) ---------- */
static Inst INSTR[NTIMBRES] = {
  /* nome         tab atq_ms dec sust solta vib brilho */
  {"Quadrada",     0,   2,    0, 100,  7,   0, 256, 0, 0},
  {"Triangular",   1,   2,    0, 100,  7,   0, 256, 0, 0},
  {"Senoidal",     2,   2,    0, 100,  7,   0, 256, 0, 0},
  {"Serra",        3,   2,    0, 100,  7,   0, 256, 0, 0},
  {"Piano",        4,   2,   14,   0, 10,   0,  60, 0, 0},
  {"Orgao",        5,   8,    0, 100,  9,   0, 256, 0, 0},
  {"Flauta",       6,  40,   12,  85, 10,   5, 256, 0, 0},
  {"Clarinete",    7,  25,   11,  90,  9,   0, 150, 0, 0},
  {"Cordas",       8, 120,    0, 100, 11,   8, 120, 0, 0},
  {"Trompete",     9,  35,   12,  85,  9,   3,  40, 0, 0},
  {"Violao",      10,   1,   13,   0, 10,   0,  30, 0, 0},
  {"Sino",        11,   1,   15,   0, 12,   0, 256, 0, 0},
  {"Ocarina",     12,  40,   12,  90, 10,   4, 256, 0, 0},
};

/* ---------- Vozes ---------- */
/* estagio: 0 parada, 1 ataque, 2 decaimento/sustentacao, 3 soltura, 4 silenciosa com a tecla apertada */
typedef struct {
  uint32_t fase;
  uint32_t env;
  uint32_t lfoFase;
  int32_t  filtro;
  volatile uint32_t inc;
  volatile uint8_t  estagio;
  volatile uint8_t  inst;
  volatile uint8_t  ligada;
  uint8_t  canal;       /* MIDI: canal e nota original (para achar a voz no note off) */
  uint8_t  nota;
  uint8_t  midi_tocada; /* nota real (com transposicao), para ligaduras */
  uint16_t gL, gR;      /* ganho de cada lado (0..256) */
  uint32_t ordem;
} Voz;

static Voz vozes[NVOZES];
static int8_t tecla_nota[NTECLAS] = {60, 62, 64, 65, 67, 69, 71, 72, 74, 76};  /* Do4..Si4, Do5, Re5, Mi5 */

static volatile int volume = 128;
static int volume_pct = 50;
static int timbre = 0;
static int oitava = 4;
static bool estereo = true;
static uint32_t contador_ordem = 0;

static TIM_HandleTypeDef *tim;
static DMA_HandleTypeDef *dma;
static uint16_t buf[BUF_AMOSTRAS * 2];   /* intercalado: L, R, L, R ... */

static uint32_t ciclos_media = 0, ciclos_pico = 0;
static volatile uint8_t vozes_ativas = 0;

/* ---------- Geracao das tabelas de onda ---------- */
static uint32_t isqrt32(uint32_t x) {
  uint32_t r = 0, bit = 1UL << 30;
  while (bit > x) bit >>= 2;
  while (bit) {
    if (x >= r + bit) { x -= r + bit; r = (r >> 1) + bit; }
    else r >>= 1;
    bit >>= 2;
  }
  return r;
}

/* soma de harmonicos: h[k] (0..100) e a amplitude do harmonico k+1 */
static void tabela_harmonicos(int idx, const uint8_t *h, int n) {
  int32_t pico = 1;
  uint64_t soma2 = 0;
  for (int i = 0; i < 256; i++) {
    int32_t x = 0;
    for (int k = 0; k < n; k++) x += (int32_t)h[k] * SIN256[((k + 1) * i) & 255];
    int32_t a = (x < 0) ? -x : x;
    if (a > pico) pico = a;
    soma2 += (uint64_t)((int64_t)x * x);
  }
  uint32_t rms = isqrt32((uint32_t)(soma2 / 256));
  if (rms < 1) rms = 1;
  int64_t esc = ((int64_t)127 << 16) / pico;
  int64_t escRms = ((int64_t)70 << 16) / rms;
  if (escRms > esc) esc = (escRms < esc * 3 / 2) ? escRms : esc * 3 / 2;

  for (int i = 0; i < 256; i++) {
    int32_t x = 0;
    for (int k = 0; k < n; k++) x += (int32_t)h[k] * SIN256[((k + 1) * i) & 255];
    int64_t v = ((int64_t)x * esc) >> 16;
    if (v > 127) v = 127;
    if (v < -127) v = -127;
    TAB[idx][i] = (int8_t)v;
  }
}

static void gerar_tabelas(void) {
  for (int i = 0; i < 256; i++) {
    TAB[0][i] = (i < 128) ? 90 : -90;
    TAB[1][i] = (int8_t)((i < 128) ? (-127 + i * 2) : (127 - (i - 128) * 2));
    TAB[2][i] = SIN256[i];
    TAB[3][i] = (int8_t)(i - 128);
  }
  static const uint8_t piano[]     = {100, 65, 45, 30, 22, 14, 10, 6, 4};
  static const uint8_t orgao[]     = {100, 85, 55, 65, 20, 30, 10, 25};
  static const uint8_t flauta[]    = {100, 25, 8, 3};
  static const uint8_t clarinete[] = {100, 3, 50, 2, 35, 2, 20, 1, 12, 1, 6};
  static const uint8_t cordas[]    = {100, 55, 40, 30, 25, 20, 16, 13, 10, 8, 6, 5};
  static const uint8_t trompete[]  = {100, 90, 80, 70, 55, 45, 35, 28, 20, 15, 10, 8};
  static const uint8_t violao[]    = {100, 70, 45, 30, 25, 15, 10, 6, 4};
  static const uint8_t ocarina[]   = {100, 8, 2};
  tabela_harmonicos(4,  piano,     sizeof(piano));
  tabela_harmonicos(5,  orgao,     sizeof(orgao));
  tabela_harmonicos(6,  flauta,    sizeof(flauta));
  tabela_harmonicos(7,  clarinete, sizeof(clarinete));
  tabela_harmonicos(8,  cordas,    sizeof(cordas));
  tabela_harmonicos(9,  trompete,  sizeof(trompete));
  tabela_harmonicos(10, violao,    sizeof(violao));
  tabela_harmonicos(12, ocarina,   sizeof(ocarina));
  for (int i = 0; i < 256; i++) {                     /* sino: modulacao de frequencia */
    int idx = (i + (122 * SIN256[(3 * i) & 255]) / 127) & 255;
    TAB[11][i] = SIN256[idx];
  }
}

void audio_inst_atualizar(int t) {
  Inst *I = &INSTR[t];
  uint32_t amostras = ((uint32_t)I->ataque_ms * AUDIO_FS) / 1000UL;
  if (amostras < 1) amostras = 1;
  I->ataque_inc = ENV_MAX / amostras;
  if (I->ataque_inc < 1) I->ataque_inc = 1;
  I->sust_nivel = (ENV_MAX / 100UL) * I->sustenta;
  if (I->solta < 1) I->solta = 1;
}

void audio_inst_harmonicos(int t, const uint8_t *h, int n) {
  if (t < 0 || t >= NTIMBRES || n < 1) return;
  if (n > NHARM_MAX) n = NHARM_MAX;
  INSTR[t].tabela = (uint8_t)t;          /* passa a usar a propria tabela */
  tabela_harmonicos(t, h, n);
}

/* ---------- Calculo das amostras (roda na interrupcao do DMA) ---------- */
static inline int32_t voz_amostra(Voz *z) {
  if (z->ligada) {
    if (z->estagio == 0 || z->estagio == 3) z->estagio = 1;
  } else if (z->estagio == 1 || z->estagio == 2 || z->estagio == 4) {
    z->estagio = 3;
  }
  const Inst *I = &INSTR[z->inst];

  switch (z->estagio) {
    case 1:
      z->env += I->ataque_inc;
      if (z->env >= ENV_MAX) { z->env = ENV_MAX; z->estagio = 2; }
      break;
    case 2:
      if (I->decai && z->env > I->sust_nivel) {
        z->env -= (z->env - I->sust_nivel) >> I->decai;
        if (I->sust_nivel < LIMIAR_ENV && z->env < LIMIAR_ENV) z->estagio = 4;
      }
      break;
    case 3:
      z->env -= z->env >> I->solta;
      if (z->env < LIMIAR_ENV) { z->env = 0; z->estagio = 0; }
      break;
    default:
      break;
  }
  if (z->estagio == 0 || z->estagio == 4) return 0x7FFFFFFF;   /* nao soma */

  uint32_t inc = z->inc;
  if (I->vibrato) {
    z->lfoFase += LFO_INC;
    int32_t m = (int32_t)SIN256[z->lfoFase >> 24] * (int32_t)I->vibrato;
    inc = (uint32_t)((int32_t)inc + ((((int32_t)(inc >> 12)) * m) >> 5));
  }
  z->fase += inc;
  int32_t s = TAB[I->tabela][z->fase >> 24];
  int32_t amp = (int32_t)(z->env >> 16);

  if (I->brilho < 256) {
    int32_t k = (int32_t)I->brilho + (((256 - (int32_t)I->brilho) * amp) >> 8);
    z->filtro += (((s * 256) - z->filtro) * k) >> 8;
    s = z->filtro >> 8;
  }
  return (s * amp) >> 7;       /* +-254 */
}

static void render(uint16_t *out, int n) {
  uint32_t t0 = DWT->CYCCNT;
  input_scan();

  int ativas_max = 0;
  for (int a = 0; a < n; a++) {
    if ((a & 3) == 0) input_encoder_tick();

    int32_t L = 0, R = 0;
    int nat = 0;
    for (int v = 0; v < NVOZES; v++) {
      Voz *z = &vozes[v];
      if (z->estagio == 0 && !z->ligada) continue;
      int32_t s = voz_amostra(z);
      if (s == 0x7FFFFFFF) continue;
      L += (s * z->gL) >> 8;
      R += (s * z->gR) >> 8;
      nat++;
    }
    if (nat > ativas_max) ativas_max = nat;
    if (nat > 1) { L = L * 2 / (nat + 1); R = R * 2 / (nat + 1); }
    L = (L * volume) >> 8;
    R = (R * volume) >> 8;
    L += PWM_CENTRO; R += PWM_CENTRO;
    if (L < 0) L = 0;
    if (L > (int32_t)PWM_ARR) L = PWM_ARR;
    if (R < 0) R = 0;
    if (R > (int32_t)PWM_ARR) R = PWM_ARR;
    out[2 * a] = (uint16_t)L;
    out[2 * a + 1] = (uint16_t)R;
  }
  vozes_ativas = (uint8_t)ativas_max;

  uint32_t c = DWT->CYCCNT - t0;
  ciclos_media = ciclos_media - (ciclos_media >> 4) + (c >> 4);
  if (c > ciclos_pico) ciclos_pico = c;
}

static void dma_meia(DMA_HandleTypeDef *h)  { (void)h; render(&buf[0], AUDIO_BLOCO); }
static void dma_cheia(DMA_HandleTypeDef *h) { (void)h; render(&buf[AUDIO_BLOCO * 2], AUDIO_BLOCO); }

/* ---------- Ganho estereo ---------- */
static void voz_pan(Voz *z, uint8_t pan) {      /* pan: 0 = esquerda, 128 = centro, 255 = direita */
  if (!estereo) { z->gL = 256; z->gR = 256; return; }
  uint32_t p = pan;
  uint32_t gl = 2 * (256 - p), gr = 2 * (p + 1);
  z->gL = (uint16_t)(gl > 256 ? 256 : gl);
  z->gR = (uint16_t)(gr > 256 ? 256 : gr);
}

/* ---------- Frequencia ---------- */
static uint32_t inc_midi(int midi) {
  if (midi < 12) midi = 12;
  if (midi > 127) midi = 127;
  int oct = midi / 12 - 1;
  uint32_t inc = INC0[midi % 12] << oct;
  return inc > INC_MAX ? INC_MAX : inc;
}

uint32_t audio_freq_hz_midi(int midi) {
  return (uint32_t)(((uint64_t)inc_midi(midi) * AUDIO_FS) >> 32);
}

/* ---------- Teclas ---------- */
static uint8_t pan_tecla(int i) { return (uint8_t)(48 + i * 160 / (NTECLAS - 1)); }   /* espalha da esquerda para a direita */

void audio_tecla(int i, bool apertada) {
  Voz *z = &vozes[i];
  if (apertada) {
    if (z->ligada) return;
    z->inc = inc_midi(tecla_nota[i] + audio_transposicao());
    z->inst = (uint8_t)timbre;
    voz_pan(z, pan_tecla(i));
    z->lfoFase = 0;
    z->ligada = 1;
    z->estagio = 1;
  } else {
    z->ligada = 0;
  }
}

void audio_set_tecla_nota(int i, int midi) {
  if (i < 0 || i >= NTECLAS) return;
  if (midi < 12) midi = 12;
  if (midi > 115) midi = 115;
  tecla_nota[i] = (int8_t)midi;
}
int audio_get_tecla_nota(int i) { return tecla_nota[i]; }
int audio_tecla_nota_real(int i) { return tecla_nota[i] + audio_transposicao(); }

/* ---------- Vozes da musica ---------- */
void audio_voz_nota(int v, int midi, int inst, uint8_t pan) {
  Voz *z = &vozes[v];
  uint32_t inc = inc_midi(midi);
  if (inst < 0 || inst >= NTIMBRES) inst = timbre;
  /* ligadura: mesma nota, mesmo instrumento e ainda ligada -> o som continua */
  if (z->ligada && z->midi_tocada == (uint8_t)midi && z->inst == (uint8_t)inst) return;
  z->inc = inc;
  z->midi_tocada = (uint8_t)midi;
  z->inst = (uint8_t)inst;
  voz_pan(z, pan);
  z->lfoFase = 0;
  z->ordem = ++contador_ordem;
  z->ligada = 1;
  z->estagio = 1;          /* ataque a partir do nivel atual: sem estalo */
}

void audio_voz_mudo(int v) {
  vozes[v].ligada = 0;
}

int audio_mus_alocar(void) {
  int melhor = -1;
  uint32_t menor_env = 0xFFFFFFFF, mais_velha = 0xFFFFFFFF;
  for (int v = VOZ_MUS0; v < NVOZES; v++)            /* 1) voz parada */
    if (!vozes[v].ligada && vozes[v].estagio == 0) return v;
  for (int v = VOZ_MUS0; v < NVOZES; v++) {           /* 2) voz soltando com o menor volume */
    if (!vozes[v].ligada && vozes[v].env < menor_env) { menor_env = vozes[v].env; melhor = v; }
  }
  if (melhor >= 0) return melhor;
  for (int v = VOZ_MUS0; v < NVOZES; v++) {           /* 3) rouba a nota mais antiga */
    if (vozes[v].ordem < mais_velha) { mais_velha = vozes[v].ordem; melhor = v; }
  }
  vozes[melhor].ligada = 0;
  return melhor;
}

void audio_mus_marcar(int v, uint8_t canal, uint8_t nota) {
  vozes[v].canal = canal;
  vozes[v].nota = nota;
}

int audio_mus_procurar(uint8_t canal, uint8_t nota) {
  for (int v = VOZ_MUS0; v < NVOZES; v++)
    if (vozes[v].ligada && vozes[v].canal == canal && vozes[v].nota == nota) return v;
  return -1;
}

void audio_mus_parar_todas(void) {
  for (int v = VOZ_MUS0; v < NVOZES; v++) vozes[v].ligada = 0;
}

/* ---------- Ajustes ---------- */
void audio_set_volume_pct(int pct) {
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  volume_pct = pct;
  volume = (pct * 256 + 50) / 100;
}
int audio_get_volume_pct(void) { return volume_pct; }

void audio_set_timbre(int t) {
  if (t < 0) t = 0;
  if (t >= NTIMBRES) t = NTIMBRES - 1;
  timbre = t;
}
int audio_get_timbre(void) { return timbre; }

void audio_set_oitava(int o) {
  if (o < 2) o = 2;
  if (o > 7) o = 7;
  oitava = o;
  for (int i = 0; i < NTECLAS; i++) vozes[i].inc = inc_midi(tecla_nota[i] + audio_transposicao());
}
int audio_get_oitava(void) { return oitava; }
int audio_transposicao(void) { return (oitava - 4) * 12; }

void audio_set_estereo(bool on) { estereo = on; }
bool audio_get_estereo(void) { return estereo; }

const char *audio_nome_timbre(int t) { return (t >= 0 && t < NTIMBRES) ? INSTR[t].nome : "?"; }
Inst *audio_inst(int t) { return (t >= 0 && t < NTIMBRES) ? &INSTR[t] : 0; }

int audio_procurar_timbre(const char *arg) {
  char a[16];
  int n = 0;
  while (*arg == ' ') arg++;
  while (arg[n] && n < 15 && (uint8_t)arg[n] < 128 && arg[n] != ' ') { a[n] = (char)tolower((uint8_t)arg[n]); n++; }
  a[n] = 0;
  if (n == 0) return -1;
  if (isdigit((uint8_t)a[0])) { int t = atoi(a); return (t >= 0 && t < NTIMBRES) ? t : -1; }
  for (int i = 0; i < NTIMBRES; i++) {
    const char *nm = INSTR[i].nome;
    int k = 0;
    while (k < n && nm[k] && tolower((uint8_t)nm[k]) == a[k]) k++;
    if (k == n) return i;
  }
  return -1;
}

/* ---------- Diagnostico ---------- */
#define CICLOS_BLOCO ((72000000UL / AUDIO_FS) * AUDIO_BLOCO)
int audio_cpu_pct(void) { return (int)((ciclos_media * 100UL) / CICLOS_BLOCO); }
int audio_cpu_pico_pct(void) { int p = (int)((ciclos_pico * 100UL) / CICLOS_BLOCO); ciclos_pico = 0; return p; }
int audio_vozes_ativas(void) { return vozes_ativas; }

/* ---------- Inicializacao ---------- */
void audio_init(TIM_HandleTypeDef *htim_pwm, DMA_HandleTypeDef *hdma_update) {
  tim = htim_pwm;
  dma = hdma_update;

  gerar_tabelas();
  for (int t = 0; t < NTIMBRES; t++) audio_inst_atualizar(t);
  audio_set_volume_pct(50);
  audio_set_oitava(4);

  /* contador de ciclos (para o comando "cpu") */
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

  for (int i = 0; i < BUF_AMOSTRAS * 2; i++) buf[i] = PWM_CENTRO;

  /* DMA circular: buffer -> TIM1->DMAR (burst de 2 registradores: CCR1 e CCR2) */
  /* garante a configuracao do DMA mesmo se algo ficar diferente no CubeMX */
  HAL_DMA_DeInit(dma);
  dma->Init.Direction = DMA_MEMORY_TO_PERIPH;
  dma->Init.PeriphInc = DMA_PINC_DISABLE;
  dma->Init.MemInc = DMA_MINC_ENABLE;
  dma->Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
  dma->Init.MemDataAlignment = DMA_MDATAALIGN_HALFWORD;
  dma->Init.Mode = DMA_CIRCULAR;
  dma->Init.Priority = DMA_PRIORITY_VERY_HIGH;
  HAL_DMA_Init(dma);
  dma->XferHalfCpltCallback = dma_meia;
  dma->XferCpltCallback = dma_cheia;
  HAL_DMA_Start_IT(dma, (uint32_t)buf, (uint32_t)&tim->Instance->DMAR, BUF_AMOSTRAS * 2);

  tim->Instance->DCR = TIM_DMABASE_CCR1 | TIM_DMABURSTLENGTH_2TRANSFERS;
  __HAL_TIM_ENABLE_DMA(tim, TIM_DMA_UPDATE);
  HAL_TIM_PWM_Start(tim, TIM_CHANNEL_1);
  HAL_TIM_PWM_Start(tim, TIM_CHANNEL_2);
}
