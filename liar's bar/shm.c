#include "shm.h"

static HANDLE             g_map = NULL;
static MemCompartilhada  *g_ptr = NULL;
static HANDLE             g_mtx = NULL;

int shm_abrir(void)
{
    g_map = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                               0, sizeof(MemCompartilhada), SHM_NOME_ARQ);
    if (!g_map) return -1;

    g_ptr = (MemCompartilhada *)MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS,
                                              0, 0, sizeof(MemCompartilhada));
    if (!g_ptr) { CloseHandle(g_map); g_map = NULL; return -1; }

    g_mtx = CreateMutexA(NULL, FALSE, SHM_NOME_MUTEX);
    if (!g_mtx) {
        UnmapViewOfFile(g_ptr);
        CloseHandle(g_map);
        g_ptr = NULL; g_map = NULL;
        return -1;
    }
    return 0;
}

void shm_lock(void)   { if (g_mtx) WaitForSingleObject(g_mtx, INFINITE); }
void shm_unlock(void) { if (g_mtx) ReleaseMutex(g_mtx); }
MemCompartilhada *shm_ptr(void) { return g_ptr; }

void shm_fechar(void)
{
    if (g_ptr) { UnmapViewOfFile(g_ptr); g_ptr = NULL; }
    if (g_map) { CloseHandle(g_map);    g_map = NULL; }
    if (g_mtx) { CloseHandle(g_mtx);    g_mtx = NULL; }
}