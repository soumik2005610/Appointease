/*
 * AppointEase - backend (v3)
 * OPD appointments, X-ray / CT / MRI bookings, bed management,
 * and logins for staff, doctors and patients (each role sees its own portal).
 * A tiny HTTP server in C (POSIX sockets) + SQLite. It serves the frontend and a JSON API.
 *
 * Build:  make
 * Run:    ./appointease        (from the backend/ folder)
 * Open:   http://localhost:8080
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <signal.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <sqlite3.h>

#define PORT 8080
#define REQ_MAX 16384
#define FIELD_MAX 256
#define SESSION_SECONDS 43200      /* a login lasts 12 hours */

static sqlite3 *db;

/* ---------- who is making this request ---------- */
enum { R_NONE = 0, R_ADMIN = 1, R_DOCTOR = 2, R_PATIENT = 4 };
#define ANY_ROLE (R_ADMIN | R_DOCTOR | R_PATIENT)

typedef struct { int role, user_id, doctor_id, patient_id; char name[FIELD_MAX]; } Me;
static Me me;                  /* filled in for every request from the session cookie */
static char cur_token[80];     /* the session cookie sent with this request */
static char set_cookie[220];   /* Set-Cookie header to send back (empty = none) */

/* ---------- growable string buffer ---------- */
typedef struct { char *data; size_t len, cap; } Buf;

static void buf_add(Buf *b, const char *s) {
    size_t n = strlen(s);
    if (b->len + n + 1 > b->cap) {
        b->cap = (b->len + n + 1) * 2;
        b->data = realloc(b->data, b->cap);
    }
    memcpy(b->data + b->len, s, n + 1);
    b->len += n;
}

static void buf_addf(Buf *b, const char *fmt, ...) {
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    buf_add(b, tmp);
}

/* append a JSON-escaped, quoted string */
static void buf_json_str(Buf *b, const char *s) {
    buf_add(b, "\"");
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"') buf_add(b, "\\\"");
        else if (c == '\\') buf_add(b, "\\\\");
        else if (c < 0x20) buf_addf(b, "\\u%04x", c);
        else { char t[2] = {(char)c, 0}; buf_add(b, t); }
    }
    buf_add(b, "\"");
}

/* ---------- HTTP helpers ---------- */
static const char *status_text(int code) {
    switch (code) {
        case 200: return "OK";
        case 201: return "Created";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 409: return "Conflict";
        default:  return "Internal Server Error";
    }
}

static void send_response(int fd, int code, const char *type, const char *body, size_t len) {
    char head[512];
    int n = snprintf(head, sizeof head,
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
        "Cache-Control: no-store\r\n%sConnection: close\r\n\r\n",
        code, status_text(code), type, len, set_cookie);
    send(fd, head, n, 0);
    size_t sent = 0;
    while (sent < len) {
        ssize_t r = send(fd, body + sent, len - sent, 0);
        if (r <= 0) break;
        sent += (size_t)r;
    }
}

static void send_json(int fd, int code, Buf *b) {
    send_response(fd, code, "application/json", b->data ? b->data : "", b->len);
}

static void send_error(int fd, int code, const char *msg) {
    Buf b = {0};
    buf_add(&b, "{\"error\":");
    buf_json_str(&b, msg);
    buf_add(&b, "}");
    send_json(fd, code, &b);
    free(b.data);
}

static void send_ok(int fd) {
    Buf b = {0};
    buf_add(&b, "{\"ok\":true}");
    send_json(fd, 200, &b);
    free(b.data);
}

static void send_id(int fd, int id) {   /* reply to an INSERT with the new row's id */
    Buf b = {0};
    buf_addf(&b, "{\"id\":%d}", id);
    send_json(fd, 201, &b);
    free(b.data);
}

/* ---------- request parameter parsing (a=b&c=d) ---------- */
static void url_decode(char *dst, const char *src, size_t max) {
    size_t i = 0;
    while (*src && i + 1 < max) {
        if (*src == '+') { dst[i++] = ' '; src++; }
        else if (*src == '%' && isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2])) {
            char hex[3] = {src[1], src[2], 0};
            dst[i++] = (char)strtol(hex, NULL, 16);
            src += 3;
        } else dst[i++] = *src++;
    }
    dst[i] = 0;
}

/* find key in "a=b&c=d"; returns 1 if found */
static int find_param(const char *qs, const char *key, char *out, size_t outsz) {
    size_t klen = strlen(key);
    while (qs && *qs) {
        if (strncmp(qs, key, klen) == 0 && qs[klen] == '=') {
            const char *v = qs + klen + 1;
            size_t vlen = strcspn(v, "&");
            char raw[FIELD_MAX * 3];
            if (vlen >= sizeof raw) vlen = sizeof raw - 1;
            memcpy(raw, v, vlen);
            raw[vlen] = 0;
            url_decode(out, raw, outsz);
            return 1;
        }
        qs = strchr(qs, '&');
        if (qs) qs++;
    }
    return 0;
}

/* look in the query string first, then in the request body */
static int get_param(const char *query, const char *body, const char *key, char *out, size_t outsz) {
    out[0] = 0;
    return find_param(query, key, out, outsz) || find_param(body, key, out, outsz);
}

static int valid_date(const char *s) {   /* YYYY-MM-DD */
    int y, m, d;
    if (strlen(s) != 10 || sscanf(s, "%d-%d-%d", &y, &m, &d) != 3) return 0;
    return y >= 2000 && y <= 2100 && m >= 1 && m <= 12 && d >= 1 && d <= 31;
}

static int valid_time(const char *s) {   /* HH:MM */
    int h, m;
    if (strlen(s) != 5 || sscanf(s, "%d:%d", &h, &m) != 2) return 0;
    return h >= 0 && h <= 23 && m >= 0 && m <= 59;
}

/* ---------- small database helpers ---------- */
static sqlite3_stmt *prep(const char *sql) {
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    return st;
}

/* run a query that returns one number; a and b are bound to ?1 and ?2 */
static int scalar_int2(const char *sql, int a, int b) {
    sqlite3_stmt *st = prep(sql);
    int n = 0;
    sqlite3_bind_int(st, 1, a);
    sqlite3_bind_int(st, 2, b);
    if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return n;
}

