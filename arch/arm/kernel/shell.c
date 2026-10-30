/*
 * shell.c -- Interactive shell for OsitoK AArch64
 */
#include "../include/hal.h"
#include "../include/types.h"
#include "task.h"
#include "http.h"
#include "../include/fs.h"
#include "../fs/ositofs3.h"
#include <stdbool.h>

extern void hda_play_tone(uint32_t freq_hz, uint32_t duration_ms);
extern bool hda_is_ready(void);
extern int proc_run(const char *path, int argc, char **argv);
extern void net_icmp_send_echo(const uint8_t dst_ip[4], uint16_t seq);
extern void net_poll(void);
extern void net_hold(void);
extern void net_release(void);
extern void cmd_model(int argc, char **argv);   /* llm.c */
extern void cmd_chat(int argc, char **argv);
extern void cmd_llm(int argc, char **argv);

#define MAX_ARGS 64                     /* wildcards can give many */

static bool arg_quoted[MAX_ARGS];       /* parse_args: the argument had quotes */

/* Split at spaces; '...' and "..." keep spaces, '|', '<' and '>' as text.
 * The quotes come out in place (the text only moves left). */
static int parse_args(char *line, char **argv)
{
    int argc = 0;
    char *w = line;
    while (*line && argc < MAX_ARGS) {
        while (*line == ' ') line++;
        if (!*line) break;
        argv[argc] = w;
        arg_quoted[argc] = false;
        char q = 0;
        while (*line && (q || *line != ' ')) {
            if (!q && (*line == '\'' || *line == '"')) {
                q = *line++;
                arg_quoted[argc] = true;
            } else if (q && *line == q) {
                q = 0;
                line++;
            } else {
                *w++ = *line++;
            }
        }
        if (*line) line++;
        *w++ = 0;
        argc++;
    }
    return argc;
}

static void cmd_help(void)
{
    serial_puts("Commands:\n");
    serial_puts("  help     Show this help\n");
    serial_puts("  ps       List tasks\n");
    serial_puts("  mem      Show heap stats\n");
    serial_puts("  uname    Show system info\n");
    serial_puts("  uptime   Show uptime\n");
    serial_puts("  ls       List a directory [path]\n");
    serial_puts("  cat      Show files <path>... (or its input)\n");
    serial_puts("  cd, pwd  Change / show the current directory (paths may be relative)\n");
    serial_puts("  mkdir    Make directories <path>...; rmdir removes empty ones\n");
    serial_puts("  rm       Delete files <path>...\n");
    serial_puts("  mv, cp   Rename or move, copy <from> <to> (into <to> if a directory)\n");
    serial_puts("  head     First lines [-n N] [path]; tail the last ones\n");
    serial_puts("  grep     Lines containing a text [-i] [-v] [-c] <text> [path]\n");
    serial_puts("  wc       Lines, words, bytes [path]\n");
    serial_puts("  random   Random bytes in hex [n], and their source\n");
    serial_puts("  date     The time of day, UTC, from NTP [sync: ask again]\n");
    serial_puts("  history  Lines typed before [clear]; Up/Down recall them, Tab completes\n");
    serial_puts("           Pipes: a | b; redirection: > file, >> file, < file, << WORD; quotes\n");
    serial_puts("           a ; b, a && b, a || b; wildcards * and ? (models/*.gguf)\n");
    serial_puts("  exec     Run a program from the filesystem <path> [args...]\n");
    serial_puts("  ping     Ping IP address\n");
    serial_puts("  cpus     List the online CPUs\n");
    serial_puts("  cpubench Timed task per CPU, all at once [any [n]: n unpinned]\n");
    serial_puts("           [mix: busy + light unpinned tasks, where they settle]\n");
    serial_puts("  cpufreq  CPU frequencies [max | min | <domain> <level>]\n");
    serial_puts("           [l3 | try <domain> <level> <s> [stay|busy]: watched run]\n");
    serial_puts("  temp     Temperature sensors, CPU thermal limit [limit <cap> <release>]\n");
    serial_puts("  burn     Keep CPUs busy in the background <cpu|all> <seconds>\n");
    serial_puts("  echo     Print text\n");
    serial_puts("  hexdump  Hex dump file\n");
    serial_puts("  df       Filesystem free space\n");
    serial_puts("  halt     Shutdown system\n");
    serial_puts("  resolve  DNS lookup\n");
    serial_puts("  net      USB networking (NCM): state [up | down | dns <ip>]\n");
    serial_puts("  https    GET <host> [path] over TLS 1.3 (else 1.2), certificate verified\n");
    serial_puts("  beep     Play a tone\n");
    serial_puts("  desktop  The desktop on the panel, phablet layout [off]\n");
    serial_puts("  model    Local LLM: the loaded one [<file.gguf>: load it]\n");
    serial_puts("  chat     Talk to the loaded model <text>\n");
    serial_puts("  llm      Compute path [simd on|off | smp on|off | selftest]\n");
    serial_puts("  reboot   Reboot system [bootloader: into fastboot]\n");
    serial_puts("  pmic     PMIC arbiter + restart reason [<sid> <hex addr> | list <sid>]\n");
    serial_puts("  rpmh     Apps RSC + AOP command DB [<resource>]\n");
    serial_puts("  keys     Power / volume keys: now and press counts\n");
    serial_puts("  screen   Screen mode [debug | normal]: diagnostics or full console\n");
    serial_puts("  ufs      UFS storage [luns | gpt <lun> | read <lun> <lba> [off] | mount |\n");
    serial_puts("           mkfs userdata | migrate userdata: v2 to v3, files kept]:\n");
    serial_puts("           writes go only to userdata (OsitoFS v3)\n");
    serial_puts("  put      Store a file sent over USB <path> (host: rog5-put.ps1)\n");
    serial_puts("  get      Send a file to the host over USB <path> (host: rog5-get.ps1)\n");
    serial_puts("  screenshot The panel as a BMP file [name, default screen.bmp]\n");
    serial_puts("  touch    Touchscreen: state, wiring, supplies [on | read | off]\n");
    serial_puts("  kload    Receive a kernel over USB and run it\n");
    serial_puts("  dmesg    Show the console log since boot\n");
    serial_puts("  clear    Clear screen\n");
}

static void cmd_ps(void)
{
    serial_puts("ID  State    Pri  CPU  Util  Ticks   Name\n");
    serial_puts("--  -------  ---  ---  ----  ------  ----\n");
    for (int i = 0; i < MAX_TASKS; i++) {
        if (task_pool[i].state == TASK_STATE_FREE) continue;

        /* ID */
        if (i < 10) serial_puts(" ");
        serial_putdec(i);
        serial_puts("  ");

        /* State */
        const char *st;
        switch (task_pool[i].state) {
        case TASK_STATE_READY:   st = "ready  "; break;
        case TASK_STATE_RUNNING: st = "RUN    "; break;
        case TASK_STATE_BLOCKED: st = "blocked"; break;
        case TASK_STATE_DEAD:    st = "dead   "; break;
        default:                 st = "???    "; break;
        }
        serial_puts(st);
        serial_puts("  ");

        /* Priority */
        if (task_pool[i].priority < 10) serial_puts(" ");
        serial_putdec(task_pool[i].priority);
        serial_puts("   ");

        /* CPU it runs or last ran on; '*' = not pinned */
        serial_putdec(task_pool[i].on_cpu);
        serial_puts(task_pool[i].cpu == TASK_ANY_CPU ? "*   " : "    ");

        /* Utilization the scheduler places it by; idle tasks have none */
        if (task_pool[i].idle) {
            serial_puts("   -  ");
        } else {
            uint32_t pct = task_pool[i].util * 100u / UTIL_MAX;
            serial_puts(pct < 10 ? "  " : pct < 100 ? " " : "");
            serial_putdec(pct);
            serial_puts("%  ");
        }

        /* Ticks */
        serial_putdec(task_pool[i].ticks_run);
        /* Pad ticks to 6 chars */
        uint32_t t = task_pool[i].ticks_run;
        int digits = 1;
        while (t >= 10) { t /= 10; digits++; }
        for (int p = digits; p < 6; p++) serial_putc(' ');
        serial_puts("  ");

        /* Name */
        serial_puts(task_pool[i].name ? task_pool[i].name : "?");
        serial_puts("\n");
    }
}

