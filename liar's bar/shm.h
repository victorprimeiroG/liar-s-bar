#ifndef SHM_H
#define SHM_H

#include <windows.h>

#define SHM_NOME_ARQ   "TrabalhoSO_MemoriaJogo"
#define SHM_NOME_MUTEX "TrabalhoSO_MutexJogo"

/* Estado espelhado entre os DOIS processos (memoria compartilhada real) */
typedef struct {
    volatile LONG vez;
    volatile LONG rodada;
    volatile LONG placar[2];
    volatile LONG jogadas;
    volatile LONG codigoUltima;   /* hash simples da ultima jogada */
} MemCompartilhada;

int  shm_abrir(void);
void shm_fechar(void);
void shm_lock(void);
void shm_unlock(void);
MemCompartilhada *shm_ptr(void);

#endif