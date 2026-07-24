#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <getopt.h>
#include <time.h>
#include <errno.h>
#include <stdarg.h>
#include <hunspell.h>

#define DEFAULT_AFF          "/usr/share/hunspell/en_US.aff"
#define DEFAULT_DIC          "/usr/share/hunspell/en_US.dic"
#define DEFAULT_PAGER        "less"
#define DEFAULT_PAGER_ARGS   "-R"
#define ANSI_BOLD_ON         "\033[1m"
#define ANSI_BOLD_OFF        "\033[22m"
#define ANSI_BOLD_ON_LEN     (sizeof(ANSI_BOLD_ON)  - 1)
#define ANSI_BOLD_OFF_LEN    (sizeof(ANSI_BOLD_OFF) - 1)
#define SMALLBUF             256
#define CACHE_SIZE           4096
#define CACHE_WAYS           2
#define OUTBUF_SIZE          8192
#define BUFSIZE              4096
#define MAX_DEFAULT_PAGER_ARGS 4

typedef enum {
    LOG_DEBUG,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR
} log_level_t;

typedef struct {
    char        *aff_path;
    char        *dic_path;
    char        *pager;
    char       **pager_args;
    int          pager_argc;
    int          color;
    char        *custom_dict;
    log_level_t  log_level;
} config_t;

typedef struct {
    char word[SMALLBUF];
    int  correct;
    int  occupied;
} cache_entry_t;

typedef struct {
    cache_entry_t ways[CACHE_WAYS];
} cache_set_t;

typedef enum {
    ST_NEWLINE,
    ST_NOISE,
    ST_WORD,
    ST_NOTWORD,
    ST_ESCAPE
} parser_state_t;

typedef struct {
    Hunhandle   *hun;
    cache_set_t  cache[CACHE_SIZE];
    unsigned int cache_mask;
    parser_state_t state;
    int          esclen;
    char         escape[SMALLBUF];
    int          wordlen;
    char         word[SMALLBUF];
    int          resetlen;
    char         reset[SMALLBUF];
    int          line;
    char         outbuf[OUTBUF_SIZE];
    int          outbuf_len;
} state_t;

static config_t              g_config;
static volatile sig_atomic_t g_terminate = 0;

static void log_msg(log_level_t level, const char *fmt, ...) {
    if (level < g_config.log_level) return;
    struct tm tm_buf;
    time_t    now = time(NULL);
    localtime_r(&now, &tm_buf);
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "[%02d:%02d:%02d] ", tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
    va_end(args);
}

static void *xmalloc(size_t size) {
    void *p = malloc(size);
    if (!p) { log_msg(LOG_ERROR, "Out of memory"); exit(EXIT_FAILURE); }
    return p;
}

static void *xrealloc(void *ptr, size_t size) {
    void *p = realloc(ptr, size);
    if (!p) { log_msg(LOG_ERROR, "Out of memory"); exit(EXIT_FAILURE); }
    return p;
}

static char *xstrdup(const char *s) {
    char *p = strdup(s);
    if (!p) { log_msg(LOG_ERROR, "Out of memory"); exit(EXIT_FAILURE); }
    return p;
}

static void outbuf_flush(state_t *st) {
    if (st->outbuf_len > 0) {
        write(STDOUT_FILENO, st->outbuf, st->outbuf_len);
        st->outbuf_len = 0;
    }
}

static void outbuf_write(state_t *st, const char *data, int len) {
    if (len <= 0) return;
    if (st->outbuf_len + len > OUTBUF_SIZE) outbuf_flush(st);
    if (len >= OUTBUF_SIZE) {
        write(STDOUT_FILENO, data, len);
        return;
    }
    memcpy(st->outbuf + st->outbuf_len, data, len);
    st->outbuf_len += len;
}

static unsigned int hash_word(const char *word) {
    unsigned int h = 2166136261u;
    while (*word) { h ^= (unsigned char)*word++; h *= 16777619u; }
    return h;
}

