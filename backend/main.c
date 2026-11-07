/*
 * AppointEase - backend (v2)
 * Hospital services: OPD appointments, X-ray / CT / MRI bookings, bed management.
 * A tiny HTTP server in C (POSIX sockets) + SQLite. It serves the frontend and a JSON API.
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

static void send_ok(int fd) {
    Buf b = {0};
    buf_add(&b, "{\"ok\":true}");
    send_json(fd, 200, &b);
    free(b.data);
}

static void send_new_id(int fd) {   /* reply to an INSERT with the new row's id */
    Buf b = {0};
    buf_addf(&b, "{\"id\":%lld}", (long long)sqlite3_last_insert_rowid(db));
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

/* run a query that returns one number; the id is bound to ?1 */
static int scalar_int(const char *sql, int id) {
    sqlite3_stmt *st = prep(sql);
    int n = 0;
    sqlite3_bind_int(st, 1, id);
    if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return n;
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

/* ---------- doctors and patients ---------- */
static void add_person(int fd, const char *q, const char *body, int is_doctor) {
    char name[FIELD_MAX], extra[FIELD_MAX];
    get_param(q, body, "name", name, sizeof name);
    get_param(q, body, is_doctor ? "specialization" : "phone", extra, sizeof extra);
    if (!name[0]) return send_error(fd, 400, "Name is required.");
    sqlite3_stmt *st = prep(is_doctor
        ? "INSERT INTO doctors(name, specialization) VALUES(?, ?)"
        : "INSERT INTO patients(name, phone) VALUES(?, ?)");
    sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, extra, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) return send_error(fd, 500, "Could not save.");
    send_new_id(fd);
}

/* refused while the person still has bookings or is admitted */
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

    sqlite3_stmt *st;
    if (!is_doctor) {   /* old discharge records would block the delete */
        st = prep("DELETE FROM admissions WHERE patient_id=?");
        sqlite3_bind_int(st, 1, id);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    st = prep(is_doctor ? "DELETE FROM doctors WHERE id=?" : "DELETE FROM patients WHERE id=?");
    sqlite3_bind_int(st, 1, id);
    sqlite3_step(st);
    sqlite3_finalize(st);
    send_ok(fd);
}

/* ---------- time-slot bookings ----------
 * is_service = 0 : OPD appointment (the resource is a doctor)
 * is_service = 1 : X-ray / CT / MRI booking (the resource is a machine room)
 * The same rule protects both: one resource, one date and time, one booking. */
