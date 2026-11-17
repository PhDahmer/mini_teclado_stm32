#ifndef MUSICA_MEM_H
#define MUSICA_MEM_H
/* Memoria compartilhada entre a musica RTTTL e a MIDI (so uma fica guardada por vez). */
#include <stdint.h>
#define MUSICA_MEM_BYTES 6400
extern uint8_t musica_mem[MUSICA_MEM_BYTES];
#endif
