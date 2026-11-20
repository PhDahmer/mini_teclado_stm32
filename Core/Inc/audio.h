#ifndef AUDIO_H
#define AUDIO_H
/*
 * Motor de audio do mini teclado.
 * TIM1 gera o PWM estereo (CH1 = PA8 esquerdo, CH2 = PA9 direito), 9 bits a 140,6 kHz.
 * O contador de repeticao do TIM1 (RCR = 6) gera um "update" a cada 7 periodos = 20089 Hz,
 * que dispara o DMA1 canal 5 em modo burst: a cada amostra o DMA grava CCR1 e CCR2 sozinho.
 * A CPU so trabalha quando metade do buffer acaba (interrupcao de meia/fim de transferencia).
 */
#include <stdint.h>
#include <stdbool.h>
#include "stm32f1xx_hal.h"

#define AUDIO_FS      20089u     /* 72 MHz / (512 * 7) */
#define PWM_ARR       511u
#define AUDIO_BLOCO   64         /* amostras calculadas por vez (meio buffer) */

#define NTECLAS       10
#define VOZ_MUS0      NTECLAS    /* vozes 10..17 sao da musica (RTTTL / MIDI) */
#define NVOZES_MUS    16         /* vozes para musica (melodia + bateria) */
#define NVOZES        (NTECLAS + NVOZES_MUS)
#define NTIMBRES      15         /* 13 com tabela de onda + EPiano (FM) + Bateria */
#define TIMBRE_BATERIA 14        /* nas teclas: cada tecla toca uma peca da bateria */
#define NTABELAS      13         /* tabelas de onda na RAM */
#define NHARM_MAX     12

typedef struct {
  const char *nome;
  uint8_t  tabela;      /* qual forma de onda usa (0..12) */
  uint16_t ataque_ms;   /* tempo de subida */
  uint8_t  decai;       /* queda ate a sustentacao: maior = mais lento (0 = nao cai) */
  uint8_t  sustenta;    /* nivel mantido com a tecla apertada (0..100 %) */
  uint8_t  solta;       /* queda ao soltar: maior = mais lento */
  uint8_t  vibrato;     /* profundidade do vibrato (0 = sem) */
  uint16_t brilho;      /* 256 = sem filtro; menor = mais abafado */
  uint8_t  chorus;      /* 2o oscilador desafinado: 0 = desligado, 8..12 (maior = mais sutil) */
  uint8_t  fm_razao;    /* sintese FM: 0 = desligada; razao do modulador x4 (4 = 1:1, 14 = 1:3,5) */
  uint8_t  fm_indice;   /* FM: quanto o modulador "mexe" no som (0..255) */
  uint32_t ataque_inc;  /* calculado a partir de ataque_ms */
  uint32_t sust_nivel;  /* calculado a partir de sustenta */
} Inst;

void audio_init(TIM_HandleTypeDef *htim_pwm, DMA_HandleTypeDef *hdma_update);

/* Teclas (chamado pela leitura dos botoes) */
void audio_tecla(int i, bool apertada);
void audio_set_tecla_nota(int i, int midi);   /* nota MIDI da tecla (60 = Do4) */
int  audio_get_tecla_nota(int i);             /* sem a transposicao do encoder */
int  audio_tecla_nota_real(int i);            /* com a transposicao */

/* Vozes da musica */
void audio_voz_nota(int v, int midi, int inst, uint8_t pan, uint8_t forca);  /* forca 0..3 (3 = mais forte) */
void audio_voz_mudo(int v);
void audio_bateria(int v, int nota_gm, uint8_t forca);  /* toca uma peca de bateria (nota General MIDI, canal 10) */
int  audio_mus_alocar(void);                  /* escolhe uma voz livre para o MIDI */
void audio_mus_marcar(int v, uint8_t canal, uint8_t nota);
int  audio_mus_procurar(uint8_t canal, uint8_t nota);
void audio_mus_parar_todas(void);

/* Ajustes */
void audio_set_volume_pct(int pct);
int  audio_get_volume_pct(void);
void audio_set_timbre(int t);
int  audio_get_timbre(void);
void audio_set_oitava(int o);                 /* 2..7, 4 = normal */
int  audio_get_oitava(void);
int  audio_transposicao(void);                /* (oitava - 4) * 12 */
void audio_set_estereo(bool on);
bool audio_get_estereo(void);

/* Instrumentos */
const char *audio_nome_timbre(int t);
int   audio_procurar_timbre(const char *arg);
Inst *audio_inst(int t);
void  audio_inst_atualizar(int t);            /* recalcula depois de mudar os campos */
void  audio_inst_harmonicos(int t, const uint8_t *h, int n);

/* Diagnostico */
uint32_t audio_freq_hz_midi(int midi);
int  audio_cpu_pct(void);                     /* carga media da CPU no calculo do audio */
int  audio_cpu_pico_pct(void);
int  audio_vozes_ativas(void);

#endif
