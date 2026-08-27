#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <process.h>

#define MAX_MSG 1024

static SOCKET sockfd;
static volatile int running = 1;

static void send_line(const char *line) {
    int len = (int)strlen(line);
    int total = 0;
    while (total < len) {
        int n = send(sockfd, line + total, len - total, 0);
        if (n <= 0) { running = 0; return; }
        total += n;
    }
}

/* Thread lecture clavier : envoie chaque ligne saisie au serveur. */
static unsigned __stdcall input_thread(void *param) {
    char line[MAX_MSG];
    while (running && fgets(line, sizeof(line), stdin)) {
        /* retire le \n pour l'affichage utilisateur (le serveur en ajoute) */
        for (char *p = line; *p; p++) {
            if (*p == '\n') { *p = '\0'; break; }
        }
        if (running) {
            char out[MAX_MSG + 2];
            snprintf(out, sizeof(out), "%s\n", line);
            send_line(out);
        }
    }
    running = 0;
    shutdown(sockfd, SD_SEND);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("Usage : %s <adresse> <port>\n", argv[0]);
        printf("Exemple : %s 127.0.0.1 5555\n", argv[0]);
        return 1;
    }

    const char *host = argv[1];
    int port = atoi(argv[2]);

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "Echec WSAStartup\n");
        return 1;
    }

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == INVALID_SOCKET) {
        fprintf(stderr, "Erreur socket\n");
        return 1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = inet_addr(host);
    if (addr.sin_addr.s_addr == INADDR_NONE) {
        fprintf(stderr, "Adresse invalide : %s\n", host);
        return 1;
    }

    if (connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        fprintf(stderr, "Connexion impossible a %s:%d (%d)\n", host, port, WSAGetLastError());
        return 1;
    }

    printf("Connecte a %s:%d\n", host, port);
    printf("Commandes : /nom <pseudo> | /join <salon> | /leave | /rooms | /history | /quit\n\n");

    /* lance la lecture du clavier dans un thread */
    _beginthreadex(NULL, 0, input_thread, NULL, 0, NULL);

    /* boucle principale : attend les messages du serveur */
    char buf[MAX_MSG];
    while (running) {
        int n = recv(sockfd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) break;
        buf[n] = '\0';
        fputs(buf, stdout);
        fflush(stdout);
    }

    running = 0;
    closesocket(sockfd);
    WSACleanup();
    return 0;
}
