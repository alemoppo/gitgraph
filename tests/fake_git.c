/* fake_git.c - finto git.exe per i test di sicurezza.
 *
 * Non fa nulla di utile: quando viene eseguito scrive un file sentinella
 * accanto a se stesso e termina. Serve a verificare che gitgraph NON lo
 * esegua al posto del vero git: con la risoluzione in percorso assoluto il
 * programma viene cercato solo nelle voci assolute del PATH, mai nella
 * directory corrente, quindi questo eseguibile non deve mai partire.
 *
 * Il nome del file sentinella si puo' cambiare con la variabile d'ambiente
 * GG_FAKE_GIT_SENTINEL, cosi' il test non dipende dalla posizione.
 */
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    char self[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, self, (DWORD)sizeof self);
    const char *sentinel = getenv("GG_FAKE_GIT_SENTINEL");
    char path[MAX_PATH];

    if (sentinel && *sentinel) {
        snprintf(path, sizeof path, "%s", sentinel);
    } else {
        /* accanto all'eseguibile, con nome fisso */
        if (n == 0 || n >= sizeof self)
            return 1;
        char *slash = strrchr(self, '\\');
        if (slash)
            *slash = '\0';
        snprintf(path, sizeof path, "%s\\git_runs.txt", self);
    }

    HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        const char msg[] = "eseguito\n";
        DWORD written = 0;
        WriteFile(h, msg, (DWORD)(sizeof msg - 1), &written, NULL);
        CloseHandle(h);
    }
    /* Non e' git: fallisce, cosi' un eventuale uso accidentale si vede */
    return 1;
}