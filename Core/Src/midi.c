/*
 * Tocador de MIDI convertido em texto (ver midi.h e enviar_midi.py).
 * Os eventos (4 bytes cada) sao gravados na memoria FLASH, uma pagina de 1 KB por vez,
 * enquanto a musica chega pelo Bluetooth. Depois sao lidos direto da flash para tocar.
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

uint8_t musica_mem[MUSICA_MEM_BYTES] __attribute__((aligned(4)));   /* usada pela RTTTL */

typedef struct {
  uint16_t dt;      /* ms desde o evento anterior */
  uint8_t  tipo;    /* bit 7 = liga, bit 6 = so espera; bits 4-5 = forca (0..3); bits 0-3 = canal */
  uint8_t  nota;
} Evento;

/* ---------- Area da flash reservada para a musica ---------- */
#ifndef MUS_FLASH_INI
#define MUS_FLASH_INI   0x08010000UL                /* comeco da 2a metade da flash de 128 KB */
#endif
#define PAGINA          1024UL                      /* tamanho da pagina de flash do F103C8 */
#define EVS_PAGINA      (PAGINA / 4)                /* 256 eventos por pagina */
#define EV_FLASH        ((const Evento *)(MUS_FLASH_INI + PAGINA))   /* eventos depois do cabecalho */
#define MAGICO          0x4944494DUL                /* "MIDI": marca que existe musica valida */

typedef struct {                                    /* 1a pagina: informacoes da musica */
  uint32_t magico;
  uint32_t n_ev;
  uint32_t dur;
  char     nome[16];
  int8_t   inst[16];
} Cabecalho;

#define TIPO_LIGA 0x80
#define TIPO_NOP  0x40

static const char *NOMES_CROM[12] = {"Do", "Do#", "Re", "Re#", "Mi", "Fa", "Fa#", "Sol", "Sol#", "La", "La#", "Si"};
static const uint8_t PAN_CANAL[16] = {128, 72, 184, 100, 156, 56, 200, 128, 88, 128, 168, 64, 192, 112, 144, 128};

static char nome[16] = "";
static int8_t inst_canal[16];
static uint16_t n_ev = 0;
static uint32_t dur_total = 0;
static uint32_t pend_ms = 0;        /* espera acumulada antes do proximo evento */
static bool carregando = false, guardada = false, estourou = false, erro_flash = false;

static uint32_t pagina_buf[EVS_PAGINA];   /* eventos esperando para ir para a flash (1 KB) */
static uint16_t buf_n = 0, pagina_atual = 0;

static bool tocando = false;
static uint16_t idx = 0;
static uint32_t inicio_ms = 0, prox_ms = 0;
static char nota_atual[8] = "--";
static uint32_t versao = 0;

/* ---------- Flash ---------- */
/* apaga uma pagina e grava 'nwords' palavras de 32 bits; confere lendo de volta */
static bool flash_gravar_pagina(uint32_t end, const uint32_t *dados, uint32_t nwords) {
  FLASH_EraseInitTypeDef e = {0};
  uint32_t erro_pag = 0;
  e.TypeErase = FLASH_TYPEERASE_PAGES;
  e.PageAddress = end;
  e.NbPages = 1;

  HAL_FLASH_Unlock();
  bool ok = (HAL_FLASHEx_Erase(&e, &erro_pag) == HAL_OK);
  for (uint32_t i = 0; ok && i < nwords; i++) {
    ok = (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, end + 4 * i, dados[i]) == HAL_OK)
         && (*(volatile uint32_t *)(end + 4 * i) == dados[i]);
  }
  HAL_FLASH_Lock();
  return ok;
}

static void descarregar_pagina(void) {
  if (buf_n == 0) return;
  if (pagina_atual >= MIDI_PAGINAS) { estourou = true; buf_n = 0; return; }
  uint32_t end = MUS_FLASH_INI + PAGINA * (1 + pagina_atual);
  if (!flash_gravar_pagina(end, pagina_buf, buf_n)) erro_flash = true;
  pagina_atual++;
  buf_n = 0;
}

