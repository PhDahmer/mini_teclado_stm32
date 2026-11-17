#ifndef MIDI_H
#define MIDI_H
/*
 * Musica MIDI recebida como texto (gerado pelo enviar_midi.py):
 *   MIDI:nome:i0,i1,...,i15:T450 +2.74 T900 -2.74 ... FIM
 *   (linhas seguintes comecam com MIDI+)
 * Tnnn = espera nnn ms | +c.n = liga a nota n no canal c | -c.n = desliga | FIM = acabou
 * A musica fica guardada na RAM e pode ser repetida.
 */
#include <stdint.h>
#include <stdbool.h>

#define MIDI_MAX_EVENTOS 1600

void midi_linha(const char *linha);     /* recebe uma linha MIDI: ou MIDI+ */
bool midi_carregando(void);
bool midi_tocar(void);                  /* toca a musica guardada */
void midi_parar(void);
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

#endif
