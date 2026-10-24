/*
 * Integration test for castcore (xbox-cast). GPL-3.0-or-later.
 *
 * Starts a receiver on 127.0.0.1 with a recording mdns_publish callback, then checks:
 *   - both services are published with the TXT keys iOS needs (features, deviceid, pk, model, ...),
 *   - GET /info over the RTSP port answers a binary plist with the advertised display + features,
 *   - the ed25519 identity persists in data_dir (same pk after a restart),
 *   - xc_stop withdraws the services, xc_destroy returns.
 */
#include <sys/time.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <plist/plist.h>

#include "castcore.h"

static int failures;
#define CHECK(cond, what) do { int ok_ = (cond); printf("  %s %s\n", ok_ ? "ok  " : "FAIL", what); if (!ok_) failures++; } while (0)

#define MAX_TXT 32
typedef struct {
    char type[32], name[128];
    unsigned short port;
    char key[MAX_TXT][32], value[MAX_TXT][256];
    size_t count;
} service;

static service services[4];
static int nservices, unpublished;

static int rec_publish(void *ctx, const char *type, const char *name, unsigned short port, const xc_txt_entry *txt, size_t n)
{
    service *s;
    (void) ctx;
    if (nservices >= 4) return -1;
    s = &services[nservices++];
    snprintf(s->type, sizeof(s->type), "%s", type);
    snprintf(s->name, sizeof(s->name), "%s", name);
    s->port = port;
    for (size_t i = 0; i < n && i < MAX_TXT; i++) {
        snprintf(s->key[i], sizeof(s->key[i]), "%s", txt[i].key);
        snprintf(s->value[i], sizeof(s->value[i]), "%s", txt[i].value);
    }
    s->count = n < MAX_TXT ? n : MAX_TXT;
    return 0;
}

static void rec_unpublish(void *ctx) { (void) ctx; unpublished++; }

static void log_cb(void *ctx, xc_log_level l, const char *msg) { (void) ctx; (void) l; printf("       [core] %s\n", msg); }

static const char *txt(const service *s, const char *key)
{
    for (size_t i = 0; i < s->count; i++) if (!strcmp(s->key[i], key)) return s->value[i];
    return NULL;
}

static const service *find(const char *type)
{
    for (int i = 0; i < nservices; i++) if (!strcmp(services[i].type, type)) return &services[i];
    return NULL;
}

/* GET /info; returns the body length, body in *out (malloc'ed) */
static long get_info(unsigned short port, char **out, char *status, size_t status_len)
{
    struct sockaddr_in a;
    char req[256], *buf = (char *) malloc(65536), *body;
    long total = 0, clen = -1;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct timeval tv = { 3, 0 };
    *out = NULL;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (connect(s, (struct sockaddr *) &a, sizeof(a))) { close(s); free(buf); return -1; }
    snprintf(req, sizeof(req), "GET /info RTSP/1.0\r\nCSeq: 1\r\nX-Apple-ProtocolVersion: 1\r\nUser-Agent: AirPlay/900.0\r\n\r\n");
    send(s, req, strlen(req), 0);
    for (;;) {
        ssize_t n = recv(s, buf + total, 65535 - total, 0);
        if (n <= 0) break;
        total += n;
        buf[total] = 0;
        body = strstr(buf, "\r\n\r\n");
        if (body) {
            char *cl = strcasestr(buf, "Content-Length:");
            clen = cl ? atol(cl + 15) : 0;
            if (total - (body + 4 - buf) >= clen) break;
        }
    }
    close(s);
    body = strstr(buf, "\r\n\r\n");
    if (!body) { free(buf); return -1; }
    snprintf(status, status_len, "%.*s", (int) strcspn(buf, "\r\n"), buf);
    *out = (char *) malloc((size_t) clen + 1);
    memcpy(*out, body + 4, (size_t) clen);
    free(buf);
    return clen;
}

