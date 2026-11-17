#ifndef CMD_H
#define CMD_H
/* Comandos pelo Bluetooth (HC-05 na USART2: PA2 = TX, PA3 = RX, 115200 baud). RX e TX com DMA. */
#include "stm32f1xx_hal.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

void cmd_init(UART_HandleTypeDef *huart);
void cmd_atualizar(void);
void cmd_print(const char *s);
void cmd_println(const char *s);
void cmd_processar(char *linha);

/* configura o HC-05 (em modo AT) para 115200 baud; escreve as respostas em 'saida' */
bool cmd_hc05_115200(char *saida, size_t tam);

/* mensagem temporaria na tela (comando "tela") */
const char *cmd_meme_texto(void);
uint32_t cmd_meme_ate(void);
uint32_t cmd_meme_versao(void);

#endif
