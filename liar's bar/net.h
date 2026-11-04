#ifndef NET_H
#define NET_H

/* ATENCAO: winsock2.h SEMPRE antes de windows.h (evita o conflito com winsock.h) */
#include <winsock2.h>
#include <ws2tcpip.h>

#define NET_LINHA 1024

typedef struct {
    SOCKET s;
    char   buf[NET_LINHA];
    int    len;
} NetConexao;

int    net_iniciar(void);
void   net_encerrar(void);
SOCKET net_servidor(int porta);
int    net_aceitar(SOCKET srv, NetConexao *out);
int    net_conectar(const char *ip, int porta, NetConexao *out);
int    net_enviar(NetConexao *c, const char *fmt, ...);
int    net_receber(NetConexao *c, char *linha, int tam);
void   net_fechar(NetConexao *c);

#endif