static void cmd_mem(void)
{
    serial_puts("RAM:  ");
    serial_putdec(mem_get_total() / (1024 * 1024));
    serial_puts(" MB total, ");
    serial_putdec(mem_get_free() / (1024 * 1024));
    serial_puts(" MB free, ");
    serial_putdec(mem_get_used() / (1024 * 1024));
    serial_puts(" MB used\n");
    serial_puts("Heap: ");
    serial_putdec(heap_get_used() / 1024);
    serial_puts(" KB used / ");
    serial_putdec(heap_get_total() / 1024);
    serial_puts(" KB total\n");
}

static void cmd_uname(void)
{
    /* The build stamp tells kernels apart: rog5-boot.sh --flash compares
     * it with the one in the boot.img it flashed */
    serial_puts("OsitoK AArch64 v0.1 (bare-metal), built " __DATE__ " " __TIME__ "\n");
}

static void cmd_uptime(void)
{
    uint64_t ticks = timer_get_tick_count();
    uint64_t secs = ticks / 100;
    uint64_t mins = secs / 60;
    serial_puts("Up ");
    if (mins > 0) {
        serial_putdec(mins);
        serial_puts("m ");
    }
    serial_putdec(secs % 60);
    serial_puts("s (");
    serial_putdec(ticks);
    serial_puts(" ticks)\n");
}

/* Desktop GUI task (gui_task.c) */
extern void gui_task(void *arg);

/* `desktop`: the GUI on the panel, on whichever CPU is fastest and free;
 * `desktop off`: back to the console */
static void cmd_desktop(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "off") == 0) {
        if (desktop_active)
            desktop_close();
        else
            serial_puts("desktop: not running\n");
        return;
    }
    if (desktop_active) {
        serial_puts("desktop: already running (desktop off)\n");
        return;
    }
    task_create_on("desktop", gui_task, (void *)0, 2, TASK_ANY_CPU);
}

/* Simple atoi for shell args */
static uint32_t shell_atoi(const char *s)
{
    uint32_t n = 0;
    while (*s >= '0' && *s <= '9')
        n = n * 10 + (*s++ - '0');
    return n;
}

static void cmd_beep(int argc, char **argv)
{
    if (!hda_is_ready()) {
        serial_puts("HDA not initialized\n");
        return;
    }
    uint32_t freq = 440;
    uint32_t ms   = 500;
    if (argc >= 2) freq = shell_atoi(argv[1]);
    if (argc >= 3) ms   = shell_atoi(argv[2]);
    if (freq == 0) freq = 440;
    if (ms == 0)   ms   = 500;
    hda_play_tone(freq, ms);
}

/* ── Files: OsitoFS v3, paths relative to the current directory ── */

/* Errors skip a capture, as stderr would: to the console, not into the
 * file or the next stage of a pipe */
/* To the console even inside a pipe or `>`, as stderr would be */
static void note_puts(const char *s1, const char *s2, const char *s3)
{
    void (*fn)(char) = console_capture(NULL);
    serial_puts(s1);
    serial_puts(s2);
    serial_puts(s3);
    console_capture(fn);
}

/* For && and ||: a command failed if it reported an error here, its
 * program exited non-zero, or there's no such command. (Messages other
 * files print with serial_puts don't count.) */
static int cmd_failed;

static void err_puts(const char *s1, const char *s2, const char *s3)
{
    cmd_failed = 1;
    note_puts(s1, s2, s3);
}

static int need_fs(const char *cmd)
{
    if (osfs3_is_mounted())
        return 1;
    err_puts(cmd, ": no filesystem mounted (ufs mount)\n", "");
    return 0;
}

static void say_err(const char *cmd, const char *path, const char *what)
{
    err_puts(cmd, ": ", path);
    err_puts(": ", what, "\n");
}

static void put_right(uint64_t v, int width)
{
    char buf[21];
    int n = 0;
    do {
        buf[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    for (int i = n; i < width; i++)
        serial_putc(' ');
    while (n)
        serial_putc(buf[--n]);
}

/* A command's output, captured for `>` or the next stage of a pipe */
struct capture {
    char    *buf;
    uint32_t len, max;
    int      overflow;
};
static struct capture *cap;             /* the one console_capture fills */

static void capture_char(char c)
{
    if (cap->len < cap->max)
        cap->buf[cap->len++] = c;
    else
        cap->overflow = 1;
}

/* A command's input, from the previous stage or `<` */
struct input {
    const char *data;
    uint64_t    len, pos;
    int         set;
};
static struct input stdin_in;

int shell_stdin_read(char *buf, int n)
{
    if (!stdin_in.set)
        return -1;
    uint64_t left = stdin_in.len - stdin_in.pos;
    if ((uint64_t)n > left)
        n = (int)left;
    memcpy(buf, stdin_in.data + stdin_in.pos, (size_t)n);
    stdin_in.pos += (uint64_t)n;
    return n;
}

/* Bytes out: straight into a capture, else to the console (where a NUL
 * would end a string, and means nothing anyway) */
static void put_bytes(const char *p, uint64_t n)
{
    if (cap) {
        for (uint64_t i = 0; i < n; i++)
            capture_char(p[i]);
        return;
    }
    char chunk[257];
    uint32_t k = 0;
    for (uint64_t i = 0; i < n; i++) {
        if (p[i])
            chunk[k++] = p[i];
        if (k == sizeof(chunk) - 1 || i + 1 == n) {
            chunk[k] = 0;
            serial_puts(chunk);
            k = 0;
        }
    }
}

/* A filter's text: the file named, or else the command's input. *pages is
 * what to free (0: nothing). 0, or -1 with the reason printed. */
static int text_of(const char *cmd, const char *path, const char **data,
                   uint64_t *len, uint64_t *pages)
{
    *pages = 0;
    if (!path) {
        if (!stdin_in.set) {
            err_puts(cmd, ": no file and no input (a pipe or <)\n", "");
            return -1;
        }
        *data = stdin_in.data + stdin_in.pos;
        *len = stdin_in.len - stdin_in.pos;
        return 0;
    }
    if (!need_fs(cmd))
        return -1;
    void *f = fs_find(path);
    if (!f) {
        say_err(cmd, path, "no such file");
        return -1;
    }
    *len = osfs3_file_size(f);
    uint64_t n = ((*len ? *len : 1) + 4095) / 4096;    /* an empty file too */
    char *buf = mem_alloc_aligned_high(n * 4096, 4096);
    if (!buf || osfs3_read_file(f, 0, buf, *len) < 0) {
        say_err(cmd, path, buf ? "read error" : "out of memory");
        if (buf)
            mem_free_pages(buf, n);
        return -1;
    }
    *data = buf;
    *pages = n;
    return 0;
}

/* `date [sync]`: the time of day (UTC), from NTP */
static void cmd_date(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "sync") == 0) {
        net_hold();
        ntp_sync();
        net_release();
    }
    uint32_t t = ntp_get_utc();
    if (!t) {
        err_puts("date: unknown (no NTP yet: `net up`, or `date sync`)\n", "", "");
        return;
    }
    ntp_print(t);
    serial_puts(" UTC\n");
}