static void book_slot(int fd, const char *q, const char *body, int is_service) {
    char rid[32], pid[32], date[32], time[32];
    get_param(q, body, is_service ? "service_id" : "doctor_id", rid, sizeof rid);
    get_param(q, body, "patient_id", pid, sizeof pid);
    get_param(q, body, "date", date, sizeof date);
    get_param(q, body, "time", time, sizeof time);
    int resource_id = atoi(rid), patient_id = atoi(pid);

    if (resource_id <= 0 || patient_id <= 0)
        return send_error(fd, 400, is_service ? "Choose a service and a patient." : "Choose a doctor and a patient.");
    if (!valid_date(date)) return send_error(fd, 400, "Date must look like 2026-12-31.");
    if (!valid_time(time)) return send_error(fd, 400, "Time must look like 09:00.");

    const char *table = is_service ? "service_bookings" : "appointments";
    const char *rcol  = is_service ? "service_id" : "doctor_id";
    char sql[256];

    snprintf(sql, sizeof sql, "SELECT COUNT(*) FROM %s WHERE %s=? AND date=? AND time=?", table, rcol);
    if (count_rows(sql, resource_id, date, time) > 0)
        return send_error(fd, 409, is_service ? "That room is already booked at that time."
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
    send_new_id(fd);
}

static void cancel_slot(int fd, const char *q, const char *body, int is_service) {
    char idstr[32];
    get_param(q, body, "id", idstr, sizeof idstr);
    int id = atoi(idstr);
    if (id <= 0) return send_error(fd, 400, "id is required.");
    sqlite3_stmt *st = prep(is_service ? "DELETE FROM service_bookings WHERE id=?"
                                       : "DELETE FROM appointments WHERE id=?");
    sqlite3_bind_int(st, 1, id);
    sqlite3_step(st);
    sqlite3_finalize(st);
    send_ok(fd);
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
    send_new_id(fd);
}

static void discharge_patient(int fd, const char *q, const char *body) {
    char idstr[32];
    get_param(q, body, "id", idstr, sizeof idstr);
    int id = atoi(idstr);
    if (id <= 0) return send_error(fd, 400, "id is required.");
    sqlite3_stmt *st = prep("UPDATE admissions SET discharged_on = date('now') WHERE id=? AND discharged_on IS NULL");
    sqlite3_bind_int(st, 1, id);
    sqlite3_step(st);
    sqlite3_finalize(st);
    send_ok(fd);
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
    if (!strcmp(path, "/") || !strcmp(path, "/index.html")) { file = "index.html"; type = "text/html"; }
    else if (!strcmp(path, "/style.css")) { file = "style.css"; type = "text/css"; }
    else if (!strcmp(path, "/app.js")) { file = "app.js"; type = "text/javascript"; }

    if (!file) return send_error(fd, 404, "Not found.");

    /* Try local folder first (for Render/Docker), fallback to ../frontend/ (for local dev) */
    FILE *f = fopen(file, "rb");
    if (!f) {
        if (!strcmp(file, "index.html")) f = fopen("../frontend/index.html", "rb");
        else if (!strcmp(file, "style.css")) f = fopen("../frontend/style.css", "rb");
        else if (!strcmp(file, "app.js")) f = fopen("../frontend/app.js", "rb");
    }

    if (!f) return send_error(fd, 404, "Frontend file missing.");

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
static void route_api(int fd, int get, int post, int del, const char *p, const char *q, const char *body) {
    if (!strcmp(p, "doctors")) {
        if (get) query_json(fd, prep("SELECT id, name, specialization FROM doctors ORDER BY name"));
        else if (post) add_person(fd, q, body, 1);
        else if (del) delete_person(fd, q, body, 1);
        else send_error(fd, 400, "Bad method.");
    } else if (!strcmp(p, "patients")) {
        if (get) query_json(fd, prep("SELECT id, name, phone FROM patients ORDER BY name"));
        else if (post) add_person(fd, q, body, 0);
        else if (del) delete_person(fd, q, body, 0);
        else send_error(fd, 400, "Bad method.");
    } else if (!strcmp(p, "appointments")) {
        if (get) query_json(fd, prep(
            "SELECT a.id, d.name AS doctor, p.name AS patient, a.date, a.time "
            "FROM appointments a JOIN doctors d ON d.id=a.doctor_id JOIN patients p ON p.id=a.patient_id "
            "ORDER BY a.date, a.time"));
        else if (post) book_slot(fd, q, body, 0);
        else if (del) cancel_slot(fd, q, body, 0);
        else send_error(fd, 400, "Bad method.");
    } else if (!strcmp(p, "availability") && get) {
        availability(fd, q, 0);
    } else if (!strcmp(p, "services") && get) {
        query_json(fd, prep("SELECT id, name, kind FROM services ORDER BY id"));
    } else if (!strcmp(p, "service-bookings")) {
        if (get) query_json(fd, prep(
            "SELECT b.id, s.name AS service, p.name AS patient, b.date, b.time "
            "FROM service_bookings b JOIN services s ON s.id=b.service_id JOIN patients p ON p.id=b.patient_id "
            "ORDER BY b.date, b.time"));
        else if (post) book_slot(fd, q, body, 1);
        else if (del) cancel_slot(fd, q, body, 1);
        else send_error(fd, 400, "Bad method.");
    } else if (!strcmp(p, "service-availability") && get) {
        availability(fd, q, 1);
    } else if (!strcmp(p, "wards") && get) {
        query_json(fd, prep(
            "SELECT w.id, w.name, w.total_beds AS total,"
            " (SELECT COUNT(*) FROM admissions a WHERE a.ward_id=w.id AND a.discharged_on IS NULL) AS occupied,"
            " w.total_beds - (SELECT COUNT(*) FROM admissions a WHERE a.ward_id=w.id AND a.discharged_on IS NULL) AS available "
            "FROM wards w ORDER BY w.id"));
    } else if (!strcmp(p, "admissions")) {
        if (get) query_json(fd, prep(
            "SELECT a.id, w.name AS ward, p.name AS patient, a.admitted_on "
            "FROM admissions a JOIN wards w ON w.id=a.ward_id JOIN patients p ON p.id=a.patient_id "
            "WHERE a.discharged_on IS NULL ORDER BY a.admitted_on, a.id"));
        else if (post) admit_patient(fd, q, body);
        else if (del) discharge_patient(fd, q, body);
        else send_error(fd, 400, "Bad method.");
    } else if (!strcmp(p, "dashboard") && get) {
        dashboard(fd, q);
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
    "CREATE TABLE IF NOT EXISTS appointments("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  doctor_id INTEGER NOT NULL REFERENCES doctors(id),"
    "  patient_id INTEGER NOT NULL REFERENCES patients(id),"
    "  date TEXT NOT NULL, time TEXT NOT NULL,"
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

/* sample data so the demo is never empty (only fills tables that have no rows) */
static void seed(void) {
    if (scalar_int("SELECT COUNT(*) FROM services", 0) == 0)
        sqlite3_exec(db, "INSERT INTO services(name, kind) VALUES"
            "('X-Ray Room','xray'),('CT Scan Room','ct'),('MRI Room','mri');", NULL, NULL, NULL);
    if (scalar_int("SELECT COUNT(*) FROM wards", 0) == 0)
        sqlite3_exec(db, "INSERT INTO wards(name, total_beds) VALUES"
            "('General Ward',20),('ICU',6),('Maternity Ward',8),('Pediatric Ward',10);", NULL, NULL, NULL);
    if (scalar_int("SELECT COUNT(*) FROM doctors", 0) == 0)
        sqlite3_exec(db, "INSERT INTO doctors(name, specialization) VALUES"
            "('Dr. Anita Rao','Cardiology'),('Dr. Sameer Das','Orthopedics'),('Dr. Rishita Biswas','Radiology');",
            NULL, NULL, NULL);
    if (scalar_int("SELECT COUNT(*) FROM patients", 0) == 0)
        sqlite3_exec(db, "INSERT INTO patients(name, phone) VALUES"
            "('Riya Sen','9830011111'),('Amit Ghosh','9830022222');", NULL, NULL, NULL);
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
