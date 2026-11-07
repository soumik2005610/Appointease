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
#define ADMIN_PIN "1234"

static sqlite3 *db;

typedef struct { char *data; size_t len, cap; } Buf;

static void buf_init(Buf *b) { b->data = NULL; b->len = b->cap = 0; }
static void buf_cat(Buf *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        b->cap = (b->cap ? b->cap * 2 : 1024) + n;
        b->data = realloc(b->data, b->cap);
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = 0;
}
static void buf_free(Buf *b) { free(b->data); }

static void send_response(int fd, int code, const char *type, const char *data, size_t len) {
    char h[256];
    int hlen = snprintf(h, sizeof h,
        "HTTP/1.1 %d OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\nAccess-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: GET, POST, DELETE, OPTIONS\r\nConnection: close\r\n\r\n",
        code, type, len);
    send(fd, h, hlen, 0);
    if (data && len) send(fd, data, len, 0);
}

static void send_json(int fd, const char *json) {
    send_response(fd, 200, "application/json", json, strlen(json));
}

static void send_error(int fd, int code, const char *msg) {
    char j[512];
    snprintf(j, sizeof j, "{\"error\":\"%s\"}", msg);
    send_response(fd, code, "application/json", j, strlen(j));
}

static void get_param(const char *q, const char *body, const char *key, char *out, size_t max) {
    out[0] = 0;
    const char *src = (body && *body) ? body : q;
    if (!src || !*src) return;
    size_t klen = strlen(key);
    const char *p = src;
    while ((p = strstr(p, key))) {
        if ((p == src || p[-1] == '&' || p[-1] == '?') && p[klen] == '=') {
            p += klen + 1;
            const char *e = strchr(p, '&');
            size_t len = e ? (size_t)(e - p) : strlen(p);
            if (len >= max) len = max - 1;
            size_t o = 0;
            for (size_t i = 0; i < len; i++) {
                if (p[i] == '%' && i + 2 < len && isxdigit(p[i+1]) && isxdigit(p[i+2])) {
                    char hex[3] = { p[i+1], p[i+2], 0 };
                    out[o++] = (char)strtol(hex, NULL, 16);
                    i += 2;
                } else if (p[i] == '+') out[o++] = ' ';
                else out[o++] = p[i];
            }
            out[o] = 0;
            return;
        }
        p++;
    }
}

static sqlite3_stmt* prep(const char *sql) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return NULL;
    return st;
}

static void json_escape(Buf *b, const char *s) {
    if (!s) return;
    while (*s) {
        if (*s == '"') buf_cat(b, "\\\"", 2);
        else if (*s == '\\') buf_cat(b, "\\\\", 2);
        else if (*s == '\n') buf_cat(b, "\\n", 2);
        else buf_cat(b, s, 1);
        s++;
    }
}

static void query_json(int fd, sqlite3_stmt *st) {
    if (!st) { send_error(fd, 500, "DB error"); return; }
    Buf b; buf_init(&b);
    buf_cat(&b, "[", 1);
    int cols = sqlite3_column_count(st), row = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (row++) buf_cat(&b, ",", 1);
        buf_cat(&b, "{", 1);
        for (int i = 0; i < cols; i++) {
            if (i) buf_cat(&b, ",", 1);
            const char *name = sqlite3_column_name(st, i);
            buf_cat(&b, "\"", 1); json_escape(&b, name); buf_cat(&b, "\":", 2);
            if (sqlite3_column_type(st, i) == SQLITE_NULL) {
                buf_cat(&b, "null", 4);
            } else if (sqlite3_column_type(st, i) == SQLITE_INTEGER) {
                char num[32]; snprintf(num, sizeof num, "%lld", sqlite3_column_int64(st, i));
                buf_cat(&b, num, strlen(num));
            } else {
                buf_cat(&b, "\"", 1); json_escape(&b, (const char*)sqlite3_column_text(st, i)); buf_cat(&b, "\"", 1);
            }
        }
        buf_cat(&b, "}", 1);
    }
    buf_cat(&b, "]", 1);
    sqlite3_finalize(st);
    send_json(fd, b.data ? b.data : "[]");
    buf_free(&b);
}

static void send_new_id(int fd) {
    char j[64];
    snprintf(j, sizeof j, "{\"id\":%lld}", sqlite3_last_insert_rowid(db));
    send_json(fd, j);
}

