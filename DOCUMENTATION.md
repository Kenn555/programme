# Documentation complète — Chat multiclient en C

Ce document décrit l'**architecture**, les **flux** et les **dépendances** du
chat multiclient en C (serveur + client) avec salons et historique SQLite.

---

## 1. Vue d'ensemble

Le système est composé de deux programmes distincts qui dialoguent par socket TCP :

```
┌───────────────┐        TCP          ┌───────────────┐
│  client.exe   │ ◄─────────────────► │  server.exe   │
│  (N instances)│  lignes texte \n    │  (1 instance) │
└───────────────┘                     └──────┬────────┘
                                            │ SQLite
                                      ┌─────▼─────┐
                                      │  chat.db  │
                                      └───────────┘
```

- **Client** : interface utilisateur, envoie des commandes/messages, affiche les
  réponses avec mise en forme colorée.
- **Serveur** : orchestre les connexions, gère les salons, persiste les messages
  dans SQLite et diffuse (broadcast) les messages aux clients concernés.

Le serveur est **mono-processus et mono-thread** : il gère tous les clients avec
`select()` (multiplexage d'E/S), aucune thread n'est créée côté serveur.

Le client utilise **une `thread`** uniquement pour lire le clavier ; sa boucle
principale utilise `select()` sur la socket.

---

## 2. Architecture du serveur (`server.c`)

### 2.1 Structures de données

```c
typedef struct {
    int  id;              /* ID SQLite du salon */
    char name[MAX_ROOMNAME];       /* 32 caractères */
    int  fds[MAX_CLIENTS];         /* sockets des membres */
    int  count;                    /* nb de membres */
} Room;

typedef struct {
    int  fd;                      /* socket du client */
    char pseudo[MAX_PSEUDO];      /* 32 caractères */
    int  room_idx;                /* index dans rooms[] ; -1 = aucun */
    char inbuf[MAX_MSG];          /* buffer de réception ligne */
    int  inlen;
} Client;
```

- Tableaux statiques : `clients[MAX_CLIENTS]` (30), `rooms[MAX_ROOMS]` (20).
- Un **salon `general` par défaut** est créé au démarrage.
- `room_idx` relie chaque client à son salon courant.

### 2.2 Boucle principale (`main`)

```
WSAStartup
db_init()                       -- ouvre chat.db, crée les tables
db_get_or_create_room("general")
socket / bind / listen
BOUCLE :
  select(listen_sock + toutes les sockets clients)
  si listen_sock prêt  -> accept_client()
  pour chaque client prêt -> read_client()
```

`select()` surveille simultanément :
1. la **socket d'écoute** (nouvelle connexion),
2. toutes les **sockets clients** (données entrantes, déconnexion).

Aucune attente bloquante : `select` rend la main dès qu'un descripteur est prêt.

### 2.3 Gestion d'un client (`read_client`)

1. `recv()` les octets disponibles.
2. Alimente `inbuf` et découpe en **lignes terminées par `\n`**.
3. Chaque ligne complète passe dans `handle_line()`.

Si `recv` retourne `<= 0` → déconnexion : notification aux autres membres du
salon, retrait de la liste, fermeture de la socket.

### 2.4 Répartition commande / message (`handle_line`)

- Ligne commençant par `/` → commande.
- Sinon → message de discussion (`do_broadcast_text`).

### 2.5 Couche SQLite

| Fonction | Rôle |
|----------|------|
| `db_init()` | Ouvre `chat.db`, `CREATE TABLE IF NOT EXISTS` |
| `db_get_or_create_room(name)` | Retourne l'ID d'un salon, le crée s'il manque |
| `db_insert_message(...)` | Insère un message avec date/heure locale |
| `db_last_message_date(room)` | Date du dernier message (détection changement de jour) |
| `db_send_history(fd, room)` | Envoie les 50 derniers messages chrono. avec bandeaux de date |

---

## 3. Architecture du client (`client.c`)

### 3.1 Initialisation

1. `enable_ansi()` active le rendu des couleurs ANSI sur la console Windows.
2. `WSAStartup` + création socket.
3. `connect()` à `adresse:port`.

### 3.2 Deux chemins d'exécution

```
Thread d'entrée (input_thread)        Boucle principale (main)
  fgets() → lit le clavier        ┐   select()/recv() → messages serveur
  send_line() → envoie au réseau  ┘   print_line() → affichage coloré
```

- **Thread clavier** : lit une ligne au clavier, l'envoie telle quelle (le
  serveur ajoute lui-même l'heure lors du broadcast de retour).
- **Boucle réseau** : reçoit les données du serveur, découpe en lignes et les
  affiche via `print_line()`.

### 3.3 Mise en forme (`print_line`)

Classement automatique de chaque ligne reçue :

| Type de ligne | Détection | Mise en forme |
|---------------|-----------|---------------|
| Message `[HH:MM] pseudo: contenu` | motif `[` `]` `: ` | heure **grise**, pseudo **couleur stable**, contenu normal |
| Bandeau de date `----- jj/mm/aaaa -----` | commence par `---` | **cyan gras** |
| Entête `--- ... ---` | commence par `---` | **cyan gras** |
| `[system] ...` | préfixe `[system]` | **rouge** |
| Notices (rejoint/quitte/déconnexion) | mots-clés | **cyan** |
| Autre | — | couleur par défaut |

Couleur du pseudo : hash du nom → palette fixe de 6 couleurs.

---

## 4. Protocole (format des échanges)

Le protocole est **orienté ligne** (chaque message se termine par `\n`), texte
brut, encodé en UTF-8/ASCII selon la console.

```
Client ──► Serveur :          "<ligne>\n"
Serveur ──► Client :          caractères + "\n" (peut contenir plusieurs lignes)
```

### Exemples de lignes serveur → client

```
--- Bienvenue dans le chat ! ---
--- Salons ---
general (0 connecte(s))
--- Fin ---
----- 27/08/2026 -----
[11:53] Alice: salut tout le monde
[system] Vous n'etes dans aucun salon. Tapez /join <salon>
Alice a rejoint le salon
```

### Commandes client → serveur

| Commande | Flux | Effet serveur |
|----------|------|---------------|
| `/register <pseudo> <mdp>` | ligne unique | crée le compte (unique) puis connecte |
| `/login <pseudo> <mdp>` | ligne unique | authentifie et connecte le client |
| `/nom <pseudo>` | ligne unique | change le pseudo du client (après connexion) |
| `/join <salon>` | ligne unique | crée/rejoint le salon, envoie `history` |
| `/leave` | ligne unique | retour au salon `general` |
| `/rooms` | ligne unique | envoie la liste des salons |
| `/history` | ligne unique | envoie les 50 derniers messages |
| `<texte>` | ligne unique | insert SQLite + broadcast au salon |
| `/quit` | ligne unique | ferme la connexion |

> **Authentification obligatoire** : à la connexion, seul `/register`, `/login`
> et `/quit` sont acceptés. Toute autre commande ou message déclenche
> `[system] Veuillez vous connecter...` jusqu'à validation.

---

## 5. Flux détaillés

### 5.1 Authentification (register / login)

```
CLIENT                        SERVEUR                     SQLITE
  │                              │                          │
  ├── TCP connect ──────────────►│ accept_client()          │
  │                              ├── envoie "Bienvenue"     │
  │◄── invite auth ──────────────┤  (+ /register / /login)  │
  │                              │   (room_idx = -1,        │
  │                              │    authenticated = 0)    │
  │                              │                          │
  ├── /register Alice mdp ──────►│ do_register()            │
  │                              ├── valid_username/mdp     │
  │                              ├── hash mdp (SHA-256+salt)│
  │                              ├── INSERT INTO users ────►│
  │                              ├── authenticated = 1      │
  │                              ├── pseudo = "Alice"       │
  │◄── "Compte cree. Bienvenue" ─┤                          │
  │                              ├── auth_complete_session()│
  │                              ├── room_add_client(general)│
  │                              ├── db_send_history(general)│
  │◄── historique + "X a rejoint"┤                          │
  │                              │                          │
  ├── /login Alice mdp ─────────►│ do_login()               │
  │                              ├── hash mdp, compare    ─►│ SELECT password_hash
  │                              ├── (si OK) authenticated  │
  │◄── "Connecte. Bienvenue" ────┤  puis auth_complete_session
```

`hash_password(user, mdp)` calcule `SHA-256(user + ":" + mdp)` → stocké en hexa
(64 caractères). Jamais de mot de passe en clair dans `chat.db`.

### 5.2 Connexion d'un client (après authentification)

```
CLIENT                        SERVEUR                     SQLITE
  │                              │                          │
  ├── TCP connect ──────────────►│ accept_client()          │
  │                              ├── envoie "Bienvenue"     │
  │                              ├── envoie liste salons    │
  │                              ├── room_add_client(general)│
  │                              ├── db_send_history(general)│
  │                              │      │──── SELECT messages ──>│
  │                              │      │<──── 50 lignes ──────── │
  │◄── welcome + salons + hist ──┤      │                        │
  │◄── "X a rejoint le chat" ────┤ (broadcast aux autres)        │
```

### 5.2 Envoi d'un message (`do_broadcast_text`)

```
CLIENT A                      SERVEUR                       SQLITE
  │                              │                             │
  ├── "coucou\n" ───────────────►│                              │
  │                              ├─ db_last_message_date(room)  │
  │                              ├── (si changement de jour)    │
  │                              │     envoie bandeau ─── toutes │
  │                              ├─ db_insert_message ─────────►│
  │                              ├── broadcast "[HH:MM] A: coucou"
  │◄── message ─────────────────┤        (à A et tous du salon) │
```

### 5.3 Rejoindre un salon (`/join`)

```
  ┌── quitte l'ancien salon (retrait membre + notification)
  ├── db_get_or_create_room(name)   → ID salon
  ├── room_register_from_db         → index mémoire
  ├── room_add_client               → ajoute au salon
  ├── "Vous avez rejoint le salon [x]"
  ├── "X a rejoint le salon"        (broadcast aux membres)
  └── db_send_history(room)         → bandeaux + 50 messages
```

### 5.4 Déconnexion (`/quit` ou fermeture)

```
recv() <= 0 → broadcast "X s'est deconnecte" aux membres du salon
            → room_remove_client()
            → closesocket()
```

### 5.5 Persistance de l'historique

- Chaque message est inséré avec sa **date/heure locale** (`created_at` au format
  `YYYY-MM-DD HH:MM`), pas le timestamp UTC du serveur SQLite par défaut.
- À `join`/`history`, le serveur relit les 50 derniers messages dans l'ordre
  chronologique et **regroupe par date** (bandeau `----- jj/mm/aaaa -----` dès
  que la date change).
- Détection du changement de jour en direct via `db_last_message_date`.

---

## 6. Base de données SQLite (`chat.db`)

### Schéma

```sql
CREATE TABLE rooms (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT UNIQUE NOT NULL
);

CREATE TABLE users (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    username TEXT UNIQUE NOT NULL,
    password_hash TEXT NOT NULL,
    created_at DATETIME DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE messages (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    room_id INTEGER NOT NULL,
    username TEXT NOT NULL,
    content TEXT NOT NULL,
    created_at DATETIME DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (room_id) REFERENCES rooms(id)
);
```

Les tables sont créées automatiquement si `chat.db` n'existe pas. Une table
d'index sur `messages(room_id, id)` améliorerait les gros volumes.

### Requêtes principales

- Insérer : `INSERT INTO messages(room_id, username, content, created_at) VALUES(?,?,?,?)`
- Dernier message : `SELECT created_at FROM messages WHERE room_id=? ORDER BY id DESC LIMIT 1`
- Historique : 
  ```sql
  WITH last AS (
    SELECT id, username, content, created_at FROM messages
    WHERE room_id=? ORDER BY id DESC LIMIT 50
  ) SELECT username, content, created_at FROM last ORDER BY id ASC;
  ```

---

## 7. Dépendances

### 7.1 Serveur (`server.exe`)

| Dépendance | Type | Rôle |
|------------|------|------|
| `winsock2.h` / `ws2_32` | en-tête / lib système Windows | sockets TCP (`socket`, `bind`, `listen`, `select`, `recv`, `send`, `accept`) |
| `windows.h` | en-tête système | types Windows (`SOCKET`, `fd_set`, `WSAStartup`...) |
| `sqlite3.h` / `sqlite3.c` | amalgamation (inclus au projet) | base de données locale persistante |
| `stdio.h`, `stdlib.h`, `string.h`, `time.h` | bibliothèque C standard | E/S, conversion, chaînes, horodatage |

**Résolution** : le code propriétaire (`server.c`) est compilé AVEC `sqlite3.c`
(amalgamation) en un seul exécutable. Aucune bibliothèque externe à installer.

### 7.2 Client (`client.exe`)

| Dépendance | Type | Rôle |
|------------|------|------|
| `winsock2.h` / `ws2_32` | en-tête / lib système Windows | sockets TCP (`socket`, `connect`, `recv`, `send`) |
| `windows.h` | en-tête système | console (`SetConsoleMode`/ANSI), types |
| `conio.h` / `process.h` | en-tête système | `_beginthreadex` (thread de lecture clavier) |
| `stdio.h`, `stdlib.h`, `string.h` | bibliothèque C standard | E/S, chaînes |

**Le client n'utilise PAS SQLite** — toute la persistance est côté serveur.

### 7.3 Outils de compilation

- Compilateur : **GCC MinGW-w64** (MSYS2 UCRT64), version testée 14.2.0.
- Lien avec la lib Winsock `-lws2_32`.
- Options serveur : `-DSQLITE_THREADSAFE=0` (mono-thread → SQLite plus rapide),
  `-DSQLITE_OMIT_LOAD_EXTENSION` (désactive le chargement de modules).

---

## 8. Compilation

```bash
make
# équivalent :
gcc -Wall -O2 -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION \
    -o server.exe server.c sqlite3.c -lws2_32
gcc -Wall -O2 -o client.exe client.c -lws2_32
```

---

## 9. Limites connues et évolutions

| Limite actuelle | Amélioration possible |
|-----------------|------------------------|
| `MAX_CLIENTS` fixé à 30 | liste chaînée + allocation dynamique pour un nombre illimité |
| `MAX_ROOMS` fixé à 20 | allocation dynamique, gestion de la suppression |
| TCP en clair | chiffrement TLS |
| Historique limité à 50 lignes affichées | pagination / recherche dans l'historique |
| Pas de salon privé | messages directs (1:1) ou salon par mot de passe |
| Heure `HH:MM` seulement | ajouter `SS` optionnel ou fuseaux par client |
| Connexions par login | authentification (mot de passe) |
