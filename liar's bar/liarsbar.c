/* =========================================================================
 *  LIAR'S BAR SIMPLIFICADO (1v1)  -  Winsock2 + pthreads + memoria compartilhada
 *
 *  REGRAS:
 *    - Baralho: 6 valores (A 2 3 4 5 6) x 6 copias = 36 cartas.
 *    - Mao de 6 cartas para cada jogador, recomposta a cada rodada.
 *    - Valor da mesa comeca em A e so sobe (declarado >= mesa).
 *    - Turno: JOGAR 1..3 cartas viradas declarando um valor, ou DUVIDAR.
 *    - Duvida: revela as cartas.
 *        todas iguais ao declarado -> quem duvidou paga
 *        alguma diferente         -> quem jogou paga
 *    - Revolver: 6 camaras, 2 balas. Tiro = eliminado.
 *        camaras esgotadas sem morte -> recarrega com 1 bala a mais.
 *
 *  NOTACAO:
 *      SEMENTE:<n>                      host -> cliente (semente comum)
 *      L<vez>:J:<valor>:<c1>,<c2>,<c3>  jogar declarando <valor>
 *      L<vez>:D                         duvidar
 *      BYE                              encerrar
 *      Ex: L1:J:3:3,2   (J1 diz que sao dois 3, mas sao um 3 e um 2)
 * ========================================================================= */

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <pthread.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>

#include "net.h"
#include "shm.h"

#define VALORES        6
#define COPIA_VALOR    6
#define BARALHO        (VALORES * COPIA_VALOR)
#define MAO_INICIAL    6
#define MAX_MAO        16
#define MAX_JOGA       3
#define CAMARAS        6
#define BALAS_INICIAL  2

typedef struct {
    int mao[2][MAX_MAO];
    int qtdMao[2];

    int monte[BARALHO];
    int qtdMonte;
    int descarte[BARALHO];
    int qtdDescarte;

    int mesa;
    int pendente;
    int ultJog;
    int ultValor;
    int ultCartas[MAX_JOGA];
    int ultQtd;

    int tambor[CAMARAS];
    int pos;
    int balas;
    int disparos;

    unsigned rng;

    int vez;
    int rodada;
    int fim;
    int vencedor;

    char motivo[160];
} Liars;

typedef struct {
    int tipo;                  /* 0 = jogar | 1 = duvidar */
    int valor;                 /* valor declarado             */
    int cartas[MAX_JOGA];      /* cartas REAIS jogadas        */
    int qtd;
} Cmd;

/* ------------------------- estado global ------------------------- */
static Liars            g;
static pthread_mutex_t  mtx;
static pthread_cond_t   cond;
static pthread_mutex_t  mtxEnvio;
static NetConexao       conn;
static int              eu = 1;
static int              ehHost = 0;
static volatile int     rodando = 1;

/* ------------------------- utilitarios --------------------------- */
static char valorChar(int v)
{
    static const char *S = "A23456";
    if (v < 1 || v > VALORES) return '?';
    return S[v - 1];
}

static int valorDeChar(char ch)
{
    if (ch == 'A' || ch == 'a') return 1;
    if (ch >= '2' && ch <= '6') return ch - '0';
    return -1;
}

static void ordenar(int *v, int n)
{
    for (int i = 1; i < n; i++) {
        int k = v[i], j = i - 1;
        while (j >= 0 && v[j] > k) { v[j + 1] = v[j]; j--; }
        v[j + 1] = k;
    }
}

static void enviar(const char *fmt, ...)
{
    char buf[NET_LINHA];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    pthread_mutex_lock(&mtxEnvio);
    net_enviar(&conn, "%s", buf);
    pthread_mutex_unlock(&mtxEnvio);
}

/* ------------------------- baralho / tambor ---------------------- */
static void embaralhar(int *v, int n, unsigned semente)
{
    unsigned x = semente ? semente : 1u;

    for (int i = n - 1; i > 0; i--) {
        int k;
        x = x * 1103515245u + 12345u;
        k = (int)((x >> 16) % (unsigned)(i + 1));
        int t = v[i]; v[i] = v[k]; v[k] = t;
    }
}