static int cache_lookup(state_t *st, const char *word) {
    unsigned int   h   = hash_word(word) & st->cache_mask;
    cache_set_t   *set = &st->cache[h];
    for (int w = 0; w < CACHE_WAYS; w++) {
        if (set->ways[w].occupied && strcmp(set->ways[w].word, word) == 0)
            return set->ways[w].correct;
    }
    return -1;
}

static void cache_store(state_t *st, const char *word, int correct) {
    unsigned int  h      = hash_word(word) & st->cache_mask;
    cache_set_t  *set    = &st->cache[h];
    int           target = 0;
    for (int w = 0; w < CACHE_WAYS; w++) {
        if (!set->ways[w].occupied) { target = w; break; }
    }
    strncpy(set->ways[target].word, word, SMALLBUF - 1);
    set->ways[target].word[SMALLBUF - 1] = '\0';
    set->ways[target].correct            = correct;
    set->ways[target].occupied           = 1;
}

static int check_word(state_t *st) {
    if (st->wordlen == 0 || st->wordlen >= SMALLBUF) return 1;
    st->word[st->wordlen] = '\0';
    int cached = cache_lookup(st, st->word);
    if (cached >= 0) return cached;
    int result = Hunspell_spell(st->hun, st->word);
    cache_store(st, st->word, result);
    return result;
}

static void check_and_emit(state_t *st) {
    if (st->wordlen == 0) return;
    if (check_word(st)) {
        outbuf_write(st, st->word, st->wordlen);
        return;
    }
    if (g_config.color) {
        outbuf_write(st, ANSI_BOLD_ON,  ANSI_BOLD_ON_LEN);
        outbuf_write(st, st->word,      st->wordlen);
        outbuf_write(st, ANSI_BOLD_OFF, ANSI_BOLD_OFF_LEN);
        outbuf_write(st, st->reset,     st->resetlen);
    } else {
        outbuf_write(st, st->word, st->wordlen);
    }
}

static int utf8_char_len(unsigned char c) {
    if (c < 0x80)               return 1;
    if ((c & 0xE0) == 0xC0)     return 2;
    if ((c & 0xF0) == 0xE0)     return 3;
    if ((c & 0xF8) == 0xF0)     return 4;
    return 1;
}

static int is_utf8_continuation(unsigned char c) {
    return (c & 0xC0) == 0x80;
}

static void escape_append(state_t *st, char c) {
    if (st->esclen < SMALLBUF) st->escape[st->esclen++] = c;
}

static void escape_commit(state_t *st) {
    st->resetlen = st->esclen;
    memcpy(st->reset, st->escape, st->esclen);
}

static int process_multibyte(state_t *st, const char *buf, size_t len, int clen, const char **last) {
    if (len < (size_t)clen) return -1;
    for (int i = 1; i < clen; i++) {
        if (!is_utf8_continuation((unsigned char)buf[i])) return 0;
    }
    switch (st->state) {
        case ST_WORD:
            if (st->wordlen + clen < SMALLBUF) {
                memcpy(st->word + st->wordlen, buf, clen);
                st->wordlen += clen;
            } else {
                outbuf_write(st, st->word, st->wordlen);
                st->state = ST_NOTWORD;
            }
            break;
        case ST_ESCAPE:
            for (int i = 0; i < clen && st->esclen < SMALLBUF; i++)
                st->escape[st->esclen++] = buf[i];
            break;
        default:
            if (*last < buf) outbuf_write(st, *last, (int)(buf - *last));
            st->state = ST_NOISE;
            break;
    }
    return clen;
}

static void process_letter(state_t *st, char c, const char **last, const char *buf) {
    switch (st->state) {
        case ST_WORD:
            if (st->wordlen < SMALLBUF)
                st->word[st->wordlen++] = c;
            else {
                outbuf_write(st, st->word, SMALLBUF);
                st->state = ST_NOTWORD;
            }
            break;
        case ST_NOTWORD:
            break;
        case ST_ESCAPE:
            escape_append(st, c);
            if (c == 'm') escape_commit(st);
            st->state = ST_NOISE;
            break;
        default:
            if (*last < buf) outbuf_write(st, *last, (int)(buf - *last));
            st->state   = ST_WORD;
            st->wordlen = 1;
            st->word[0] = c;
            break;
    }
}

