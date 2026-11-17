/*
 * Leitor de musicas RTTTL sem travar o programa.
 * Extensoes: duracoes quebradas (12 = tercina), ponto antes ou depois da nota
 * e '~' depois da nota = ligadura (o som continua na proxima nota).
 */
#include "rtttl.h"
#include "audio.h"
#include "musica_mem.h"
#include "stm32f1xx_hal.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>

static const char MUSICA_STORMS[] =
  "Storms:d=4,o=5,b=108:"
  "4d4,4a4,4a4,8p,8e4,2b4,4f4,4c5,4c5,8p,8e4,2b4,"
  "4d4,4a4,4a4,8p,8e4,2b4,4f4,4c5,4c5,8p,8e4,2b4,"
  "8d4,8f4,2d5,8d4,8f4,2d5,4e5.,8f5,8e5,8e5,8e5,8c5,2a4,"
  "4a4,4d4,8f4,8g4,2a4.,4a4,4d4,8f4,8g4,2e4.,"
  "8d4,8f4,2d5,8d4,8f4,2d5,4e5.,8f5,8e5,8e5,8e5,8c5,2a4,"
  "4a4,4d4,8f4,8g4,2a4,4a4,1d4";

static const char *NOMES_CROM[12] = {"Do", "Do#", "Re", "Re#", "Mi", "Fa", "Fa#", "Sol", "Sol#", "La", "La#", "Si"};

static const char *texto = MUSICA_STORMS;   /* musica atual (na flash ou em musica_mem) */
static bool guardada = false;               /* musica_mem tem uma RTTTL */
static char nome[20];
static const char *cursor;
static int d_padrao = 4, o_padrao = 6, bpm = 63, bpm_forcado = 0, total_notas = 0;

static bool tocando = false, em_tom = false;
static uint32_t inicio_ms, nota_ate16, tom_ate16, versao;
static char nota_atual[8] = "--";
static char historico[24] = "";

static void historico_add(const char *s) {
  size_t n = strlen(s);
  while (strlen(historico) + n + 1 > 20) {
    char *sp = strchr(historico, ' ');
    if (!sp) { historico[0] = 0; break; }
    memmove(historico, sp + 1, strlen(sp + 1) + 1);
  }
  strcat(historico, s);
  strcat(historico, " ");
}

bool rtttl_iniciar(const char *src, bool copiar) {
  size_t len = strlen(src);
  const char *c1 = strchr(src, ':');
  const char *c2 = c1 ? strchr(c1 + 1, ':') : NULL;
  if (!c1 || !c2 || len >= RTTTL_MAX) return false;

  if (copiar) {
    if ((const char *)musica_mem != src) memcpy(musica_mem, src, len + 1);
    texto = (const char *)musica_mem;
    guardada = true;
    c1 = texto + (c1 - src);
    c2 = texto + (c2 - src);
  } else {
    texto = src;
  }

  size_t nl = (size_t)(c1 - texto);
  if (nl >= sizeof(nome)) nl = sizeof(nome) - 1;
  memcpy(nome, texto, nl);
  nome[nl] = 0;
  while (nl > 0 && nome[nl - 1] == ' ') nome[--nl] = 0;
  if (nl == 0) strcpy(nome, "Musica");

  d_padrao = 4; o_padrao = 6; bpm = 63;
  const char *q = c1 + 1;
  while (q < c2) {
    while (q < c2 && (*q == ',' || *q == ' ')) q++;
    if (q >= c2) break;
    char chave = (char)tolower((uint8_t)*q++);
    if (q < c2 && *q == '=') {
      q++;
      int v = 0;
      while (q < c2 && isdigit((uint8_t)*q)) { v = v * 10 + (*q - '0'); q++; }
      if (v > 0) {
        if (chave == 'd') d_padrao = v;
        else if (chave == 'o') o_padrao = v;
        else if (chave == 'b') bpm = v;
      }
    }
    while (q < c2 && *q != ',') q++;
  }

  cursor = c2 + 1;
  total_notas = 0;
  if (*cursor) { total_notas = 1; for (const char *k = cursor; *k; k++) if (*k == ',') total_notas++; }

  strcpy(nota_atual, "--");
  historico[0] = 0;
  em_tom = false;
  nota_ate16 = 0;
  tom_ate16 = 0;
  inicio_ms = HAL_GetTick();
  versao++;
  tocando = true;
  return true;
}

