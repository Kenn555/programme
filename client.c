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

#define RESET   "\033[0m"
#define BOLD    "\033[1m"
#define GRAY    "\033[90m"
#define RED     "\033[31m"
#define GREEN   "\033[32m"
#define YELLOW  "\033[33m"
#define BLUE    "\033[34m"
#define MAG     "\033[35m"
#define CYAN    "\033[36m"
#define WHITE   "\033[97m"

static void enable_ansi(void) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(h, &mode))
        SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
}

/* Couleur stable attribuée par pseudo (palette type WhatsApp). */
static const char *name_color(const char *name) {
    unsigned h = 0;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++)
        h = h * 31 + *p;
    static const char *palette[] = { GREEN, YELLOW, BLUE, MAG, CYAN, WHITE };
    return palette[h % 6];
}

static void send_line(const char *line) {
    int len = (int)strlen(line);
    int total = 0;
    while (total < len) {
        int n = send(sockfd, line + total, len - total, 0);
        if (n <= 0) { running = 0; return; }
        total += n;
    }
}

/* Affiche une ligne reçue avec mise en forme colorée. */
static void print_line(const char *line) {
    if (strncmp(line, "[system]", 8) == 0) {
        printf("%s%s%s\n", RED, line + 8, RESET);
        return;
    }

    if (line[0] == '-' && line[1] == '-' && line[2] == '-') {
        /* bandeau de date "----- jj/mm/aaaa -----" et entetes "---" */
        printf("%s%s%s%s\n", CYAN, BOLD, line, RESET);
        return;
    }

    /* message "[HH:MM] pseudo: contenu" */
    if (line[0] == '[') {
        const char *close = strchr(line, ']');
        if (close) {
            const char *colon = strstr(close + 1, ": ");
            if (colon) {
                size_t tl = close - line - 1;
                if (tl > 7) tl = 7;
                char timebuf[8];
                memcpy(timebuf, line + 1, tl);
                timebuf[tl] = '\0';

                /* nom avec espaces de tête retirés */
                const char *np = close + 1;
                while (np < colon && (*np == ' ' || *np == '\t')) np++;
                size_t nl = colon - np;
                if (nl > 63) nl = 63;
                char namebuf[64];
                memcpy(namebuf, np, nl);
                namebuf[nl] = '\0';

                printf("%s[%s]%s %s%s%s%s: %s%s\n",
                       GRAY, timebuf, RESET,
                       name_color(namebuf), BOLD, namebuf, RESET,
                       RESET, colon + 2);
                return;
            }
        }
    }

    if (strstr(line, "rejoint") || strstr(line, "quitt")
        || strstr(line, "deconnecte") || strstr(line, "Bienvenue")) {
        printf("%s%s%s\n", CYAN, line, RESET);
        return;
    }

    printf("%s%s\n", RESET, line);
}

/* Thread lecture clavier : envoie chaque ligne saisie au serveur. */
static unsigned __stdcall input_thread(void *param) {
    char line[MAX_MSG];
    while (running && fgets(line, sizeof(line), stdin)) {
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

    enable_ansi();

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

    printf("%s%sConnecte a %s:%d%s\n", GREEN, BOLD, host, port, RESET);
    printf("%sCommandes :%s\n", CYAN, RESET);
    printf("  %s/register <pseudo> <mdp>%s  %s/login <pseudo> <mdp>%s\n",
           GREEN, RESET, GREEN, RESET);
    printf("  /nom <pseudo>  /join <salon>  /leave  /rooms  /history  %s/quit%s\n",
           RED, RESET);
    printf("\n");

    _beginthreadex(NULL, 0, input_thread, NULL, 0, NULL);

    char buf[MAX_MSG];
    char line[MAX_MSG];
    int len = 0;
    while (running) {
        int n = recv(sockfd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) break;
        buf[n] = '\0';

        for (int i = 0; i < n; i++) {
            char ch = buf[i];
            if (ch == '\n') {
                line[len] = '\0';
                if (len > 0) print_line(line);
                len = 0;
            } else if (len < MAX_MSG - 1) {
                line[len++] = ch;
            }
        }
        fflush(stdout);
    }

    running = 0;
    closesocket(sockfd);
    WSACleanup();
    return 0;
}
