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
#define LIMIAR_ENV  (2UL << 16)               /* abaixo disso o volume e inaudivel (amp < 2 de 255): a voz e liberada */
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

static int8_t TAB[NTABELAS][256];

/* ---------- Instrumentos (podem ser alterados pelo Bluetooth) ---------- */
static Inst INSTR[NTIMBRES] = {
  /* nome         tab atq_ms dec sust solta vib brilho chorus fm indice */
  {"Quadrada",     0,   2,    0, 100,  7,   0, 256,    0,   0,   0, 0, 0},
  {"Triangular",   1,   2,    0, 100,  7,   0, 256,    0,   0,   0, 0, 0},
  {"Senoidal",     2,   2,    0, 100,  7,   0, 256,    0,   0,   0, 0, 0},
  {"Serra",        3,   2,    0, 100,  7,   0, 256,    0,   0,   0, 0, 0},
  {"Piano",        4,   2,   14,   0, 10,   0,  60,   11,   0,   0, 0, 0},
  {"Orgao",        5,   8,    0, 100,  9,   0, 256,   10,   0,   0, 0, 0},
  {"Flauta",       6,  40,   12,  85, 10,   5, 256,    0,   0,   0, 0, 0},
  {"Clarinete",    7,  25,   11,  90,  9,   0, 150,    0,   0,   0, 0, 0},
  {"Cordas",       8, 120,    0, 100, 11,   8, 120,    9,   0,   0, 0, 0},
  {"Trompete",     9,  35,   12,  85,  9,   3,  40,   11,   0,   0, 0, 0},
  {"Violao",      10,   1,   13,   0, 10,   0,  30,    0,   0,   0, 0, 0},
  {"Sino",         2,   1,   15,   0, 12,   0, 256,    0,  14, 150, 0, 0},
  {"Ocarina",     12,  40,   12,  90, 10,   4, 256,    0,   0,   0, 0, 0},
  {"EPiano",       2,   1,   13,   0, 10,   0, 256,    0,   4, 200, 0, 0},
  {"Bateria",      2,   1,    0, 100, 10,   0, 256,    0,   0,   0, 0, 0},   /* sintetizada (ver BAT[]) */
};

/* ---------- Bateria (sintetizada: senoide que cai de tom + ruido) ---------- */
/* inc = frequencia * 2^32 / 20089 (~213794 por Hz) */
#define HZ(f) ((uint32_t)((f) * 213794UL))
typedef struct {
  uint32_t inc_ini, inc_fim;   /* tom no comeco e no fim (o tom "cai" rapido, como num tambor) */
  uint8_t  queda_tom;          /* velocidade da queda do tom (maior = mais lenta) */
  uint8_t  decai;              /* duracao do som (maior = mais longo) */
  uint8_t  tom, ruido;         /* quanto de senoide e quanto de ruido (0..255) */
  uint8_t  agudo;              /* 1 = ruido so com os agudos (chimbal, pratos) */
  uint8_t  pan;                /* posicao no estereo */
} Bat;

enum { B_NADA, B_BUMBO, B_CAIXA, B_ARO, B_CHIMBAL_F, B_CHIMBAL_A, B_PRATO, B_CONDUCAO, B_TOM, B_CONGA, B_CHOCALHO };
static const Bat BAT[] = {
  /*                ini        fim       queda dec  tom ruido agudo pan */
  [B_NADA]      = {0,         0,          0,  10,   0,   0, 0, 128},
  [B_BUMBO]     = {HZ(160),   HZ(48),     9,  12, 255,  24, 0, 128},
  [B_CAIXA]     = {HZ(230),   HZ(175),   10,  11, 110, 210, 1, 120},
  [B_ARO]       = {HZ(420),   HZ(380),    9,   9, 140, 150, 1, 120},
  [B_CHIMBAL_F] = {0,         0,          0,   8,   0, 230, 1, 170},
  [B_CHIMBAL_A] = {0,         0,          0,  11,   0, 210, 1, 170},
  [B_PRATO]     = {0,         0,          0,  13,   0, 200, 1,  80},
  [B_CONDUCAO]  = {HZ(880),   HZ(870),   12,  12,  50, 130, 1, 190},
  [B_TOM]       = {HZ(150),   HZ(95),    10,  12, 235,  40, 0, 128},   /* o tom muda conforme a nota */
  [B_CONGA]     = {HZ(300),   HZ(250),   10,  10, 190,  25, 0,  96},   /* congas e bongos */
  [B_CHOCALHO]  = {0,         0,          0,   8,   0, 105, 1, 200},   /* maracas, chocalho, pandeiro */
};