static void montarTambor(Liars *L, int balas)
{
    unsigned x = L->rng ^ (unsigned)(L->disparos * 2654435761u) ^ 0x9E3779B9u;

    if (balas > CAMARAS - 1) balas = CAMARAS - 1;
    if (balas < 1) balas = 1;

    for (int i = 0; i < CAMARAS; i++) L->tambor[i] = 0;
    for (int i = 0; i < balas; i++)   L->tambor[i] = 1;

    for (int i = CAMARAS - 1; i > 0; i--) {
        int k;
        x = x * 1103515245u + 12345u;
        k = (int)((x >> 16) % (unsigned)(i + 1));
        int t = L->tambor[i]; L->tambor[i] = L->tambor[k]; L->tambor[k] = t;
    }

    L->pos   = 0;
    L->balas = balas;
}

static int removerValor(Liars *L, int j, int valor)
{
    for (int i = 0; i < L->qtdMao[j]; i++) {
        if (L->mao[j][i] == valor) {
            L->mao[j][i] = L->mao[j][L->qtdMao[j] - 1];
            L->qtdMao[j]--;
            return 1;
        }
    }
    return 0;
}

static void recomporMaos(Liars *L)
{
    for (int j = 0; j < 2; j++) {
        while (L->qtdMao[j] < MAO_INICIAL) {
            if (L->qtdMonte == 0) {
                if (L->qtdDescarte == 0) break;
                for (int i = 0; i < L->qtdDescarte; i++) L->monte[i] = L->descarte[i];
                L->qtdMonte    = L->qtdDescarte;
                L->qtdDescarte = 0;
                L->rng = L->rng * 1103515245u + 12345u;
                embaralhar(L->monte, L->qtdMonte, L->rng);
            }
            L->mao[j][L->qtdMao[j]] = L->monte[--L->qtdMonte];
            L->qtdMao[j]++;
        }
    }
}

static void iniciar(Liars *L, unsigned semente)
{
    int k = 0;

    memset(L, 0, sizeof(*L));

    L->rng = semente ? semente : 1u;

    for (int v = 1; v <= VALORES; v++)
        for (int c = 0; c < COPIA_VALOR; c++)
            L->monte[k++] = v;

    L->qtdMonte = BARALHO;
    embaralhar(L->monte, L->qtdMonte, L->rng);

    L->qtdDescarte = 0;
    L->qtdMao[0]   = 0;
    L->qtdMao[1]   = 0;

    montarTambor(L, BALAS_INICIAL);
    recomporMaos(L);

    L->mesa     = 1;
    L->pendente = 0;
    L->ultJog   = -1;
    L->ultQtd   = 0;
    L->vez      = 1;
    L->rodada   = 1;
    L->fim      = 0;
}

/* ------------------------- resolucao da duvida ------------------- */
static void resolver(Liars *L, int duvidou)
{
    int mentiu = 0;
    int pagador;

    for (int i = 0; i < L->ultQtd; i++)
        if (L->ultCartas[i] != L->ultValor) mentiu = 1;

    pagador = mentiu ? L->ultJog : duvidou;

    printf("\n============== DUVIDA ==============\n");
    printf(" Declarado por J%d: '%c'\n", L->ultJog + 1, valorChar(L->ultValor));
    printf(" Cartas reais     : ");
    for (int i = 0; i < L->ultQtd; i++) printf("%c ", valorChar(L->ultCartas[i]));
    printf("\n");

    if (mentiu) printf(" MENTIRA! J%d enganou.\n", L->ultJog + 1);
    else        printf(" VERDADE! J%d falou a verdade.\n", L->ultJog + 1);

    printf(" Paga o revolver  : J%d\n", pagador + 1);

    if (L->tambor[L->pos]) {
        L->pos++;
        L->disparos++;
        snprintf(L->motivo, sizeof(L->motivo),
                 "J%d puxou o gatilho e levou um tiro (camara %d).", pagador + 1, L->pos);
        L->fim      = 1;
        L->vencedor = (pagador == 0) ? 2 : 1;
        printf(" *** CLICK *** J%d morreu.\n", pagador + 1);
        printf("====================================\n");
        return;
    }

    L->pos++;
    L->disparos++;
    printf(" CLICK vazio. J%d sobreviveu. Camaras usadas: %d/%d\n",
           pagador + 1, L->pos, CAMARAS);

    if (L->pos >= CAMARAS) {
        montarTambor(L, L->balas + 1);
        printf(" Tambor recarregado com %d balas.\n", L->balas);
    }

    L->mesa     = 1;
    L->pendente = 0;
    L->ultQtd   = 0;
    L->ultJog   = -1;

    recomporMaos(L);
    L->vez = (pagador == 0) ? 2 : 1;

    printf(" Nova rodada. Abre: J%d\n", L->vez);
    printf("====================================\n");
}

