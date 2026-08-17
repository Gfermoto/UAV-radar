/**
 * @file WebCredentials.cpp
 */
#include "WebCredentials.h"
#include "Config.h"
#include <Preferences.h>
#include <ctype.h>
#include <string.h>
#include <strings.h>

namespace WebCredentials {

static char s_cachedUser[WEB_CRED_USER_MAX + 1];
static char s_cachedPass[WEB_CRED_PASS_MAX];
static bool s_haveCache = false;

static void cacheSet(const char *user, const char *pass) {
    strncpy(s_cachedUser, user, sizeof(s_cachedUser) - 1);
    s_cachedUser[sizeof(s_cachedUser) - 1] = '\0';
    strncpy(s_cachedPass, pass, sizeof(s_cachedPass) - 1);
    s_cachedPass[sizeof(s_cachedPass) - 1] = '\0';
    s_haveCache = true;
}

static bool decodeBase64(const char *in, size_t inLen, uint8_t *out, size_t outCap, size_t *outLen) {
    static const int8_t T[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-2,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
    };
    if (!in || !out || !outLen || outCap == 0) return false;
    size_t o = 0;
    uint32_t buf = 0;
    int nbits = 0;
    for (size_t i = 0; i < inLen; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '=' || c == '\r' || c == '\n' || c == ' ') break;
        int8_t v = T[c];
        if (v < 0) return false;
        buf = (buf << 6) | (uint32_t)v;
        nbits += 6;
        if (nbits >= 8) {
            nbits -= 8;
            if (o >= outCap) return false;
            out[o++] = (uint8_t)((buf >> nbits) & 0xFF);
        }
    }
    *outLen = o;
    return true;
}

bool ctEq(const char *a, const char *b) {
    if (!a || !b) return false;
    size_t la = strlen(a), lb = strlen(b);
    size_t n = la > lb ? la : lb;
    uint8_t diff = (uint8_t)(la ^ lb);
    for (size_t i = 0; i < n; i++) {
        unsigned char ca = i < la ? (unsigned char)a[i] : 0;
        unsigned char cb = i < lb ? (unsigned char)b[i] : 0;
        diff |= (uint8_t)(ca ^ cb);
    }
    return diff == 0;
}

bool validateUser(const char *user) {
    if (!user) return false;
    size_t n = strlen(user);
    if (n < 1 || n > WEB_CRED_USER_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        char c = user[i];
        if (!(isalnum((unsigned char)c) || c == '_' || c == '.' || c == '-')) return false;
    }
    return true;
}

bool validatePass(const char *pass) {
    if (!pass) return false;
    size_t n = strlen(pass);
    if (n < 8 || n >= WEB_CRED_PASS_MAX) return false;
    // Basic auth is user:pass — colon in password is ambiguous for parsers.
    for (size_t i = 0; i < n; i++) {
        if (pass[i] == ':') return false;
    }
    return true;
}

bool load(char *user, size_t userLen, char *pass, size_t passLen) {
    if (!user || userLen < 2 || !pass || passLen < 2) return false;
    if (s_haveCache) {
        strncpy(user, s_cachedUser, userLen - 1);
        user[userLen - 1] = '\0';
        strncpy(pass, s_cachedPass, passLen - 1);
        pass[passLen - 1] = '\0';
        return true;
    }
    Preferences prefs;
    if (!prefs.begin("rtspmic", true)) {
        strncpy(user, WEB_UI_USER, userLen - 1);
        user[userLen - 1] = '\0';
        strncpy(pass, WEB_UI_PASSWORD, passLen - 1);
        pass[passLen - 1] = '\0';
        cacheSet(user, pass);
        return true;
    }
    // Missing keys log ERROR every getString — use defaults without re-query spam.
    String u = prefs.isKey("web_user") ? prefs.getString("web_user", WEB_UI_USER)
                                       : String(WEB_UI_USER);
    String p = prefs.isKey("web_pass") ? prefs.getString("web_pass", WEB_UI_PASSWORD)
                                       : String(WEB_UI_PASSWORD);
    prefs.end();
    strncpy(user, u.c_str(), userLen - 1);
    user[userLen - 1] = '\0';
    strncpy(pass, p.c_str(), passLen - 1);
    pass[passLen - 1] = '\0';
    cacheSet(user, pass);
    return true;
}

bool save(const char *user, const char *pass) {
    if (!validateUser(user) || !validatePass(pass)) return false;
    Preferences prefs;
    if (!prefs.begin("rtspmic", false)) return false;
    bool ok = prefs.putString("web_user", user) && prefs.putString("web_pass", pass);
    prefs.end();
    if (ok) cacheSet(user, pass);
    return ok;
}

bool parseBasicAuthHeader(const char *authHeader,
                          char *outUser, size_t userLen,
                          char *outPass, size_t passLen) {
    if (!authHeader || !outUser || !outPass || userLen < 2 || passLen < 2) return false;
    outUser[0] = '\0';
    outPass[0] = '\0';

    const char *p = authHeader;
    while (*p == ' ' || *p == '\t') p++;
    // Skip "Authorization:" if present
    if (strncasecmp(p, "Authorization:", 14) == 0) {
        p += 14;
        while (*p == ' ' || *p == '\t') p++;
    }
    if (strncasecmp(p, "Basic ", 6) != 0) return false;
    p += 6;
    while (*p == ' ' || *p == '\t') p++;

    size_t b64Len = 0;
    while (p[b64Len] && p[b64Len] != '\r' && p[b64Len] != '\n') b64Len++;
    while (b64Len > 0 && (p[b64Len - 1] == ' ' || p[b64Len - 1] == '\t')) b64Len--;
    if (b64Len == 0 || b64Len > 128) return false;

    uint8_t decoded[WEB_CRED_USER_MAX + WEB_CRED_PASS_MAX + 4];
    size_t decLen = 0;
    if (!decodeBase64(p, b64Len, decoded, sizeof(decoded) - 1, &decLen)) return false;
    decoded[decLen] = '\0';

    char *colon = (char *)memchr(decoded, ':', decLen);
    if (!colon) return false;
    size_t uLen = (size_t)(colon - (char *)decoded);
    size_t pLen = decLen - uLen - 1;
    if (uLen == 0 || uLen >= userLen || pLen == 0 || pLen >= passLen) return false;
    memcpy(outUser, decoded, uLen);
    outUser[uLen] = '\0';
    memcpy(outPass, colon + 1, pLen);
    outPass[pLen] = '\0';
    return true;
}

bool verifyBasicAuth(const char *authHeader, const char *user, const char *pass) {
    char gotUser[WEB_CRED_USER_MAX + 1];
    char gotPass[WEB_CRED_PASS_MAX];
    if (!parseBasicAuthHeader(authHeader, gotUser, sizeof(gotUser), gotPass, sizeof(gotPass))) {
        return false;
    }
    // Без short-circuit: оба сравнения всегда выполняются (CT по обоим полям)
    const bool u = ctEq(gotUser, user);
    const bool p = ctEq(gotPass, pass);
    return u & p;
}

bool isDefaultPassword() {
    char user[WEB_CRED_USER_MAX + 1];
    char pass[WEB_CRED_PASS_MAX];
    if (!load(user, sizeof(user), pass, sizeof(pass))) return true;
    return ctEq(pass, WEB_UI_DEFAULT_PASSWORD);
}

}  // namespace WebCredentials
