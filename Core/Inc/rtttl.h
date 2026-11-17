#ifndef RTTTL_H
#define RTTTL_H
#include <stdint.h>
#include <stdbool.h>

#define RTTTL_MAX 1600

bool rtttl_iniciar(const char *texto, bool copiar);  /* copiar = false para textos fixos na flash */
void rtttl_parar(void);
void rtttl_atualizar(void);
bool rtttl_tocando(void);
const char *rtttl_nome(void);
const char *rtttl_nota_atual(void);
const char *rtttl_historico(void);
uint32_t rtttl_versao(void);
int  rtttl_bpm(void);
int  rtttl_total_notas(void);
void rtttl_set_bpm_forcado(int bpm);
int  rtttl_get_bpm_forcado(void);
const char *rtttl_storms(void);
bool rtttl_tem_guardada(void);
const char *rtttl_guardada(void);

void rtttl_memoria_perdida(void);

#endif
