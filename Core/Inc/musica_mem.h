#ifndef MUSICA_MEM_H
#define MUSICA_MEM_H
/* Memoria da musica RTTTL recebida pelo Bluetooth (a MIDI agora fica na flash). */
#include <stdint.h>
#define MUSICA_MEM_BYTES 1600
extern uint8_t musica_mem[MUSICA_MEM_BYTES];
#endif