static void process_alnum_or_underscore(state_t *st, const char *buf, const char **last) {
    switch (st->state) {
        case ST_ESCAPE:
            escape_append(st, *buf);
            break;
        case ST_WORD:
            outbuf_write(st, st->word, st->wordlen);
            *last     = buf;
            st->state = ST_NOTWORD;
            break;
        default:
            st->state = ST_NOTWORD;
            break;
    }
}

static void process_apostrophe(state_t *st, char c, const char *buf, size_t len) {
    (void)c;
    if (st->state == ST_WORD && len > 1 &&
        ((buf[1] >= 'a' && buf[1] <= 'z') || (buf[1] >= 'A' && buf[1] <= 'Z')) &&
        st->wordlen < SMALLBUF) {
        st->word[st->wordlen++] = '\'';
    }
}

static void process_control(state_t *st, char c, const char **last, const char *buf) {
    switch (st->state) {
        case ST_ESCAPE:
            escape_append(st, c);
            return;
        case ST_WORD:
            check_and_emit(st);
            *last = buf;
            break;
        default:
            break;
    }
    switch (c) {
        case '\n':
            st->state = ST_NEWLINE;
            st->line++;
            break;
        case '\033':
            st->state      = ST_ESCAPE;
            st->esclen     = 1;
            st->escape[0]  = '\033';
            break;
        default:
            st->state = ST_NOISE;
            break;
    }
}

static void process_buffer(state_t *st, const char *buf, size_t len) {
    const char *last = buf;
    while (len > 0) {
        unsigned char c    = (unsigned char)*buf;
        int           clen = utf8_char_len(c);
        if (clen > 1) {
            int adv = process_multibyte(st, buf, len, clen, &last);
            if (adv < 0) break;
            if (adv == 0) { buf++; len--; continue; }
            buf  += adv;
            len  -= adv;
            last  = buf;
            continue;
        }
        char lc = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : (char)c;
        if (lc >= 'a' && lc <= 'z') {
            process_letter(st, lc, &last, buf);
        } else if ((c >= '0' && c <= '9') || c == '_') {
            process_alnum_or_underscore(st, buf, &last);
        } else if (c == '\'') {
            int handled = 0;
            if (st->state == ST_WORD && len > 1 &&
                ((buf[1] >= 'a' && buf[1] <= 'z') || (buf[1] >= 'A' && buf[1] <= 'Z')) &&
                st->wordlen < SMALLBUF) {
                st->word[st->wordlen++] = '\'';
                handled = 1;
            }
            if (!handled) process_control(st, (char)c, &last, buf);
        } else {
            process_control(st, (char)c, &last, buf);
        }
        buf++;
        len--;
    }
    if (st->state != ST_WORD && last < buf)
        outbuf_write(st, last, (int)(buf - last));
}

static void load_dictionary_file(Hunhandle *handle, const char *filename) {
    if (!filename) return;
    struct stat st;
    if (stat(filename, &st) == 0 && S_ISREG(st.st_mode)) {
        if (Hunspell_add_dic(handle, filename) != 0)
            log_msg(LOG_WARN, "Failed to add dictionary: %s", filename);
    }
}

static void load_user_dictionaries(Hunhandle *hun) {
    if (g_config.custom_dict)
        load_dictionary_file(hun, g_config.custom_dict);
    const char *home = getenv("HOME");
    if (home) {
        char path[BUFSIZE];
        snprintf(path, sizeof(path), "%s/.dictionary", home);
        load_dictionary_file(hun, path);
    }
    load_dictionary_file(hun, ".dictionary");
}

static void signal_handler(int sig) {
    (void)sig;
    g_terminate = 1;
}