/* `random [n]`: n random bytes in hex (32), and where they come from */
static void cmd_random(int argc, char **argv)
{
    uint32_t n = argc > 1 ? shell_atoi(argv[1]) : 32;
    if (n == 0 || n > 4096)
        n = 32;
    static uint8_t buf[4096];
    random_bytes(buf, n);
    static const char hex[] = "0123456789abcdef";
    for (uint32_t i = 0; i < n; i++) {
        char two[3] = { hex[buf[i] >> 4], hex[buf[i] & 15], 0 };
        serial_puts(two);
        if (i % 32 == 31 || i + 1 == n)
            serial_puts("\n");
    }
    note_puts(random_hw() ? "(hardware PRNG + timer jitter, HMAC-DRBG)\n"
                          : "(timer jitter only, HMAC-DRBG: no hardware generator)\n", "", "");
}

/* `ls [path]`: a directory's entries, sizes first, directories marked */
static void cmd_ls(int argc, char **argv)
{
    if (!need_fs("ls"))
        return;
    char abs[FS_PATH_MAX];
    const char *path = argc > 1 ? argv[1] : ".";
    if (fs_path(path, abs) < 0) {
        say_err("ls", path, "path too long");
        return;
    }
    void *f = osfs3_find(abs);
    if (f) {
        put_right(osfs3_file_size(f), 10);
        serial_puts("  ");
        serial_puts(path);
        serial_puts("\n");
        return;
    }
    osfs3_dir_cursor_t cur;
    if (osfs3_dir_cursor_open_ci(abs, &cur) < 0) {
        say_err("ls", path, "no such file or directory");
        return;
    }
    static char name[OSFS3_NAME_MAX + 1];
    bool dir;
    uint64_t size;
    uint32_t files = 0, dirs = 0;
    uint64_t bytes = 0;
    while (osfs3_dir_cursor_next(&cur, name, sizeof(name), &dir, &size, NULL) == 1) {
        if (dir) {
            serial_puts("     <dir>  ");
            dirs++;
        } else {
            put_right(size, 10);
            serial_puts("  ");
            files++;
            bytes += size;
        }
        serial_puts(name);
        serial_puts(dir ? "/\n" : "\n");
    }
    serial_putdec(files);
    serial_puts(" files, ");
    serial_putdec(dirs);
    serial_puts(" directories, ");
    serial_putdec(bytes);
    serial_puts(" bytes\n");
}

/* `cat [file...]`: the files one after another, 4 KB at a time, or the
 * command's input. On the console a missing last newline is added. */
static void cmd_cat(int argc, char **argv)
{
    char last = '\n';
    if (argc < 2) {
        if (!stdin_in.set) {
            serial_puts("Usage: cat <file>...  (or a pipe or < into it)\n");
            return;
        }
        uint64_t n = stdin_in.len - stdin_in.pos;
        put_bytes(stdin_in.data + stdin_in.pos, n);
        stdin_in.pos = stdin_in.len;
        if (n)
            last = stdin_in.data[stdin_in.len - 1];
    } else if (!need_fs("cat")) {
        return;
    }
    /* 1 MB at a time: a read smaller than v3's 64 KB block reads the whole
     * block anyway, and 4 KB pieces took 28 s for 113 MB */
    static char *cat_buf;
    const uint64_t chunk = 1U << 20;
    if (argc > 1 && !cat_buf && !(cat_buf = mem_alloc_aligned_high(chunk, 4096))) {
        err_puts("cat: out of memory\n", "", "");
        return;
    }
    for (int i = 1; i < argc; i++) {
        void *f = fs_find(argv[i]);
        if (!f) {
            say_err("cat", argv[i], "no such file");
            continue;
        }
        uint64_t size = osfs3_file_size(f);
        for (uint64_t off = 0; off < size; off += chunk) {
            uint64_t n = size - off < chunk ? size - off : chunk;
            if (osfs3_read_file(f, off, cat_buf, n) < 0) {
                say_err("cat", argv[i], "read error");
                break;
            }
            put_bytes(cat_buf, n);
            last = cat_buf[n - 1];
        }
    }
    if (!cap && last != '\n')
        serial_puts("\n");
}

/* The lines of a text: each one's start and length, without its '\n' */
static uint64_t next_line(const char *d, uint64_t len, uint64_t pos, uint64_t *line_len)
{
    uint64_t e = pos;
    while (e < len && d[e] != '\n')
        e++;
    *line_len = e - pos;
    return e < len ? e + 1 : e;
}

static int parse_n(int *argc, char ***argv, int def)
{
    if (*argc >= 3 && strcmp((*argv)[1], "-n") == 0) {
        int n = (int)shell_atoi((*argv)[2]);
        *argc -= 2;
        *argv += 2;
        return n;
    }
    return def;
}

/* `head [-n N] [file]`, `tail [-n N] [file]`: the first or last N lines (10) */
static void cmd_head_tail(int argc, char **argv)
{
    int tail = argv[0][0] == 't';
    const char *cmd = argv[0];
    int want = parse_n(&argc, &argv, 10);
    const char *d;
    uint64_t len, pages;
    if (text_of(cmd, argc > 1 ? argv[1] : NULL, &d, &len, &pages) < 0)
        return;
    uint64_t start = 0;
    if (tail) {
        /* Back from the end, past `want` newlines (a last one ends a line) */
        uint64_t p = len;
        int lines = 0;
        if (p && d[p - 1] == '\n')
            p--;
        while (p > 0) {
            if (d[p - 1] == '\n' && ++lines == want)
                break;
            p--;
        }
        start = want > 0 ? p : len;
        put_bytes(d + start, len - start);
    } else {
        uint64_t p = 0, l;
        for (int i = 0; i < want && p < len; i++)
            p = next_line(d, len, p, &l);
        put_bytes(d, p);
    }
    if (pages)
        mem_free_pages((void *)d, pages);
}

