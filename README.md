# Chat multi-client en C

Chat en réseau TCP avec un serveur qui gère plusieurs clients en parallèle grâce à
`select()` (pas de threads côté serveur) et des **salons de discussion** dont
l'historique est **persisté dans SQLite** (`chat.db`).

## Fonctionnalités

- Serveur multi-clients via `select()` (jusqu'à `MAX_CLIENTS`, 30 par défaut)
- **Salons** : rejoindre/créer un salon, revenir au salon général
- **Historique persistant** : chaque message est stocké dans SQLite. Quand un
  client rejoint un salon, il reçoit les 50 derniers messages passés.
- Sauvegarde automatique dans `chat.db` dès le lancement du serveur

## Compilation (Windows / MinGW)

```bash
make
# ou directement :
gcc -Wall -O2 -DSQLITE_THREADSAFE=0 -o server.exe server.c sqlite3.c -lws2_32
gcc -Wall -O2 -o client.exe client.c -lws2_32
```

## Utilisation

Terminal 1 (serveur) :
```bash
server.exe 5555
```

Terminal 2, 3, ... (clients) :
```bash
client.exe 127.0.0.1 5555
```

## Commandes côté client

| Commande | Effet |
|----------|-------|
| `/nom <pseudo>` | Changer de pseudo (défaut "ClientXX") |
| `/join <salon>` | Rejoindre ou créer un salon → reçoit l'historique |
| `/leave` | Revenir au salon général |
| `/rooms` | Lister les salons et leur nombre de connectés |
| `/history` | Afficher les 50 derniers messages du salon courant |
| `/quit` | Quitter le chat |
| `<texte>` | Envoyer un message au salon courant (sauvegardé en base) |

## Fonctionnement

- **Serveur** : `select()` surveille la socket d'écoute et toutes les sockets
  clients, sans thread par client. Les messages reçus sont insérés dans SQLite
  puis broadcastés à tous les clients du même salon.
- **Salons** : stockés dans la table `rooms` ; les messages dans `messages`,
  reliés à leur salon par `room_id`. Le salon `general` est créé au lancement.
- **Historique** : à l'arrivée dans un salon, le serveur renvoie les
  `HISTORY_LIMIT` derniers messages (50) via `db_send_history()`.
- **Client** : un thread lit le clavier et envoie les lignes ; le thread
  principal surveille la socket pour afficher les messages entrants.

## Base de données (`chat.db`)

```sql
CREATE TABLE rooms (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT UNIQUE NOT NULL
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

Les tables sont créées automatiquement si `chat.db` n'existe pas.

## Limites connues / pistes d'amélioration

- Pas de chiffrement (TCP en clair) — à ajouter avec TLS si besoin réel.
- L'historique n'est chargé qu'à la connexion `/join`, pas de recherche en
  direct dans les messages.
- `MAX_CLIENTS` fixé statiquement ; passage à une liste chaînée possible.
- Suppression de salon non implémentée (les salons restent en base).