/* ------------------------- aplicacao de comando ------------------ */
static int aplicar(Liars *L, int vez, const Cmd *c)
{
    Liars backup;
    int j;

    if (L->fim) return 0;
    if (vez != L->vez) return 0;

    j = vez - 1;
    backup = *L;

    if (c->tipo == 0) {
        if (c->qtd < 1 || c->qtd > MAX_JOGA) return 0;
        if (c->valor < 1 || c->valor > VALORES) return 0;
        if (c->valor < L->mesa) return 0;
        if (L->qtdMao[j] < c->qtd) return 0;

        for (int i = 0; i < c->qtd; i++)
            if (!removerValor(L, j, c->cartas[i])) { *L = backup; return 0; }

        for (int i = 0; i < c->qtd; i++)
            L->descarte[L->qtdDescarte++] = c->cartas[i];

        L->ultJog   = j;
        L->ultValor = c->valor;
        L->ultQtd   = c->qtd;
        for (int i = 0; i < c->qtd; i++) L->ultCartas[i] = c->cartas[i];

        L->pendente = 1;
        L->mesa     = c->valor;
        L->vez      = 3 - L->vez;
        L->rodada++;
        return 1;
    }

    if (!L->pendente) return 0;
    if (L->ultJog == j) return 0;

    resolver(L, j);
    L->rodada++;
    return 1;
}

/* ------------------------- notacao ------------------------------- */
static int parseLiars(const char *linha, int *vez, Cmd *c)
{
    char buf[NET_LINHA];
    char *p, *corpo;

    if (!linha || (linha[0] != 'L' && linha[0] != 'l')) return 0;
    if (strlen(linha) >= sizeof(buf)) return 0;
    strcpy(buf, linha);

    p = strchr(buf, ':');
    if (!p) return 0;
    *p = 0;

    if (sscanf(buf, "L%d", vez) != 1) return 0;
    if (*vez != 1 && *vez != 2) return 0;

    corpo = p + 1;

    if (corpo[0] == 'D' || corpo[0] == 'd') { c->tipo = 1; c->qtd = 0; return 1; }
    if (corpo[0] != 'J' && corpo[0] != 'j') return 0;

    c->tipo = 0;
    corpo++;
    if (*corpo != ':') return 0;
    corpo++;

    c->valor = valorDeChar(corpo[0]);
    if (c->valor < 1) return 0;
    corpo++;
    if (*corpo != ':') return 0;
    corpo++;

    c->qtd = 0;
    while (*corpo) {
        int cv = valorDeChar(corpo[0]);
        if (cv < 1) return 0;
        if (c->qtd >= MAX_JOGA) return 0;
        c->cartas[c->qtd++] = cv;
        corpo++;
        if (*corpo == ',') corpo++;
        else if (*corpo) return 0;
    }

    return (c->qtd >= 1);
}