static char fold(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

/* `grep [-i] [-v] [-c] <text> [file]`: the lines that contain the text (-v:
 * that don't; -i: in any case; -c: how many) */
static void cmd_grep(int argc, char **argv)
{
    int icase = 0, invert = 0, count = 0, a = 1;
    for (; a < argc && argv[a][0] == '-' && argv[a][1]; a++) {
        for (const char *o = argv[a] + 1; *o; o++) {
            if (*o == 'i') icase = 1;
            else if (*o == 'v') invert = 1;
            else if (*o == 'c') count = 1;
        }
    }
    if (a >= argc) {
        serial_puts("Usage: grep [-i] [-v] [-c] <text> [file]\n");
        return;
    }
    const char *pat = argv[a];
    uint64_t plen = strlen(pat);
    const char *d;
    uint64_t len, pages;
    if (text_of("grep", a + 1 < argc ? argv[a + 1] : NULL, &d, &len, &pages) < 0)
        return;
    uint64_t matches = 0, pos = 0, l;
    while (pos < len) {
        uint64_t start = pos;
        pos = next_line(d, len, pos, &l);
        int hit = plen == 0;
        for (uint64_t i = 0; !hit && i + plen <= l; i++) {
            uint64_t k = 0;
            while (k < plen && (icase ? fold(d[start + i + k]) == fold(pat[k])
                                      : d[start + i + k] == pat[k]))
                k++;
            hit = k == plen;
        }
        if (hit != invert) {
            matches++;
            if (!count) {
                put_bytes(d + start, l);
                put_bytes("\n", 1);
            }
        }
    }
    if (count) {
        serial_putdec(matches);
        serial_puts("\n");
    }
    if (pages)
        mem_free_pages((void *)d, pages);
}

/* `wc [file]`: lines, words and bytes */
static void cmd_wc(int argc, char **argv)
{
    const char *d;
    uint64_t len, pages;
    if (text_of("wc", argc > 1 ? argv[1] : NULL, &d, &len, &pages) < 0)
        return;
    uint64_t lines = 0, words = 0;
    int in_word = 0;
    for (uint64_t i = 0; i < len; i++) {
        char c = d[i];
        if (c == '\n')
            lines++;
        int space = c == ' ' || c == '\n' || c == '\t' || c == '\r';
        if (!space && !in_word)
            words++;
        in_word = !space;
    }
    serial_putdec(lines);
    serial_puts(" ");
    serial_putdec(words);
    serial_puts(" ");
    serial_putdec(len);
    if (argc > 1) {
        serial_puts(" ");
        serial_puts(argv[1]);
    }
    serial_puts("\n");
    if (pages)
        mem_free_pages((void *)d, pages);
}

static void cmd_pwd(void)
{
    serial_puts(fs_cwd());
    serial_puts("\n");
}

static void cmd_cd(int argc, char **argv)
{
    if (!need_fs("cd"))
        return;
    const char *path = argc > 1 ? argv[1] : "/";
    if (fs_chdir(path) < 0)
        say_err("cd", path, "not a directory");
}

/* mkdir, rmdir and rm: one path each, the v3 call's answer */
static void cmd_path_op(int argc, char **argv)
{
    if (argc < 2) {
        serial_puts("Usage: ");
        serial_puts(argv[0]);
        serial_puts(" <path>...\n");
        return;
    }
    if (!need_fs(argv[0]))
        return;
    for (int i = 1; i < argc; i++) {
        char abs[FS_PATH_MAX];
        if (fs_path(argv[i], abs) < 0) {
            say_err(argv[0], argv[i], "path too long");
            continue;
        }
        int rc;
        const char *why;
        if (strcmp(argv[0], "mkdir") == 0) {
            rc = osfs3_mkdir(abs);
            why = "exists, or its parent doesn't";
        } else if (strcmp(argv[0], "rmdir") == 0) {
            rc = osfs3_rmdir(abs);
            why = "not an empty directory";
        } else {
            rc = osfs3_delete(abs);
            why = rc == -2 ? "in use" : "no such file";
        }
        if (rc < 0)
            say_err(argv[0], argv[i], why);
    }
}

/* mv and cp: both paths made absolute, and <to> a directory meaning the
 * same name inside it. 0, or -1 with the reason printed. */
static int from_to(const char *cmd, char **argv, char from[FS_PATH_MAX], char to[FS_PATH_MAX])
{
    if (fs_path(argv[1], from) < 0 || fs_path(argv[2], to) < 0) {
        err_puts(cmd, ": path too long\n", "");
        return -1;
    }
    uint32_t ino = osfs3_resolve_path(to);
    if (ino && osfs3_is_dir(ino)) {
        const char *base = from;
        for (const char *p = from; *p; p++)
            if (*p == '/')
                base = p + 1;
        uint32_t n = (uint32_t)strlen(to);
        if (n + 1 + strlen(base) >= FS_PATH_MAX) {
            err_puts(cmd, ": path too long\n", "");
            return -1;
        }
        if (n > 1)
            to[n++] = '/';
        strcpy(to + n, base);
    }
    return 0;
}

/* `mv <from> <to>`: rename, or move into <to> when it's a directory */
static void cmd_mv(int argc, char **argv)
{
    if (argc != 3) {
        serial_puts("Usage: mv <from> <to>\n");
        return;
    }
    char from[FS_PATH_MAX], to[FS_PATH_MAX];
    if (!need_fs("mv") || from_to("mv", argv, from, to) < 0)
        return;
    if (osfs3_rename(from, to, false) < 0)
        say_err("mv", argv[1], "can't move it there (missing, or the target exists)");
}

/* `cp <from> <to>`: a copy, replacing <to>, or into <to> when it's a
 * directory */
static void cmd_cp(int argc, char **argv)
{
    if (argc != 3) {
        serial_puts("Usage: cp <from> <to>\n");
        return;
    }
    char from[FS_PATH_MAX], to[FS_PATH_MAX];
    if (!need_fs("cp") || from_to("cp", argv, from, to) < 0)
        return;
    if (strcmp(from, to) == 0) {
        say_err("cp", argv[1], "is the target itself");
        return;
    }
    const char *d;
    uint64_t len, pages;
    if (text_of("cp", from, &d, &len, &pages) < 0)
        return;
    if (fs_write_file(to, d, len) < 0)
        say_err("cp", argv[2], "can't write it");
    mem_free_pages((void *)d, pages);
}

static void cmd_echo(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (i > 1) serial_putc(' ');
        serial_puts(argv[i]);
    }
    serial_puts("\n");
}

static void cmd_hexdump(int argc, char **argv)
{
    if (argc < 2) { serial_puts("Usage: hexdump <file> [offset] [len]\n"); return; }
    if (!need_fs("hexdump")) return;
    void *f = fs_find(argv[1]);
    if (!f) { serial_puts("File not found\n"); return; }
    uint64_t off = (argc >= 3) ? shell_atoi(argv[2]) : 0;
    uint64_t len = (argc >= 4) ? shell_atoi(argv[3]) : 256;
    uint64_t size = osfs3_file_size(f);
    if (len > 512) len = 512;
    static uint8_t hbuf[512];
    if (off > size) off = size;
    if (off + len > size) len = size - off;
    if (osfs3_read_file(f, off, hbuf, len) < 0) { serial_puts("Read error\n"); return; }
    for (uint64_t i = 0; i < len; i += 16) {
        serial_puthex(off + i, 4);
        serial_puts(": ");
        for (int j = 0; j < 16 && i + j < len; j++) {
            serial_puthex(hbuf[i + j], 2);
            serial_putc(' ');
        }
        serial_puts(" ");
        for (int j = 0; j < 16 && i + j < len; j++) {
            char c = (char)hbuf[i + j];
            serial_putc((c >= 32 && c < 127) ? c : '.');
        }
        serial_puts("\n");
    }
}

static void cmd_df(void)
{
    if (!need_fs("df"))
        return;
    uint64_t bs = osfs3_block_size(), total = osfs3_total_blocks(), free = osfs3_free_blocks();
    serial_puts("OsitoFS v3 [");
    serial_puts(osfs3_label());
    serial_puts("]: ");
    serial_putdec(osfs3_file_count());
    serial_puts(" files, ");
    serial_putdec((total - free) * bs >> 20);
    serial_puts(" MB used, ");
    serial_putdec(free * bs >> 20);
    serial_puts(" MB free of ");
    serial_putdec(total * bs >> 20);
    serial_puts(" MB (");
    serial_putdec(bs >> 10);
    serial_puts(" KB blocks)\n");
}