static int scalar_int(const char *sql, int id) { return scalar_int2(sql, id, 0); }

/* run a statement that takes one id in ?1 and returns nothing */
static void exec_id(const char *sql, int id) {
    sqlite3_stmt *st = prep(sql);
    sqlite3_bind_int(st, 1, id);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

/* count rows for (id, date, time): used for conflict detection */
static int count_rows(const char *sql, int id, const char *date, const char *time) {
    sqlite3_stmt *st = prep(sql);
    int n = 0;
    sqlite3_bind_int(st, 1, id);
    sqlite3_bind_text(st, 2, date, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, time, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return n;
}

/* send the rows of a SELECT as a JSON array; column names become the keys */
static void query_json(int fd, sqlite3_stmt *st) {
    Buf b = {0};
    int cols = sqlite3_column_count(st);
    buf_add(&b, "[");
    int first = 1;
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (!first) buf_add(&b, ",");
        first = 0;
        buf_add(&b, "{");
        for (int i = 0; i < cols; i++) {
            if (i) buf_add(&b, ",");
            buf_json_str(&b, sqlite3_column_name(st, i));
            buf_add(&b, ":");
            if (sqlite3_column_type(st, i) == SQLITE_INTEGER)
                buf_addf(&b, "%d", sqlite3_column_int(st, i));
            else
                buf_json_str(&b, (const char *)sqlite3_column_text(st, i));
        }
        buf_add(&b, "}");
    }
    buf_add(&b, "]");
    sqlite3_finalize(st);
    send_json(fd, 200, &b);
    free(b.data);
}

/* ---------- passwords: salted SHA-256, repeated 10,000 times ---------- */
static const uint32_t K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(uint32_t st[8], const unsigned char blk[64]) {
    uint32_t w[64], a, b, c, d, e, f, g, h;
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)blk[i*4] << 24) | ((uint32_t)blk[i*4+1] << 16) | ((uint32_t)blk[i*4+2] << 8) | blk[i*4+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROTR(w[i-15], 7) ^ ROTR(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = ROTR(w[i-2], 17) ^ ROTR(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a = st[0]; b = st[1]; c = st[2]; d = st[3]; e = st[4]; f = st[5]; g = st[6]; h = st[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ROTR(e, 6) ^ ROTR(e, 11) ^ ROTR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (ROTR(a, 2) ^ ROTR(a, 13) ^ ROTR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d; st[4] += e; st[5] += f; st[6] += g; st[7] += h;
}

static void sha256(const unsigned char *data, size_t len, unsigned char out[32]) {
    uint32_t st[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    size_t i = 0;
    for (; i + 64 <= len; i += 64) sha256_block(st, data + i);

    unsigned char tail[128];
    size_t rem = len - i;
    memcpy(tail, data + i, rem);
    tail[rem++] = 0x80;
    size_t padded = (rem <= 56) ? 64 : 128;
    memset(tail + rem, 0, padded - rem);
    uint64_t bits = (uint64_t)len * 8;
    for (int j = 0; j < 8; j++) tail[padded - 1 - j] = (unsigned char)(bits >> (8 * j));
    sha256_block(st, tail);
    if (padded == 128) sha256_block(st, tail + 64);

    for (int j = 0; j < 8; j++) {
        out[j*4] = (unsigned char)(st[j] >> 24); out[j*4+1] = (unsigned char)(st[j] >> 16);
        out[j*4+2] = (unsigned char)(st[j] >> 8); out[j*4+3] = (unsigned char)st[j];
    }
}

static void to_hex(const unsigned char *in, size_t n, char *out) {
    static const char *digits = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out[i*2] = digits[in[i] >> 4]; out[i*2+1] = digits[in[i] & 15]; }
    out[n*2] = 0;
}

/* nbytes of randomness from the operating system, written as hex */
static void random_hex(char *out, int nbytes) {
    unsigned char r[64];
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f || fread(r, 1, (size_t)nbytes, f) != (size_t)nbytes) {
        fprintf(stderr, "Cannot read /dev/urandom\n");
        exit(1);
    }
    fclose(f);
    to_hex(r, (size_t)nbytes, out);
}

static void hash_password(const char *salt, const char *password, char out[65]) {
    unsigned char buf[256], h[32];
    size_t sl = strlen(salt), pl = strlen(password);
    memcpy(buf, salt, sl);
    memcpy(buf + sl, password, pl);
    sha256(buf, sl + pl, h);
    for (int i = 0; i < 10000; i++) {
        memcpy(buf, h, 32);
        memcpy(buf + 32, salt, sl);
        memcpy(buf + 32 + sl, password, pl);
        sha256(buf, 32 + sl + pl, h);
    }
    to_hex(h, 32, out);
}

static int safe_eq(const char *a, const char *b) {   /* compares without stopping at the first difference */
    size_t la = strlen(a), lb = strlen(b);
    if (la != lb) return 0;
    unsigned char diff = 0;
    for (size_t i = 0; i < la; i++) diff |= (unsigned char)(a[i] ^ b[i]);
    return diff == 0;
}

/* ---------- users and sessions ---------- */
static const char *role_name(int r) {
    return r == R_ADMIN ? "admin" : r == R_DOCTOR ? "doctor" : r == R_PATIENT ? "patient" : "";
}

static int role_from_text(const char *s) {
    if (!strcmp(s, "admin")) return R_ADMIN;
    if (!strcmp(s, "doctor")) return R_DOCTOR;
    if (!strcmp(s, "patient")) return R_PATIENT;
    return R_NONE;
}

/* lower-cases the username and returns an error message, or NULL if both are fine */
static const char *check_credentials(char *username, const char *password) {
    size_t n = strlen(username);
    if (n < 3 || n > 30) return "Username must be 3 to 30 characters.";
    for (size_t i = 0; i < n; i++) {
        username[i] = (char)tolower((unsigned char)username[i]);
        if (!isalnum((unsigned char)username[i]) && username[i] != '_' && username[i] != '.')
            return "Username can only use letters, numbers, _ and .";
    }
    size_t pl = strlen(password);
    if (pl < 6 || pl > 64) return "Password must be 6 to 64 characters.";
    return NULL;
}

static int username_taken(const char *username) {
    sqlite3_stmt *st = prep("SELECT COUNT(*) FROM users WHERE username=?");
    int n = 0;
    sqlite3_bind_text(st, 1, username, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return n > 0;
}

/* returns the new user's id, or 0 on failure */
static int make_user(const char *username, const char *password, const char *role, int doctor_id, int patient_id) {
    char salt[33], hash[65];
    random_hex(salt, 16);
    hash_password(salt, password, hash);
    sqlite3_stmt *st = prep("INSERT INTO users(username, pass_hash, salt, role, doctor_id, patient_id) VALUES(?,?,?,?,?,?)");
    sqlite3_bind_text(st, 1, username, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, hash, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, salt, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, role, -1, SQLITE_TRANSIENT);
    if (doctor_id > 0) sqlite3_bind_int(st, 5, doctor_id); else sqlite3_bind_null(st, 5);
    if (patient_id > 0) sqlite3_bind_int(st, 6, patient_id); else sqlite3_bind_null(st, 6);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? (int)sqlite3_last_insert_rowid(db) : 0;
}

/* fill `me` from a session token (leaves role = R_NONE if the token is unknown or expired) */
static void load_session(const char *token) {
    memset(&me, 0, sizeof me);
    sqlite3_stmt *st = prep(
        "SELECT u.id, u.role, COALESCE(u.doctor_id,0), COALESCE(u.patient_id,0),"
        " COALESCE((SELECT name FROM doctors WHERE id=u.doctor_id),"
        "          (SELECT name FROM patients WHERE id=u.patient_id), u.username) "
        "FROM sessions s JOIN users u ON u.id = s.user_id "
        "WHERE s.token=?1 AND s.created > CAST(strftime('%s','now') AS INTEGER) - 43200");
    sqlite3_bind_text(st, 1, token, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) {
        me.user_id = sqlite3_column_int(st, 0);
        me.role = role_from_text((const char *)sqlite3_column_text(st, 1));
        me.doctor_id = sqlite3_column_int(st, 2);
        me.patient_id = sqlite3_column_int(st, 3);
        snprintf(me.name, sizeof me.name, "%s", (const char *)sqlite3_column_text(st, 4));
    }
    sqlite3_finalize(st);
}

static void start_session(int user_id) {
    char token[65];
    random_hex(token, 32);
    sqlite3_exec(db, "DELETE FROM sessions WHERE created < CAST(strftime('%s','now') AS INTEGER) - 43200", NULL, NULL, NULL);
    sqlite3_stmt *st = prep("INSERT INTO sessions(token, user_id, created) VALUES(?, ?, CAST(strftime('%s','now') AS INTEGER))");
    sqlite3_bind_text(st, 1, token, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 2, user_id);
    sqlite3_step(st);
    sqlite3_finalize(st);
    snprintf(set_cookie, sizeof set_cookie,
             "Set-Cookie: session=%s; Path=/; HttpOnly; SameSite=Lax; Max-Age=%d\r\n", token, SESSION_SECONDS);
    load_session(token);
}

/* pick the session token out of the Cookie header */
static void token_from_request(const char *req, char *tok, size_t size) {
    tok[0] = 0;
    const char *hdr_end = strstr(req, "\r\n\r\n");
    const char *c = strcasestr(req, "\r\nCookie:");
    if (!c || (hdr_end && c > hdr_end)) return;
    c += 9;
    const char *eol = strstr(c, "\r\n");
    const char *s = strstr(c, "session=");
    if (!s || (eol && s > eol)) return;
    s += 8;
    size_t n = strspn(s, "0123456789abcdef");
    if (n >= size) n = size - 1;
    memcpy(tok, s, n);
    tok[n] = 0;
}

/* returns 1 if the logged-in role is in `mask`; otherwise sends 401 / 403 and returns 0 */
static int allow(int fd, int mask) {
    if (me.role == R_NONE) { send_error(fd, 401, "Please log in."); return 0; }
    if (!(me.role & mask)) { send_error(fd, 403, "You are not allowed to do that."); return 0; }
    return 1;
}

/* ---------- login, register, logout ---------- */
static void send_me(int fd) {
    Buf b = {0};
    buf_add(&b, "{\"role\":");
    buf_json_str(&b, role_name(me.role));
    buf_add(&b, ",\"name\":");
    buf_json_str(&b, me.name);
    buf_add(&b, "}");
    send_json(fd, 200, &b);
    free(b.data);
}

static void login(int fd, const char *q, const char *body) {
    char user[FIELD_MAX], pass[FIELD_MAX], want[32];
    get_param(q, body, "username", user, sizeof user);
    get_param(q, body, "password", pass, sizeof pass);
    get_param(q, body, "role", want, sizeof want);
    for (char *c = user; *c; c++) *c = (char)tolower((unsigned char)*c);

    int uid = 0;
    char stored[80] = "", salt[40] = "", role[20] = "";
    sqlite3_stmt *st = prep("SELECT id, pass_hash, salt, role FROM users WHERE username=?");
    sqlite3_bind_text(st, 1, user, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) {
        uid = sqlite3_column_int(st, 0);
        snprintf(stored, sizeof stored, "%s", (const char *)sqlite3_column_text(st, 1));
        snprintf(salt, sizeof salt, "%s", (const char *)sqlite3_column_text(st, 2));
        snprintf(role, sizeof role, "%s", (const char *)sqlite3_column_text(st, 3));
    }
    sqlite3_finalize(st);

    char hash[65] = "";
    if (uid && strlen(pass) <= 64) hash_password(salt, pass, hash);
    if (!uid || !safe_eq(hash, stored)) return send_error(fd, 401, "Wrong username or password.");

    if (want[0] && strcmp(want, role) != 0) {   /* right password, wrong login tab */
        char msg[100];
        const char *tab = !strcmp(role, "admin") ? "Staff" : !strcmp(role, "doctor") ? "Doctor" : "Patient";
        snprintf(msg, sizeof msg, "This account belongs to the %s tab.", tab);
        return send_error(fd, 401, msg);
    }
    start_session(uid);
    send_me(fd);
}

static void register_patient(int fd, const char *q, const char *body) {
    char name[FIELD_MAX], phone[FIELD_MAX], user[FIELD_MAX], pass[FIELD_MAX];
    get_param(q, body, "name", name, sizeof name);
    get_param(q, body, "phone", phone, sizeof phone);
    get_param(q, body, "username", user, sizeof user);
    get_param(q, body, "password", pass, sizeof pass);
    if (!name[0]) return send_error(fd, 400, "Name is required.");
    const char *bad = check_credentials(user, pass);
    if (bad) return send_error(fd, 400, bad);
    if (username_taken(user)) return send_error(fd, 409, "That username is already taken.");

    sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);
    sqlite3_stmt *st = prep("INSERT INTO patients(name, phone) VALUES(?, ?)");
    sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, phone, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    int patient_id = (int)sqlite3_last_insert_rowid(db);
    int user_id = (rc == SQLITE_DONE) ? make_user(user, pass, "patient", 0, patient_id) : 0;
    sqlite3_exec(db, user_id ? "COMMIT" : "ROLLBACK", NULL, NULL, NULL);
    if (!user_id) return send_error(fd, 500, "Could not register.");

    start_session(user_id);   /* log the new patient in straight away */
    send_me(fd);
}

static void logout(int fd) {
    if (cur_token[0]) {
        sqlite3_stmt *st = prep("DELETE FROM sessions WHERE token=?");
        sqlite3_bind_text(st, 1, cur_token, -1, SQLITE_TRANSIENT);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    memset(&me, 0, sizeof me);
    snprintf(set_cookie, sizeof set_cookie, "Set-Cookie: session=; Path=/; HttpOnly; SameSite=Lax; Max-Age=0\r\n");
    send_ok(fd);
}

/* ---------- doctors and patients (staff only) ---------- */
static void add_doctor(int fd, const char *q, const char *body) {
    char name[FIELD_MAX], spec[FIELD_MAX], user[FIELD_MAX], pass[FIELD_MAX];
    get_param(q, body, "name", name, sizeof name);
    get_param(q, body, "specialization", spec, sizeof spec);
    get_param(q, body, "username", user, sizeof user);
    get_param(q, body, "password", pass, sizeof pass);
    if (!name[0]) return send_error(fd, 400, "Doctor name is required.");
    const char *bad = check_credentials(user, pass);   /* every doctor gets a login */
    if (bad) return send_error(fd, 400, bad);
    if (username_taken(user)) return send_error(fd, 409, "That username is already taken.");

    sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);
    sqlite3_stmt *st = prep("INSERT INTO doctors(name, specialization) VALUES(?, ?)");
    sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, spec, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    int doctor_id = (int)sqlite3_last_insert_rowid(db);
    int ok = (rc == SQLITE_DONE) && make_user(user, pass, "doctor", doctor_id, 0) > 0;
    sqlite3_exec(db, ok ? "COMMIT" : "ROLLBACK", NULL, NULL, NULL);
    if (!ok) return send_error(fd, 500, "Could not save doctor.");
    send_id(fd, doctor_id);
}

static void add_patient(int fd, const char *q, const char *body) {   /* a patient added by staff has no login */
    char name[FIELD_MAX], phone[FIELD_MAX];
    get_param(q, body, "name", name, sizeof name);
    get_param(q, body, "phone", phone, sizeof phone);
    if (!name[0]) return send_error(fd, 400, "Patient name is required.");
    sqlite3_stmt *st = prep("INSERT INTO patients(name, phone) VALUES(?, ?)");
    sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, phone, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) return send_error(fd, 500, "Could not save patient.");
    send_id(fd, (int)sqlite3_last_insert_rowid(db));
}

/* refused while the person still has bookings or is admitted; their login is removed with them */
static void delete_person(int fd, const char *q, const char *body, int is_doctor) {
    char idstr[32];
    get_param(q, body, "id", idstr, sizeof idstr);
    int id = atoi(idstr);
    if (id <= 0) return send_error(fd, 400, "id is required.");

    int used = is_doctor
        ? scalar_int("SELECT COUNT(*) FROM appointments WHERE doctor_id=?1", id)
        : scalar_int("SELECT (SELECT COUNT(*) FROM appointments WHERE patient_id=?1)"
                     " + (SELECT COUNT(*) FROM service_bookings WHERE patient_id=?1)"
                     " + (SELECT COUNT(*) FROM admissions WHERE patient_id=?1 AND discharged_on IS NULL)", id);
    if (used > 0)
        return send_error(fd, 409, "Cannot delete: they still have bookings or an admission. Remove those first.");

    if (is_doctor) {
        exec_id("DELETE FROM sessions WHERE user_id IN (SELECT id FROM users WHERE doctor_id=?1)", id);
        exec_id("DELETE FROM users WHERE doctor_id=?1", id);
        exec_id("DELETE FROM doctors WHERE id=?1", id);
    } else {
        exec_id("DELETE FROM sessions WHERE user_id IN (SELECT id FROM users WHERE patient_id=?1)", id);
        exec_id("DELETE FROM users WHERE patient_id=?1", id);
        exec_id("DELETE FROM admissions WHERE patient_id=?1", id);   /* old discharge records */
        exec_id("DELETE FROM patients WHERE id=?1", id);
    }
    send_ok(fd);
}

/* simulated confirmation messages, printed on the server console (kept from your version) */
static void text_of(const char *sql, int id, char *out, size_t size) {
    sqlite3_stmt *st = prep(sql);
    snprintf(out, size, "%s", "-");
    sqlite3_bind_int(st, 1, id);
    if (sqlite3_step(st) == SQLITE_ROW) snprintf(out, size, "%s", (const char *)sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
}

static void print_confirmation(int is_service, int resource_id, int patient_id, const char *date, const char *time) {
    char resource[FIELD_MAX], patient[FIELD_MAX];
    text_of(is_service ? "SELECT name FROM services WHERE id=?1" : "SELECT name FROM doctors WHERE id=?1",
            resource_id, resource, sizeof resource);
    text_of("SELECT name FROM patients WHERE id=?1", patient_id, patient, sizeof patient);
    printf("\n======================================================\n");
    printf("         APPOINTEASE APPOINTMENT CONFIRMATION\n");
    printf("======================================================\n");
    printf(" Booking type : %s\n", is_service ? "Diagnostic service" : "OPD doctor consult");
    printf(" Patient      : %s\n", patient);
    printf(" %-12s : %s\n", is_service ? "Service" : "Doctor", resource);
    printf(" Date & time  : %s at %s\n", date, time);
    printf("------------------------------------------------------\n");
    printf(" MESSAGE TO PATIENT: Dear %s, your booking for %s on %s at %s is confirmed.\n", patient, resource, date, time);
    printf(" MESSAGE TO %s: Patient %s is scheduled for %s at %s.\n",
           is_service ? "DEPARTMENT" : "DOCTOR", patient, date, time);
    printf("======================================================\n\n");
    fflush(stdout);
}

/* ---------- time-slot bookings ----------
 * is_service = 0 : OPD appointment (the resource is a doctor)
 * is_service = 1 : X-ray / CT / MRI booking (the resource is a machine room)
 * A patient can only book for themselves; staff can book for any patient. */
static void book_slot(int fd, const char *q, const char *body, int is_service) {
    char rid[32], pid[32], date[32], time[32];
    get_param(q, body, is_service ? "service_id" : "doctor_id", rid, sizeof rid);
    get_param(q, body, "patient_id", pid, sizeof pid);
    get_param(q, body, "date", date, sizeof date);
    get_param(q, body, "time", time, sizeof time);
    int resource_id = atoi(rid);
    int patient_id = (me.role == R_PATIENT) ? me.patient_id : atoi(pid);

    if (resource_id <= 0 || patient_id <= 0)
        return send_error(fd, 400, is_service ? "Choose a service and a patient." : "Choose a doctor and a patient.");
    if (!valid_date(date)) return send_error(fd, 400, "Date must look like 2026-12-31.");
    if (!valid_time(time)) return send_error(fd, 400, "Time must look like 09:00.");

    const char *table = is_service ? "service_bookings" : "appointments";
    const char *rcol  = is_service ? "service_id" : "doctor_id";
    char sql[256];

    snprintf(sql, sizeof sql, "SELECT COUNT(*) FROM %s WHERE %s=? AND date=? AND time=?", table, rcol);
    if (count_rows(sql, resource_id, date, time) > 0)
        return send_error(fd, 409, is_service ? "That slot is already booked for this service."
                                              : "This doctor is already booked at that time.");

    snprintf(sql, sizeof sql, "SELECT COUNT(*) FROM %s WHERE patient_id=? AND date=? AND time=?", table);
    if (count_rows(sql, patient_id, date, time) > 0)
        return send_error(fd, 409, "This patient already has a booking at that time.");

    snprintf(sql, sizeof sql, "INSERT INTO %s(%s, patient_id, date, time) VALUES(?,?,?,?)", table, rcol);
    sqlite3_stmt *st = prep(sql);
    sqlite3_bind_int(st, 1, resource_id);
    sqlite3_bind_int(st, 2, patient_id);
    sqlite3_bind_text(st, 3, date, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, time, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) return send_error(fd, 400, "Selected doctor, service or patient does not exist.");
    int new_id = (int)sqlite3_last_insert_rowid(db);
    print_confirmation(is_service, resource_id, patient_id, date, time);
    send_id(fd, new_id);
}

/* staff can cancel anything; a doctor only their own appointments; a patient only their own bookings */
static void cancel_slot(int fd, const char *q, const char *body, int is_service) {
    char idstr[32];
    get_param(q, body, "id", idstr, sizeof idstr);
    int id = atoi(idstr);
    if (id <= 0) return send_error(fd, 400, "id is required.");

    int ok = (me.role == R_ADMIN);
    if (me.role == R_DOCTOR && !is_service)
        ok = scalar_int2("SELECT COUNT(*) FROM appointments WHERE id=?1 AND doctor_id=?2", id, me.doctor_id) > 0;
    if (me.role == R_PATIENT)
        ok = scalar_int2(is_service ? "SELECT COUNT(*) FROM service_bookings WHERE id=?1 AND patient_id=?2"
                                    : "SELECT COUNT(*) FROM appointments WHERE id=?1 AND patient_id=?2",
                         id, me.patient_id) > 0;
    if (!ok) return send_error(fd, 403, "You can only cancel your own bookings.");

    exec_id(is_service ? "DELETE FROM service_bookings WHERE id=?1" : "DELETE FROM appointments WHERE id=?1", id);
    send_ok(fd);
}

/* a doctor marks one of their own appointments as completed */
static void complete_appointment(int fd, const char *q, const char *body) {
    char idstr[32];
    get_param(q, body, "id", idstr, sizeof idstr);
    int id = atoi(idstr);
    if (id <= 0) return send_error(fd, 400, "id is required.");
    if (me.role == R_DOCTOR &&
        scalar_int2("SELECT COUNT(*) FROM appointments WHERE id=?1 AND doctor_id=?2", id, me.doctor_id) == 0)
        return send_error(fd, 403, "You can only complete your own appointments.");
    exec_id("UPDATE appointments SET status='Completed' WHERE id=?1", id);
    send_ok(fd);
}

/* staff see everything; a doctor sees their own appointments; a patient sees their own */
static void list_appointments(int fd) {
    const char *filter = me.role == R_DOCTOR ? " WHERE a.doctor_id=?1"
                       : me.role == R_PATIENT ? " WHERE a.patient_id=?1" : "";
    char sql[600];
    snprintf(sql, sizeof sql,
        "SELECT a.id, d.name AS doctor, p.name AS patient, p.phone AS phone, a.date, a.time, a.status "
        "FROM appointments a JOIN doctors d ON d.id=a.doctor_id JOIN patients p ON p.id=a.patient_id%s "
        "ORDER BY a.date, a.time", filter);
    sqlite3_stmt *st = prep(sql);
    if (filter[0]) sqlite3_bind_int(st, 1, me.role == R_DOCTOR ? me.doctor_id : me.patient_id);
    query_json(fd, st);
}

static void list_service_bookings(int fd) {
    const char *filter = me.role == R_PATIENT ? " WHERE b.patient_id=?1" : "";
    char sql[600];
    snprintf(sql, sizeof sql,
        "SELECT b.id, s.name AS service, p.name AS patient, b.date, b.time "
        "FROM service_bookings b JOIN services s ON s.id=b.service_id JOIN patients p ON p.id=b.patient_id%s "
        "ORDER BY b.date, b.time", filter);
    sqlite3_stmt *st = prep(sql);
    if (filter[0]) sqlite3_bind_int(st, 1, me.patient_id);
    query_json(fd, st);
}

/* {"booked":["09:00","11:00"]} for one doctor / room on one date */
static void availability(int fd, const char *q, int is_service) {
    char rid[32], date[32];
    get_param(q, "", is_service ? "service_id" : "doctor_id", rid, sizeof rid);
    get_param(q, "", "date", date, sizeof date);
    int resource_id = atoi(rid);
    if (resource_id <= 0 || !valid_date(date)) return send_error(fd, 400, "id and date are required.");
    sqlite3_stmt *st = prep(is_service
        ? "SELECT time FROM service_bookings WHERE service_id=? AND date=? ORDER BY time"
        : "SELECT time FROM appointments WHERE doctor_id=? AND date=? ORDER BY time");
    sqlite3_bind_int(st, 1, resource_id);
    sqlite3_bind_text(st, 2, date, -1, SQLITE_TRANSIENT);
    Buf b = {0};
    buf_add(&b, "{\"booked\":[");
    int first = 1;
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (!first) buf_add(&b, ",");
        first = 0;
        buf_json_str(&b, (const char *)sqlite3_column_text(st, 0));
    }
    buf_add(&b, "]}");
    sqlite3_finalize(st);
    send_json(fd, 200, &b);
    free(b.data);
}

/* ---------- bed management ---------- */
static void admit_patient(int fd, const char *q, const char *body) {
    char wid[32], pid[32], date[32];
    get_param(q, body, "ward_id", wid, sizeof wid);
    get_param(q, body, "patient_id", pid, sizeof pid);
    get_param(q, body, "date", date, sizeof date);
    int ward_id = atoi(wid), patient_id = atoi(pid);

    if (ward_id <= 0 || patient_id <= 0) return send_error(fd, 400, "Choose a ward and a patient.");
    if (!valid_date(date)) return send_error(fd, 400, "Date must look like 2026-12-31.");

    if (scalar_int("SELECT COUNT(*) FROM admissions WHERE patient_id=?1 AND discharged_on IS NULL", patient_id) > 0)
        return send_error(fd, 409, "This patient is already admitted.");

    int free_beds = scalar_int(
        "SELECT w.total_beds - (SELECT COUNT(*) FROM admissions a WHERE a.ward_id = w.id AND a.discharged_on IS NULL) "
        "FROM wards w WHERE w.id=?1", ward_id);
    if (free_beds <= 0) return send_error(fd, 409, "No beds available in this ward.");

    sqlite3_stmt *st = prep("INSERT INTO admissions(ward_id, patient_id, admitted_on) VALUES(?,?,?)");
    sqlite3_bind_int(st, 1, ward_id);
    sqlite3_bind_int(st, 2, patient_id);
    sqlite3_bind_text(st, 3, date, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) return send_error(fd, 400, "Ward or patient does not exist.");
    send_id(fd, (int)sqlite3_last_insert_rowid(db));
}

static void discharge_patient(int fd, const char *q, const char *body) {
    char idstr[32];
    get_param(q, body, "id", idstr, sizeof idstr);
    int id = atoi(idstr);
    if (id <= 0) return send_error(fd, 400, "id is required.");
    exec_id("UPDATE admissions SET discharged_on = date('now') WHERE id=?1 AND discharged_on IS NULL", id);
    send_ok(fd);
}

/* staff and doctors see all admitted patients; a patient sees only their own admission */
static void list_admissions(int fd) {
    const char *filter = me.role == R_PATIENT ? " AND a.patient_id=?1" : "";
    char sql[500];
    snprintf(sql, sizeof sql,
        "SELECT a.id, w.name AS ward, p.name AS patient, a.admitted_on "
        "FROM admissions a JOIN wards w ON w.id=a.ward_id JOIN patients p ON p.id=a.patient_id "
        "WHERE a.discharged_on IS NULL%s ORDER BY a.admitted_on, a.id", filter);
    sqlite3_stmt *st = prep(sql);
    if (filter[0]) sqlite3_bind_int(st, 1, me.patient_id);
    query_json(fd, st);
}

/* ---------- dashboard: one row of totals for the given date ---------- */
static void dashboard(int fd, const char *q) {
    char date[32];
    get_param(q, "", "date", date, sizeof date);
    if (!valid_date(date)) return send_error(fd, 400, "date is required.");
    sqlite3_stmt *st = prep(
        "SELECT"
        " (SELECT COUNT(*) FROM appointments WHERE date=?1) AS opd,"
        " (SELECT COUNT(*) FROM service_bookings b JOIN services s ON s.id=b.service_id WHERE b.date=?1 AND s.kind='xray') AS xray,"
        " (SELECT COUNT(*) FROM service_bookings b JOIN services s ON s.id=b.service_id WHERE b.date=?1 AND s.kind='ct') AS ct,"
        " (SELECT COUNT(*) FROM service_bookings b JOIN services s ON s.id=b.service_id WHERE b.date=?1 AND s.kind='mri') AS mri,"
        " (SELECT COUNT(*) FROM doctors) AS doctors,"
        " (SELECT COUNT(*) FROM patients) AS patients,"
        " (SELECT COUNT(*) FROM admissions WHERE discharged_on IS NULL) AS admitted,"
        " (SELECT COALESCE(SUM(total_beds),0) FROM wards)"
        "   - (SELECT COUNT(*) FROM admissions WHERE discharged_on IS NULL) AS beds_free");
    sqlite3_bind_text(st, 1, date, -1, SQLITE_TRANSIENT);
    query_json(fd, st);
}

/* ---------- static files (only these three) ---------- */
static void serve_static(int fd, const char *path) {
    const char *file = NULL, *type = "text/plain";
    if (!strcmp(path, "/") || !strcmp(path, "/index.html")) { file = "../frontend/index.html"; type = "text/html; charset=utf-8"; }
    else if (!strcmp(path, "/style.css")) { file = "../frontend/style.css"; type = "text/css"; }
    else if (!strcmp(path, "/app.js"))    { file = "../frontend/app.js";    type = "application/javascript"; }
    if (!file) return send_error(fd, 404, "Not found.");

    FILE *f = fopen(file, "rb");
    if (!f) return send_error(fd, 404, "Frontend file missing. Run the server from the backend/ folder.");
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    char *data = malloc((size_t)size + 1);
    size_t got = fread(data, 1, (size_t)size, f);
    fclose(f);
    send_response(fd, 200, type, data, got);
    free(data);
}

/* ---------- request routing: every API call states who may use it ---------- */
static void route_api(int fd, int get, int post, int del, const char *p, const char *q, const char *body) {
    /* open to everyone */
    if (!strcmp(p, "login") && post) return login(fd, q, body);
    if (!strcmp(p, "register") && post) return register_patient(fd, q, body);
    if (!strcmp(p, "me") && get) return send_me(fd);
    if (!strcmp(p, "logout") && post) return logout(fd);

    if (!strcmp(p, "doctors")) {
        if (get) { if (allow(fd, ANY_ROLE)) query_json(fd, prep("SELECT id, name, specialization FROM doctors ORDER BY name")); }
        else if (post) { if (allow(fd, R_ADMIN)) add_doctor(fd, q, body); }
        else if (del) { if (allow(fd, R_ADMIN)) delete_person(fd, q, body, 1); }
        else send_error(fd, 400, "Bad method.");
    } else if (!strcmp(p, "patients")) {
        if (!allow(fd, R_ADMIN)) return;
        if (get) query_json(fd, prep("SELECT id, name, phone FROM patients ORDER BY name"));
        else if (post) add_patient(fd, q, body);
        else if (del) delete_person(fd, q, body, 0);
        else send_error(fd, 400, "Bad method.");
    } else if (!strcmp(p, "appointments")) {
        if (get) { if (allow(fd, ANY_ROLE)) list_appointments(fd); }
        else if (post) { if (allow(fd, R_ADMIN | R_PATIENT)) book_slot(fd, q, body, 0); }
        else if (del) { if (allow(fd, ANY_ROLE)) cancel_slot(fd, q, body, 0); }
        else send_error(fd, 400, "Bad method.");
    } else if (!strcmp(p, "complete") && post) {
        if (allow(fd, R_ADMIN | R_DOCTOR)) complete_appointment(fd, q, body);
    } else if (!strcmp(p, "availability") && get) {
        if (allow(fd, ANY_ROLE)) availability(fd, q, 0);
    } else if (!strcmp(p, "services") && get) {
        if (allow(fd, ANY_ROLE)) query_json(fd, prep("SELECT id, name, kind FROM services ORDER BY id"));
    } else if (!strcmp(p, "service-bookings")) {
        if (!allow(fd, R_ADMIN | R_PATIENT)) return;
        if (get) list_service_bookings(fd);
        else if (post) book_slot(fd, q, body, 1);
        else if (del) cancel_slot(fd, q, body, 1);
        else send_error(fd, 400, "Bad method.");
    } else if (!strcmp(p, "service-availability") && get) {
        if (allow(fd, ANY_ROLE)) availability(fd, q, 1);
    } else if (!strcmp(p, "wards") && get) {
        if (allow(fd, ANY_ROLE)) query_json(fd, prep(
            "SELECT w.id, w.name, w.total_beds AS total,"
            " (SELECT COUNT(*) FROM admissions a WHERE a.ward_id=w.id AND a.discharged_on IS NULL) AS occupied,"
            " w.total_beds - (SELECT COUNT(*) FROM admissions a WHERE a.ward_id=w.id AND a.discharged_on IS NULL) AS available "
            "FROM wards w ORDER BY w.id"));
    } else if (!strcmp(p, "admissions")) {
        if (get) { if (allow(fd, ANY_ROLE)) list_admissions(fd); }
        else if (post) { if (allow(fd, R_ADMIN)) admit_patient(fd, q, body); }
        else if (del) { if (allow(fd, R_ADMIN)) discharge_patient(fd, q, body); }
        else send_error(fd, 400, "Bad method.");
    } else if (!strcmp(p, "dashboard") && get) {
        if (allow(fd, R_ADMIN)) dashboard(fd, q);
    } else {
        send_error(fd, 404, "Unknown endpoint.");
    }
}

static int read_request(int fd, char *req, size_t cap) {
    size_t n = 0;
    char *hdr_end = NULL;
    while (n < cap - 1) {
        ssize_t r = recv(fd, req + n, cap - 1 - n, 0);
        if (r <= 0) break;
        n += (size_t)r;
        req[n] = 0;
        hdr_end = strstr(req, "\r\n\r\n");
        if (hdr_end) {
            int cl = 0;
            char *p = strcasestr(req, "Content-Length:");
            if (p && p < hdr_end) cl = atoi(p + 15);
            size_t have = n - (size_t)(hdr_end + 4 - req);
            if (have >= (size_t)cl) break;
        }
    }
    return hdr_end ? 0 : -1;
}

static void handle_client(int fd) {
    char req[REQ_MAX], method[16], path[512];
    if (read_request(fd, req, sizeof req) < 0) return;
    if (sscanf(req, "%15s %511s", method, path) != 2) return;

    memset(&me, 0, sizeof me);          /* start every request logged out ... */
    set_cookie[0] = 0;
    token_from_request(req, cur_token, sizeof cur_token);
    if (cur_token[0]) load_session(cur_token);   /* ... then look up the session cookie */

    char *body = strstr(req, "\r\n\r\n") + 4;
    char *query = strchr(path, '?');
    if (query) *query++ = 0; else query = "";

    int get = !strcmp(method, "GET"), post = !strcmp(method, "POST"), del = !strcmp(method, "DELETE");

    if (!strncmp(path, "/api/", 5)) route_api(fd, get, post, del, path + 5, query, body);
    else if (get) serve_static(fd, path);
    else send_error(fd, 400, "Bad request.");
}

/* ---------- start-up ---------- */
static const char *SCHEMA =
    "PRAGMA foreign_keys = ON;"
    "CREATE TABLE IF NOT EXISTS doctors("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL, specialization TEXT DEFAULT '');"
    "CREATE TABLE IF NOT EXISTS patients("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL, phone TEXT DEFAULT '');"
    "CREATE TABLE IF NOT EXISTS users("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  username TEXT NOT NULL UNIQUE, pass_hash TEXT NOT NULL, salt TEXT NOT NULL,"
    "  role TEXT NOT NULL CHECK (role IN ('admin','doctor','patient')),"
    "  doctor_id INTEGER REFERENCES doctors(id), patient_id INTEGER REFERENCES patients(id));"
    "CREATE TABLE IF NOT EXISTS sessions("
    "  token TEXT PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES users(id), created INTEGER NOT NULL);"
    "CREATE TABLE IF NOT EXISTS appointments("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  doctor_id INTEGER NOT NULL REFERENCES doctors(id),"
    "  patient_id INTEGER NOT NULL REFERENCES patients(id),"
    "  date TEXT NOT NULL, time TEXT NOT NULL, status TEXT NOT NULL DEFAULT 'Booked',"
    "  UNIQUE(doctor_id, date, time));"          /* database-level double-booking guard */
    "CREATE TABLE IF NOT EXISTS services("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL, kind TEXT NOT NULL);"
    "CREATE TABLE IF NOT EXISTS service_bookings("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  service_id INTEGER NOT NULL REFERENCES services(id),"
    "  patient_id INTEGER NOT NULL REFERENCES patients(id),"
    "  date TEXT NOT NULL, time TEXT NOT NULL,"
    "  UNIQUE(service_id, date, time));"
    "CREATE TABLE IF NOT EXISTS wards("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL, total_beds INTEGER NOT NULL);"
    "CREATE TABLE IF NOT EXISTS admissions("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  ward_id INTEGER NOT NULL REFERENCES wards(id),"
    "  patient_id INTEGER NOT NULL REFERENCES patients(id),"
    "  admitted_on TEXT NOT NULL, discharged_on TEXT);";

