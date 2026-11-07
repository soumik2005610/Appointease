/*
 * AppointEase - backend
 * A tiny HTTP server in C (POSIX sockets) + SQLite.
 * It serves the frontend files and a JSON API.
 *
 * Build:  make
 * Run:    ./appointease        (from the backend/ folder)
 * Open:   http://localhost:8080
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
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

static sqlite3 *db;

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
        case 404: return "Not Found";
        case 409: return "Conflict";
        default:  return "Internal Server Error";
    }
}

static void send_response(int fd, int code, const char *type, const char *body, size_t len) {
    char head[256];
    int n = snprintf(head, sizeof head,
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
        "Connection: close\r\n\r\n", code, status_text(code), type, len);
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
/* ---------- API handlers ---------- */
static void list_doctors(int fd) {
    sqlite3_stmt *st;
    Buf b = {0};
    sqlite3_prepare_v2(db, "SELECT id, name, specialization FROM doctors ORDER BY name", -1, &st, NULL);
    buf_add(&b, "[");
    int first = 1;
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (!first) buf_add(&b, ",");
        first = 0;
        buf_addf(&b, "{\"id\":%d,\"name\":", sqlite3_column_int(st, 0));
        buf_json_str(&b, (const char *)sqlite3_column_text(st, 1));
        buf_add(&b, ",\"specialization\":");
        buf_json_str(&b, (const char *)sqlite3_column_text(st, 2));
        buf_add(&b, "}");
    }
    buf_add(&b, "]");
    sqlite3_finalize(st);
    send_json(fd, 200, &b);
    free(b.data);
}

static void add_doctor(int fd, const char *q, const char *body) {
    char name[FIELD_MAX], spec[FIELD_MAX];
    get_param(q, body, "name", name, sizeof name);
    get_param(q, body, "specialization", spec, sizeof spec);
    if (!name[0]) return send_error(fd, 400, "Doctor name is required.");
    sqlite3_stmt *st;
    sqlite3_prepare_v2(db, "INSERT INTO doctors(name, specialization) VALUES(?, ?)", -1, &st, NULL);
    sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, spec, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) return send_error(fd, 500, "Could not save doctor.");
    Buf b = {0};
    buf_addf(&b, "{\"id\":%lld}", (long long)sqlite3_last_insert_rowid(db));
    send_json(fd, 201, &b);
    free(b.data);
}

static void list_patients(int fd) {
    sqlite3_stmt *st;
    Buf b = {0};
    sqlite3_prepare_v2(db, "SELECT id, name, phone FROM patients ORDER BY name", -1, &st, NULL);
    buf_add(&b, "[");
    int first = 1;
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (!first) buf_add(&b, ",");
        first = 0;
        buf_addf(&b, "{\"id\":%d,\"name\":", sqlite3_column_int(st, 0));
        buf_json_str(&b, (const char *)sqlite3_column_text(st, 1));
        buf_add(&b, ",\"phone\":");
        buf_json_str(&b, (const char *)sqlite3_column_text(st, 2));
        buf_add(&b, "}");
    }
    buf_add(&b, "]");
    sqlite3_finalize(st);
    send_json(fd, 200, &b);
    free(b.data);
}

static void add_patient(int fd, const char *q, const char *body) {
    char name[FIELD_MAX], phone[FIELD_MAX];
    get_param(q, body, "name", name, sizeof name);
    get_param(q, body, "phone", phone, sizeof phone);
    if (!name[0]) return send_error(fd, 400, "Patient name is required.");
    sqlite3_stmt *st;
    sqlite3_prepare_v2(db, "INSERT INTO patients(name, phone) VALUES(?, ?)", -1, &st, NULL);
    sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, phone, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) return send_error(fd, 500, "Could not save patient.");
    Buf b = {0};
    buf_addf(&b, "{\"id\":%lld}", (long long)sqlite3_last_insert_rowid(db));
    send_json(fd, 201, &b);
    free(b.data);
}
static void list_appointments(int fd) {
    sqlite3_stmt *st;
    Buf b = {0};
    sqlite3_prepare_v2(db,
        "SELECT a.id, d.name, p.name, a.date, a.time "
        "FROM appointments a "
        "JOIN doctors d ON d.id = a.doctor_id "
        "JOIN patients p ON p.id = a.patient_id "
        "ORDER BY a.date, a.time", -1, &st, NULL);
    buf_add(&b, "[");
    int first = 1;
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (!first) buf_add(&b, ",");
        first = 0;
        buf_addf(&b, "{\"id\":%d,\"doctor\":", sqlite3_column_int(st, 0));
        buf_json_str(&b, (const char *)sqlite3_column_text(st, 1));
        buf_add(&b, ",\"patient\":");
        buf_json_str(&b, (const char *)sqlite3_column_text(st, 2));
        buf_add(&b, ",\"date\":");
        buf_json_str(&b, (const char *)sqlite3_column_text(st, 3));
        buf_add(&b, ",\"time\":");
        buf_json_str(&b, (const char *)sqlite3_column_text(st, 4));
        buf_add(&b, "}");
    }
    buf_add(&b, "]");
    sqlite3_finalize(st);
    send_json(fd, 200, &b);
    free(b.data);
}