static void cmd_halt(void)
{
    serial_puts("System halting...\n");
    /* PSCI SYSTEM_OFF via HVC */
    register uint64_t x0 __asm__("x0") = 0x84000008ULL;
    __asm__ volatile("hvc #0" : "+r"(x0) : : "x1", "x2", "x3");
    for (;;) __asm__ volatile("wfe");
}

/* `https <host> [path]`: GET over TLS 1.3, else 1.2 (http.c, tls13.c,
 * tls.c), the server's certificate verified (tls_verify.c); the status and
 * the first 600 bytes of the body */
static void cmd_https(int argc, char **argv)
{
    if (argc < 2) {
        serial_puts("Usage: https <host> [path]\n");
        return;
    }
    static http_session_t s;
    static http_response_t resp;
    static char body[601];
    const char *path = argc > 2 ? argv[2] : "/";
    static const char *const headers[] = { "User-Agent: Osito-K", "Connection: close", 0 };
    net_hold();
    if (http_open(&s, argv[1]) < 0) {
        net_release();
        serial_puts("https: no connection (DNS, TCP or the TLS handshake)\n");
        return;
    }
    int n = -1;
    if (http_request(&s, "GET", path, argv[1], headers, 0, 0, &resp) == 0)
        n = http_read_body_full(&s, &resp, body, sizeof(body) - 1);
    http_close(&s);
    net_release();
    if (n < 0) {
        serial_puts("https: request failed\n");
        return;
    }
    body[n] = 0;
    serial_puts("HTTP ");
    serial_putdec((uint64_t)resp.status_code);
    serial_puts(", first ");
    serial_putdec((uint64_t)n);
    serial_puts(" bytes:\n");
    serial_puts(body);
    serial_puts("\n");
}

/* `screenshot [name]`: the panel as a 24-bit BMP in OsitoFS, screen.bmp by
 * default (rog5-screenshot.ps1 fetches it as a PNG). The panel's 32 bpp
 * pixels are 0x00RRGGBB, so B, G, R in memory, BMP's own order. */