/* ------------------------- renderizacao -------------------------- */
static void ajudaJogo(void)
{
    printf("\n-------------- COMO JOGAR --------------\n");
    printf(" Baralho: A 2 3 4 5 6 (6 copias de cada). Mao de %d cartas.\n", MAO_INICIAL);
    printf(" O valor da mesa comeca em A e so sobe.\n");
    printf(" No seu turno:\n");
    printf("   j <valor> <posicoes>  jogar 1 a 3 cartas dizendo que sao <valor>\n");
    printf("                         ex: j 3 1 4   (cartas [1] e [4], declarando 3)\n");
    printf("   d                     duvidar da jogada anterior\n");
    printf("   h ajuda   |   q sair\n");
    printf(" Duvida certa -> quem mentiu paga. Duvida errada -> quem duvidou paga.\n");
    printf(" Revolver: %d camaras, %d balas. Tiro = eliminado.\n", CAMARAS, BALAS_INICIAL);
    printf("---------------------------------------\n\n");
}

static void mostrar(const Liars *L)
{
    int ordem[MAX_MAO];
    int n, j;

    j = eu - 1;
    n = L->qtdMao[j];

    for (int i = 0; i < n; i++) ordem[i] = L->mao[j][i];
    ordenar(ordem, n);

    printf("\n=========================================================\n");
    printf(" RODADA %d  |  MESA: '%c'  |  VEZ: J%d%s\n",
           L->rodada, valorChar(L->mesa), L->vez,
           (L->vez == eu) ? " (VOCE)" : " (oponente)");
    printf(" REVOLVER: %d/%d camaras puxadas  |  balas: %d\n",
           L->pos, CAMARAS, L->balas);
    printf(" MONTE: %d  |  DESCARTE: %d  |  MAO DO OPONENTE: %d cartas\n",
           L->qtdMonte, L->qtdDescarte, L->qtdMao[1 - j]);
    printf("---------------------------------------------------------\n");

    if (L->pendente)
        printf(" JOGADA PENDENTE: J%d baixou %d carta(s) dizendo ser '%c'\n",
               L->ultJog + 1, L->ultQtd, valorChar(L->ultValor));
    else
        printf(" Nenhuma jogada pendente. Voce abre a mesa.\n");

    printf("---------------------------------------------------------\n");
    printf(" SUA MAO (%d cartas):\n   ", n);
    for (int i = 0; i < n; i++) printf("[%d]%c  ", i + 1, valorChar(ordem[i]));
    printf("\n=========================================================\n");
}

/* ------------------------- memoria compartilhada ----------------- */
static void espelharMemoria(void)
{
    MemCompartilhada *m = shm_ptr();
    if (!m) return;

    shm_lock();
    m->vez          = g.vez;
    m->rodada       = g.rodada;
    m->placar[0]    = g.pos;
    m->placar[1]    = g.disparos;
    m->jogadas      = m->jogadas + 1;
    m->codigoUltima = (g.mesa * 31 + g.ultQtd * 17 + g.vez * 7 + g.ultValor);
    shm_unlock();
}

/* ------------------------- thread receptora ---------------------- */
static void *threadRecebe(void *arg)
{
    char linha[NET_LINHA];
    (void)arg;

    while (rodando) {
        Cmd c;
        int vez;

        if (net_receber(&conn, linha, sizeof(linha)) != 0) break;

        printf("\n[RX] %s\n", linha);

        pthread_mutex_lock(&mtx);

        if (strncmp(linha, "BYE", 3) == 0) {
            printf("\nO oponente encerrou a partida.\n");
            rodando = 0;
            pthread_cond_broadcast(&cond);
        } else if (parseLiars(linha, &vez, &c)) {
            if (!aplicar(&g, vez, &c))
                printf("  [ERRO] comando remoto recusado: %s\n", linha);
            espelharMemoria();
            pthread_cond_broadcast(&cond);
        } else {
            printf("  [ERRO] mensagem desconhecida: %s\n", linha);
        }

        pthread_mutex_unlock(&mtx);
    }

    rodando = 0;
    pthread_cond_broadcast(&cond);
    return NULL;
}