static void add_person(int fd, const char *q, const char *body, int is_doc) {
    char name[FIELD_MAX], extra[FIELD_MAX];
    get_param(q, body, "name", name, sizeof name);
    get_param(q, body, is_doc ? "specialization" : "phone", extra, sizeof extra);
    if (!*name) return send_error(fd, 400, "Name is required.");
    sqlite3_stmt *st = prep(is_doc ? "INSERT INTO doctors(name,specialization) VALUES(?,?)" : "INSERT INTO patients(name,phone) VALUES(?,?)");
    sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, extra, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) != SQLITE_DONE) { sqlite3_finalize(st); return send_error(fd, 500, "Failed to insert."); }
    sqlite3_finalize(st);
    send_new_id(fd);
}

static void delete_person(int fd, const char *q, const char *body, int is_doc) {
    char idstr[32];
    get_param(q, body, "id", idstr, sizeof idstr);
    int id = atoi(idstr);
    if (id <= 0) return send_error(fd, 400, "Invalid ID.");
    sqlite3_stmt *st = prep(is_doc ? "DELETE FROM doctors WHERE id=?" : "DELETE FROM patients WHERE id=?");
    sqlite3_bind_int(st, 1, id);
    sqlite3_step(st);
    sqlite3_finalize(st);
    send_json(fd, "{\"success\":true}");
}

static void generate_appointment_letter(const char *doctor_or_service, const char *patient, const char *date, const char *time, int is_service) {
    printf("\n======================================================\n");
    printf("         APPOINTEASE APPOINTMENT CONFIRMATION         \n");
    printf("======================================================\n");
    printf(" Booking Type   : %s\n", is_service ? "Diagnostic Service" : "OPD Doctor Consult");
    printf("------------------------------------------------------\n");
    printf(" Patient Name   : %s\n", patient);
    printf(" Resource/Doctor: %s\n", doctor_or_service);
    printf(" Date & Time    : %s at %s\n", date, time);
    printf("------------------------------------------------------\n");
    printf(" MESSAGE TO PATIENT:\n");
    printf(" Dear %s, your appointment for %s on %s at %s has been confirmed.\n", patient, doctor_or_service, date, time);
    printf("\n MESSAGE TO %s:\n", is_service ? "DEPARTMENT" : "DOCTOR");
    printf(" Notice: Patient %s is scheduled for %s at %s.\n", patient, date, time);
    printf("======================================================\n\n");
    fflush(stdout);
}

static void book_slot(int fd, const char *q, const char *body, int is_service) {
    char rid[32], pid[32], date[32], time[32];
    get_param(q, body, is_service ? "service_id" : "doctor_id", rid, sizeof rid);
    get_param(q, body, "patient_id", pid, sizeof pid);
    get_param(q, body, "date", date, sizeof date);
    get_param(q, body, "time", time, sizeof time);
    int resource_id = atoi(rid), patient_id = atoi(pid);
    if (resource_id <= 0 || patient_id <= 0 || !*date || !*time) return send_error(fd, 400, "Missing fields.");
    
    sqlite3_stmt *st = prep(is_service ? 
        "INSERT INTO service_bookings(service_id,patient_id,date,time) VALUES(?,?,?,?)" :
        "INSERT INTO appointments(doctor_id,patient_id,date,time) VALUES(?,?,?,?)");
    sqlite3_bind_int(st, 1, resource_id);
    sqlite3_bind_int(st, 2, patient_id);
    sqlite3_bind_text(st, 3, date, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, time, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) return send_error(fd, 400, "Booking failed.");

    char rname[FIELD_MAX] = "Assigned Resource", pname[FIELD_MAX] = "Patient";
    char name_sql[256];
    snprintf(name_sql, sizeof name_sql, "SELECT name FROM %s WHERE id=%d", is_service ? "services" : "doctors", resource_id);
    sqlite3_stmt *st_name = prep(name_sql);
    if (sqlite3_step(st_name) == SQLITE_ROW) {
        strncpy(rname, (const char *)sqlite3_column_text(st_name, 0), sizeof(rname) - 1);
    }
    sqlite3_finalize(st_name);

    snprintf(name_sql, sizeof name_sql, "SELECT name FROM patients WHERE id=%d", patient_id);
    st_name = prep(name_sql);
    if (sqlite3_step(st_name) == SQLITE_ROW) {
        strncpy(pname, (const char *)sqlite3_column_text(st_name, 0), sizeof(pname) - 1);
    }
    sqlite3_finalize(st_name);

    generate_appointment_letter(rname, pname, date, time, is_service);
    send_new_id(fd);
}

static void cancel_slot(int fd, const char *q, const char *body, int is_service) {
    char idstr[32];
    get_param(q, body, "id", idstr, sizeof idstr);
    int id = atoi(idstr);
    if (id <= 0) return send_error(fd, 400, "Invalid ID.");
    sqlite3_stmt *st = prep(is_service ? "DELETE FROM service_bookings WHERE id=?" : "DELETE FROM appointments WHERE id=?");
    sqlite3_bind_int(st, 1, id);
    sqlite3_step(st);
    sqlite3_finalize(st);
    send_json(fd, "{\"success\":true}");
}