static void seed_doctor(const char *name, const char *spec, const char *username) {
    sqlite3_stmt *st = prep("INSERT INTO doctors(name, specialization) VALUES(?,?)");
    sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, spec, -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
    make_user(username, "doctor123", "doctor", (int)sqlite3_last_insert_rowid(db), 0);
}

static void seed_patient(const char *name, const char *phone, const char *username) {
    sqlite3_stmt *st = prep("INSERT INTO patients(name, phone) VALUES(?,?)");
    sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, phone, -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
    make_user(username, "patient123", "patient", 0, (int)sqlite3_last_insert_rowid(db));
}

/* sample data and demo logins so the demo is never empty (only fills tables that have no rows) */
static void seed(void) {
    if (scalar_int("SELECT COUNT(*) FROM services", 0) == 0)
        sqlite3_exec(db, "INSERT INTO services(name, kind) VALUES"
            "('X-Ray','xray'),('CT Scan','ct'),('MRI','mri');", NULL, NULL, NULL);
    if (scalar_int("SELECT COUNT(*) FROM wards", 0) == 0)
        sqlite3_exec(db, "INSERT INTO wards(name, total_beds) VALUES"
            "('General Ward',10),('ICU',4),('Pediatric Ward',6);", NULL, NULL, NULL);
    if (scalar_int("SELECT COUNT(*) FROM doctors", 0) == 0) {
        seed_doctor("Dr. Anita Rao", "Cardiology", "dr.rao");
        seed_doctor("Dr. Sameer Das", "Orthopedics", "dr.das");
        seed_doctor("Dr. Rishita Biswas", "Radiology", "dr.biswas");
    }
    if (scalar_int("SELECT COUNT(*) FROM patients", 0) == 0) {
        seed_patient("Riya Sen", "9830011111", "riya");
        seed_patient("Amit Ghosh", "9830022222", "amit");
    }
    if (scalar_int("SELECT COUNT(*) FROM users WHERE role='admin'", 0) == 0) {
        const char *pw = getenv("ADMIN_PASSWORD");   /* set this on the host to choose your own */
        make_user("admin", (pw && strlen(pw) >= 6 && strlen(pw) <= 64) ? pw : "admin123", "admin", 0, 0);
    }
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);

    if (sqlite3_open("appointease.db", &db) != SQLITE_OK) {
        fprintf(stderr, "Cannot open database: %s\n", sqlite3_errmsg(db));
        return 1;
    }
    char *err = NULL;
    if (sqlite3_exec(db, SCHEMA, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "Schema error: %s\n", err);
        return 1;
    }
    seed();

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    const char *env_port = getenv("PORT");   /* hosts like Render provide PORT */
    int port = env_port ? atoi(env_port) : PORT;
    addr.sin_port = htons(port);
    if (bind(srv, (struct sockaddr *)&addr, sizeof addr) < 0 || listen(srv, 16) < 0) {
        perror("bind/listen");
        return 1;
    }
    printf("AppointEase running at http://localhost:%d  (Ctrl+C to stop)\n", port);

    for (;;) {
        int fd = accept(srv, NULL, NULL);
        if (fd < 0) continue;
        struct timeval tv = {5, 0};   /* don't let a stalled client block us */
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        handle_client(fd);
        close(fd);
    }
}
