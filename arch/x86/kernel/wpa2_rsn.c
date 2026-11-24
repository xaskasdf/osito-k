#include "wpa2_rsn.h"
#include <string.h>

const unsigned char wpa2_assoc_rsn[22] = {
    48,20,1,0,0,15,172,4,1,0,0,15,172,4,1,0,0,15,172,2,0,0
};
static unsigned rsn_u16(const unsigned char *p) { return p[0] | ((unsigned)p[1] << 8); }

const char *wpa2_rsn_select(const unsigned char *ie, unsigned n, enum wpa2_cipher *group)
{
    static const unsigned char ccmp[4] = {0, 0x0f, 0xac, 4}, psk[4] = {0, 0x0f, 0xac, 2};
    if (group) *group = WPA2_CIPHER_NONE;
    if (!ie || n < 8 || n > 255 || rsn_u16(ie) != 1) return "malformed RSN element";
    if (memcmp(ie + 2, ccmp, 3) || (ie[5] != WPA2_CIPHER_CCMP && ie[5] != WPA2_CIPHER_TKIP))
        return "unsupported RSN group cipher";
    unsigned pos = 6;
    for (unsigned list = 0; list < 2; list++) {
        if (n - pos < 2) return "truncated RSN suite count";
        unsigned count = rsn_u16(ie + pos); pos += 2;
        if (!count || count > (n - pos) / 4) return "invalid RSN suite list";
        int found = 0;
        for (unsigned i = 0; i < count; i++, pos += 4)
            if (!memcmp(ie + pos, list ? psk : ccmp, 4)) found = 1;
        if (!found) return list ? "WPA2-PSK is not offered" : "CCMP pairwise cipher is not offered";
    }
    if (n == pos) goto accepted;
    if (n - pos < 2) return "truncated RSN capabilities";
    unsigned caps = rsn_u16(ie + pos); pos += 2;
    if (caps & (1U << 6)) return "required protected management frames are not implemented";
    if (n == pos) goto accepted;
    if (n - pos < 2) return "truncated RSN PMKID count";
    unsigned count = rsn_u16(ie + pos); pos += 2;
    if (count > (n - pos) / 16) return "truncated RSN PMKID list";
    pos += count * 16;
    if (n != pos && n - pos != 4) return "invalid RSN optional fields";
accepted:
    if (group) *group = (enum wpa2_cipher)ie[5];
    return 0;
}

const char *wpa2_rsn_check(const unsigned char *ie, unsigned n)
{
    return wpa2_rsn_select(ie, n, 0);
}

int wpa2_rsn_assoc(unsigned char out[22], const unsigned char *ie, unsigned n)
{
    enum wpa2_cipher group;
    if (!out || wpa2_rsn_select(ie, n, &group)) return -1;
    memcpy(out, wpa2_assoc_rsn, sizeof(wpa2_assoc_rsn));
    out[7] = group;
    return 0;
}