/* agrupa as ~47 pecas da bateria General MIDI nos tipos acima */
static uint8_t tipo_bateria(int n) {
  switch (n) {
    case 35: case 36:                      return B_BUMBO;
    case 38: case 40: case 39:             return B_CAIXA;
    case 37:                               return B_ARO;
    case 42: case 44:                      return B_CHIMBAL_F;
    case 46:                               return B_CHIMBAL_A;
    case 49: case 52: case 55: case 57:    return B_PRATO;
    case 51: case 53: case 59:             return B_CONDUCAO;
    case 41: case 43: case 45: case 47: case 48: case 50: return B_TOM;
    case 60: case 61: case 62: case 63: case 64: case 65: case 66: return B_CONGA;
    case 54: case 58: case 69: case 70: case 82: return B_CHOCALHO;
    case 56: case 67: case 68: case 75: case 76: case 77: return B_ARO;   /* cowbell, agogo, claves, wood block */
    default:                               return (n < 35) ? B_BUMBO : B_CHOCALHO;
  }
}

static uint32_t lfsr = 0x12345678UL;      /* gerador de ruido (xorshift de 32 bits) */

/* ---------- Vozes ---------- */
/* estagio: 0 parada, 1 ataque, 2 decaimento/sustentacao, 3 soltura, 4 silenciosa com a tecla apertada */
typedef struct {
  uint32_t fase;
  uint32_t fase2;       /* 2o oscilador (chorus) ou modulador (FM) */
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
  uint8_t  bateria;     /* 0 = instrumento normal; senao, o tipo de peca de bateria */
  uint32_t inc_fim;     /* bateria: tom final */
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
    TAB[1][i] = (int8_t)((i < 128) ? (-127 + i * 2) : (127 - (i - 128) * 2));
    TAB[2][i] = SIN256[i];
  }
  /* quadrada e serra montadas por harmonicos (limitados), para nao gerar "aliasing" nos agudos */
  static const uint8_t quadrada[] = {100, 0, 33, 0, 20, 0, 14, 0, 11, 0, 9};
  static const uint8_t serra[]    = {100, 50, 33, 25, 20, 17, 14, 12, 11, 10};
  tabela_harmonicos(0, quadrada, sizeof(quadrada));
  tabela_harmonicos(3, serra,    sizeof(serra));
  for (int i = 0; i < 256; i++) TAB[0][i] = (int8_t)((TAB[0][i] * 13) / 16);   /* quadrada: mesmo volume de antes */
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
  for (int i = 0; i < 256; i++) {                     /* tabela 11: base para o "harm sino" */
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
  if (t < 0 || t >= NTABELAS || n < 1) return;
  if (n > NHARM_MAX) n = NHARM_MAX;
  INSTR[t].tabela = (uint8_t)t;          /* passa a usar a propria tabela */
  INSTR[t].fm_razao = 0;                 /* e deixa de ser FM */
  tabela_harmonicos(t, h, n);
}

/* ---------- Calculo das amostras (roda na interrupcao do DMA) ---------- */

/* le a tabela com interpolacao linear: mistura os 2 pontos vizinhos conforme a fase */
static inline int32_t ler_tab(const int8_t *t, uint32_t fase) {
  uint32_t i = fase >> 24;
  int32_t fr = (int32_t)((fase >> 16) & 0xFF);
  int32_t a = t[i];
  int32_t b = t[(i + 1) & 255];
  return a + (((b - a) * fr) >> 8);
}


/* Cada voz calcula o BLOCO inteiro (64 amostras) de uma vez e soma em mixL/mixR.
   O estado da voz fica em variaveis locais (registradores do processador) durante o bloco,
   em vez de ser lido e gravado na memoria a cada amostra: fica bem mais rapido. */