/* returns number of rows matching: used for conflict detection */
static int count_rows(const char *sql, int id, const char *date, const char *time) {
    sqlite3_stmt *st;
    int n = 0;
    sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    sqlite3_bind_int(st, 1, id);
    sqlite3_bind_text(st, 2, date, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, time, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return n;
}

static void book_appointment(int fd, const char *q, const char *body) {
    char did[32], pid[32], date[32], time[32];
    get_param(q, body, "doctor_id", did, sizeof did);
    get_param(q, body, "patient_id", pid, sizeof pid);
    get_param(q, body, "date", date, sizeof date);
    get_param(q, body, "time", time, sizeof time);
    int doctor_id = atoi(did), patient_id = atoi(pid);

    if (doctor_id <= 0 || patient_id <= 0) return send_error(fd, 400, "Choose a doctor and a patient.");
    if (!valid_date(date)) return send_error(fd, 400, "Date must look like 2026-12-31.");
    if (!valid_time(time)) return send_error(fd, 400, "Time must look like 09:00.");

    /* the key feature: conflict detection */
    if (count_rows("SELECT COUNT(*) FROM appointments WHERE doctor_id=? AND date=? AND time=?",
                   doctor_id, date, time) > 0)
        return send_error(fd, 409, "This doctor is already booked at that time.");
    if (count_rows("SELECT COUNT(*) FROM appointments WHERE patient_id=? AND date=? AND time=?",
                   patient_id, date, time) > 0)
        return send_error(fd, 409, "This patient already has an appointment at that time.");

    sqlite3_stmt *st;
    sqlite3_prepare_v2(db,
        "INSERT INTO appointments(doctor_id, patient_id, date, time) VALUES(?,?,?,?)", -1, &st, NULL);
    sqlite3_bind_int(st, 1, doctor_id);
    sqlite3_bind_int(st, 2, patient_id);
    sqlite3_bind_text(st, 3, date, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, time, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) return send_error(fd, 400, "Doctor or patient does not exist.");

    Buf b = {0};
    buf_addf(&b, "{\"id\":%lld}", (long long)sqlite3_last_insert_rowid(db));
    send_json(fd, 201, &b);
    free(b.data);
}

static void cancel_appointment(int fd, const char *q, const char *body) {
    char idstr[32];
    get_param(q, body, "id", idstr, sizeof idstr);
    int id = atoi(idstr);
    if (id <= 0) return send_error(fd, 400, "Appointment id is required.");
    sqlite3_stmt *st;
    sqlite3_prepare_v2(db, "DELETE FROM appointments WHERE id=?", -1, &st, NULL);
    sqlite3_bind_int(st, 1, id);
    sqlite3_step(st);
    sqlite3_finalize(st);
    Buf b = {0};
    buf_add(&b, "{\"ok\":true}");
    send_json(fd, 200, &b);
    free(b.data);
}

/* GET /api/availability?doctor_id=1&date=2026-12-01  ->  {"booked":["09:00","11:00"]} */
static void availability(int fd, const char *q) {
    char did[32], date[32];
    get_param(q, "", "doctor_id", did, sizeof did);
    get_param(q, "", "date", date, sizeof date);
    int doctor_id = atoi(did);
    if (doctor_id <= 0 || !valid_date(date)) return send_error(fd, 400, "doctor_id and date are required.");
    sqlite3_stmt *st;
    Buf b = {0};
    sqlite3_prepare_v2(db, "SELECT time FROM appointments WHERE doctor_id=? AND date=? ORDER BY time", -1, &st, NULL);
    sqlite3_bind_int(st, 1, doctor_id);
    sqlite3_bind_text(st, 2, date, -1, SQLITE_TRANSIENT);
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

/* ---------- request routing ---------- */
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

    char *body = strstr(req, "\r\n\r\n") + 4;
    char *query = strchr(path, '?');
    if (query) *query++ = 0; else query = "";

    int get = !strcmp(method, "GET"), post = !strcmp(method, "POST"), del = !strcmp(method, "DELETE");

    if (!strncmp(path, "/api/", 5)) {
        if (!strcmp(path, "/api/doctors")) {
            if (get) list_doctors(fd); else if (post) add_doctor(fd, query, body); else send_error(fd, 400, "Bad method.");
        } else if (!strcmp(path, "/api/patients")) {
            if (get) list_patients(fd); else if (post) add_patient(fd, query, body); else send_error(fd, 400, "Bad method.");
        } else if (!strcmp(path, "/api/appointments")) {
            if (get) list_appointments(fd);
            else if (post) book_appointment(fd, query, body);
            else if (del) cancel_appointment(fd, query, body);
            else send_error(fd, 400, "Bad method.");
        } else if (!strcmp(path, "/api/availability") && get) {
            availability(fd, query);
        } else send_error(fd, 404, "Unknown endpoint.");
    } else if (get) {
        serve_static(fd, path);
    } else send_error(fd, 400, "Bad request.");
}

/* ---------- start-up ---------- */
static const char *SCHEMA =
    "PRAGMA foreign_keys = ON;"
    "CREATE TABLE IF NOT EXISTS doctors("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL, specialization TEXT DEFAULT '');"
    "CREATE TABLE IF NOT EXISTS patients("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL, phone TEXT DEFAULT '');"
    "CREATE TABLE IF NOT EXISTS appointments("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  doctor_id INTEGER NOT NULL REFERENCES doctors(id),"
    "  patient_id INTEGER NOT NULL REFERENCES patients(id),"
    "  date TEXT NOT NULL, time TEXT NOT NULL,"
    "  UNIQUE(doctor_id, date, time));";   /* database-level double-booking guard */

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

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
        const char *env_port = getenv("PORT");
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



