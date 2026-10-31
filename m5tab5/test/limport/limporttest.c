/* SPDX-License-Identifier: MIT
 * limport_core.c on the host, against fixtures from gen_fixture.py. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "limport_core.h"

static int checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static char *slurp(const char *dir, const char *name, size_t *len)
{
    char p[512];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "rb");
    if (!f) { printf("cannot open %s\n", p); exit(2); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc(n + 1);
    if (fread(b, 1, n, f) != (size_t)n) exit(2);
    b[n] = 0; fclose(f); *len = n;
    return b;
}

static char exp_ssid[16][64], exp_pw[16][80];
static int n_exp;
static uint8_t exp_key[16];

static void load_expect(const char *dir)
{
    size_t len; char *t = slurp(dir, "expect.txt", &len);
    for (char *line = strtok(t, "\n"); line; line = strtok(NULL, "\n")) {
        char *tab = strchr(line, '\t'); *tab = 0;
        if (!strcmp(line, "KEY")) {
            for (int i = 0; i < 16; i++) { unsigned v; sscanf(tab + 1 + 2 * i, "%2x", &v); exp_key[i] = (uint8_t)v; }
        } else {
            snprintf(exp_ssid[n_exp], sizeof exp_ssid[0], "%s", line);
            snprintf(exp_pw[n_exp], sizeof exp_pw[0], "%s", tab + 1);
            n_exp++;
        }
    }
    free(t);
}

static bool scan_file(const char *dir, const char *name, size_t chunk, const li_net_t *nets,
                      int nn, uint8_t *key)
{
    size_t len; char *b = slurp(dir, name, &len);
    li_scan_t s; li_scan_init(&s);
    bool found = false;
    for (size_t off = 0; off < len && !found; off += chunk)
        found = li_scan_feed(&s, (uint8_t *)b + off, len - off < chunk ? len - off : chunk, nets, nn, key);
    if (!found) found = li_scan_end(&s, nets, nn, key);
    free(b);
    return found;
}

static bool marker_in(const char *dir, const char *name, size_t chunk)
{
    size_t len; char *b = slurp(dir, name, &len);
    li_marker_t m; li_marker_init(&m);
    bool found = false;
    for (size_t off = 0; off < len && !found; off += chunk)
        found = li_marker_feed(&m, (uint8_t *)b + off, len - off < chunk ? len - off : chunk);
    free(b);
    return found;
}

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    const char *dir = argv[1];
    load_expect(dir);

    /* both layouts of the config parse to the same networks */
    const char *cfgs[] = { "config.conf", "config_min.conf" };
    for (int c = 0; c < 2; c++) {
        size_t len; char *j = slurp(dir, cfgs[c], &len);
        li_net_t nets[16]; int skipped = -1;
        const int n = li_parse_config(j, len, nets, 16, &skipped);
        CHECK(n == n_exp);                      /* the open net and the 33-byte SSID are not among them */
        CHECK(skipped == 2);
        for (int i = 0; i < n && i < n_exp; i++) CHECK(!strcmp(nets[i].ssid, i == 1 ? "Caf\xc3\xa9 \xe2\x98\x95" : exp_ssid[i]));

        /* the key decrypts every one of them */
        for (int i = 0; i < n; i++) {
            char pw[LI_CT_MAX + 1];
            const int l = li_decrypt_net(exp_key, &nets[i], pw, sizeof pw);
            CHECK(l > 0 && !strcmp(pw, exp_pw[i]));
        }
        CHECK(li_key_ok(exp_key, nets, n));
        uint8_t wrong[16]; memcpy(wrong, exp_key, 16); wrong[7] ^= 1;
        CHECK(!li_key_ok(wrong, nets, n));

        /* the scan finds it, however the file is cut into reads -- 4090 and
         * the key's offset make it straddle a 4096-byte boundary */
        const size_t chunks[] = { 1, 7, 64, 1000, 4096, 100000 };
        for (size_t k = 0; k < sizeof chunks / sizeof chunks[0]; k++) {
            uint8_t key[16] = { 0 };
            CHECK(scan_file(dir, "launcher.bin", chunks[k], nets, n, key));
            CHECK(!memcmp(key, exp_key, 16));
            CHECK(!scan_file(dir, "nokey.bin", chunks[k], nets, n, key));
            CHECK(marker_in(dir, "launcher.bin", chunks[k]));
        }
        /* one network alone is enough to find it */
        uint8_t key1[16] = { 0 };
        CHECK(scan_file(dir, "launcher.bin", 4096, &nets[0], 1, key1) && !memcmp(key1, exp_key, 16));
        free(j);
    }

    /* the pieces */
    bool psk;
    CHECK(li_classify("12345678", &psk) && !psk);
    CHECK(!li_classify("1234567", &psk));
    CHECK(li_classify("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", &psk) && psk);
    CHECK(!li_classify("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdeg", &psk));
    uint8_t o[8];
    CHECK(li_b64_decode("QUJD", o, 8) == 3 && !memcmp(o, "ABC", 3));
    CHECK(li_b64_decode("QUJDREVG", o, 4) == -1);
    CHECK(li_b64_decode("QU\nJD", o, 8) == 3);

    /* damaged and odd documents: refused or empty, never a crash */
    li_net_t nets[4]; int sk;
    const char *bad[] = { "", "[", "{\"wifi\":", "{\"wifi\":[{\"ssid\":\"a\",", "[{\"wifi\":[]}]",
                          "{\"a\":1}", "{\"wifi\":[{\"ssid\":\"\\ud800\"}]}", "{\"wifi\":{}}", "null" };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        const int n = li_parse_config(bad[i], strlen(bad[i]), nets, 4, &sk);
        CHECK(n <= 0);
    }
    CHECK(li_parse_config("[{\"wifi\":[]}]", 13, nets, 4, &sk) == 0);   /* a wifi array, no entries */
    {   /* deep nesting is refused, not recursed into without limit */
        char deep[200]; memset(deep, '[', 150); deep[150] = 0;
        CHECK(li_parse_config(deep, strlen(deep), nets, 4, &sk) == -1);
    }
    {   /* a cap smaller than the list: the rest are skipped, not lost silently */
        size_t len; char *j = slurp(dir, "config_min.conf", &len);
        int skipped; const int n = li_parse_config(j, len, nets, 2, &skipped);
        CHECK(n == 2 && skipped == 2 + (n_exp - 2));
        free(j);
    }

    printf("%d checks, %d failures\n", checks, fails);
    return fails != 0;
}
