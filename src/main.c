/* adbi - ADB client for old iOS (wireless debugging pairing + TLS connect) */
#include "adbi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <ctype.h>
#include <strings.h>
#include <signal.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <openssl/err.h>

static char g_serial[300];
static volatile sig_atomic_t g_winch;
static struct termios g_oldtio;
static int g_tty_raw;

static void on_winch(int s) { (void)s; g_winch = 1; }
static void restore_tty(void) { if (g_tty_raw) { tcsetattr(0, TCSADRAIN, &g_oldtio); g_tty_raw = 0; } }

static void usage(void) {
    fputs(
"adbi " ADBI_VERSION " - ADB over Wi-Fi (Android 11+ wireless debugging) for old iOS\n\n"
"  adbi discover [seconds]          list wireless-debugging devices found via mDNS\n"
"  adbi pair [HOST:PORT] [CODE]     pair using the 6-digit code (auto-discovers if no address)\n"
"  adbi connect [HOST[:PORT]]       check connection (port auto-discovered if omitted), remember target\n"
"  adbi [-s HOST:PORT] COMMAND ...\n"
"      shell [cmd...]               interactive shell, or run a command\n"
"      exec-out cmd...              run a command, raw binary output (no pty)\n"
"      push LOCAL REMOTE | pull REMOTE [LOCAL]\n"
"      install [-r] FILE.apk\n"
"      key NAME...                  home back up down left right ok menu play pause volup ... or a number\n"
"      text STRING | tap X Y | swipe X1 Y1 X2 Y2 [ms]\n"
"      screencap FILE.png | launch PACKAGE | getprop [name] | logcat [args]\n"
"  adbi keygen                      create/show the key (~/.adbi/adbkey, or $ADBI_KEY)\n\n"
"Target comes from -s, $ADBI_SERIAL, or the last successful 'adbi connect'.\n"
"Port 5555 style (adb tcpip) devices also work: adbi connect 192.168.1.5:5555\n", stderr);
}

/* ---- target handling ---- */
static void save_default(const char *s) {
    char p[600]; FILE *f;
    if (adbi_path(p, sizeof p, "default")) return;
    if ((f = fopen(p, "w"))) { fprintf(f, "%s\n", s); fclose(f); }
}
static int load_default(char *out, size_t n) {
    char p[600]; FILE *f;
    if (adbi_path(p, sizeof p, "default") || !(f = fopen(p, "r"))) return -1;
    int ok = fgets(out, (int)n, f) != NULL; fclose(f);
    out[strcspn(out, "\r\n")] = 0;
    return ok && *out ? 0 : -1;
}

static int split_target(const char *t, char *host, size_t hl, int *port) {
    const char *c = strrchr(t, ':');
    *port = 0;
    if (c && c != t) { snprintf(host, hl, "%.*s", (int)(c - t), t); *port = atoi(c + 1); }
    else snprintf(host, hl, "%s", t);
    return *host ? 0 : -1;
}

static int resolve_target(const char *t, char *host, size_t hl, int *port) {
    char buf[300];
    if (!t || !*t) {
        const char *e = getenv("ADBI_SERIAL");
        if (e && *e) t = e;
        else if (!load_default(buf, sizeof buf)) t = buf;
        else { fprintf(stderr, "adbi: no target. Use -s HOST:PORT or run 'adbi connect' first.\n"); return -1; }
    }
    if (split_target(t, host, hl, port)) return -1;
    if (*port <= 0) {
        char h2[128]; int p2 = 0;
        fprintf(stderr, "adbi: looking up port for %s via mDNS...\n", host);
        int r = mdns_find(MDNS_CONNECT, host, 5000, 0, h2, sizeof h2, &p2);
        if (r == 1) { *port = p2; }
        else { *port = 5555; if (r < 0) fprintf(stderr, "adbi: mDNS not available, assuming port 5555\n"); }
    }
    return 0;
}

static int open_target(adb_conn *c) {
    char host[256]; int port;
    if (resolve_target(g_serial, host, sizeof host, &port)) return -1;
    return adb_connect(c, host, port);
}

/* ---- shell / exec streams ---- */
static void put_v2(adb_conn *c, int id, const void *d, size_t n) {
    uint8_t *b = malloc(n + 5);
    if (!b) return;
    b[0] = id; put32le(b + 1, (uint32_t)n); if (n) memcpy(b + 5, d, n);
    stream_write(c, b, n + 5);
    free(b);
}

