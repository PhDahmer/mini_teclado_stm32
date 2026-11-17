/*
 * Leitura das 10 teclas, do encoder e do botao SW.
 * Roda dentro da interrupcao do DMA de audio, entao nao perde toques
 * mesmo enquanto a tela esta sendo atualizada no loop principal.
 */
#include "input.h"
#include "audio.h"
#include "stm32f1xx_hal.h"

/* Teclas na coluna da esquerda da Blue Pill (USB para cima), de baixo para cima:
   PB9, PB8, PB7, PB6, PB5, PB4, PB3, PA15, PA12, PA10
   (PB3, PB4 e PA15 ficam livres porque o CubeMX usa so o SWD, sem JTAG) */
static GPIO_TypeDef *const PORTA_TECLA[NTECLAS] = {
  GPIOB, GPIOB, GPIOB, GPIOB, GPIOB, GPIOB, GPIOB, GPIOA, GPIOA, GPIOA
};
static const uint16_t PINO_TECLA[NTECLAS] = {
  GPIO_PIN_9, GPIO_PIN_8, GPIO_PIN_7, GPIO_PIN_6, GPIO_PIN_5,
  GPIO_PIN_4, GPIO_PIN_3, GPIO_PIN_15, GPIO_PIN_12, GPIO_PIN_10
};

#define ENC_CLK GPIO_PIN_4     /* porta A */
#define ENC_DT  GPIO_PIN_5
#define ENC_SW  GPIO_PIN_6

#define DEBOUNCE_TECLA 2       /* 2 blocos seguidos iguais (~6 ms) */
#define DEBOUNCE_SW    4       /* ~13 ms */

static const int8_t TABELA_ENC[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};

static uint8_t estado[NTECLAS], cont[NTECLAS];
static volatile uint16_t teclas_bits;

static uint8_t enc_prev;
static int8_t enc_acum;
static volatile int8_t enc_delta;

static uint8_t sw_estado, sw_cont;
static volatile uint8_t sw_clique;

static uint8_t enc_ler(void) {
  uint32_t a = GPIOA->IDR;
  return (uint8_t)((((a & ENC_CLK) ? 1u : 0u) << 1) | ((a & ENC_DT) ? 1u : 0u));
}

void input_init(void) {
  enc_prev = enc_ler();
}

void input_encoder_tick(void) {
  uint8_t atual = enc_ler();
  if (atual == enc_prev) return;
  enc_acum = (int8_t)(enc_acum + TABELA_ENC[(enc_prev << 2) | atual]);
  enc_prev = atual;
  if (enc_acum >= 4) { enc_acum = 0; enc_delta++; }
  else if (enc_acum <= -4) { enc_acum = 0; enc_delta--; }
}

void input_scan(void) {
  uint32_t a = GPIOA->IDR, b = GPIOB->IDR;
  uint16_t bits = 0;
  for (int i = 0; i < NTECLAS; i++) {
    uint32_t idr = (PORTA_TECLA[i] == GPIOA) ? a : b;
    uint8_t lido = (idr & PINO_TECLA[i]) ? 0 : 1;     /* nivel baixo = apertada */
    if (lido == estado[i]) cont[i] = 0;
    else if (++cont[i] >= DEBOUNCE_TECLA) { estado[i] = lido; cont[i] = 0; }
    if (estado[i]) bits |= (uint16_t)(1u << i);
    audio_tecla(i, estado[i]);
  }
  teclas_bits = bits;

  uint8_t sw = (GPIOA->IDR & ENC_SW) ? 0 : 1;
  if (sw == sw_estado) sw_cont = 0;
  else if (++sw_cont >= DEBOUNCE_SW) {
    sw_estado = sw;
    sw_cont = 0;
    if (sw) sw_clique = 1;
  }
}

uint16_t input_teclas(void) { return teclas_bits; }

int input_encoder_delta(void) {
  __disable_irq();
  int d = enc_delta;
  enc_delta = 0;
  __enable_irq();
  return d;
}

bool input_sw_clique(void) {
  __disable_irq();
  bool c = sw_clique;
  sw_clique = 0;
  __enable_irq();
  return c;
}
