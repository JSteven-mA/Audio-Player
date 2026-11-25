/* main.c - Interfaz de línea de comandos (hilo principal).
 * Solo parsea comandos y llama a la API del motor; nunca toca el búfer ni el
 * dispositivo de audio. */
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include "engine.h"

static void on_sigint(int sig) { (void)sig; }   /* solo interrumpe fgets() para salir limpio */

static int parse_idx(const char *s, size_t *out)  /* 1-based -> 0-based */
{
    if (!s) return -1;
    char *end; errno = 0;
    unsigned long v = strtoul(s, &end, 10);
    if (errno || *end || v == 0) return -1;
    *out = v - 1;
    return 0;
}

/* Expande "~/" al HOME (el shell no lo hace: el comando llega por stdin). */
static void expand_tilde(const char *in, char *out, size_t n)
{
    const char *home = getenv("HOME");
    if (in[0] == '~' && (in[1] == '/' || in[1] == '\0') && home)
        snprintf(out, n, "%s%s", home, in + 1);
    else
        snprintf(out, n, "%s", in);
}

static void help(void)
{
    puts("Comandos:\n"
         "  add <archivo>      agrega a la lista       rm <n>        elimina la pista n\n"
         "  mv <de> <a>        reordena                clear         vacia la lista\n"
         "  list               muestra la lista        status        estado del motor\n"
         "  play               inicia / reanuda        pause         pausa / reanuda\n"
         "  stop               detiene                 next | prev   siguiente / anterior\n"
         "  jump <n>           reproduce la pista n    wait          espera a que termine todo\n"
         "  sleep <seg>        pausa el CLI (scripts)  help | quit");
}

int main(int argc, char **argv)
{
    sink_kind_t kind = SINK_APLAY;
    int autoplay = 0;
    engine_t e;

    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigint; sigaction(SIGINT, &sa, NULL);   /* sin SA_RESTART */

    if (engine_init(&e, SINK_NULL) != 0) { fprintf(stderr, "init fallo\n"); return 1; }
    int ai = 1;
    for (; ai < argc && argv[ai][0] == '-'; ai++) {
        if (!strcmp(argv[ai], "--sink") && ai + 1 < argc) {
            const char *k = argv[++ai];
            if (!strcmp(k, "aplay")) kind = SINK_APLAY;
            else if (!strcmp(k, "paplay")) kind = SINK_PAPLAY;
            else if (!strcmp(k, "null")) kind = SINK_NULL;
            else { fprintf(stderr, "sink invalido: %s\n", k); engine_shutdown(&e); return 1; }
        } else if (!strcmp(argv[ai], "--autoplay")) autoplay = 1;
        else { fprintf(stderr, "uso: %s [--sink aplay|paplay|null] [--autoplay] [archivos...]\n", argv[0]); engine_shutdown(&e); return 1; }
    }
    e.sink_kind = kind;                       /* antes de lanzar hilos */
    if (engine_start(&e) != 0) { fprintf(stderr, "no se pudieron crear hilos\n"); engine_shutdown(&e); return 1; }

    for (; ai < argc; ai++)
        if (engine_add(&e, argv[ai]) != 0) fprintf(stderr, "no se pudo agregar: %s\n", argv[ai]);
    if (autoplay) engine_play(&e);

    puts("Reproductor concurrente - escribe 'help' para ver los comandos");
    char line[1024];
    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *cmd = strtok(line, " \t");
        if (!cmd) continue;
        char *a1 = strtok(NULL, "");
        while (a1 && (*a1 == ' ' || *a1 == '\t')) a1++;
        size_t i, j;

        if (!strcmp(cmd, "quit") || !strcmp(cmd, "exit")) break;
        else if (!strcmp(cmd, "help")) help();
        else if (!strcmp(cmd, "add")) {
            if (!a1 || !*a1) puts("uso: add <archivo>");
            else {
                char full[PL_PATH_MAX];
                expand_tilde(a1, full, sizeof full);
                if (engine_add(&e, full) != 0) printf("no se pudo agregar '%s' (no existe, sin permiso o ruta muy larga)\n", full);
                else printf("agregada: %s\n", full);
            }
        }
        else if (!strcmp(cmd, "rm")) {
            if (parse_idx(a1, &i) || engine_remove(&e, i)) puts("indice invalido"); else puts("eliminada");
        }
        else if (!strcmp(cmd, "mv")) {
            char *s2 = a1 ? strchr(a1, ' ') : NULL;
            if (s2) *s2++ = '\0';
            if (parse_idx(a1, &i) || parse_idx(s2, &j) || engine_move(&e, i, j)) puts("uso: mv <de> <a>"); else puts("movida");
        }
        else if (!strcmp(cmd, "clear")) { engine_clear(&e); puts("lista vaciada"); }
        else if (!strcmp(cmd, "list")) engine_print_list(&e, stdout);
        else if (!strcmp(cmd, "status")) engine_print_status(&e, stdout);
        else if (!strcmp(cmd, "play")) { if (engine_play(&e)) puts("lista vacia"); else puts("play"); }
        else if (!strcmp(cmd, "pause")) {
            puts(engine_pause_toggle(&e) == ST_PAUSED ? "pausado" : "reanudado/sin efecto");
        }
        else if (!strcmp(cmd, "stop")) { engine_stop(&e); puts("stop"); }
        else if (!strcmp(cmd, "next")) { if (!engine_next(&e)) puts("fin de la lista"); else puts("next"); }
        else if (!strcmp(cmd, "prev")) { if (engine_prev(&e)) puts("lista vacia"); else puts("prev"); }
        else if (!strcmp(cmd, "jump")) { if (parse_idx(a1, &i) || engine_jump(&e, i)) puts("indice invalido"); else puts("jump"); }
        else if (!strcmp(cmd, "wait")) engine_wait_idle(&e);
        else if (!strcmp(cmd, "sleep")) { if (a1) { double s = atof(a1); struct timespec ts = { (time_t)s, (long)((s - (time_t)s) * 1e9) }; nanosleep(&ts, NULL); } }
        else printf("comando desconocido: %s (help)\n", cmd);
        fflush(stdout);
    }
    engine_shutdown(&e);
    return 0;
}
