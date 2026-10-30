#include "../kernel/terminal.c"

extern int printf(const char *format, ...);
extern void abort(void);
extern void *malloc(size_t size);
extern void *realloc(void *ptr, size_t size);
extern void free(void *ptr);

static const char *input;
static unsigned checks;
static bool fail_alloc;
static bool capture_output;
static char serial_output[512], screen_output[512];
static unsigned serial_count, screen_count;

#define CHECK(test) do { checks++; if (!(test)) { \
    printf("FAIL line %d: %s\n", __LINE__, #test); abort(); \
} } while (0)

static void capture(char *out, unsigned *n, const char *s)
{
    if (!capture_output) return;
    while (*s) { CHECK(*n < 511); out[(*n)++] = *s++; }
    out[*n] = 0;
}
void serial_puts(const char *s) { capture(serial_output, &serial_count, s); }
void fb_puts(const char *s) { capture(screen_output, &screen_count, s); }
void fb_puts_color(const char *s, uint32_t c) { (void)c; fb_puts(s); }
void fb_putchar(char c) { char s[2] = {c, 0}; fb_puts(s); }
void *kmalloc(uint64_t size) { return fail_alloc ? NULL : malloc(size); }
void *krealloc(void *ptr, uint64_t size) { return fail_alloc ? NULL : realloc(ptr, size); }
void kfree(void *ptr) { free(ptr); }
char kb_getchar(void) { CHECK(*input != 0); return *input++; }
int kb_getchar_interruptible(bool (*cancel)(void *), void *context)
{
    if (cancel(context)) return -1;
    return (unsigned char)kb_getchar();
}

static unsigned empty_polls, closed_polls;
static bool want_event;
static void expect(const char *keys, const char *expected);
static bool prompt_poll(bool empty)
{
    if (empty) empty_polls++; else closed_polls++;
    return empty && want_event;
}

static void test_prompt_event(void)
{
    term_init(); expect("history\n", "history");
    char *line = (char *)1;
    uint64_t n = 123;
    int history_count = hist_count;
    want_event = true; input = "";
    CHECK(term_readline_alloc_poll(NULL, &line, &n, prompt_poll) == TERM_READ_EVENT);
    CHECK(!line && !n && empty_polls == 1 && closed_polls == 1);
    CHECK(!line_len && hist_count == history_count);
    want_event = false; input = "local\n";
    CHECK(term_readline_alloc_poll(NULL, &line, &n, prompt_poll) == TERM_READ_OK);
    CHECK(line && n == 5 && !strcmp(line, "local")); kfree(line);
    CHECK(hist_count == history_count + 1 && closed_polls > 5);
    want_event = true;
    unsigned before = empty_polls;
    input = "wifi\n";
    CHECK(term_readline_alloc(NULL, &line, &n) == TERM_READ_OK);
    CHECK(!strcmp(line, "wifi") && empty_polls == before); kfree(line);
    char fixed[32]; input = "heredoc\n";
    CHECK(term_readline(NULL, fixed, sizeof(fixed)) == 7);
    CHECK(!strcmp(fixed, "heredoc") && empty_polls == before);
    input = "password\n";
    CHECK(term_read_secret(NULL, fixed, sizeof(fixed), false) == 8);
    CHECK(!strcmp(fixed, "password") && empty_polls == before);
    term_init(); fail_alloc = true; input = "";
    before = closed_polls;
    CHECK(term_readline_alloc_poll(NULL, &line, &n, prompt_poll) == TERM_READ_NOMEM);
    CHECK(!line && !n && closed_polls == before + 1);
    fail_alloc = false; want_event = false;
    term_init();
}

static void expect(const char *keys, const char *expected)
{
    input = keys;
    CHECK(term_readline_edit(NULL) == TERM_READ_OK);
    CHECK(strcmp(line_buf, expected) == 0);
    CHECK(line_len == strlen(expected));
    CHECK(*input == 0);
}

static void secret_case(const char *keys, const char *expected, const char *echoed)
{
    struct { char before[4], secret[64], after[4]; } b;
    memset(&b, 0x55, sizeof(b));
    serial_count = screen_count = 0;
    capture_output = true; input = keys;
    int history_count = hist_count;
    char *last_history = history[hist_count - 1];
    int n = term_read_secret("Password: ", b.secret, sizeof(b.secret), echoed != NULL);
    CHECK(!memcmp(serial_output, "Password: ", 10));
    CHECK(!strcmp(serial_output + 10, echoed ? echoed : "\n"));
    CHECK(!strcmp(screen_output, serial_output));
    CHECK(hist_count == history_count && history[hist_count - 1] == last_history);
    CHECK(!strcmp(line_buf, "harmless"));
    CHECK(b.before[3] == 0x55 && b.after[0] == 0x55);
    if (expected) {
        CHECK(n == (int)strlen(expected) && !strcmp(b.secret, expected));
        for (unsigned i = n; i < sizeof(b.secret); i++) CHECK(!b.secret[i]);
    } else {
        CHECK(n == -1);
        for (unsigned i = 0; i < sizeof(b.secret); i++) CHECK(!b.secret[i]);
    }
    CHECK(!strcmp(input, "next"));
    capture_output = false;
}