static void serve_static(int fd, const char *path) {
    const char *fpath = (!strcmp(path, "/") || !*path) ? "index.html" : (path + 1);
    FILE *f = fopen(fpath, "rb");
    if (!f) return send_error(fd, 404, "Not found.");
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *data = malloc(sz);
    fread(data, 1, sz, f);
    fclose(f);
    const char *mime = "text/plain";
    if (strstr(fpath, ".html")) mime = "text/html";
    else if (strstr(fpath, ".css")) mime = "text/css";
    else if (strstr(fpath, ".js")) mime = "application/javascript";
    send_response(fd, 200, mime, data, sz);
    free(data);
}

static int is_admin(const char *q, const char *body) {
    char pin[64];
    get_param(q, body, "admin_pin", pin, sizeof pin);
    return strcmp(pin, ADMIN_PIN) == 0;
}

static void route_api(int fd, int get, int post, int del, const char *p, const char *q, const char *body) {
    if (del || (post && !strcmp(p, "doctors"))) {
        if (!is_admin(q, body)) {
            return send_error(fd, 401, "Unauthorized: Valid Admin PIN required.");
        }
    }
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
        if (get) query_json(fd, prep("SELECT a.id, d.name AS doctor, p.name AS patient, a.date, a.time FROM appointments a JOIN doctors d ON d.id=a.doctor_id JOIN patients p ON p.id=a.patient_id ORDER BY a.date, a.time"));
        else if (post) book_slot(fd, q, body, 0);
        else if (del) cancel_slot(fd, q, body, 0);
        else send_error(fd, 400, "Bad method.");
    } else if (!strcmp(p, "services")) {
        if (get) query_json(fd, prep("SELECT id, name FROM services ORDER BY id"));
        else send_error(fd, 400, "Bad method.");
    } else if (!strcmp(p, "service-bookings")) {
        if (get) query_json(fd, prep("SELECT b.id, s.name AS service, p.name AS patient, b.date, b.time FROM service_bookings b JOIN services s ON s.id=b.service_id JOIN patients p ON p.id=b.patient_id ORDER BY b.date, b.time"));
        else if (post) book_slot(fd, q, body, 1);
        else if (del) cancel_slot(fd, q, body, 1);
        else send_error(fd, 400, "Bad method.");
    } else {
        send_error(fd, 404, "API route not found.");
    }
}

static void handle_client(int fd) {
    char buf[REQ_MAX];
    ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) { close(fd); return; }
    buf[n] = 0;

    char method[16], path[256];
    sscanf(buf, "%15s %255s", method, path);
    int get = !strcmp(method, "GET"), post = !strcmp(method, "POST"), del = !strcmp(method, "DELETE");
    if (!strcmp(method, "OPTIONS")) { send_response(fd, 204, "text/plain", NULL, 0); close(fd); return; }

    char *body = strstr(buf, "\r\n\r\n");
    if (body) body += 4;
    char *q = strchr(path, '?');
    if (q) *q++ = 0;

    if (!strncmp(path, "/api/", 5)) {
        route_api(fd, get, post, del, path + 5, q, body);
    } else {
        serve_static(fd, path);
    }
    close(fd);
}

static void init_db() {
    sqlite3_open("appointease.db", &db);
    sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS doctors (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT, specialization TEXT);", 0, 0, 0);
    sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS patients (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT, phone TEXT);", 0, 0, 0);
    sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS appointments (id INTEGER PRIMARY KEY AUTOINCREMENT, doctor_id INT, patient_id INT, date TEXT, time TEXT);", 0, 0, 0);
    sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS services (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT);", 0, 0, 0);
    sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS service_bookings (id INTEGER PRIMARY KEY AUTOINCREMENT, service_id INT, patient_id INT, date TEXT, time TEXT);", 0, 0, 0);
    
    sqlite3_exec(db, "INSERT OR IGNORE INTO services (id, name) VALUES (1, 'X-Ray'), (2, 'CT Scan'), (3, 'MRI');", 0, 0, 0);
}

int main() {
    signal(SIGPIPE, SIG_IGN);
    init_db();
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);

    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_addr.s_addr = INADDR_ANY, .sin_port = htons(PORT) };
    bind(server_fd, (struct sockaddr *)&addr, sizeof addr);
    listen(server_fd, 10);
    printf("AppointEase server running on port %d...\n", PORT);

    while (1) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd >= 0) handle_client(client_fd);
    }
    return 0;
}
