#ifndef MIDI_H
#define MIDI_H
/*
 * Musica MIDI recebida como texto (gerado pelo enviar_midi.py):
 *   MIDI:nome:i0,i1,...,i15:T450 +2.74 T900 -2.74 ... FIM
 *   (linhas seguintes comecam com MIDI+)
 * Tnnn = espera nnn ms | +c.n = liga a nota n no canal c | -c.n = desliga | FIM = acabou
 *
 * A musica e gravada na MEMORIA FLASH (de 0x08010000 a 0x0801FFFF, 64 KB), em paginas de 1 KB.
 * Cabe ate 16128 eventos e ela continua guardada depois de desligar (comando "apagar" limpa).
 * Obs.: o STM32F103C8 e vendido com 64 KB de flash; essa area so existe nas placas com 128 KB
 * (a maioria das Blue Pill). Se a gravacao falhar, o teclado avisa.
 */
#include <stdint.h>
#include <stdbool.h>

#define MIDI_PAGINAS      63                      /* 1 pagina de cabecalho + 63 de eventos */
#define MIDI_MAX_EVENTOS  (MIDI_PAGINAS * 256)    /* 16128 */

void midi_init(void);                   /* le a musica guardada na flash (chamar na partida) */
void midi_linha(const char *linha);     /* recebe uma linha MIDI: ou MIDI+ */
bool midi_carregando(void);
bool midi_tocar(void);                  /* toca a musica guardada */
void midi_parar(void);
void midi_apagar(void);                 /* apaga a musica da flash */
void midi_atualizar(void);
bool midi_tocando(void);
bool midi_tem_guardada(void);
const char *midi_nome(void);
const char *midi_nota_atual(void);
uint32_t midi_versao(void);
uint16_t midi_eventos(void);
uint32_t midi_duracao_ms(void);
uint32_t midi_posicao_ms(void);
bool midi_estourou(void);
bool midi_erro_flash(void);

#endif
