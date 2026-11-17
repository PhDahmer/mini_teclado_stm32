/*
 * Tocador de MIDI convertido em texto (ver midi.h e enviar_midi.py).
 * Os eventos ficam guardados na RAM (4 bytes cada) e podem ser repetidos.
 * Cada canal MIDI pode ter o seu proprio timbre e, no modo estereo, a sua posicao.
 */
#include "midi.h"
#include "audio.h"
#include "musica_mem.h"
#include "stm32f1xx_hal.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

uint8_t musica_mem[MUSICA_MEM_BYTES] __attribute__((aligned(4)));
void rtttl_memoria_perdida(void);

typedef struct {
  uint16_t dt;      /* ms desde o evento anterior */
  uint8_t  tipo;    /* bit 7 = liga, bit 6 = so espera; bits 0-3 = canal */
  uint8_t  nota;
} Evento;

#define EV        ((Evento *)musica_mem)
#define TIPO_LIGA 0x80
#define TIPO_NOP  0x40

static const char *NOMES_CROM[12] = {"Do", "Do#", "Re", "Re#", "Mi", "Fa", "Fa#", "Sol", "Sol#", "La", "La#", "Si"};
static const uint8_t PAN_CANAL[16] = {128, 72, 184, 100, 156, 56, 200, 128, 88, 128, 168, 64, 192, 112, 144, 128};

static char nome[16] = "";
static int8_t inst_canal[16];
static uint16_t n_ev = 0;
static uint32_t dur_total = 0;
static uint32_t pend_ms = 0;        /* espera acumulada antes do proximo evento */
static bool carregando = false, guardada = false, estourou = false;

static bool tocando = false;
static uint16_t idx = 0;
static uint32_t inicio_ms = 0, prox_ms = 0;
static char nota_atual[8] = "--";
static uint32_t versao = 0;

static void add_evento(uint8_t tipo, uint8_t nota) {
  while (pend_ms > 0xFFFF) {                          /* espera longa: divide em eventos vazios */
    if (n_ev >= MIDI_MAX_EVENTOS) { estourou = true; return; }
    EV[n_ev].dt = 0xFFFF; EV[n_ev].tipo = TIPO_NOP; EV[n_ev].nota = 0;
    n_ev++; pend_ms -= 0xFFFF;
  }
  if (n_ev >= MIDI_MAX_EVENTOS) { estourou = true; return; }
  EV[n_ev].dt = (uint16_t)pend_ms;
  EV[n_ev].tipo = tipo;
  EV[n_ev].nota = nota;
  n_ev++;
  pend_ms = 0;
}

static void tokens(const char *p) {
  while (*p) {
    while (*p == ' ' || *p == ',') p++;
    if (!*p) break;
    if (*p == 'T' || *p == 't') {
      uint32_t v = (uint32_t)strtoul(p + 1, (char **)&p, 10);
      pend_ms += v;
      dur_total += v;
    } else if (*p == '+' || *p == '-') {
      bool liga = (*p == '+');
      char *e;
      long c = strtol(p + 1, &e, 10);
      long n = (*e == '.') ? strtol(e + 1, &e, 10) : -1;
      p = e;
      if (c >= 0 && c < 16 && n >= 0 && n < 128 && !estourou)
        add_evento((uint8_t)((liga ? TIPO_LIGA : 0) | (uint8_t)c), (uint8_t)n);
    } else if (!strncmp(p, "FIM", 3) || !strncmp(p, "fim", 3)) {
      carregando = false;
      guardada = (n_ev > 0);
      return;
    } else {
      p++;                                            /* caractere estranho: ignora */
    }
  }
}

void midi_linha(const char *linha) {
  if (!strncmp(linha, "MIDI:", 5)) {
    /* cabecalho: MIDI:nome:i0,...,i15:eventos */
    midi_parar();
    rtttl_memoria_perdida();
    const char *a = linha + 5;
    const char *b = strchr(a, ':');
    if (!b) return;
    size_t nl = (size_t)(b - a);
    if (nl >= sizeof(nome)) nl = sizeof(nome) - 1;
    memcpy(nome, a, nl);
    nome[nl] = 0;
    const char *q = b + 1;
    for (int c = 0; c < 16; c++) {
      char *e;
      long v = strtol(q, &e, 10);
      inst_canal[c] = (e != q && v >= 0 && v < NTIMBRES) ? (int8_t)v : -1;
      q = e;
      if (*q == ',') q++;
    }
    const char *c3 = strchr(q, ':');
    n_ev = 0; dur_total = 0; pend_ms = 0;
    estourou = false; guardada = false; carregando = true;
    if (c3) tokens(c3 + 1);
  } else if (!strncmp(linha, "MIDI+", 5)) {
    if (carregando) tokens(linha + 5);
  }
}

bool midi_carregando(void) { return carregando; }
bool midi_estourou(void) { return estourou; }

bool midi_tocar(void) {
  if (!guardada || n_ev == 0) return false;
  midi_parar();
  idx = 0;
  inicio_ms = HAL_GetTick();
  prox_ms = EV[0].dt;
  strcpy(nota_atual, "--");
  tocando = true;
  versao++;
  return true;
}

void midi_parar(void) {
  if (!tocando) return;
  tocando = false;
  audio_mus_parar_todas();
  versao++;
}

void midi_atualizar(void) {
  if (!tocando) return;
  uint32_t agora = HAL_GetTick() - inicio_ms;
  int limite = 32;                                    /* no maximo 32 eventos por volta do loop */
  while (tocando && agora >= prox_ms && limite--) {
    Evento e = EV[idx];
    uint8_t c = e.tipo & 0x0F;
    if (e.tipo & TIPO_NOP) {
      /* so espera */
    } else if (e.tipo & TIPO_LIGA) {
      int v = audio_mus_alocar();
      int midi = e.nota + audio_transposicao();
      uint8_t pan = PAN_CANAL[c];
      audio_mus_marcar(v, c, e.nota);
      audio_voz_nota(v, midi, inst_canal[c], pan);
      int o = midi / 12 - 1;
      if (o < 0) o = 0;
      if (o > 9) o = 9;
      snprintf(nota_atual, sizeof(nota_atual), "%s%d", NOMES_CROM[((midi % 12) + 12) % 12], o);
      versao++;
    } else {
      int v = audio_mus_procurar(c, e.nota);
      if (v >= 0) audio_voz_mudo(v);
    }
    idx++;
    if (idx >= n_ev) { midi_parar(); return; }
    prox_ms += EV[idx].dt;
  }
}

bool midi_tocando(void) { return tocando; }
bool midi_tem_guardada(void) { return guardada; }
const char *midi_nome(void) { return nome; }
const char *midi_nota_atual(void) { return nota_atual; }
uint32_t midi_versao(void) { return versao; }
uint16_t midi_eventos(void) { return n_ev; }
uint32_t midi_duracao_ms(void) { return dur_total; }
uint32_t midi_posicao_ms(void) { return tocando ? (HAL_GetTick() - inicio_ms) : 0; }