static void send_winsize(adb_conn *c) {
    struct winsize ws; char b[64];
    if (ioctl(0, TIOCGWINSZ, &ws) == 0) {
        int n = snprintf(b, sizeof b, "%dx%d,%dx%d", ws.ws_row, ws.ws_col, ws.ws_xpixel, ws.ws_ypixel);
        put_v2(c, 5, b, n);
    }
}

static void write_all(int fd, const void *b, size_t n) {
    const uint8_t *p = b;
    while (n) { ssize_t r = write(fd, p, n); if (r < 0) { if (errno == EINTR) continue; return; } p += r; n -= r; }
}

/* mode: 0 = raw stream (exec/legacy), 1 = shell v2 framed */
static int pump(adb_conn *c, int framed, int use_stdin, int tty) {
    int exit_code = 0, stdin_open = use_stdin, extra;
    if (tty && framed) { signal(SIGWINCH, on_winch); send_winsize(c); }
    for (;;) {
        while (rb_avail(c)) {
            if (!framed) { write_all(1, rb_ptr(c), rb_avail(c)); rb_consume(c, rb_avail(c)); break; }
            if (rb_avail(c) < 5) break;
            uint8_t *p = rb_ptr(c); uint32_t len = get32le(p + 1);
            if (rb_avail(c) < 5 + (size_t)len) break;
            if (p[0] == 1) write_all(1, p + 5, len);
            else if (p[0] == 2) write_all(2, p + 5, len);
            else if (p[0] == 3 && len >= 1) exit_code = p[5];
            rb_consume(c, 5 + (size_t)len);
        }
        if (c->closed) break;
        if (g_winch) { g_winch = 0; send_winsize(c); }
        if (adb_wait(c, stdin_open && !c->ok_pending ? 0 : -1, &extra) < 0) {
            if (!rb_avail(c)) break;
            continue;
        }
        if (extra && stdin_open) {
            uint8_t buf[4096];
            ssize_t n = read(0, buf, sizeof buf);
            if (n > 0) { if (framed) put_v2(c, 0, buf, n); else stream_write(c, buf, n); }
            else if (n == 0) { stdin_open = 0; if (framed) put_v2(c, 3, NULL, 0); }
        }
    }
    /* drain whatever is left */
    while (rb_avail(c)) {
        if (!framed) { write_all(1, rb_ptr(c), rb_avail(c)); rb_consume(c, rb_avail(c)); break; }
        if (rb_avail(c) < 5) break;
        uint8_t *p = rb_ptr(c); uint32_t len = get32le(p + 1);
        if (rb_avail(c) < 5 + (size_t)len) break;
        if (p[0] == 1) write_all(1, p + 5, len); else if (p[0] == 2) write_all(2, p + 5, len);
        else if (p[0] == 3 && len >= 1) exit_code = p[5];
        rb_consume(c, 5 + (size_t)len);
    }
    return exit_code;
}

static char *join_args(int argc, char **argv) {
    size_t n = 1; int i;
    for (i = 0; i < argc; i++) n += strlen(argv[i]) + 1;
    char *s = malloc(n); s[0] = 0;
    for (i = 0; i < argc; i++) { if (i) strcat(s, " "); strcat(s, argv[i]); }
    return s;
}

static int cmd_shell(int argc, char **argv, int raw_exec) {
    adb_conn c; char svc[8192]; int rc = 1;
    if (open_target(&c)) return 1;
    int has_cmd = argc > 0;
    char *cmd = has_cmd ? join_args(argc, argv) : NULL;
    int tty = isatty(0) && isatty(1) && !has_cmd && !raw_exec;
    if (raw_exec) snprintf(svc, sizeof svc, "exec:%s", cmd ? cmd : "");
    else if (c.shell_v2) {
        const char *term = getenv("TERM");
        if (tty) snprintf(svc, sizeof svc, "shell,v2,TERM=%s,pty:", term && *term ? term : "xterm");
        else snprintf(svc, sizeof svc, "shell,v2,raw:%s", cmd ? cmd : "");
    } else snprintf(svc, sizeof svc, "shell:%s", cmd ? cmd : "");
    if (stream_open(&c, svc)) { fprintf(stderr, "adbi: device refused service '%s'\n", svc); goto out; }
    if (tty) {
        struct termios t;
        tcgetattr(0, &g_oldtio); t = g_oldtio; cfmakeraw(&t); tcsetattr(0, TCSADRAIN, &t);
        g_tty_raw = 1; atexit(restore_tty);
    }
    int use_stdin = tty || (!isatty(0));
    rc = pump(&c, c.shell_v2 && !raw_exec, use_stdin, tty);
    restore_tty();
out:
    free(cmd); adb_close(&c);
    return rc;
}

