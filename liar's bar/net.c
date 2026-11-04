#include "net.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

static int g_wsa = 0;

int net_iniciar(void)
{
    WSADATA w;
    if (g_wsa) return 0;
    if (WSAStartup(MAKEWORD(2, 2), &w) != 0) return -1;
    g_wsa = 1;
    return 0;
}

void net_encerrar(void)
{
    if (g_wsa) { WSACleanup(); g_wsa = 0; }
}

SOCKET net_servidor(int porta)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in a;
    BOOL sim = TRUE;

    if (s == INVALID_SOCKET) return INVALID_SOCKET;

    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&sim, sizeof(sim));

    memset(&a, 0, sizeof(a));
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port        = htons((u_short)porta);

    if (bind(s, (struct sockaddr *)&a, sizeof(a)) == SOCKET_ERROR) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    if (listen(s, 1) == SOCKET_ERROR) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

int net_aceitar(SOCKET srv, NetConexao *out)
{
    SOCKET c = accept(srv, NULL, NULL);
    if (c == INVALID_SOCKET) return -1;
    out->s   = c;
    out->len = 0;
    out->buf[0] = 0;
    return 0;
}

int net_conectar(const char *ip, int porta, NetConexao *out)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in a;

    if (s == INVALID_SOCKET) return -1;

    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port   = htons((u_short)porta);

    if (inet_pton(AF_INET, ip, &a.sin_addr) != 1) { closesocket(s); return -1; }
    if (connect(s, (struct sockaddr *)&a, sizeof(a)) == SOCKET_ERROR) {
        closesocket(s);
        return -1;
    }

    out->s   = s;
    out->len = 0;
    out->buf[0] = 0;
    return 0;
}

int net_enviar(NetConexao *c, const char *fmt, ...)
{
    char linha[NET_LINHA];
    va_list ap;
    int total, env = 0;

    va_start(ap, fmt);
    vsnprintf(linha, sizeof(linha) - 3, fmt, ap);
    va_end(ap);

    strcat(linha, "\n");
    total = (int)strlen(linha);

    while (env < total) {
        int r = send(c->s, linha + env, total - env, 0);
        if (r <= 0) return -1;
        env += r;
    }
    return 0;
}

/* Le uma linha completa (delimitada por \n), mantendo o resto no buffer */
int net_receber(NetConexao *c, char *linha, int tam)
{
    for (;;) {
        char *nl = (char *)memchr(c->buf, '\n', c->len);

        if (nl) {
            int n = (int)(nl - c->buf);
            int k = (n < tam - 1) ? n : tam - 1;

            memcpy(linha, c->buf, k);
            linha[k] = 0;
            if (k > 0 && linha[k - 1] == '\r') linha[k - 1] = 0;

            int sobra = c->len - n - 1;
            memmove(c->buf, nl + 1, sobra);
            c->len = sobra;
            return 0;
        }

        if (c->len >= NET_LINHA - 1) c->len = 0;

        int r = recv(c->s, c->buf + c->len, NET_LINHA - 1 - c->len, 0);
        if (r <= 0) return -1;
        c->len += r;
    }
}

void net_fechar(NetConexao *c)
{
    if (c->s != INVALID_SOCKET) { closesocket(c->s); c->s = INVALID_SOCKET; }
}