static void test_secrets(void)
{
    term_init(); expect("harmless\n", "harmless");
    secret_case("password\nnext", "password", NULL);
    secret_case("passwors\bd\177d\rnext", "password", NULL);
    secret_case("pass\033[A\033[1;5D\033OFword\nnext", "password", NULL);
    secret_case("discard\025password\nnext", "password", NULL);
    secret_case("discard\003next", NULL, NULL);
    secret_case("discard\004next", NULL, NULL);
    secret_case("bad\200text\nnext", NULL, NULL);
    secret_case("bad\ttext\nnext", NULL, NULL);
    secret_case("pass\033[\nnext", "pass", NULL);
    secret_case("aZ09\nnext", "aZ09", "aZ09\n");
    secret_case("\b\177passwors\bd\177d\rnext", "password", "passwors\b \bd\b \bd\n");
    secret_case("pass\033[A\033[1;5D\033OFword\nnext", "password", "password\n");
    secret_case("old\025password\nnext", "password", "old\b \b\b \b\b \bpassword\n");
    secret_case("discard\003next", NULL, "discard\n");
    secret_case("discard\004next", NULL, "discard\n");
    secret_case("bad\200text\nnext", NULL, "bad\n");
    secret_case("bad\ttext\nnext", NULL, "bad\n");
    secret_case("pass\033[\nnext", "pass", "pass\n");
    char keys[90]; memset(keys, 'x', 80); memcpy(keys + 80, "\nnext", 6);
    secret_case(keys, NULL, NULL);
    char echoed[65]; memset(echoed, 'x', 63); echoed[63] = '\n'; echoed[64] = 0;
    secret_case(keys, NULL, echoed);
    memset(keys, 'x', 63); memcpy(keys + 63, "\nnext", 6);
    char expected[64]; memset(expected, 'x', 63); expected[63] = 0;
    secret_case(keys, expected, NULL);
    secret_case(keys, expected, echoed);
    input = "";
    CHECK(term_read_secret(NULL, NULL, 64, false) == -1 && term_read_secret(NULL, keys, 0, true) == -1);
    term_init();
}

int main(void)
{
    term_init();
    expect("echo acx\b\033[Db\033[C\n", "echo abc");
    expect("abc\033[Hq\033[Fz\n", "qabcz");
    expect("abc\033[1~q\033[4~z\n", "qabcz");
    expect("abc\033[7~q\033[8~z\n", "qabcz");
    expect("abc\033OHq\033OFz\n", "qabcz");
    expect("abc\033[H\033[3~\033[F\033[3~\n", "bc");
    expect("\033[D\033[C\033[3~\n", "");
    expect("abc\033[999999999999999999999~q\n", "abcq");
    expect("abc\033[1;5Dq\n", "abcq"); /* unknown parameters are consumed */
    expect("abc\033[2~\033[5~\033[6~q\n", "abcq");
    expect("abc\033q\n", "abcq"); /* Escape followed by an ordinary key */
    expect("abc\033\n", "abc");
    expect("abc\033[\003", "");
    expect("abc\033[12\n", "abc");
    expect("first\n", "first");
    expect("second\n", "second");
    expect("\033[A\033[A\n", "first");
    expect("\033[A\033[B\n", "");
    expect("\033[B\n", "");
    for (unsigned i = 0; i < 16; i++) expect("another\n", "another");
    expect("\033[A\033[A\033[A\033[A\033[A\033[A\033[A\033[A\033[A\n", "qabcz");
    char long_input[704], long_expected[701];
    for (unsigned i = 0; i < 700; i++) long_input[i] = long_expected[i] = 'x';
    long_input[700] = '\n'; long_input[701] = 0; long_expected[700] = 0;
    expect(long_input, long_expected);
    expect("\033[A\n", long_expected);
    term_init();
    fail_alloc = true;
    input = "";
    CHECK(term_readline_edit(NULL) == TERM_READ_NOMEM);
    fail_alloc = false;
    test_secrets();
    test_prompt_event();
    term_init();
    printf("PASS terminal keyboard sequences: %u checks\n", checks);
    return 0;
}