static void run(const char *dir, unsigned short port, char *pk_out, size_t pk_len)
{
    xc_config cfg;
    xc_callbacks cb;
    xc_receiver *r;
    const service *ap, *raop;
    char *body = NULL, status[128] = "";
    long n;
    char expect_raop[160];

    nservices = 0;
    unpublished = 0;
    memset(&cfg, 0, sizeof(cfg));
    cfg.name = "xc unit test";
    memcpy(cfg.device_id, "\x02\x11\x22\x33\x44\x55", 6);
    cfg.display_width = 1920;
    cfg.display_height = 1080;
    cfg.display_refresh = 60;
    cfg.max_fps = 60;
    cfg.allow_h265 = 1;
    cfg.port = port;
    cfg.data_dir = dir;
    memset(&cb, 0, sizeof(cb));
    cb.mdns_publish = rec_publish;
    cb.mdns_unpublish_all = rec_unpublish;
    cb.on_log = log_cb;

    r = xc_create(&cfg, &cb);
    CHECK(r != NULL, "xc_create");
    if (!r) return;
    CHECK(xc_start(r) == 0, "xc_start");

    ap = find("_airplay._tcp");
    raop = find("_raop._tcp");
    CHECK(nservices == 2 && ap && raop, "published _airplay._tcp and _raop._tcp");
    if (ap && raop) {
        snprintf(expect_raop, sizeof(expect_raop), "021122334455@%s", cfg.name);
        CHECK(!strcmp(ap->name, cfg.name), "airplay instance name = configured name");
        CHECK(!strcmp(raop->name, expect_raop), "raop instance name = <deviceid hex>@<name>");
        CHECK(ap->port == port && raop->port == port, "both on the RTSP port");
        CHECK(txt(ap, "deviceid") && !strcmp(txt(ap, "deviceid"), "02:11:22:33:44:55"), "airplay deviceid");
        CHECK(txt(ap, "features") && strchr(txt(ap, "features"), ','), "airplay features (two 32-bit words)");
        CHECK(txt(ap, "pk") && strlen(txt(ap, "pk")) == 64, "airplay pk = 32-byte ed25519 key in hex");
        CHECK(txt(ap, "model") && txt(ap, "srcvers") && txt(ap, "flags") && txt(ap, "pi"), "airplay model/srcvers/flags/pi");
        CHECK(txt(raop, "ch") && txt(raop, "cn") && txt(raop, "et") && txt(raop, "pk") && txt(raop, "ft"),
              "raop ch/cn/et/pk/ft");
        CHECK(txt(raop, "pk") && txt(ap, "pk") && !strcmp(txt(raop, "pk"), txt(ap, "pk")), "same pk in both services");
        printf("       _airplay._tcp TXT:");
        for (size_t i = 0; i < ap->count; i++) printf(" %s=%s", ap->key[i], ap->value[i]);
        printf("\n       _raop._tcp TXT:");
        for (size_t i = 0; i < raop->count; i++) printf(" %s=%s", raop->key[i], raop->value[i]);
        printf("\n");
        if (pk_out && txt(ap, "pk")) snprintf(pk_out, pk_len, "%s", txt(ap, "pk"));
    }

    n = get_info(port, &body, status, sizeof(status));
    printf("       GET /info -> \"%s\", %ld bytes\n", status, n);
    CHECK(n > 8 && strstr(status, "200"), "GET /info answers 200 with a body");
    if (n > 8) {
        plist_t root = NULL, node;
        CHECK(!memcmp(body, "bplist00", 8), "body is a binary plist");
        plist_from_bin(body, (uint32_t) n, &root);
        CHECK(root && plist_get_node_type(root) == PLIST_DICT, "plist parses as a dict");
        if (root) {
            char *s = NULL;
            uint64_t u = 0;
            node = plist_dict_get_item(root, "name");
            if (node) plist_get_string_val(node, &s);
            CHECK(s && !strcmp(s, cfg.name), "info.name");
            free(s); s = NULL;
            node = plist_dict_get_item(root, "deviceID");
            if (node) plist_get_string_val(node, &s);
            CHECK(s && !strcmp(s, "02:11:22:33:44:55"), "info.deviceID");
            free(s); s = NULL;
            node = plist_dict_get_item(root, "features");
            if (node) plist_get_uint_val(node, &u);
            CHECK(u != 0, "info.features");
            node = plist_dict_get_item(root, "displays");
            if (node && plist_array_get_size(node) > 0) {
                plist_t d = plist_array_get_item(node, 0), w = plist_dict_get_item(d, "widthPixels"),
                        h = plist_dict_get_item(d, "heightPixels");
                uint64_t wv = 0, hv = 0;
                if (w) plist_get_uint_val(w, &wv);
                if (h) plist_get_uint_val(h, &hv);
                printf("       displays[0] = %llux%llu\n", (unsigned long long) wv, (unsigned long long) hv);
                CHECK(wv == 1920 && hv == 1080, "info.displays[0] = advertised 1920x1080");
            } else {
                CHECK(0, "info.displays present");
            }
            plist_free(root);
        }
    }
    free(body);

    xc_stop(r);
    CHECK(unpublished >= 1, "xc_stop calls mdns_unpublish_all");
    xc_destroy(r);
    CHECK(1, "xc_destroy returned");
}

int main(void)
{
    char dir[] = "/tmp/xc-test-XXXXXX";
    char pk1[80] = "", pk2[80] = "";
    const char *tmp = getenv("TMPDIR");
    char tmpl[512];
    unsigned short port = (unsigned short) (20000 + (getpid() % 20000));
    snprintf(tmpl, sizeof(tmpl), "%s/xc-test-XXXXXX", tmp && tmp[0] ? tmp : "/tmp");
    if (!mkdtemp(tmpl)) { snprintf(tmpl, sizeof(tmpl), "%s", dir); if (!mkdtemp(tmpl)) return 1; }
    printf("%s, data_dir %s, port %u\n", xc_version_string(), tmpl, port);
    CHECK(xc_api_version() == 1, "API version 1");
    {
        int64_t a = xc_now_ns(), b;
        usleep(10000);
        b = xc_now_ns();
        CHECK(b - a >= 9000000 && b - a < 500000000, "xc_now_ns is monotonic nanoseconds");
    }
    printf("first start\n");
    run(tmpl, port, pk1, sizeof(pk1));
    printf("second start (identity must persist)\n");
    run(tmpl, port, pk2, sizeof(pk2));
    CHECK(pk1[0] && !strcmp(pk1, pk2), "pk identical across restarts (identity persisted in data_dir)");
    printf(failures ? "\n%d FAILURES\n" : "\nall checks passed\n", failures);
    return failures ? 1 : 0;
}