static void setup_signals(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    if (sigaction(SIGTERM, &sa, NULL) == -1)
        log_msg(LOG_WARN, "sigaction SIGTERM: %s", strerror(errno));
    if (sigaction(SIGINT, &sa, NULL) == -1)
        log_msg(LOG_WARN, "sigaction SIGINT: %s", strerror(errno));
    signal(SIGPIPE, SIG_IGN);
}

static void exec_pager(char **argv) {
    if (!argv || !argv[0]) {
        fputs("No pager specified\n", stderr);
        exit(EXIT_FAILURE);
    }
    execvp(g_config.pager, argv);
    log_msg(LOG_ERROR, "exec pager '%s': %s", g_config.pager, strerror(errno));
    exit(EXIT_FAILURE);
}

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [options] [file]\n"
        "Options:\n"
        "  -a, --affix PATH      affix file         (default: %s)\n"
        "  -d, --dictionary PATH dictionary file    (default: %s)\n"
        "  -c, --custom PATH     custom dictionary\n"
        "  -p, --pager CMD       pager command      (default: %s)\n"
        "  -P, --pager-args ARGS pager arguments    (default: %s)\n"
        "  --no-color            disable color output\n"
        "  -v, --verbose         enable debug logging\n"
        "  -h, --help            show this help\n",
        prog, DEFAULT_AFF, DEFAULT_DIC, DEFAULT_PAGER, DEFAULT_PAGER_ARGS);
}

static void pager_args_append(const char *arg) {
    g_config.pager_args = xrealloc(g_config.pager_args,
        (g_config.pager_argc + 1) * sizeof(char *));
    g_config.pager_args[g_config.pager_argc++] = xstrdup(arg);
}

static void pager_args_append_tokenized(const char *str) {
    char *copy = xstrdup(str);
    char *tok  = strtok(copy, " ");
    while (tok) { pager_args_append(tok); tok = strtok(NULL, " "); }
    free(copy);
}