static int shell_line(const char *line) {   /* run a one-liner via cmd_shell */
    char *a[1] = {(char *)line};
    return cmd_shell(1, a, 0);
}

/* ---- sync: push / pull ---- */
static int sync_fail(adb_conn *c, uint8_t *h) {
    uint32_t n = get32le(h + 4); char msg[512] = "";
    if (n > sizeof msg - 1) n = sizeof msg - 1;
    stream_read_exact(c, msg, n); msg[n] = 0;
    fprintf(stderr, "adbi: remote error: %s\n", msg);
    return 1;
}

static int cmd_push(const char *local, const char *remote) {
    adb_conn c; int rc = 1;
    FILE *f = fopen(local, "rb"); struct stat st;
    if (!f || fstat(fileno(f), &st)) { fprintf(stderr, "adbi: cannot open %s\n", local); return 1; }
    if (open_target(&c)) { fclose(f); return 1; }
    if (stream_open(&c, "sync:")) goto out;
    {
        char spec[1200]; uint8_t h[8];
        int n = snprintf(spec, sizeof spec, "%s,%d", remote, (int)(st.st_mode & 0777) | 0100000);
        memcpy(h, "SEND", 4); put32le(h + 4, n);
        uint8_t *b = malloc(8 + n); memcpy(b, h, 8); memcpy(b + 8, spec, n);
        int r = stream_write(&c, b, 8 + n); free(b);
        if (r) goto out;
        uint8_t *buf = malloc(8 + 65536); size_t got, total = 0;
        memcpy(buf, "DATA", 4);
        while ((got = fread(buf + 8, 1, 65536, f)) > 0) {
            put32le(buf + 4, (uint32_t)got);
            if (stream_write(&c, buf, 8 + got)) { free(buf); goto out; }
            total += got;
            fprintf(stderr, "\r%lu / %lu bytes", (unsigned long)total, (unsigned long)st.st_size);
        }
        fprintf(stderr, "\n");
        free(buf);
        memcpy(h, "DONE", 4); put32le(h + 4, (uint32_t)st.st_mtime);
        if (stream_write(&c, h, 8) || stream_read_exact(&c, h, 8)) goto out;
        if (!memcmp(h, "OKAY", 4)) rc = 0;
        else if (!memcmp(h, "FAIL", 4)) sync_fail(&c, h);
    }
out:
    if (rc) fprintf(stderr, "adbi: push failed\n");
    fclose(f); adb_close(&c);
    return rc;
}

static int cmd_pull(const char *remote, const char *local) {
    adb_conn c; int rc = 1; FILE *f = NULL; uint8_t h[8];
    if (open_target(&c)) return 1;
    if (stream_open(&c, "sync:")) goto out;
    memcpy(h, "RECV", 4); put32le(h + 4, (uint32_t)strlen(remote));
    {
        size_t n = strlen(remote); uint8_t *b = malloc(8 + n);
        memcpy(b, h, 8); memcpy(b + 8, remote, n);
        int r = stream_write(&c, b, 8 + n); free(b);
        if (r) goto out;
    }
    unsigned long total = 0;
    for (;;) {
        if (stream_read_exact(&c, h, 8)) goto out;
        if (!memcmp(h, "DATA", 4)) {
            uint32_t n = get32le(h + 4); uint8_t *b;
            if (n > 65536) goto out;
            b = malloc(n ? n : 1);
            if (stream_read_exact(&c, b, n)) { free(b); goto out; }
            if (!f && !(f = fopen(local, "wb"))) { fprintf(stderr, "adbi: cannot write %s\n", local); free(b); goto out; }
            fwrite(b, 1, n, f); free(b); total += n;
            fprintf(stderr, "\r%lu bytes", total);
        } else if (!memcmp(h, "DONE", 4)) { rc = 0; break; }
        else if (!memcmp(h, "FAIL", 4)) { sync_fail(&c, h); goto out; }
        else goto out;
    }
    if (!f) f = fopen(local, "wb");   /* empty file */
    fprintf(stderr, "\n");
out:
    if (rc) fprintf(stderr, "adbi: pull failed\n");
    if (f) fclose(f);
    adb_close(&c);
    return rc;
}

