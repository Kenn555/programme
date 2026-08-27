#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "sqlite3.h"

#define PORT_DEFAULT     5555
#define MAX_CLIENTS      30
#define MAX_ROOMS        20
#define MAX_PSEUDO       32
#define MAX_ROOMNAME     32
#define MAX_MSG          1024
#define HISTORY_LIMIT    50

typedef struct {
    int  id;              /* ID SQLite du salon */
    char name[MAX_ROOMNAME];
    int  fds[MAX_CLIENTS];
    int  count;
} Room;

typedef struct {
    int  fd;
    char pseudo[MAX_PSEUDO];
    int  authenticated;
    int  room_idx;        /* index dans rooms[], -1 = aucun */
    char inbuf[MAX_MSG];
    int  inlen;
} Client;

static Client clients[MAX_CLIENTS];
static Room   rooms[MAX_ROOMS];
static int    room_count = 0;
static sqlite3 *db = NULL;
static int    default_room_idx = -1;

/* ------------------------------------------------------------------ */
/* SHA-256 (hashage des mots de passe)                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    unsigned char data[64];
    unsigned int  datalen;
    unsigned long long bitlen;
    unsigned int  state[8];
} SHA256_CTX;

#define ROTRIGHT(a, b) (((a) >> (b)) | ((a) << (32 - (b))))

static const unsigned int SHA256_K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

static void sha256_transform(SHA256_CTX *ctx, const unsigned char data[]) {
    unsigned int a, b, c, d, e, f, g, h, t1, t2, m[64];
    int i;
    for (i = 0; i < 16; i++)
        m[i] = ((unsigned int)data[i * 4] << 24) | ((unsigned int)data[i * 4 + 1] << 16) |
               ((unsigned int)data[i * 4 + 2] << 8) | ((unsigned int)data[i * 4 + 3]);
    for (; i < 64; i++)
        m[i] = m[i - 16] + (ROTRIGHT(m[i - 15], 7) ^ ROTRIGHT(m[i - 15], 18) ^ (m[i - 15] >> 3)) +
               m[i - 7] + (ROTRIGHT(m[i - 2], 17) ^ ROTRIGHT(m[i - 2], 19) ^ (m[i - 2] >> 10));

    a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; d = ctx->state[3];
    e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];

    for (i = 0; i < 64; i++) {
        t1 = h + (ROTRIGHT(e, 6) ^ ROTRIGHT(e, 11) ^ ROTRIGHT(e, 25)) +
             ((e & f) ^ (~e & g)) + SHA256_K[i] + m[i];
        t2 = (ROTRIGHT(a, 2) ^ ROTRIGHT(a, 13) ^ ROTRIGHT(a, 22)) +
             ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

static void sha256_init(SHA256_CTX *ctx) {
    ctx->datalen = 0;
    ctx->bitlen = 0;
    ctx->state[0] = 0x6a09e667;
    ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372;
    ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f;
    ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab;
    ctx->state[7] = 0x5be0cd19;
}

static void sha256_update(SHA256_CTX *ctx, const unsigned char data[], size_t len) {
    size_t i;
    for (i = 0; i < len; i++) {
        ctx->data[ctx->datalen] = data[i];
        ctx->datalen++;
        if (ctx->datalen == 64) {
            sha256_transform(ctx, ctx->data);
            ctx->bitlen += 512;
            ctx->datalen = 0;
        }
    }
}

static void sha256_final(SHA256_CTX *ctx, unsigned char hash[]) {
    unsigned int i = ctx->datalen;
    ctx->data[i++] = 0x80;
    if (i > 56) {
        while (i < 64) ctx->data[i++] = 0;
        sha256_transform(ctx, ctx->data);
        i = 0;
    }
    while (i < 56) ctx->data[i++] = 0;
    ctx->bitlen += (unsigned long long)ctx->datalen * 8;
    ctx->data[63] = (unsigned char)(ctx->bitlen);
    ctx->data[62] = (unsigned char)(ctx->bitlen >> 8);
    ctx->data[61] = (unsigned char)(ctx->bitlen >> 16);
    ctx->data[60] = (unsigned char)(ctx->bitlen >> 24);
    ctx->data[59] = (unsigned char)(ctx->bitlen >> 32);
    ctx->data[58] = (unsigned char)(ctx->bitlen >> 40);
    ctx->data[57] = (unsigned char)(ctx->bitlen >> 48);
    ctx->data[56] = (unsigned char)(ctx->bitlen >> 56);
    sha256_transform(ctx, ctx->data);

    for (i = 0; i < 4; i++) {
        hash[i]      = (unsigned char)(ctx->state[0] >> (24 - i * 8));
        hash[i + 4]  = (unsigned char)(ctx->state[1] >> (24 - i * 8));
        hash[i + 8]  = (unsigned char)(ctx->state[2] >> (24 - i * 8));
        hash[i + 12] = (unsigned char)(ctx->state[3] >> (24 - i * 8));
        hash[i + 16] = (unsigned char)(ctx->state[4] >> (24 - i * 8));
        hash[i + 20] = (unsigned char)(ctx->state[5] >> (24 - i * 8));
        hash[i + 24] = (unsigned char)(ctx->state[6] >> (24 - i * 8));
        hash[i + 28] = (unsigned char)(ctx->state[7] >> (24 - i * 8));
    }
}

/* Hache username + ":" + password en hexa (64 caractères). */
static void hash_password(const char *username, const char *password, char *hexout) {
    SHA256_CTX ctx;
    unsigned char digest[32];
    char input[512];
    snprintf(input, sizeof(input), "%s:%s", username, password);
    sha256_init(&ctx);
    sha256_update(&ctx, (const unsigned char *)input, strlen(input));
    sha256_final(&ctx, digest);
    for (int i = 0; i < 32; i++)
        sprintf(hexout + i * 2, "%02x", digest[i]);
}