static int32_t mixL[AUDIO_BLOCO], mixR[AUDIO_BLOCO];

/* bateria: o som so cai (soltar a tecla nao corta, como num tambor de verdade) */
static void bloco_bateria(Voz *z, int n) {
  if (z->estagio == 0) return;
  const Bat *B = &BAT[z->bateria];
  uint32_t env = z->env, fase = z->fase, inc = z->inc;
  const uint32_t inc_fim = z->inc_fim;
  int32_t ant = z->filtro;
  uint32_t r32 = lfsr;
  const int32_t gL = z->gL, gR = z->gR;
  const int32_t tom = B->tom, ruido = B->ruido;
  const int agudo = B->agudo, dec = B->decai, qt = B->queda_tom;
  int a;
  for (a = 0; a < n; a++) {
    env -= env >> dec;
    if (env < LIMIAR_ENV) { env = 0; break; }
    int32_t amp = (int32_t)(env >> 16);
    int32_t s = 0;
    if (tom) {
      if (inc > inc_fim) inc -= (inc - inc_fim) >> qt;
      fase += inc;
      s = ler_tab(SIN256, fase) * tom;
    }
    if (ruido) {
      r32 ^= r32 << 13; r32 ^= r32 >> 17; r32 ^= r32 << 5;          /* gerador de ruido */
      int32_t r = (int8_t)(r32 >> 24);
      if (agudo) { int32_t x = r; r = (r - ant) >> 1; ant = x; }    /* tira os graves */
      s += r * ruido;
    }
    s = ((s >> 8) * amp) >> 6;                    /* bateria com o dobro do ganho: sao golpes curtos */
    mixL[a] += (s * gL) >> 8;
    mixR[a] += (s * gR) >> 8;
  }
  z->env = env; z->fase = fase; z->inc = inc; z->filtro = ant; lfsr = r32;
  if (a < n) z->estagio = 0;
}

/* instrumentos: envelope, vibrato, tabela de onda (ou FM), chorus e filtro. Retorna 1 se soou. */
static int bloco_inst(Voz *z, int n) {
  uint8_t est = z->estagio;
  if (z->ligada) { if (est == 0 || est == 3) est = 1; }
  else if (est == 1 || est == 2 || est == 4) est = 3;
  if (est == 0 || est == 4) { z->estagio = est; return 0; }

  const Inst *I = &INSTR[z->inst];
  const int8_t *tab = TAB[I->tabela];
  uint32_t env = z->env, fase = z->fase, fase2 = z->fase2, lfo = z->lfoFase;
  const uint32_t inc0 = z->inc;
  int32_t filtro = z->filtro;
  const int32_t gL = z->gL, gR = z->gR;
  const uint32_t atq = I->ataque_inc, sust = I->sust_nivel;
  const int dec = I->decai, sol = I->solta;
  const int32_t vib = I->vibrato, bri = I->brilho, cho = I->chorus;
  const uint32_t fmr = I->fm_razao;
  const int32_t fmi = I->fm_indice;
  int a;

  for (a = 0; a < n; a++) {
    /* envelope (ataque, decaimento/sustentacao, soltura) */
    if (est == 1) {
      env += atq;
      if (env >= ENV_MAX) { env = ENV_MAX; est = 2; }
    } else if (est == 2) {
      if (dec && env > sust) {
        env -= (env - sust) >> dec;
        if (sust < LIMIAR_ENV && env < LIMIAR_ENV) { est = 4; break; }   /* sumiu com a tecla apertada */
      }
    } else {
      env -= env >> sol;
      if (env < LIMIAR_ENV) { env = 0; est = 0; break; }               /* acabou a soltura */
    }

    uint32_t inc = inc0;
    if (vib) {
      lfo += LFO_INC;
      int32_t m = (int32_t)SIN256[lfo >> 24] * vib;
      inc = (uint32_t)((int32_t)inc + ((((int32_t)(inc >> 12)) * m) >> 5));
    }
    fase += inc;
    int32_t amp = (int32_t)(env >> 16);
    int32_t s;

    if (fmr) {
      /* FM: uma senoide (modulador) mexe na fase de outra (portadora).
         O indice acompanha o envelope: o som comeca brilhante e vai ficando suave. */
      fase2 += (inc >> 2) * fmr;
      int32_t m = ler_tab(SIN256, fase2);
      int32_t idx = (fmi * amp) >> 8;
      s = ler_tab(SIN256, fase + ((uint32_t)(m * idx) << 16));
    } else {
      s = ler_tab(tab, fase);
      if (cho) {
        /* chorus: 2o oscilador um pouco mais agudo; 2/3 do principal + 1/3 do desafinado
           (com um ganho de ~1,2x para compensar o volume que se perde no "batimento") */
        fase2 += inc + (inc >> cho);
        s = (s * 205 + ler_tab(tab, fase2) * 103) >> 8;
      }
    }
    if (bri < 256) {                              /* filtro que acompanha o volume ("brilho") */
      int32_t k = bri + (((256 - bri) * amp) >> 8);
      filtro += (((s * 256) - filtro) * k) >> 8;
      s = filtro >> 8;
    }
    s = (s * amp) >> 7;                           /* +-254 */
    mixL[a] += (s * gL) >> 8;
    mixR[a] += (s * gR) >> 8;
  }
  z->env = env; z->fase = fase; z->fase2 = fase2; z->lfoFase = lfo; z->filtro = filtro;
  z->estagio = est;
  return a > 0;
}