static void add_evento(uint8_t tipo, uint8_t nota) {
  if (estourou || erro_flash) return;
  while (pend_ms > 0xFFFF) {                          /* espera longa: divide em eventos vazios */
    Evento ev = {0xFFFF, TIPO_NOP, 0};
    if (n_ev >= MIDI_MAX_EVENTOS) { estourou = true; return; }
    memcpy(&pagina_buf[buf_n++], &ev, 4);
    n_ev++;
    pend_ms -= 0xFFFF;
    if (buf_n == EVS_PAGINA) descarregar_pagina();
  }
  if (n_ev >= MIDI_MAX_EVENTOS) { estourou = true; return; }
  Evento ev = {(uint16_t)pend_ms, tipo, nota};
  memcpy(&pagina_buf[buf_n++], &ev, 4);
  n_ev++;
  pend_ms = 0;
  if (buf_n == EVS_PAGINA) descarregar_pagina();       /* pagina cheia: grava na flash */
}

static void terminar_carga(void) {
  carregando = false;
  descarregar_pagina();
  if (n_ev == 0 || erro_flash) { guardada = false; return; }

  Cabecalho c;
  memset(&c, 0, sizeof(c));
  c.magico = MAGICO;
  c.n_ev = n_ev;
  c.dur = dur_total;
  memcpy(c.nome, nome, sizeof(c.nome));
  memcpy(c.inst, inst_canal, sizeof(c.inst));
  if (!flash_gravar_pagina(MUS_FLASH_INI, (const uint32_t *)&c, sizeof(c) / 4)) { erro_flash = true; guardada = false; return; }
  guardada = true;
}

void midi_init(void) {
  const Cabecalho *c = (const Cabecalho *)MUS_FLASH_INI;
  if (c->magico == MAGICO && c->n_ev > 0 && c->n_ev <= MIDI_MAX_EVENTOS) {
    n_ev = (uint16_t)c->n_ev;
    dur_total = c->dur;
    memcpy(nome, c->nome, sizeof(nome));
    nome[sizeof(nome) - 1] = 0;
    memcpy(inst_canal, c->inst, sizeof(inst_canal));
    guardada = true;
  }
}

void midi_apagar(void) {
  midi_parar();
  carregando = false;
  flash_gravar_pagina(MUS_FLASH_INI, NULL, 0);        /* apagar o cabecalho ja invalida a musica */
  guardada = false;
  n_ev = 0;
  nome[0] = 0;
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
      long f = 3;                                     /* forca opcional: +c.n.f (0..3) */
      if (*e == '.') { f = strtol(e + 1, &e, 10); if (f < 0) f = 0; if (f > 3) f = 3; }
      p = e;
      if (c >= 0 && c < 16 && n >= 0 && n < 128 && !estourou)
        add_evento((uint8_t)((liga ? TIPO_LIGA : 0) | ((uint8_t)f << 4) | (uint8_t)c), (uint8_t)n);
    } else if (!strncmp(p, "FIM", 3) || !strncmp(p, "fim", 3)) {
      terminar_carga();
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
    buf_n = 0; pagina_atual = 0;
    estourou = false; erro_flash = false; carregando = true;
    guardada = false;
    if (!flash_gravar_pagina(MUS_FLASH_INI, NULL, 0)) erro_flash = true;   /* apaga a musica antiga */
    if (c3) tokens(c3 + 1);
  } else if (!strncmp(linha, "MIDI+", 5)) {
    if (carregando) tokens(linha + 5);
  }
}

bool midi_carregando(void) { return carregando; }
bool midi_estourou(void) { return estourou; }
bool midi_erro_flash(void) { return erro_flash; }

bool midi_tocar(void) {
  if (!guardada || n_ev == 0) return false;
  midi_parar();
  idx = 0;
  inicio_ms = HAL_GetTick();
  prox_ms = EV_FLASH[0].dt;
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
    Evento e = EV_FLASH[idx];
    uint8_t c = e.tipo & 0x0F;
    if (e.tipo & TIPO_NOP) {
      /* so espera */
    } else if ((e.tipo & TIPO_LIGA) && c == 9) {
      int v = audio_mus_alocar();                     /* canal 10 do MIDI = bateria */
      audio_mus_marcar(v, c, e.nota);
      audio_bateria(v, e.nota, (e.tipo >> 4) & 3);
    } else if (e.tipo & TIPO_LIGA) {
      int v = audio_mus_alocar();
      int midi = e.nota + audio_transposicao();
      uint8_t pan = PAN_CANAL[c];
      audio_mus_marcar(v, c, e.nota);
      audio_voz_nota(v, midi, inst_canal[c], pan, (e.tipo >> 4) & 3);
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
    prox_ms += EV_FLASH[idx].dt;
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