/* ---- sugar ---- */
static const struct { const char *n; int code; } KEYS[] = {
    {"home",3},{"back",4},{"up",19},{"down",20},{"left",21},{"right",22},{"ok",23},{"center",23},
    {"select",23},{"enter",66},{"menu",82},{"play",126},{"pause",127},{"playpause",85},{"stop",86},
    {"next",87},{"prev",88},{"rewind",89},{"ff",90},{"volup",24},{"voldown",25},{"mute",164},
    {"power",26},{"wakeup",224},{"sleep",223},{"tv",170},{"guide",172},{"settings",176},{"search",84},
    {"del",67},{"chup",166},{"chdown",167},{"info",165},{"input",178},{"apps",187},{"assist",219},{NULL,0}};

static int cmd_key(int argc, char **argv) {
    char line[1024] = ""; int i, j;
    for (i = 0; i < argc; i++) {
        int code = -1;
        if (isdigit((unsigned char)argv[i][0])) code = atoi(argv[i]);
        else for (j = 0; KEYS[j].n; j++) if (!strcasecmp(KEYS[j].n, argv[i])) code = KEYS[j].code;
        if (code < 0) { fprintf(stderr, "adbi: unknown key '%s'\n", argv[i]); return 1; }
        char t[64]; snprintf(t, sizeof t, "%sinput keyevent %d", i ? "; " : "", code);
        strncat(line, t, sizeof line - strlen(line) - 1);
    }
    return shell_line(line);
}

static int cmd_text(int argc, char **argv) {
    char *s = join_args(argc, argv), line[8192] = "input text '";
    size_t o = strlen(line);
    for (char *p = s; *p && o < sizeof line - 8; p++) {
        if (*p == ' ') { line[o++] = '%'; line[o++] = 's'; }
        else if (*p == '\'') { memcpy(line + o, "'\\''", 4); o += 4; }
        else line[o++] = *p;
    }
    line[o++] = '\''; line[o] = 0; free(s);
    return shell_line(line);
}

static int cmd_connect(const char *arg) {
    char host[256]; int port; adb_conn c; char ds[300];
    if (!arg || !*arg) {
        char h2[128]; int r = mdns_find(MDNS_CONNECT, NULL, 5000, 0, h2, sizeof h2, &port);
        if (r != 1) { fprintf(stderr, "adbi: no wireless-debugging device found. Give HOST[:PORT].\n"); return 1; }
        snprintf(ds, sizeof ds, "%s:%d", h2, port); arg = ds;
    }
    if (resolve_target(arg, host, sizeof host, &port)) return 1;
    if (adb_connect(&c, host, port)) return 1;
    printf("connected to %s:%d\n%s\n", host, port, c.banner);
    adb_close(&c);
    char s[300]; snprintf(s, sizeof s, "%s:%d", host, port);
    save_default(s);
    return 0;
}

static int cmd_pair(int argc, char **argv) {
    char host[256] = "", code[64] = ""; int port = 0, i;
    for (i = 0; i < argc; i++) {
        int digits = 1; for (const char *p = argv[i]; *p; p++) if (!isdigit((unsigned char)*p)) digits = 0;
        if (digits && strlen(argv[i]) == 6) snprintf(code, sizeof code, "%s", argv[i]);
        else if (split_target(argv[i], host, sizeof host, &port)) return 1;
        else if (port <= 0) { fprintf(stderr, "adbi: pairing needs HOST:PORT (the port shown in the pairing dialog)\n"); return 1; }
    }
    if (!*host) {
        fprintf(stderr, "adbi: looking for a device showing the pairing dialog...\n");
        int r = mdns_find(MDNS_PAIRING, NULL, 10000, 0, host, sizeof host, &port);
        if (r != 1) { fprintf(stderr, "adbi: no pairing service found. Open 'Pair device with pairing code' on the device,\n      then run: adbi pair HOST:PORT CODE\n"); return 1; }
        fprintf(stderr, "adbi: found %s:%d\n", host, port);
    }
    if (!*code) {
        fprintf(stderr, "Enter pairing code: "); fflush(stderr);
        if (!fgets(code, sizeof code, stdin)) return 1;
        code[strcspn(code, "\r\n ")] = 0;
    }
    if (!*code) return 1;
    return adbi_pair(host, port, code);
}