void rtttl_parar(void) {
  if (!tocando) return;
  tocando = false;
  em_tom = false;
  audio_voz_mudo(VOZ_MUS0);
  versao++;
}

static void proxima_nota(uint32_t agora16) {
  const char *p = cursor;
  while (*p == ',' || *p == ' ') p++;
  if (*p == 0) { cursor = p; rtttl_parar(); return; }

  int dur = 0;
  while (isdigit((uint8_t)*p)) { dur = dur * 10 + (*p - '0'); p++; }
  if (dur == 0) dur = d_padrao;
  int pontuada = 0;
  if (*p == '.') { pontuada = 1; p++; }
  if (*p == 0) { cursor = p; rtttl_parar(); return; }

  char letra = (char)tolower((uint8_t)*p++);
  int semitom;
  switch (letra) {
    case 'c': semitom = 0; break;
    case 'd': semitom = 2; break;
    case 'e': semitom = 4; break;
    case 'f': semitom = 5; break;
    case 'g': semitom = 7; break;
    case 'a': semitom = 9; break;
    case 'b': case 'h': semitom = 11; break;
    default: semitom = -1; break;
  }
  int pausa = (semitom < 0);
  if (*p == '#') { if (!pausa) semitom++; p++; }
  if (*p == '.') { pontuada = 1; p++; }
  int oit = o_padrao;
  if (isdigit((uint8_t)*p)) { oit = *p - '0'; p++; }
  if (*p == '.') { pontuada = 1; p++; }
  int ligada = 0;
  if (*p == '~') { ligada = 1; p++; }
  cursor = p;

  int bpm_usado = (bpm_forcado > 0) ? bpm_forcado : bpm;
  uint32_t total16 = (240000UL * 16UL / (uint32_t)bpm_usado) / (uint32_t)dur;
  if (pontuada) total16 = total16 * 3 / 2;

  /* agenda a partir do horario PLANEJADO: atrasos nao se acumulam */
  uint32_t base16 = nota_ate16;
  if (agora16 > base16 + 150UL * 16UL) inicio_ms += (agora16 - base16) / 16;

  if (pausa) {
    audio_voz_mudo(VOZ_MUS0);
    em_tom = false;
    strcpy(nota_atual, "--");
  } else {
    int midi = 12 * (oit + 1) + semitom + audio_transposicao();
    audio_voz_nota(VOZ_MUS0, midi, audio_get_timbre(), 128);
    em_tom = true;
    tom_ate16 = ligada ? (base16 + total16 + 16UL * 600000UL) : (base16 + total16 * 9 / 10);
    int o = midi / 12 - 1;
    if (o < 0) o = 0;
    if (o > 9) o = 9;
    snprintf(nota_atual, sizeof(nota_atual), "%s%d", NOMES_CROM[midi % 12], o);
    historico_add(nota_atual);
  }
  nota_ate16 = base16 + total16;
  versao++;
}

void rtttl_atualizar(void) {
  if (!tocando) return;
  uint32_t agora16 = (HAL_GetTick() - inicio_ms) * 16UL;
  if (em_tom && agora16 >= tom_ate16) { audio_voz_mudo(VOZ_MUS0); em_tom = false; }
  if (agora16 >= nota_ate16) proxima_nota(agora16);
}

bool rtttl_tocando(void) { return tocando; }
const char *rtttl_nome(void) { return nome; }
const char *rtttl_nota_atual(void) { return nota_atual; }
const char *rtttl_historico(void) { return historico; }
uint32_t rtttl_versao(void) { return versao; }
int rtttl_bpm(void) { return bpm; }
int rtttl_total_notas(void) { return total_notas; }
void rtttl_set_bpm_forcado(int b) { bpm_forcado = (b < 0) ? 0 : b; }
int rtttl_get_bpm_forcado(void) { return bpm_forcado; }
const char *rtttl_storms(void) { return MUSICA_STORMS; }
bool rtttl_tem_guardada(void) { return guardada; }
const char *rtttl_guardada(void) { return guardada ? (const char *)musica_mem : MUSICA_STORMS; }

/* chamado pelo MIDI quando ele passa a usar a memoria compartilhada */
void rtttl_memoria_perdida(void) {
  if (texto == (const char *)musica_mem) { rtttl_parar(); texto = MUSICA_STORMS; }
  guardada = false;
}
