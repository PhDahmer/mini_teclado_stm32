#ifndef INPUT_H
#define INPUT_H
/*
 * Botoes e encoder (todos com pull-up interno, ligados ao GND quando acionados).
 *   Teclas 1..10 : PB9, PB8, PB7, PB6, PB5, PB4, PB3, PA15, PA12, PA10
 *   Encoder      : CLK = PA4, DT = PA5, SW = PA6
 * A leitura roda dentro da interrupcao do DMA de audio (nao perde toques).
 */
#include <stdint.h>
#include <stdbool.h>

void input_init(void);
void input_scan(void);          /* a cada bloco de audio (~3,2 ms): teclas e SW */
void input_encoder_tick(void);  /* a cada 4 amostras (~5 kHz): encoder */
uint16_t input_teclas(void);    /* bit i = tecla i apertada */
int  input_encoder_delta(void);
bool input_sw_clique(void);

#endif