/* ------------------------- turno local --------------------------- */
static void turnoLocal(void)
{
    char linha[256];

    for (;;) {
        Liars  snap;
        int    ordem[MAX_MAO];
        int    n;
        Cmd    c;
        char   notacao[NET_LINHA];

        pthread_mutex_lock(&mtx);
        snap = g;
        pthread_mutex_unlock(&mtx);

        mostrar(&snap);

        n = snap.qtdMao[eu - 1];
        for (int i = 0; i < n; i++) ordem[i] = snap.mao[eu - 1][i];
        ordenar(ordem, n);

        printf("\n>>> SUA VEZ (J%d)\n", eu);
        if (snap.pendente)
            printf("    d = DUVIDAR da jogada de J%d\n", snap.ultJog + 1);
        printf("    j <valor> <posicoes> = JOGAR   ex: j 3 1 4\n");
        printf("    h = ajuda  |  q = sair\n    > ");
        fflush(stdout);

        if (!fgets(linha, sizeof(linha), stdin)) { rodando = 0; return; }
        linha[strcspn(linha, "\r\n")] = 0;
        if (linha[0] == 0) continue;

        if (linha[0] == 'q' || linha[0] == 'Q') { rodando = 0; return; }
        if (linha[0] == 'h' || linha[0] == 'H') { ajudaJogo(); continue; }

        if (linha[0] == 'd' || linha[0] == 'D') {
            if (!snap.pendente) { printf("  Nao ha jogada para duvidar.\n"); continue; }
            c.tipo  = 1;
            c.valor = 0;
            c.qtd   = 0;
            snprintf(notacao, sizeof(notacao), "L%d:D", eu);
        } else if (linha[0] == 'j' || linha[0] == 'J') {
            char *p = linha + 1;
            int   k = 0;

            while (*p == ' ') p++;

            c.valor = valorDeChar(*p);
            if (c.valor < 1) { printf("  Valor invalido. Use A 2 3 4 5 6.\n"); continue; }
            p++;

            c.tipo = 0;
            c.qtd  = 0;
            int erro = 0;

            while (*p && !erro) {
                int pos;

                while (*p == ' ' || *p == ',' || *p == ';') p++;
                if (*p == 0) break;

                if (*p < '0' || *p > '9') { erro = 1; break; }

                pos = 0;
                while (*p >= '0' && *p <= '9') { pos = pos * 10 + (*p - '0'); p++; }

                if (pos < 1 || pos > n)      { erro = 1; break; }
                if (c.qtd >= MAX_JOGA)       { erro = 1; break; }

                c.cartas[c.qtd++] = ordem[pos - 1];
            }

            if (erro || c.qtd < 1) {
                printf("  Informe de 1 a %d posicoes validas da sua mao.\n", MAX_JOGA);
                continue;
            }

            k += snprintf(notacao + k, sizeof(notacao) - (size_t)k,
                          "L%d:J:%c:", eu, valorChar(c.valor));
            for (int i = 0; i < c.qtd; i++)
                k += snprintf(notacao + k, sizeof(notacao) - (size_t)k, "%s%c",
                              (i == 0) ? "" : ",", valorChar(c.cartas[i]));
        } else {
            printf("  Comando invalido. Digite h para ajuda.\n");
            continue;
        }

        pthread_mutex_lock(&mtx);

        if (g.fim || g.vez != eu) {
            pthread_mutex_unlock(&mtx);
            printf("  Turno ja passou.\n");
            return;
        }

        if (!aplicar(&g, eu, &c)) {
            char mesa = valorChar(g.mesa);
            pthread_mutex_unlock(&mtx);
            printf("  Jogada ilegal. Declare valor >= '%c' e use cartas da sua mao.\n", mesa);
            continue;
        }

        espelharMemoria();
        pthread_mutex_unlock(&mtx);

        enviar("%s", notacao);
        printf("[TX] %s\n", notacao);
        return;
    }
}