static void parse_args(int argc, char **argv) {
    static struct option long_opts[] = {
        {"affix",       required_argument, 0, 'a'},
        {"dictionary",  required_argument, 0, 'd'},
        {"custom",      required_argument, 0, 'c'},
        {"pager",       required_argument, 0, 'p'},
        {"pager-args",  required_argument, 0, 'P'},
        {"no-color",    no_argument,       0, 'C'},
        {"verbose",     no_argument,       0, 'v'},
        {"help",        no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    g_config.aff_path    = xstrdup(DEFAULT_AFF);
    g_config.dic_path    = xstrdup(DEFAULT_DIC);
    g_config.pager       = xstrdup(DEFAULT_PAGER);
    g_config.pager_argc  = 0;
    g_config.pager_args  = NULL;
    g_config.color       = 1;
    g_config.custom_dict = NULL;
    g_config.log_level   = LOG_INFO;

    int opt;
    while ((opt = getopt_long(argc, argv, "a:d:c:p:P:Cvh", long_opts, NULL)) != -1) {
        switch (opt) {
            case 'a': free(g_config.aff_path);  g_config.aff_path  = xstrdup(optarg); break;
            case 'd': free(g_config.dic_path);  g_config.dic_path  = xstrdup(optarg); break;
            case 'c': g_config.custom_dict = xstrdup(optarg);                         break;
            case 'p': free(g_config.pager);     g_config.pager     = xstrdup(optarg); break;
            case 'P': pager_args_append_tokenized(optarg);                             break;
            case 'C': g_config.color      = 0;                                        break;
            case 'v': g_config.log_level  = LOG_DEBUG;                                break;
            case 'h': usage(argv[0]); exit(EXIT_SUCCESS);
            default:  usage(argv[0]); exit(EXIT_FAILURE);
        }
    }

    if (optind < argc) {
        if (g_config.pager_argc == 0)
            pager_args_append(g_config.pager);
        pager_args_append(argv[optind]);
    }

    if (g_config.pager_argc == 0) {
        pager_args_append(g_config.pager);
        pager_args_append_tokenized(DEFAULT_PAGER_ARGS);
    }
}

static int setup_pipe(int pipefd[2]) {
    if (pipe(pipefd) == -1) {
        log_msg(LOG_ERROR, "pipe: %s", strerror(errno));
        return -1;
    }
    return 0;
}

static pid_t spawn_pager(int pipefd[2], Hunhandle *hun) {
    pid_t pid = fork();
    if (pid == -1) {
        log_msg(LOG_ERROR, "fork: %s", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        close(pipefd[1]);
        if (dup2(pipefd[0], STDIN_FILENO) == -1) {
            log_msg(LOG_ERROR, "dup2: %s", strerror(errno));
            exit(EXIT_FAILURE);
        }
        close(pipefd[0]);
        Hunspell_destroy(hun);
        char **pager_argv = xmalloc((g_config.pager_argc + 1) * sizeof(char *));
        for (int i = 0; i < g_config.pager_argc; i++)
            pager_argv[i] = g_config.pager_args[i];
        pager_argv[g_config.pager_argc] = NULL;
        exec_pager(pager_argv);
        exit(EXIT_FAILURE);
    }
    close(pipefd[0]);
    if (dup2(pipefd[1], STDOUT_FILENO) == -1) {
        log_msg(LOG_ERROR, "dup2: %s", strerror(errno));
        close(pipefd[1]);
        return -1;
    }
    close(pipefd[1]);
    return pid;
}

static void init_state(state_t *st, Hunhandle *hun) {
    memset(st, 0, sizeof(*st));
    st->hun        = hun;
    st->state      = ST_NEWLINE;
    st->resetlen   = ANSI_BOLD_OFF_LEN;
    st->cache_mask = CACHE_SIZE - 1;
    memcpy(st->reset, ANSI_BOLD_OFF, ANSI_BOLD_OFF_LEN);
}

static void run_loop(state_t *st) {
    char    buf[BUFSIZE];
    ssize_t n;
    while (!g_terminate && (n = read(STDIN_FILENO, buf, sizeof(buf))) > 0)
        process_buffer(st, buf, (size_t)n);
    if (st->state == ST_WORD)
        check_and_emit(st);
    outbuf_flush(st);
}

static void cleanup_config(void) {
    for (int i = 0; i < g_config.pager_argc; i++)
        free(g_config.pager_args[i]);
    free(g_config.pager_args);
    free(g_config.aff_path);
    free(g_config.dic_path);
    free(g_config.pager);
    free(g_config.custom_dict);
}

int main(int argc, char **argv) {
    parse_args(argc, argv);
    setup_signals();

    if (isatty(STDIN_FILENO)) {
        char **new_argv = xmalloc((argc + 1) * sizeof(char *));
        new_argv[0] = xstrdup(g_config.pager);
        for (int i = 1; i < argc; i++)
            new_argv[i] = xstrdup(argv[i]);
        new_argv[argc] = NULL;
        exec_pager(new_argv);
        return EXIT_FAILURE;
    }

    Hunhandle *hun = Hunspell_create(g_config.aff_path, g_config.dic_path);
    if (!hun) {
        log_msg(LOG_ERROR, "Hunspell_create failed: %s / %s",
                g_config.aff_path, g_config.dic_path);
        return EXIT_FAILURE;
    }

    load_user_dictionaries(hun);

    int pipefd[2];
    if (setup_pipe(pipefd) == -1) {
        Hunspell_destroy(hun);
        return EXIT_FAILURE;
    }

    pid_t pid = spawn_pager(pipefd, hun);
    if (pid == -1) {
        Hunspell_destroy(hun);
        return EXIT_FAILURE;
    }

    state_t st;
    init_state(&st, hun);
    run_loop(&st);

    close(STDOUT_FILENO);
    int status;
    waitpid(pid, &status, 0);
    Hunspell_destroy(hun);
    cleanup_config();

    return WIFEXITED(status) ? WEXITSTATUS(status) : EXIT_FAILURE;
}