/* ------------------------------------------------------------------ */
/* UTILITAIRES RESEAU                                                  */
/* ------------------------------------------------------------------ */

static int send_all(int fd, const char *data) {
    int total = 0;
    int len = (int)strlen(data);
    while (total < len) {
        int n = send(fd, data + total, len - total, 0);
        if (n <= 0) return -1;
        total += n;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* CHERCHE UN CLIENT / SALON                                           */
/* ------------------------------------------------------------------ */

static int find_room_by_name(const char *name) {
    for (int i = 0; i < room_count; i++)
        if (strcmp(rooms[i].name, name) == 0) return i;
    return -1;
}

/* ------------------------------------------------------------------ */
/* SQLITE                                                              */
/* ------------------------------------------------------------------ */

static void db_init(void) {
    char *err = NULL;
    if (sqlite3_open("chat.db", &db) != SQLITE_OK) {
        fprintf(stderr, "Impossible d'ouvrir chat.db : %s\n", sqlite3_errmsg(db));
        exit(1);
    }
    const char *schema =
        "CREATE TABLE IF NOT EXISTS rooms ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  name TEXT UNIQUE NOT NULL);"
        "CREATE TABLE IF NOT EXISTS users ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  username TEXT UNIQUE NOT NULL,"
        "  password_hash TEXT NOT NULL,"
        "  created_at DATETIME DEFAULT CURRENT_TIMESTAMP);"
        "CREATE TABLE IF NOT EXISTS messages ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  room_id INTEGER NOT NULL,"
        "  username TEXT NOT NULL,"
        "  content TEXT NOT NULL,"
        "  created_at DATETIME DEFAULT CURRENT_TIMESTAMP,"
        "  FOREIGN KEY (room_id) REFERENCES rooms(id));";
    if (sqlite3_exec(db, schema, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "Erreur SQLite schema : %s\n", err);
        sqlite3_free(err);
        exit(1);
    }
}

/* Retourne l'ID SQLite du salon (créé si absent), ou -1 en cas d'erreur. */
static int db_get_or_create_room(const char *name) {
    sqlite3_stmt *stmt;
    int id = -1;

    const char *q = "SELECT id FROM rooms WHERE name = ?1;";
    if (sqlite3_prepare_v2(db, q, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
    if (sqlite3_step(stmt) == SQLITE_ROW)
        id = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);

    if (id >= 0) return id;

    q = "INSERT INTO rooms(name) VALUES(?1);";
    if (sqlite3_prepare_v2(db, q, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    id = (int)sqlite3_last_insert_rowid(db);
    sqlite3_finalize(stmt);
    return id;
}

/* Crée un utilisateur. Retourne 0 si OK, -1 si le nom existe déjà. */
static int db_create_user(const char *username, const char *password) {
    sqlite3_stmt *stmt;
    const char *q = "SELECT 1 FROM users WHERE username = ?1;";
    if (sqlite3_prepare_v2(db, q, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
    int exists = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
    if (exists) return -1;

    char hash[65];
    hash_password(username, password, hash);
    q = "INSERT INTO users(username, password_hash) VALUES(?1, ?2);";
    if (sqlite3_prepare_v2(db, q, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, hash, -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return 0;
}

/* Vérifie le couple login/mot de passe. Retourne 1 si OK, 0 sinon. */
static int db_check_user(const char *username, const char *password) {
    sqlite3_stmt *stmt;
    const char *q = "SELECT password_hash FROM users WHERE username = ?1;";
    if (sqlite3_prepare_v2(db, q, -1, &stmt, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return 0;
    }
    const char *stored = (const char *)sqlite3_column_text(stmt, 0);
    char hash[65];
    hash_password(username, password, hash);
    int ok = stored && strcmp(stored, hash) == 0;
    sqlite3_finalize(stmt);
    return ok;
}

/* Construit la date/heure locale courante au format "YYYY-MM-DD HH:MM". */
static void now_datetime(char *out, size_t sz) {
    time_t t = time(NULL);
    struct tm tmv;
    localtime_s(&tmv, &t);
    strftime(out, sz, "%Y-%m-%d %H:%M", &tmv);
}

/* Extrait la date "YYYY-MM-DD" d'une chaine "YYYY-MM-DD HH:MM". */
static void date_part(const char *datetime, char *date, size_t sz) {
    if (strlen(datetime) >= 10) {
        snprintf(date, sz, "%.10s", datetime);
    } else {
        snprintf(date, sz, "%s", datetime);
    }
}

/* Retourne la date du dernier message d'un salon (ou NULL si vide). */
static const char *db_last_message_date(int room_id) {
    static char date[16];
    sqlite3_stmt *stmt;
    const char *q =
        "SELECT created_at FROM messages "
        "WHERE room_id = ?1 ORDER BY id DESC LIMIT 1;";
    if (sqlite3_prepare_v2(db, q, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_int(stmt, 1, room_id);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *dt = (const char *)sqlite3_column_text(stmt, 0);
        date_part(dt, date, sizeof(date));
        sqlite3_finalize(stmt);
        return date;
    }
    sqlite3_finalize(stmt);
    return NULL;
}

static void db_insert_message(int room_id, const char *username, const char *content) {
    sqlite3_stmt *stmt;
    const char *q =
        "INSERT INTO messages(room_id, username, content, created_at) "
        "VALUES(?1, ?2, ?3, ?4);";
    if (sqlite3_prepare_v2(db, q, -1, &stmt, NULL) != SQLITE_OK) return;
    sqlite3_bind_int(stmt, 1, room_id);
    sqlite3_bind_text(stmt, 2, username, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, content, -1, SQLITE_STATIC);
    char dt[24];
    now_datetime(dt, sizeof(dt));
    sqlite3_bind_text(stmt, 4, dt, -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

/* Bandeau de séparation de date, style WhatsApp/Messenger. */
static void make_date_banner(const char *datetime, char *out, size_t sz) {
    char d[16];
    date_part(datetime, d, sizeof(d));
    if (strlen(d) == 10) {
        snprintf(out, sz, "----- %.*s/%.*s/%.*s -----\n", 2, d + 8, 2, d + 5, 4, d);
    } else {
        snprintf(out, sz, "----- %s -----\n", d);
    }
}

/* Envoie au client l'historique (HISTORY_LIMIT derniers) d'un salon,
   avec heure sur chaque message et bandeau de date aux changements de jour. */
static void db_send_history(int fd, int room_id) {
    sqlite3_stmt *stmt;
    const char *q =
        "WITH last AS ("
        "  SELECT id, username, content, created_at FROM messages "
        "  WHERE room_id = ?1 ORDER BY id DESC LIMIT ?2"
        ") SELECT username, content, created_at FROM last ORDER BY id ASC;";
    if (sqlite3_prepare_v2(db, q, -1, &stmt, NULL) != SQLITE_OK) return;
    sqlite3_bind_int(stmt, 1, room_id);
    sqlite3_bind_int(stmt, 2, HISTORY_LIMIT);

    char prev_date[16] = "";
    char line[MAX_MSG + 64];
    char banner[MAX_MSG + 64];

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *uname = (const char *)sqlite3_column_text(stmt, 0);
        const char *content = (const char *)sqlite3_column_text(stmt, 1);
        const char *dt = (const char *)sqlite3_column_text(stmt, 2);
        char cur_date[16];
        date_part(dt ? dt : "", cur_date, sizeof(cur_date));

        if (prev_date[0] == '\0' || strcmp(prev_date, cur_date) != 0) {
            make_date_banner(dt ? dt : "", banner, sizeof(banner));
            send_all(fd, banner);
            snprintf(prev_date, sizeof(prev_date), "%s", cur_date);
        }

        const char *hhmm = (dt && strlen(dt) >= 16) ? dt + 11 : "";
        snprintf(line, sizeof(line), "[%s] %s: %s\n",
                 hhmm[0] ? hhmm : "??:??",
                 uname ? uname : "?",
                 content ? content : "");
        send_all(fd, line);
    }
    sqlite3_finalize(stmt);
}

/* ------------------------------------------------------------------ */
/* SALONS                                                              */
/* ------------------------------------------------------------------ */

/* Ajoute l'index d'un salon dans la structure des salons en mémoire. */
static int room_register_from_db(int room_id, const char *name) {
    int idx = find_room_by_name(name);
    if (idx >= 0) return idx;

    if (room_count >= MAX_ROOMS) return -1;
    idx = room_count++;
    rooms[idx].id = room_id;
    snprintf(rooms[idx].name, sizeof(rooms[idx].name), "%s", name);
    rooms[idx].count = 0;
    return idx;
}

static void room_add_client(int room_idx, int ci) {
    Room *r = &rooms[room_idx];
    for (int i = 0; i < r->count; i++)
        if (r->fds[i] == clients[ci].fd) return;
    if (r->count < MAX_CLIENTS)
        r->fds[r->count++] = clients[ci].fd;
    clients[ci].room_idx = room_idx;
}

static void room_remove_client(int room_idx, int fd) {
    if (room_idx < 0 || room_idx >= room_count) return;
    Room *r = &rooms[room_idx];
    for (int i = 0; i < r->count; i++) {
        if (r->fds[i] == fd) {
            r->fds[i] = r->fds[r->count - 1];
            r->count--;
            break;
        }
    }
}

static void broadcast_room(int room_idx, const char *msg, int exclude_fd) {
    if (room_idx < 0 || room_idx >= room_count) return;
    Room *r = &rooms[room_idx];
    for (int i = 0; i < r->count; i++)
        if (r->fds[i] != exclude_fd)
            send_all(r->fds[i], msg);
}

/* ------------------------------------------------------------------ */
/* COMMANDES                                                           */
/* ------------------------------------------------------------------ */

static void do_broadcast_text(int ci, const char *text) {
    int room_idx = clients[ci].room_idx;
    if (room_idx < 0) {
        send_all(clients[ci].fd, "[system] Vous n'etes dans aucun salon. Tapez /join <salon>\n");
        return;
    }
    if (strlen(text) == 0) return;

    char dt[24];
    now_datetime(dt, sizeof(dt));

    /* Bandeau de date si l'on change de jour depuis le dernier message du salon. */
    const char *prev = db_last_message_date(rooms[room_idx].id);
    char cur_date[16];
    date_part(dt, cur_date, sizeof(cur_date));
    if (prev == NULL || strcmp(prev, cur_date) != 0) {
        char banner[MAX_MSG + 64];
        make_date_banner(dt, banner, sizeof(banner));
        broadcast_room(room_idx, banner, -1);
    }

    db_insert_message(rooms[room_idx].id, clients[ci].pseudo, text);

    char line[MAX_MSG + 64];
    snprintf(line, sizeof(line), "[%s] %s: %s\n", dt + 11, clients[ci].pseudo, text);
    broadcast_room(room_idx, line, -1);
}

static void do_change_name(int ci, const char *arg) {
    char newname[MAX_PSEUDO];
    sscanf(arg, "%31s", newname);
    if (newname[0] == '\0' || strlen(newname) < 1) {
        send_all(clients[ci].fd, "Usage : /nom <pseudo>\n");
        return;
    }
    snprintf(clients[ci].pseudo, sizeof(clients[ci].pseudo), "%s", newname);
    char line[MAX_PSEUDO + 32];
    snprintf(line, sizeof(line), "Vous vous appelez desormais %s\n", newname);
    send_all(clients[ci].fd, line);
}

static void do_join(int ci, const char *arg) {
    char roomname[MAX_ROOMNAME];
    sscanf(arg, "%31s", roomname);
    if (roomname[0] == '\0') {
        send_all(clients[ci].fd, "Usage : /join <salon>\n");
        return;
    }

    int room_id = db_get_or_create_room(roomname);
    if (room_id < 0) {
        send_all(clients[ci].fd, "Erreur lors de la creation du salon\n");
        return;
    }
    int old_room = clients[ci].room_idx;
    int new_idx = room_register_from_db(room_id, roomname);
    if (new_idx < 0) {
        send_all(clients[ci].fd, "Trop de salons ouverts\n");
        return;
    }

    if (old_room >= 0)
        room_remove_client(old_room, clients[ci].fd);

    room_add_client(new_idx, ci);

    char line[MAX_MSG + 64];
    snprintf(line, sizeof(line), "Vous avez rejoint le salon [%s]\n", roomname);
    send_all(clients[ci].fd, line);

    snprintf(line, sizeof(line), "%s a rejoint le salon\n", clients[ci].pseudo);
    broadcast_room(new_idx, line, clients[ci].fd);

    send_all(clients[ci].fd, "--- Historique du salon ---\n");
    db_send_history(clients[ci].fd, room_id);
    send_all(clients[ci].fd, "--- Fin historique ---\n");
}

static void do_leave(int ci) {
    int room_idx = clients[ci].room_idx;
    if (room_idx < 0) {
        send_all(clients[ci].fd, "Vous n'etes dans aucun salon\n");
        return;
    }
    if (room_idx == default_room_idx) {
        send_all(clients[ci].fd, "Vous etes deja dans le salon general\n");
        return;
    }
    char notice[MAX_PSEUDO + 32];
    snprintf(notice, sizeof(notice), "%s a quitte le salon\n", clients[ci].pseudo);
    broadcast_room(room_idx, notice, clients[ci].fd);
    room_remove_client(room_idx, clients[ci].fd);
    room_add_client(default_room_idx, ci);
    clients[ci].room_idx = default_room_idx;
    send_all(clients[ci].fd, "Vous etes revenu dans le salon general\n");
}

static void do_list_rooms(int ci) {
    char line[MAX_ROOMNAME + 16];
    send_all(clients[ci].fd, "--- Salons ---\n");
    for (int i = 0; i < room_count; i++) {
        snprintf(line, sizeof(line), "%s (%d connecte(s))\n", rooms[i].name, rooms[i].count);
        send_all(clients[ci].fd, line);
    }
    send_all(clients[ci].fd, "--- Fin ---\n");
}

static void do_history(int ci) {
    int room_idx = clients[ci].room_idx;
    if (room_idx < 0) {
        send_all(clients[ci].fd, "Vous n'etes dans aucun salon\n");
        return;
    }
    send_all(clients[ci].fd, "--- Historique du salon ---\n");
    db_send_history(clients[ci].fd, rooms[room_idx].id);
    send_all(clients[ci].fd, "--- Fin historique ---\n");
}

/* ------------------------------------------------------------------ */
/* AUTHENTIFICATION                                                    */
/* ------------------------------------------------------------------ */

static int valid_username(const char *s) {
    if (s[0] == '\0' || strlen(s) > MAX_PSEUDO - 1) return 0;
    for (const char *p = s; *p; p++)
        if (*p == ' ' || *p == '\t' || *p == ':' || *p == '\n' || *p == '\r') return 0;
    return 1;
}

static int valid_password(const char *s) {
    int len = (int)strlen(s);
    return len >= 3 && len <= 32;
}

/* Termine la connexion d'un client authentifié (salon général + historique). */
static void auth_complete_session(int ci) {
    room_add_client(default_room_idx, ci);
    clients[ci].room_idx = default_room_idx;

    send_all(clients[ci].fd, "--- Historique du salon general ---\n");
    db_send_history(clients[ci].fd, rooms[default_room_idx].id);
    send_all(clients[ci].fd, "--- Fin historique ---\n");

    char notice[MAX_MSG];
    snprintf(notice, sizeof(notice), "%s a rejoint le chat\n", clients[ci].pseudo);
    broadcast_room(default_room_idx, notice, clients[ci].fd);
}

static void do_register(int ci, const char *arg) {
    char user[33], pass[33];
    if (sscanf(arg, "%32s %32s", user, pass) != 2) {
        send_all(clients[ci].fd, "[system] Usage : /register <pseudo> <motdepasse>\n");
        return;
    }
    if (!valid_username(user)) {
        send_all(clients[ci].fd, "[system] Pseudo invalide (pas d'espaces ni de ':').\n");
        return;
    }
    if (!valid_password(pass)) {
        send_all(clients[ci].fd, "[system] Mot de passe : 3 a 32 caracteres.\n");
        return;
    }
    if (db_create_user(user, pass) != 0) {
        send_all(clients[ci].fd, "[system] Ce pseudo est deja pris.\n");
        return;
    }
    clients[ci].authenticated = 1;
    snprintf(clients[ci].pseudo, sizeof(clients[ci].pseudo), "%.31s", user);
    char line[MAX_MSG];
    snprintf(line, sizeof(line), "[system] Compte cree. Bienvenue %s !\n", user);
    send_all(clients[ci].fd, line);
    auth_complete_session(ci);
}

static void do_login(int ci, const char *arg) {
    char user[33], pass[33];
    if (sscanf(arg, "%32s %32s", user, pass) != 2) {
        send_all(clients[ci].fd, "[system] Usage : /login <pseudo> <motdepasse>\n");
        return;
    }
    if (!db_check_user(user, pass)) {
        send_all(clients[ci].fd, "[system] Identifiants invalides.\n");
        return;
    }
    clients[ci].authenticated = 1;
    snprintf(clients[ci].pseudo, sizeof(clients[ci].pseudo), "%.31s", user);
    char line[MAX_MSG];
    snprintf(line, sizeof(line), "[system] Connecte. Bienvenue %s !\n", user);
    send_all(clients[ci].fd, line);
    auth_complete_session(ci);
}

/* ------------------------------------------------------------------ */
/* TRAITEMENT D'UNE LIGNE CLIENTS                                       */
/* ------------------------------------------------------------------ */

static void handle_line(int ci, char *line) {
    /* retire le \n / \r */
    for (char *p = line; *p; p++) {
        if (*p == '\n' || *p == '\r') { *p = '\0'; break; }
    }

    if (line[0] == '/') {
        char cmd[32];
        const char *arg = line;
        int n = 0;
        while (*arg && *arg != ' ' && n < 31) cmd[n++] = *arg++;
        cmd[n] = '\0';
        while (*arg == ' ') arg++;

        /* Commandes toujours autorisees (même non connecté). */
        if (strcmp(cmd, "/quit") == 0) {
            closesocket(clients[ci].fd);
            clients[ci].fd = -1;
            return;
        } else if (strcmp(cmd, "/login") == 0) {
            do_login(ci, arg);
            return;
        } else if (strcmp(cmd, "/register") == 0) {
            do_register(ci, arg);
            return;
        }

        /* Le reste est interdit tant que l'utilisateur n'est pas connecté. */
        if (!clients[ci].authenticated) {
            send_all(clients[ci].fd,
                     "[system] Veuillez vous connecter : /login <pseudo> ou /register <pseudo> (mot de passe demande par le client)\n");
            return;
        }

        if (strcmp(cmd, "/nom") == 0) {
            do_change_name(ci, arg);
        } else if (strcmp(cmd, "/join") == 0) {
            do_join(ci, arg);
        } else if (strcmp(cmd, "/leave") == 0) {
            do_leave(ci);
        } else if (strcmp(cmd, "/rooms") == 0) {
            do_list_rooms(ci);
        } else if (strcmp(cmd, "/history") == 0) {
            do_history(ci);
        } else {
            char line2[MAX_MSG + 16];
            snprintf(line2, sizeof(line2), "Commande inconnue : %s\n", cmd);
            send_all(clients[ci].fd, line2);
        }
        return;
    }

    /* Message de discussion : interdit avant connexion. */
    if (!clients[ci].authenticated) {
        send_all(clients[ci].fd,
                 "[system] Veuillez vous connecter : /login <pseudo> ou /register <pseudo> (mot de passe demande par le client)\n");
        return;
    }

    do_broadcast_text(ci, line);
}

static void read_client(int ci) {
    Client *c = &clients[ci];
    char buf[MAX_MSG];
    int n = recv(c->fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) {
        /* deconnexion */
        if (c->room_idx >= 0) {
            char notice[MAX_PSEUDO + 8];
            snprintf(notice, sizeof(notice), "%s s'est deconnecte\n", c->pseudo);
            broadcast_room(c->room_idx, notice, c->fd);
            room_remove_client(c->room_idx, c->fd);
        }
        closesocket(c->fd);
        c->fd = -1;
        return;
    }

    for (int i = 0; i < n; i++) {
        char ch = buf[i];
        if (ch == '\n') {
            c->inbuf[c->inlen] = '\0';
            handle_line(ci, c->inbuf);
            c->inlen = 0;
        } else if (c->inlen < MAX_MSG - 1) {
            c->inbuf[c->inlen++] = ch;
        }
    }
}

/* ------------------------------------------------------------------ */
/* ACCEPTATION CLIENTS                                                 */
/* ------------------------------------------------------------------ */

static void accept_client(SOCKET listen_sock) {
    struct sockaddr_in addr;
    int addrlen = sizeof(addr);
    SOCKET newfd = accept(listen_sock, (struct sockaddr *)&addr, &addrlen);
    if (newfd == INVALID_SOCKET) return;

    int ci = -1;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd == -1) { ci = i; break; }
    }
    if (ci < 0) {
        send_all(newfd, "Serveur plein, reessayez plus tard\n");
        closesocket(newfd);
        return;
    }

    clients[ci].fd = newfd;
    clients[ci].inlen = 0;
    clients[ci].room_idx = -1;
    clients[ci].authenticated = 0;
    snprintf(clients[ci].pseudo, sizeof(clients[ci].pseudo), "Client%d", (int)newfd);

    send_all(newfd, "--- Bienvenue dans le chat ! ---\n");
    send_all(newfd, "[system] --- Authentification requise ---\n");
    send_all(newfd, "[system] Nouveau compte : /register <pseudo>\n");
    send_all(newfd, "[system] Deja inscrit :  /login <pseudo>\n");
    send_all(newfd, "[system] (le mot de passe est demande en masque par le client)\n");
    send_all(newfd, "[system] ---\n");
}

/* ------------------------------------------------------------------ */
/* MAIN                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv) {
    int port = argc > 1 ? atoi(argv[1]) : PORT_DEFAULT;
    if (port <= 0) port = PORT_DEFAULT;

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "Echec WSAStartup\n");
        return 1;
    }

    db_init();

    /* Salon general par defaut. */
    int gen_id = db_get_or_create_room("general");
    default_room_idx = room_register_from_db(gen_id, "general");

    for (int i = 0; i < MAX_CLIENTS; i++)
        clients[i].fd = -1;
    for (int i = 0; i < MAX_ROOMS; i++)
        rooms[i].count = 0;

    SOCKET listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sock == INVALID_SOCKET) {
        fprintf(stderr, "Erreur socket\n");
        return 1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((unsigned short)port);

    if (bind(listen_sock, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        fprintf(stderr, "Erreur bind sur le port %d : %d\n", port, WSAGetLastError());
        return 1;
    }
    listen(listen_sock, 10);

    printf("Serveur chat en ecoute sur le port %d\n", port);
    printf("Historique sauvegarde dans chat.db\n");
    printf("Ctrl+C pour arreter\n\n");

    while (1) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(listen_sock, &readfds);
        int maxfd = (int)listen_sock;

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].fd != -1) {
                FD_SET(clients[i].fd, &readfds);
                if (clients[i].fd > maxfd) maxfd = clients[i].fd;
            }
        }

        int activity = select(maxfd + 1, &readfds, NULL, NULL, NULL);
        if (activity < 0) {
            fprintf(stderr, "Erreur select : %d\n", WSAGetLastError());
            break;
        }

        if (FD_ISSET(listen_sock, &readfds))
            accept_client(listen_sock);

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].fd != -1 && FD_ISSET(clients[i].fd, &readfds))
                read_client(i);
        }
    }

    sqlite3_close(db);
    closesocket(listen_sock);
    WSACleanup();
    return 0;
}