static void cmd_screenshot(int argc, char **argv)
{
    uint64_t base;
    uint32_t w, h, w_mm, h_mm;
    if (platform_panel(&base, &w, &h, &w_mm, &h_mm) < 0) {
        serial_puts("screenshot: no panel\n");
        return;
    }
    if (!need_fs("screenshot"))
        return;
    const char *name = argc > 1 ? argv[1] : "screen.bmp";
    uint32_t row = (w * 3 + 3) & ~3U;
    uint32_t size = 54 + row * h;
    uint8_t *bmp = mem_alloc_aligned_high(size, 4096);
    if (!bmp) {
        serial_puts("screenshot: out of memory\n");
        return;
    }
    memset(bmp, 0, 54);
    uint32_t fields[][2] = {
        { 2, size }, { 10, 54 },                /* file size, pixel data offset */
        { 14, 40 }, { 18, w }, { 22, h },       /* BITMAPINFOHEADER, bottom-up */
        { 34, row * h },
        { 38, 2835 }, { 42, 2835 },             /* 72 dpi */
    };
    bmp[0] = 'B';
    bmp[1] = 'M';
    for (uint32_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
        for (int b = 0; b < 4; b++)
            bmp[fields[i][0] + b] = (uint8_t)(fields[i][1] >> (8 * b));
    bmp[26] = 1;                                /* planes */
    bmp[28] = 24;                               /* bits per pixel */
    const volatile uint32_t *fb = (const volatile uint32_t *)base;
    for (uint32_t y = 0; y < h; y++) {
        const volatile uint32_t *src = fb + (uint64_t)(h - 1 - y) * w;
        uint8_t *dst = bmp + 54 + (uint64_t)y * row;
        for (uint32_t x = 0; x < w; x++) {
            uint32_t px = src[x];
            dst[3 * x] = (uint8_t)px;
            dst[3 * x + 1] = (uint8_t)(px >> 8);
            dst[3 * x + 2] = (uint8_t)(px >> 16);
        }
    }
    int ok = fs_write_file(name, bmp, size) == 0;
    mem_free_pages(bmp, (size + 4095) / 4096);
    serial_puts("screenshot: ");
    serial_puts(name);
    serial_puts(ok ? ", " : ": storing it failed\n");
    if (!ok)
        return;
    serial_putdec(w);
    serial_puts("x");
    serial_putdec(h);
    serial_puts(", ");
    serial_putdec(size);
    serial_puts(" bytes\n");
}

static void cmd_resolve(int argc, char **argv)
{
    if (argc < 2) { serial_puts("Usage: resolve <hostname>\n"); return; }
    extern int net_dns_resolve(const char *hostname, uint8_t ip_out[4]);
    uint8_t ip[4];
    net_hold();
    int err = net_dns_resolve(argv[1], ip);
    net_release();
    if (err == 0) {
        serial_puts(argv[1]);
        serial_puts(" -> ");
        serial_putdec(ip[0]); serial_putc('.');
        serial_putdec(ip[1]); serial_putc('.');
        serial_putdec(ip[2]); serial_putc('.');
        serial_putdec(ip[3]); serial_puts("\n");
    } else {
        serial_puts("DNS resolution failed\n");
    }
}

/* One command, its redirections already taken out */
static void dispatch(int argc, char **argv)
{
    if      (strcmp(argv[0], "help") == 0)    cmd_help();
    else if (strcmp(argv[0], "ps") == 0)      cmd_ps();
    else if (strcmp(argv[0], "mem") == 0)     cmd_mem();
    else if (strcmp(argv[0], "uname") == 0)   cmd_uname();
    else if (strcmp(argv[0], "uptime") == 0)  cmd_uptime();
    else if (strcmp(argv[0], "kload") == 0)   platform_kload();
    else if (strcmp(argv[0], "dmesg") == 0)   klog_dump();
    else if (strcmp(argv[0], "ls") == 0)      cmd_ls(argc, argv);
    else if (strcmp(argv[0], "cat") == 0)    cmd_cat(argc, argv);
    else if (strcmp(argv[0], "pwd") == 0)    cmd_pwd();
    else if (strcmp(argv[0], "cd") == 0)     cmd_cd(argc, argv);
    else if (strcmp(argv[0], "mkdir") == 0 || strcmp(argv[0], "rmdir") == 0 ||
             strcmp(argv[0], "rm") == 0)     cmd_path_op(argc, argv);
    else if (strcmp(argv[0], "mv") == 0)     cmd_mv(argc, argv);
    else if (strcmp(argv[0], "cp") == 0)     cmd_cp(argc, argv);
    else if (strcmp(argv[0], "head") == 0 || strcmp(argv[0], "tail") == 0)
        cmd_head_tail(argc, argv);
    else if (strcmp(argv[0], "grep") == 0)   cmd_grep(argc, argv);
    else if (strcmp(argv[0], "wc") == 0)     cmd_wc(argc, argv);
    else if (strcmp(argv[0], "random") == 0) cmd_random(argc, argv);
    else if (strcmp(argv[0], "date") == 0)   cmd_date(argc, argv);
    else if (strcmp(argv[0], "history") == 0) {
        if (argc > 1 && strcmp(argv[1], "clear") == 0)
            term_history_clear();
        else
            term_history_print();
    }
    else if (strcmp(argv[0], "exec") == 0) {
        if (argc < 2) serial_puts("Usage: exec <filename> [args...]\n");
        else if (proc_run(argv[1], argc - 1, argv + 1) != 0) cmd_failed = 1;
    }
    else if (strcmp(argv[0], "ping") == 0) {
        if (argc < 2) { serial_puts("Usage: ping <ip>\n"); }
        else {
            uint8_t ip[4] = {0};
            /* Simple IP parser: a.b.c.d */
            const char *p = argv[1];
            for (int i = 0; i < 4 && *p; i++) {
                uint32_t v = 0;
                while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
                ip[i] = (uint8_t)v;
                if (*p == '.') p++;
            }
            serial_puts("PING ");
            serial_puts(argv[1]);
            serial_puts("\n");
            /* Four echoes, a second each. The first may go to ARP:
             * net.c drops a packet whose next hop isn't resolved yet. */
            extern uint32_t net_icmp_get_rx_count(void);
            net_hold();
            for (uint16_t seq = 1; seq <= 4; seq++) {
                uint32_t before = net_icmp_get_rx_count();
                uint64_t t0 = timer_ms(), t = 0;
                net_icmp_send_echo(ip, seq);
                while (net_icmp_get_rx_count() == before && (t = timer_ms() - t0) < 1000) {
                    net_poll();
                    task_yield();
                }
                serial_puts(net_icmp_get_rx_count() != before ? "  reply, " : "  timeout\n");
                if (net_icmp_get_rx_count() != before) {
                    serial_putdec(t);
                    serial_puts(" ms\n");
                }
            }
            net_release();
        }
    }
    else if (strcmp(argv[0], "cpus") == 0) {
        smp_print_cpus();
    }
    else if (strcmp(argv[0], "cpubench") == 0 && argc > 1 &&
             strcmp(argv[1], "mix") == 0) {
        smp_bench_mix();
    }
    else if (strcmp(argv[0], "cpubench") == 0) {
        int any = argc > 1 && strcmp(argv[1], "any") == 0;
        int count = 0;
        for (const char *p = argc > 2 ? argv[2] : ""; *p >= '0' && *p <= '9'; p++)
            count = count * 10 + (*p - '0');
        smp_bench(any, count);
    }
    else if (strcmp(argv[0], "cpufreq") == 0) {
        platform_cpufreq_cmd(argc, argv);
    }
    else if (strcmp(argv[0], "temp") == 0) {
        platform_temp_cmd(argc, argv);
    }
    else if (strcmp(argv[0], "burn") == 0) {
        int cpu = -1, secs = 0;
        if (argc == 3 && strcmp(argv[1], "all") != 0) {
            cpu = 0;
            for (const char *p = argv[1]; *p >= '0' && *p <= '9'; p++)
                cpu = cpu * 10 + (*p - '0');
        }
        for (const char *p = argc == 3 ? argv[2] : ""; *p >= '0' && *p <= '9'; p++)
            secs = secs * 10 + (*p - '0');
        if (argc != 3 || secs <= 0 || cpu >= MAX_CPUS)
            serial_puts("usage: burn <cpu|all> <seconds>\n");
        else
            smp_burn(cpu, secs);
    }
    else if (strcmp(argv[0], "svctest") == 0) {
        /* Inline SVC test — no ELF loading */
        serial_puts("[TEST] Calling SVC write...\n");
        const char *msg = "SVC write works!\n";
        register uint64_t x0 __asm__("x0") = 1;        /* fd */
        register uint64_t x1 __asm__("x1") = (uint64_t)msg;
        register uint64_t x2 __asm__("x2") = 17;       /* len */
        register uint64_t x8 __asm__("x8") = 64;       /* SYS_write */
        __asm__ volatile("svc #0" : "+r"(x0)
                         : "r"(x1), "r"(x2), "r"(x8)
                         : "memory");
        serial_puts("[TEST] SVC returned, x0=");
        serial_putdec(x0);
        serial_puts("\n");
    }
    else if (strcmp(argv[0], "echo") == 0)    cmd_echo(argc, argv);
    else if (strcmp(argv[0], "hexdump") == 0) cmd_hexdump(argc, argv);
    else if (strcmp(argv[0], "df") == 0)      cmd_df();
    else if (strcmp(argv[0], "halt") == 0)    cmd_halt();
    else if (strcmp(argv[0], "resolve") == 0) cmd_resolve(argc, argv);
    else if (strcmp(argv[0], "net") == 0)     platform_net_cmd(argc, argv);
    else if (strcmp(argv[0], "https") == 0)   cmd_https(argc, argv);
    else if (strcmp(argv[0], "beep") == 0)    cmd_beep(argc, argv);
    else if (strcmp(argv[0], "desktop") == 0) cmd_desktop(argc, argv);
    else if (strcmp(argv[0], "model") == 0)   cmd_model(argc, argv);
    else if (strcmp(argv[0], "chat") == 0)    cmd_chat(argc, argv);
    else if (strcmp(argv[0], "llm") == 0)     cmd_llm(argc, argv);
    else if (strcmp(argv[0], "reboot") == 0) {
        if (argc > 1 && strcmp(argv[1], "bootloader") == 0)
            platform_reboot_bootloader();
        else
            platform_reboot();
    }
    else if (strcmp(argv[0], "pmic") == 0)    platform_pmic_cmd(argc, argv);
    else if (strcmp(argv[0], "rpmh") == 0)    platform_rpmh_cmd(argc, argv);
    else if (strcmp(argv[0], "keys") == 0)    platform_keys_cmd(argc, argv);
    else if (strcmp(argv[0], "screen") == 0)  platform_screen_cmd(argc, argv);
    else if (strcmp(argv[0], "ufs") == 0)     platform_ufs_cmd(argc, argv);
    else if (strcmp(argv[0], "put") == 0)     platform_put_cmd(argc, argv);
    else if (strcmp(argv[0], "get") == 0)     platform_get_cmd(argc, argv);
    else if (strcmp(argv[0], "screenshot") == 0) cmd_screenshot(argc, argv);
    else if (strcmp(argv[0], "touch") == 0)   platform_touch_cmd(argc, argv);
    else if (strcmp(argv[0], "clear") == 0)   serial_puts("\033[2J\033[H");
    else {
        err_puts("Unknown: ", argv[0], "\n");
    }
}

/* ── Pipes and redirection ──────────────────────────────────────
 *
 * As x86's shell: `a | b | c` runs the stages in turn, each one's output
 * captured (console_capture: only this task's, so other tasks' logs stay on
 * the console) and handed to the next as its input; `> file` and `>> file`
 * capture a command's output into a file, `< file` makes a file its input,
 * `<< WORD` the lines typed after it (read_heredoc). cat, head, tail, grep,
 * wc and the apps' read(0) take that input. Pipelines chain with ;, && and
 * || (run_list), and words with * or ? expand to file names (glob_expand).
 */

#define PIPE_STAGES 8
/* A command's output goes to high memory: 256 MB (the phone's models fit),
 * or 16 MB where there's less (virt) */
static int capture_open(struct capture *c)
{
    static const uint32_t sizes[] = { 256U << 20, 16U << 20 };
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        c->buf = mem_alloc_aligned_high(sizes[i], 4096);
        if (c->buf) {
            c->max = sizes[i];
            return 0;
        }
    }
    err_puts("shell: out of memory\n", "", "");
    return -1;
}

static void capture_close(struct capture *c)
{
    if (c->buf)
        mem_free_pages(c->buf, c->max / 4096);
    c->buf = NULL;
}

