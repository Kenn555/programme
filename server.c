#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

static void db_insert_message(int room_id, const char *username, const char *content) {
    sqlite3_stmt *stmt;
    const char *q = "INSERT INTO messages(room_id, username, content) VALUES(?1, ?2, ?3);";
    if (sqlite3_prepare_v2(db, q, -1, &stmt, NULL) != SQLITE_OK) return;
    sqlite3_bind_int(stmt, 1, room_id);
    sqlite3_bind_text(stmt, 2, username, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, content, -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

/* Envoie au client l'historique des HISTORY_LIMIT derniers messages d'un salon. */
static void db_send_history(int fd, int room_id) {
    sqlite3_stmt *stmt;
    const char *q =
        "SELECT username, content FROM messages "
        "WHERE room_id = ?1 ORDER BY id DESC LIMIT ?2;";
    if (sqlite3_prepare_v2(db, q, -1, &stmt, NULL) != SQLITE_OK) return;
    sqlite3_bind_int(stmt, 1, room_id);
    sqlite3_bind_int(stmt, 2, HISTORY_LIMIT);

    /* Récupère les messages dans l'ordre chronologique. */
    int rows = 0;
    char **names = NULL, **contents = NULL;
    int *order = NULL;
    while (sqlite3_step(stmt) == SQLITE_ROW && rows < HISTORY_LIMIT) {
        char **tmp = realloc(names, (rows + 1) * sizeof(char *));
        if (!tmp) break;
        names = tmp;
        contents = realloc(contents, (rows + 1) * sizeof(char *));
        order = realloc(order, (rows + 1) * sizeof(int));
        const char *n = (const char *)sqlite3_column_text(stmt, 0);
        const char *c = (const char *)sqlite3_column_text(stmt, 1);
        names[rows] = _strdup(n ? n : "");
        contents[rows] = _strdup(c ? c : "");
        order[rows] = rows;
        rows++;
    }
    sqlite3_finalize(stmt);

    /* Tableau order[] déjà dans l'ordre DESC (rows-1 = plus ancien). */
    char line[MAX_MSG + 64];
    for (int i = rows - 1; i >= 0; i--) {
        snprintf(line, sizeof(line), "[historique] %s: %s\n", names[i], contents[i]);
        send_all(fd, line);
        free(names[i]);
        free(contents[i]);
    }
    free(names);
    free(contents);
    free(order);
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
        send_all(clients[ci].fd, "Vous n'etes dans aucun salon. Tapez /join <salon>\n");
        return;
    }
    if (strlen(text) == 0) return;

    db_insert_message(rooms[room_idx].id, clients[ci].pseudo, text);

    char line[MAX_MSG + 64];
    snprintf(line, sizeof(line), "[%s] %s: %s\n", rooms[room_idx].name, clients[ci].pseudo, text);
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

        if (strcmp(cmd, "/quit") == 0) {
            closesocket(clients[ci].fd);
            clients[ci].fd = -1;
            return;
        } else if (strcmp(cmd, "/nom") == 0) {
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
    snprintf(clients[ci].pseudo, sizeof(clients[ci].pseudo), "Client%d", (int)newfd);

    send_all(newfd, "--- Bienvenue dans le chat ! ---\n");
    do_list_rooms(ci);

    /* Ajoute au salon general et charge l'historique. */
    room_add_client(default_room_idx, ci);
    send_all(newfd, "--- Historique du salon general ---\n");
    db_send_history(newfd, rooms[default_room_idx].id);
    send_all(newfd, "--- Fin historique ---\n");

    char notice[MAX_PSEUDO + 16];
    snprintf(notice, sizeof(notice), "%s a rejoint le chat\n", clients[ci].pseudo);
    broadcast_room(default_room_idx, notice, newfd);
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