int main(int argc, char **argv) {
    int i = 1;
    signal(SIGPIPE, SIG_IGN);
    SSL_library_init(); SSL_load_error_strings(); OpenSSL_add_all_algorithms();
    while (i < argc && !strcmp(argv[i], "-s") && i + 1 < argc) { snprintf(g_serial, sizeof g_serial, "%s", argv[i + 1]); i += 2; }
    if (i >= argc) { usage(); return 2; }
    const char *cmd = argv[i++]; int n = argc - i; char **a = argv + i;

    if (!strcmp(cmd, "keygen")) {
        EVP_PKEY *pk; char pub[1024];
        if (keys_load(&pk, pub, sizeof pub)) return 1;
        printf("%s\n", pub); return 0;
    }
    if (!strcmp(cmd, "discover")) {
        int secs = n > 0 ? atoi(a[0]) : 4; char h[128]; int p;
        if (mdns_find(MDNS_PAIRING, NULL, secs * 1000, 1, h, sizeof h, &p) < 0) { fprintf(stderr, "adbi: built without mDNS support\n"); return 1; }
        mdns_find(MDNS_CONNECT, NULL, secs * 1000, 1, h, sizeof h, &p);
        return 0;
    }
    if (!strcmp(cmd, "pair")) return cmd_pair(n, a);
    if (!strcmp(cmd, "connect")) return cmd_connect(n > 0 ? a[0] : NULL);
    if (!strcmp(cmd, "shell")) return cmd_shell(n, a, 0);
    if (!strcmp(cmd, "exec-out") || !strcmp(cmd, "exec")) return cmd_shell(n, a, 1);
    if (!strcmp(cmd, "push") && n == 2) return cmd_push(a[0], a[1]);
    if (!strcmp(cmd, "pull") && n >= 1) {
        const char *b = strrchr(a[0], '/');
        return cmd_pull(a[0], n > 1 ? a[1] : (b ? b + 1 : a[0]));
    }
    if (!strcmp(cmd, "install") && n >= 1) {
        const char *apk = a[n - 1]; char line[1200]; int r;
        r = cmd_push(apk, "/data/local/tmp/adbi-install.apk");
        if (r) return r;
        snprintf(line, sizeof line, "pm install %s /data/local/tmp/adbi-install.apk; rm -f /data/local/tmp/adbi-install.apk",
                 n > 1 && !strcmp(a[0], "-r") ? "-r" : "");
        return shell_line(line);
    }
    if (!strcmp(cmd, "key")) return cmd_key(n, a);
    if (!strcmp(cmd, "text") && n) return cmd_text(n, a);
    if (!strcmp(cmd, "tap") && n == 2) { char l[128]; snprintf(l, sizeof l, "input tap %d %d", atoi(a[0]), atoi(a[1])); return shell_line(l); }
    if (!strcmp(cmd, "swipe") && n >= 4) {
        char l[160]; snprintf(l, sizeof l, "input swipe %d %d %d %d %d", atoi(a[0]), atoi(a[1]), atoi(a[2]), atoi(a[3]), n > 4 ? atoi(a[4]) : 300);
        return shell_line(l);
    }
    if (!strcmp(cmd, "launch") && n == 1) {
        char l[300]; snprintf(l, sizeof l, "monkey -p %s -c android.intent.category.LAUNCHER 1", a[0]); return shell_line(l);
    }
    if (!strcmp(cmd, "getprop")) { char *b[8]; int k; b[0] = "getprop"; for (k = 0; k < n && k < 6; k++) b[k + 1] = a[k]; return cmd_shell(k + 1, b, 0); }
    if (!strcmp(cmd, "logcat")) { char *b[16]; int k; b[0] = "logcat"; for (k = 0; k < n && k < 14; k++) b[k + 1] = a[k]; return cmd_shell(k + 1, b, 0); }
    if (!strcmp(cmd, "screencap") && n == 1) {
        int fd = open(a[0], O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) { perror("adbi"); return 1; }
        dup2(fd, 1); close(fd);
        char *b[2] = {"screencap", "-p"}; return cmd_shell(2, b, 1);
    }
    usage();
    return 2;
}