static void capture_too_big(const char *what, const struct capture *c)
{
    void (*fn)(char) = console_capture(NULL);
    serial_puts("shell: ");
    serial_puts(what);
    serial_puts(" went over ");
    serial_putdec(c->max >> 20);
    serial_puts(" MB, cut short\n");
    console_capture(fn);
}

/* ── Wildcards ──────────────────────────────────────────────────
 *
 * An unquoted word with * or ? in its last component becomes the names
 * that match in that directory, sorted, the directory part kept as typed;
 * with no match it stays as it is (as in sh). Case doesn't matter, as in
 * OsitoFS lookups, and * doesn't match a leading dot unless typed.
 */

static char glob_pool[8192];
static uint32_t glob_used;

static bool has_wild(const char *s)
{
    for (; *s; s++)
        if (*s == '*' || *s == '?')
            return true;
    return false;
}

static bool glob_match(const char *p, const char *s)
{
    const char *star = NULL, *back = NULL;
    while (*s) {
        if (*p == '?' || (*p != '*' && fold(*p) == fold(*s))) {
            p++;
            s++;
        } else if (*p == '*') {
            star = p++;
            back = s;
        } else if (star) {
            p = star + 1;
            s = ++back;
        } else {
            return false;
        }
    }
    while (*p == '*')
        p++;
    return !*p;
}

/* word's matches into argv from n, sorted; the new count */
static int glob_expand(char *word, char **argv, int n)
{
    int slash = -1;
    for (int i = 0; word[i]; i++)
        if (word[i] == '/')
            slash = i;
    const char *pat = word + slash + 1;
    char dir[FS_PATH_MAX], abs[FS_PATH_MAX];
    osfs3_dir_cursor_t cur;
    if (!osfs3_is_mounted() || slash >= FS_PATH_MAX - 1 || !has_wild(pat))
        goto literal;
    if (slash < 0) {
        strcpy(dir, ".");
    } else {
        memcpy(dir, word, slash ? (size_t)slash : 1);   /* "/x": the root */
        dir[slash ? slash : 1] = 0;
    }
    for (int i = 0; dir[i]; i++)
        if (dir[i] == '*' || dir[i] == '?')
            goto literal;                               /* the last part only */
    if (fs_path(dir, abs) < 0 || osfs3_dir_cursor_open_ci(abs, &cur) < 0)
        goto literal;

    static char name[OSFS3_NAME_MAX + 1];
    bool is_dir;
    int first = n;
    while (osfs3_dir_cursor_next(&cur, name, sizeof(name), &is_dir, NULL, NULL) == 1) {
        if ((name[0] == '.' && pat[0] != '.') || !glob_match(pat, name))
            continue;
        uint32_t len = (uint32_t)(slash + 1) + (uint32_t)strlen(name) + 1;
        if (n == MAX_ARGS || glob_used + len > sizeof(glob_pool)) {
            err_puts("shell: ", word, " matches too many names\n");
            break;
        }
        char *s = glob_pool + glob_used;
        memcpy(s, word, (size_t)(slash + 1));
        strcpy(s + slash + 1, name);
        glob_used += len;
        int at = n++;
        while (at > first && strcmp(argv[at - 1], s) > 0) {
            argv[at] = argv[at - 1];
            at--;
        }
        argv[at] = s;
    }
    if (n > first)
        return n;
literal:
    if (n < MAX_ARGS)
        argv[n++] = word;
    return n;
}

/* `<< WORD` (shell_run reads the text before the line runs) */
static char here_buf[65536];
static struct {
    uint32_t len;
    int set;
} here;

static void run_command(char *seg)
{
    char *words[MAX_ARGS], *argv[MAX_ARGS];
    int nwords = parse_args(seg, words);
    const char *out = NULL, *in = NULL;
    int append = 0, n = 0, here_in = 0;
    glob_used = 0;
    for (int i = 0; i < nwords; i++) {
        char *a = words[i];
        if (!arg_quoted[i] && (a[0] == '>' || a[0] == '<')) {
            int is_out = a[0] == '>';
            int twice = a[1] == a[0];
            const char *target = a + 1 + twice;
            if (!*target)
                target = i + 1 < nwords ? words[++i] : "";
            if (!*target) {
                err_puts("shell: a redirection without a file\n", "", "");
                return;
            }
            if (is_out) {
                out = target;
                append = twice;
            } else if (twice) {
                here_in = 1;                /* target: the end word, read */
            } else {
                in = target;
            }
        } else if (n == MAX_ARGS) {
            err_puts("shell: more than 64 arguments\n", "", "");
            return;
        } else if (!arg_quoted[i] && has_wild(a)) {
            n = glob_expand(a, argv, n);
        } else {
            argv[n++] = a;
        }
    }
    int argc = n;
    if ((in || out) && !need_fs("shell"))
        return;
    if (here_in && !here.set) {
        err_puts("shell: << without its text\n", "", "");
        return;
    }

    /* < file: the whole file as the command's input; << the text typed */
    struct input saved_in = stdin_in;
    const char *in_data = NULL;
    uint64_t in_size = 0, in_pages = 0;
    if (here_in) {
        stdin_in.data = here_buf;
        stdin_in.len = here.len;
        stdin_in.pos = 0;
        stdin_in.set = 1;
    } else if (in) {
        if (text_of("shell", in, &in_data, &in_size, &in_pages) < 0)
            return;
        stdin_in.data = in_data;
        stdin_in.len = in_size;
        stdin_in.pos = 0;
        stdin_in.set = 1;
    }

    /* > file: the output captured, then written */
    struct capture c = { 0 }, *saved_cap = cap;
    void (*saved_fn)(char) = NULL;
    if (out) {
        if (capture_open(&c) < 0)
            goto done;
        cap = &c;
        saved_fn = console_capture(capture_char);
    }
    if (argc)
        dispatch(argc, argv);
    if (out) {
        console_capture(saved_fn);
        cap = saved_cap;
        void *f = append ? fs_find(out) : NULL;
        int rc = f ? (osfs3_write(f, osfs3_file_size(f), c.buf, c.len) < 0 ||
                      osfs3_sync() < 0 ? -1 : 0)
                   : fs_write_file(out, c.buf, c.len);
        if (rc < 0)
            say_err("shell", out, "can't write it");
        else if (c.overflow)
            capture_too_big(out, &c);
        capture_close(&c);
    }
done:
    if (in_pages)
        mem_free_pages((void *)in_data, in_pages);
    stdin_in = saved_in;
}

/* A pipeline: split at the '|'s outside quotes, the stages run in turn */
static void run_line(char *line)
{
    char *seg[PIPE_STAGES];
    int n = 1;
    char q = 0;
    seg[0] = line;
    for (char *p = line; *p; p++) {
        if (q) {
            if (*p == q)
                q = 0;
        } else if (*p == '\'' || *p == '"') {
            q = *p;
        } else if (*p == '|') {
            if (n == PIPE_STAGES) {
                err_puts("shell: at most 8 stages in a pipe\n", "", "");
                return;
            }
            *p = 0;
            seg[n++] = p + 1;
        }
    }
    if (n == 1) {
        run_command(line);
        return;
    }
    struct capture prev = { 0 };
    for (int i = 0; i < n; i++) {
        if (i > 0) {
            stdin_in.data = prev.buf;
            stdin_in.len = prev.len;
            stdin_in.pos = 0;
            stdin_in.set = 1;
        }
        if (i == n - 1) {
            cmd_failed = 0;                 /* a pipe's status: its last stage's */
            run_command(seg[i]);
            break;
        }
        struct capture c = { 0 };
        if (capture_open(&c) < 0)
            break;
        struct capture *saved_cap = cap;
        cap = &c;
        void (*saved_fn)(char) = console_capture(capture_char);
        run_command(seg[i]);
        console_capture(saved_fn);
        cap = saved_cap;
        capture_close(&prev);
        prev = c;
        if (c.overflow) {
            capture_too_big("a stage's output", &c);
            break;
        }
    }
    stdin_in.set = 0;
    capture_close(&prev);
}