/* ganho da mistura por numero de notas (~1/raiz(n)): o volume nao despenca com acordes */
static uint16_t GANHO_N[NVOZES + 1];       /* calculado no audio_init: 256 / raiz(n) */
static int32_t ganho_suave = 256 << 8;

/* limitador suave: acima de 180 a onda e "achatada" aos poucos em vez de cortada seco */
static inline int32_t limita(int32_t x) {
  if (x > 180) { x = 180 + ((x - 180) >> 2); if (x > 255) x = 255; }
  else if (x < -180) { x = -180 + ((x + 180) >> 2); if (x < -256) x = -256; }
  return x;
}

static void render(uint16_t *out, int n) {
  uint32_t t0 = DWT->CYCCNT;
  input_scan();

  for (int a = 0; a < n; a++) { mixL[a] = 0; mixR[a] = 0; }

  /* 1) cada voz soma o seu bloco inteiro */
  int nat = 0;
  for (int v = 0; v < NVOZES; v++) {
    Voz *z = &vozes[v];
    if (z->estagio == 0 && !z->ligada) continue;
    if (z->bateria) bloco_bateria(z, n);           /* bateria nao entra na conta do ganho (evita "bombear") */
    else nat += bloco_inst(z, n);
  }
  vozes_ativas = (uint8_t)nat;

  /* 2) mistura: ganho suave, volume, limitador e conversao para o PWM */
  const int32_t alvo = (int32_t)GANHO_N[nat] << 8;
  for (int a = 0; a < n; a++) {
    if ((a & 3) == 0) input_encoder_tick();
    if (alvo < ganho_suave) ganho_suave += (alvo - ganho_suave) >> 7;    /* mais notas: abaixa em ~6 ms */
    else                    ganho_suave += (alvo - ganho_suave) >> 11;   /* menos notas: sobe devagar (~100 ms) */
    int32_t g = (ganho_suave >> 8) * volume;          /* ate 256*256 */
    int32_t L = limita((mixL[a] * (g >> 4)) >> 12);
    int32_t R = limita((mixR[a] * (g >> 4)) >> 12);
    L += PWM_CENTRO; R += PWM_CENTRO;
    if (L < 0) L = 0;
    if (L > (int32_t)PWM_ARR) L = PWM_ARR;
    if (R < 0) R = 0;
    if (R > (int32_t)PWM_ARR) R = PWM_ARR;
    out[2 * a] = (uint16_t)L;
    out[2 * a + 1] = (uint16_t)R;
  }

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

/* forca da nota (velocity do MIDI em 4 niveis) -> ganho */
static const uint16_t NIVEL_FORCA[4] = {118, 166, 214, 256};

static void aplicar_forca(Voz *z, uint8_t forca) {
  uint32_t g = NIVEL_FORCA[forca & 3];
  z->gL = (uint16_t)((z->gL * g) >> 8);
  z->gR = (uint16_t)((z->gR * g) >> 8);
}

static void bateria_iniciar(Voz *z, int nota_gm, int pan, uint8_t forca) {
  uint8_t tp = tipo_bateria(nota_gm);
  const Bat *B = &BAT[tp];
  z->bateria = tp;
  z->inc = B->inc_ini;
  z->inc_fim = B->inc_fim;
  if (tp == B_CONGA) {                             /* congas/bongos: quanto maior a nota, mais agudo */
    int d = nota_gm - 60;
    if (d < 0) d = 0;
    z->inc = B->inc_ini + (B->inc_ini / 10) * (uint32_t)d;
    z->inc_fim = B->inc_fim + (B->inc_fim / 10) * (uint32_t)d;
  }
  if (tp == B_TOM) {                               /* tons: quanto maior a nota, mais agudo */
    int d = nota_gm - 41;
    if (d < 0) d = 0;
    z->inc = B->inc_ini + (B->inc_ini / 12) * (uint32_t)d;
    z->inc_fim = B->inc_fim + (B->inc_fim / 12) * (uint32_t)d;
  }
  voz_pan(z, (uint8_t)(pan < 0 ? B->pan : pan));
  aplicar_forca(z, forca);
  z->fase = 0;
  z->filtro = 0;
  z->env = ENV_MAX;                                /* bateria comeca no volume maximo */
  z->ordem = ++contador_ordem;
  z->estagio = 2;
}

/* teclas no timbre Bateria: bumbo, caixa, chimbal fechado, chimbal aberto, 3 tons, prato, conducao, palmas */
static const uint8_t BAT_TECLA[NTECLAS] = {36, 38, 42, 46, 41, 45, 48, 49, 51, 39};

void audio_tecla(int i, bool apertada) {
  Voz *z = &vozes[i];
  if (apertada && timbre == TIMBRE_BATERIA) {
    if (z->ligada) return;
    z->ligada = 1;
    bateria_iniciar(z, BAT_TECLA[i], -1, 3);
    return;
  }
  if (apertada) {
    if (z->ligada) return;
    z->bateria = 0;
    z->inc = inc_midi(tecla_nota[i] + audio_transposicao());
    z->inst = (uint8_t)timbre;
    voz_pan(z, pan_tecla(i));
    z->fase2 = z->fase + 0x40000000;
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
void audio_bateria(int v, int nota_gm, uint8_t forca) {
  Voz *z = &vozes[v];
  z->ligada = 1;
  bateria_iniciar(z, nota_gm, estereo ? -1 : 128, forca);
}

void audio_voz_nota(int v, int midi, int inst, uint8_t pan, uint8_t forca) {
  Voz *z = &vozes[v];
  if (inst == TIMBRE_BATERIA) { audio_bateria(v, midi, forca); return; }
  uint32_t inc = inc_midi(midi);
  if (inst < 0 || inst >= NTIMBRES) inst = timbre;
  /* ligadura: mesma nota, mesmo instrumento e ainda ligada -> o som continua */
  if (z->ligada && z->midi_tocada == (uint8_t)midi && z->inst == (uint8_t)inst) return;
  z->bateria = 0;
  z->inc = inc;
  z->midi_tocada = (uint8_t)midi;
  z->inst = (uint8_t)inst;
  voz_pan(z, pan);
  aplicar_forca(z, forca);
  z->fase2 = z->fase + 0x40000000;
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
  GANHO_N[0] = 256;
  for (int k = 1; k <= NVOZES; k++) GANHO_N[k] = (uint16_t)(65536UL / isqrt32((uint32_t)k << 16));
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

  /* O SysTick (relogio de 1 ms do HAL_GetTick) precisa poder interromper o calculo do audio.
     Se o calculo de um bloco passar de 1 ms com o SysTick abaixo dele, "tiques" se perdem,
     o relogio atrasa e as musicas tocam mais devagar. O SysTick e minusculo: nao atrapalha o som. */
  HAL_NVIC_SetPriority(SysTick_IRQn, 0, 0);
  HAL_NVIC_SetPriority(DMA1_Channel5_IRQn, 1, 0);
}