/* ------------------------- main ---------------------------------- */
static void uso(const char *prog)
{
    printf("Uso:\n");
    printf("  %s --host [porta]        (Jogador 1, abre a partida)\n", prog);
    printf("  %s --cli <ip> [porta]    (Jogador 2, conecta)\n", prog);
}

int main(int argc, char **argv)
{
    pthread_mutexattr_t attr;
    pthread_t th;
    int porta = 5050;
    unsigned semente = 0;

    if (argc < 2) { uso(argv[0]); return 1; }

    ehHost = (strcmp(argv[1], "--host") == 0 || strcmp(argv[1], "--servidor") == 0);

    if (net_iniciar() != 0) { printf("Falha no WSAStartup.\n"); return 1; }

    if (ehHost) {
        SOCKET srv;
        if (argc >= 3) porta = atoi(argv[2]);
        srv = net_servidor(porta);
        if (srv == INVALID_SOCKET) { printf("Falha ao abrir a porta %d.\n", porta); return 1; }
        printf("Aguardando o oponente na porta %d...\n", porta);
        if (net_aceitar(srv, &conn) != 0) { printf("Falha ao aceitar conexao.\n"); return 1; }
        closesocket(srv);
        eu = 1;
    } else {
        const char *ip = (argc >= 3) ? argv[2] : "127.0.0.1";
        if (argc >= 4) porta = atoi(argv[3]);
        if (net_conectar(ip, porta, &conn) != 0) {
            printf("Falha ao conectar em %s:%d\n", ip, porta);
            return 1;
        }
        eu = 2;
    }

    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&mtx, &attr);
    pthread_mutexattr_destroy(&attr);
    pthread_cond_init(&cond, NULL);
    pthread_mutex_init(&mtxEnvio, NULL);

    shm_abrir();

    if (ehHost) {
        semente = (unsigned)time(NULL) ^ (unsigned)GetCurrentProcessId();
        enviar("SEMENTE:%u", semente);
    } else {
        char linha[NET_LINHA];
        if (net_receber(&conn, linha, sizeof(linha)) != 0) {
            printf("Falha ao receber a semente.\n");
            return 1;
        }
        if (sscanf(linha, "SEMENTE:%u", &semente) != 1) {
            printf("Semente invalida: %s\n", linha);
            return 1;
        }
    }

    iniciar(&g, semente);
    espelharMemoria();

    printf("\n=========================================\n");
    printf("      LIAR'S BAR SIMPLIFICADO (1v1)\n");
    printf("=========================================\n");
    printf(" Voce e o Jogador %d (%s)\n", eu, ehHost ? "HOST" : "CLIENTE");
    printf(" Abre a partida o Jogador 1.\n");
    ajudaJogo();

    pthread_create(&th, NULL, threadRecebe, NULL);

    for (;;) {
        pthread_mutex_lock(&mtx);

        while (rodando && !g.fim && g.vez != eu)
            pthread_cond_wait(&cond, &mtx);

        if (!rodando || g.fim) {
            int  fim    = g.fim;
            int  venc   = g.vencedor;
            char motivo[160];

            snprintf(motivo, sizeof(motivo), "%s", g.motivo);
            pthread_mutex_unlock(&mtx);

            if (fim) {
                printf("\n\n=========================================\n");
                printf(" %s\n", motivo);
                if (venc == eu) printf(" *** VOCE VENCEU! ***\n");
                else            printf(" *** J%d VENCEU. ***\n", venc);
                printf("=========================================\n");
            }
            break;
        }

        pthread_mutex_unlock(&mtx);
        turnoLocal();
    }

    enviar("BYE");
    rodando = 0;
    pthread_join(th, NULL);

    shm_fechar();
    net_fechar(&conn);
    net_encerrar();
    pthread_mutex_destroy(&mtx);
    pthread_mutex_destroy(&mtxEnvio);
    pthread_cond_destroy(&cond);

    printf("\nPressione ENTER para sair...");
    getchar();
    return 0;
}