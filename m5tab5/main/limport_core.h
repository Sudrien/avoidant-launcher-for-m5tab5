/*
 * limport_core.h -- reading M5Launcher's saved Wi-Fi networks, the parts
 * that need no ESP-IDF: its config.conf, the AES-128-CBC its passwords
 * are in, and finding the key.
 *
 * M5Launcher (bmorcelli/Launcher) keeps its networks in /config.conf on
 * the card, `[ { "wifi": [ { "ssid", "pwd", "secure" }, ... ] }, ... ]`,
 * each pwd = base64( AES-128-CBC( passphrase + PKCS#7 ) ) under a key
 * built into its firmware by CI and under a fixed IV "LauncherWifiKey!".
 * Where the key is not published it is found, not stored: every
 * printable run in Launcher's own .bin is a candidate, and the right one
 * is the one that decrypts the config's entries to valid PKCS#7 over
 * printable text. That needs no known plaintext and survives the key
 * being rotated. Reimplemented from Defeatist's launcher_import.c (its
 * 5xxx-6xxx), which read it from a partition; here it is a file.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LI_MARKER       "LauncherWifiKey!"     /* the IV, and a .bin's mark */
#define LI_SSID_MAX     32
#define LI_CT_MAX       80                      /* a 64-hex PSK + a block of padding */
#define LI_MIN_CAND     8
#define LI_MAX_CAND     64

/* One network that has a password to decrypt. */
typedef struct {
    char    ssid[LI_SSID_MAX + 1];
    uint8_t ct[LI_CT_MAX];
    size_t  ct_len;
} li_net_t;

/*
 * The networks in the first "wifi" array in `json`, up to `cap`.
 * Returns how many are usable (a name, "secure" not false, a password
 * that is base64 of whole AES blocks), or -1 if there is no "wifi" array
 * or the text is damaged before it. *skipped counts the entries passed
 * over: open networks, no name or a name over 32 bytes, an undecodable
 * password, or past `cap`.
 */
int li_parse_config(const char *json, size_t len, li_net_t *nets, int cap, int *skipped);

/* Base64 into o (cap bytes). Bytes, or -1 if it does not fit. Skips
 * anything outside the alphabet; stops at '='. */
int li_b64_decode(const char *s, uint8_t *o, size_t cap);

/*
 * Decrypt one network's password with `key` into out (cap > the
 * password's length), NUL-terminated. Its length, or -1 when the
 * padding is wrong or a byte is not printable ASCII.
 */
int li_decrypt_net(const uint8_t key[16], const li_net_t *n, char *out, size_t cap);

/* Does `key` decrypt every one of the nets, as a passphrase (8 or more)? */
bool li_key_ok(const uint8_t key[16], const li_net_t *nets, int n);

/* A 64-hex value is a PSK; 8..63 characters is a passphrase; else no. */
bool li_classify(const char *s, bool *is_psk);

/*
 * Streaming key search over the bytes of a file, however it is cut into
 * reads. A run of printable bytes (up to LI_MAX_CAND) ends at the first
 * byte that is not, or when it is full; each of its suffixes of
 * LI_MIN_CAND or more is a candidate, built into a key by cycling it
 * through 16 bytes. Feed every chunk, then li_scan_end(); a true return
 * from either has put the key in key_out.
 */
typedef struct {
    uint8_t run[LI_MAX_CAND];
    size_t  rl;
} li_scan_t;

void li_scan_init(li_scan_t *s);
bool li_scan_feed(li_scan_t *s, const uint8_t *buf, size_t n,
                  const li_net_t *nets, int nn, uint8_t key_out[16]);
bool li_scan_end(li_scan_t *s, const li_net_t *nets, int nn, uint8_t key_out[16]);

/* Does a file carry LI_MARKER? Feed its chunks in order; true once seen. */
typedef struct { size_t k; } li_marker_t;
void li_marker_init(li_marker_t *m);
bool li_marker_feed(li_marker_t *m, const uint8_t *buf, size_t n);

#ifdef __cplusplus
}
#endif