static bool blank(const char *s)
{
    while (*s == ' ')
        s++;
    return !*s;
}

/* A typed line: pipelines split at ;, && and || outside quotes. After &&
 * the next one runs if the last succeeded, after || if it failed, after ;
 * regardless. A lone & is just text (nothing runs in the background). */
static void run_list(char *line)
{
    char q = 0, op = ';', *start = line;
    int failed = 0;
    for (char *p = line;; p++) {
        char c = *p;
        if (q) {
            if (c == q)
                q = 0;
            if (c)
                continue;
        } else if (c == '\'' || c == '"') {
            q = c;
            continue;
        }
        if (c && c != ';' && !((c == '&' || c == '|') && p[1] == c))
            continue;
        *p = 0;
        if (!blank(start) && (op == ';' || (op == '&') == !failed)) {
            cmd_failed = 0;
            run_line(start);
            failed = cmd_failed;
        }
        if (!c)
            break;
        op = c;
        if (c != ';')
            p++;
        start = p + 1;
    }
}

/* `<< WORD` in a line (outside quotes): the lines typed after it, up to one
 * that is WORD, become the input of the command that has it. 0 if there's
 * none, 1 once read, -1 if it can't be (Ctrl+C or Ctrl+D stop it). */
static int read_heredoc(const char *line)
{
    const char *m = NULL;
    char q = 0;
    for (const char *p = line; *p && !m; p++) {
        if (q) {
            if (*p == q)
                q = 0;
        } else if (*p == '\'' || *p == '"') {
            q = *p;
        } else if (p[0] == '<' && p[1] == '<') {
            m = p;
        }
    }
    if (!m)
        return 0;
    char word[64];
    int wl = 0;
    q = 0;
    const char *t = m + 2;
    while (*t == ' ')
        t++;
    for (; *t; t++) {
        if (!q && (*t == ' ' || *t == '|' || *t == ';' || *t == '&' || *t == '<' || *t == '>'))
            break;
        if (!q && (*t == '\'' || *t == '"'))
            q = *t;
        else if (q && *t == q)
            q = 0;
        else if (wl < (int)sizeof(word) - 1)
            word[wl++] = *t;
    }
    word[wl] = 0;
    if (!wl) {
        err_puts("shell: << needs the word that ends the text\n", "", "");
        return -1;
    }
    static char l[256];
    here.len = 0;
    for (;;) {
        int n = term_readline("> ", l, sizeof(l));
        if (n < 0)
            return -1;
        if (strcmp(l, word) == 0)
            break;
        if (here.len + (uint32_t)n + 1 > sizeof(here_buf)) {
            err_puts("shell: the << text went over 64 KB\n", "", "");
            return -1;
        }
        memcpy(here_buf + here.len, l, (size_t)n);
        here.len += (uint32_t)n;
        here_buf[here.len++] = '\n';
    }
    here.set = 1;
    return 1;
}

/* ── Tab completion (term.c asks) ─────────────────────────────── */

static const char *const command_names[] = {
    "beep", "burn", "cat", "cd", "chat", "clear", "cp", "cpubench", "cpufreq",
    "cpus", "date", "desktop", "df", "dmesg", "echo", "exec", "get", "grep", "halt",
    "head", "help", "hexdump", "history", "https", "keys", "kload", "llm", "ls",
    "mem", "mkdir", "model", "mv", "net", "ping", "pmic", "ps", "put", "pwd",
    "random",
    "reboot", "resolve", "rm", "rmdir", "rpmh", "screen", "screenshot", "tail",
    "temp", "touch", "ufs", "uname", "uptime", "wc", 0
};

static struct {
    const char *prefix;
    int plen, count, clen, listing;
    char common[FS_PATH_MAX];
} cpl;

/* A candidate: a directory continues with '/', anything else with ' ' */
static void cpl_offer(const char *name, bool dir)
{
    int n = (int)strlen(name);
    if (n < cpl.plen || strncmp(name, cpl.prefix, cpl.plen) != 0 || n + 2 > FS_PATH_MAX)
        return;
    if (cpl.listing) {
        serial_puts(name);
        serial_puts(dir ? "/  " : "  ");
        return;
    }
    if (cpl.count++ == 0) {
        strcpy(cpl.common, name);
        cpl.common[n] = dir ? '/' : ' ';
        cpl.common[n + 1] = 0;
        cpl.clen = n + 1;
        return;
    }
    int k = 0;
    while (k < cpl.clen && k < n && cpl.common[k] == name[k])
        k++;
    if (k == n && k < cpl.clen && cpl.common[k] == (dir ? '/' : ' '))
        k++;
    cpl.clen = k;
    cpl.common[k] = 0;
}

static void cpl_offer_all(bool commands, const char *dir)
{
    if (commands) {
        for (int i = 0; command_names[i]; i++)
            cpl_offer(command_names[i], false);
        return;
    }
    osfs3_dir_cursor_t cur;
    static char name[OSFS3_NAME_MAX + 1];
    bool is_dir;
    if (osfs3_dir_cursor_open_ci(dir, &cur) < 0)
        return;
    while (osfs3_dir_cursor_next(&cur, name, sizeof(name), &is_dir, NULL, NULL) == 1)
        cpl_offer(name, is_dir);
}

int shell_complete(const char *word, int len, int first, char *add, int add_max)
{
    static char w[FS_PATH_MAX], dir[FS_PATH_MAX];
    add[0] = 0;
    if (len >= FS_PATH_MAX)
        return 0;
    memcpy(w, word, (size_t)len);
    w[len] = 0;
    int slash = -1;
    for (int i = 0; i < len; i++)
        if (w[i] == '/')
            slash = i;
    bool commands = first && slash < 0;
    if (!commands) {
        if (!osfs3_is_mounted())
            return 0;
        char d[FS_PATH_MAX];
        if (slash < 0) {
            strcpy(d, ".");
        } else {
            memcpy(d, w, slash ? (size_t)slash : 1);    /* "/x": the root */
            d[slash ? slash : 1] = 0;
        }
        if (fs_path(d, dir) < 0)
            return 0;
    }
    cpl.prefix = w + slash + 1;
    cpl.plen = (int)strlen(cpl.prefix);
    cpl.count = 0;
    cpl.listing = 0;
    cpl_offer_all(commands, dir);
    int extra = cpl.clen - cpl.plen;
    if (cpl.count && extra > 0 && extra < add_max) {
        memcpy(add, cpl.common + cpl.plen, (size_t)extra);
        add[extra] = 0;
    } else if (cpl.count > 1) {
        serial_puts("\n");
        cpl.listing = 1;
        cpl_offer_all(commands, dir);
        serial_puts("\n");
    }
    return cpl.count;
}

void shell_run(void)
{
    static char buf[256];

    serial_puts("\nOsitoK shell ready. Type 'help' for commands.\n\n");

    for (;;) {
        if (term_readline("shell> ", buf, sizeof(buf)) <= 0)
            continue;
        if (read_heredoc(buf) >= 0)
            run_list(buf);
        here.set = 0;
    }
}
