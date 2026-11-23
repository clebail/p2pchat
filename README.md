# P2P Chat

Micro messagerie pair-à-pair en Qt 6 / C++ (Linux, macOS, Windows) :
- **aucun serveur à héberger** ;
- **aucune configuration de box** : ça fonctionne derrière n'importe quelle box, en 4G, en CGNAT, etc. ;
- connexions sortantes uniquement vers les **ports 443/80** ;
- chiffrement de bout en bout.

## Principe

Chaque programme lance sa propre instance de **Tor** et y publie un *service onion*, c'est-à-dire une
adresse `xxxx.onion` fixe, joignable de partout sans ouvrir de port. Le réseau Tor sert uniquement de
transport. Vous n'hébergez rien, et personne ne peut lire les messages.

### Mise en relation (une seule fois)

1. Vous cliquez sur **Inviter**. Un code `P2PCHAT-…` s'affiche (votre adresse onion + un secret
   aléatoire de 128 bits), avec un bouton **Envoyer par mail**.
2. Votre correspondant installe P2P Chat, clique sur **Rejoindre** et colle le code.
3. À la première connexion, il vous transmet sa propre adresse onion.

Ensuite il n'y a plus rien à faire, même si vos IP changent. Chacun ouvre le programme quand il veut :
le premier lancé attend, le second se connecte tout seul, et chacun réessaie toutes les 30 s.
Il n'y a pas de messages hors ligne : les deux programmes doivent être ouverts en même temps.

## Installer tor

P2P Chat lance sa **propre** instance de tor. Elle est indépendante d'un éventuel service système,
et son port SOCKS est choisi au hasard.

| Système | Installation |
|---|---|
| Linux | `sudo apt install tor`. Inutile d'activer le service. |
| macOS | `brew install tor`, **ou** Tor Browser installé dans Applications, **ou** copier `tor` (+ ses `.dylib`) dans `P2PChat.app/Contents/MacOS/` |
| Windows | « Tor Expert Bundle » (torproject.org) : copier son dossier `tor\` à côté de `p2pchat.exe`. Tor Browser sur le Bureau fonctionne aussi. |

Le programme cherche aussi dans `/opt/homebrew/bin`, `/usr/local/bin` et le `PATH`. Le menu d'erreur
permet de choisir l'exécutable à la main (clé `torPath` de la configuration).

## Compiler

Dépendances : Qt 6 (Widgets, Network) et OpenSSL 3 (libcrypto).

**Linux**
```bash
sudo apt install qt6-base-dev libssl-dev tor
mkdir build && cd build && qmake6 ../p2pchat.pro && make -j
./p2pchat
```

**macOS**
```bash
brew install qt openssl@3 tor
mkdir build && cd build && qmake ../p2pchat.pro && make -j
macdeployqt P2PChat.app           # rend l'application autonome
open P2PChat.app
```
Le `.pro` trouve OpenSSL dans `/opt/homebrew/opt/openssl@3` (Apple Silicon) ou `/usr/local/opt/openssl@3` (Intel).
Sinon, définir `OPENSSL_DIR`. L'application n'étant pas signée, au premier lancement il faut faire
clic droit → **Ouvrir**.

**Windows** (MSYS2 / MinGW)
```bash
pacman -S mingw-w64-x86_64-qt6-base mingw-w64-x86_64-openssl
mkdir build && cd build && qmake6 ../p2pchat.pro && mingw32-make
windeployqt release/p2pchat.exe   # + copier libcrypto-3-x64.dll et le dossier tor\
```

## Sécurité

- Transport : service onion Tor, qui est déjà chiffré et authentifié par l'adresse onion.
- Par-dessus, à chaque connexion : échange de clés **X25519** éphémère, puis **HKDF-SHA256** salé avec le
  secret de l'invitation, puis **ChaCha20-Poly1305**, avec des compteurs qui empêchent rejeu et réordonnancement.
  Sans le secret, une connexion est rejetée dès le premier message.
- Le serveur local n'écoute que sur `127.0.0.1` : il n'est joignable que via le service onion.
- Gardez le code d'invitation pour vous, et utilisez **Réinitialiser** pour oublier le correspondant.

## Fichiers

| Fichier | Rôle |
|---|---|
| `torservice.*` | Lancement de tor, torrc, progression, adresse onion |
| `peerconnection.*` | Trames, poignée de main, messages chiffrés, ping |
| `crypto.*` | X25519, HKDF, ChaCha20-Poly1305 (OpenSSL) |
| `invitation.*` | Encodage / décodage du code d'invitation |
| `mainwindow.*` | Interface, mise en relation, reconnexion automatique, historique |

La configuration (`p2pchat.ini`) est dans `~/.config/p2pchat/` (Linux), `~/Library/Preferences/p2pchat/` (macOS)
ou `%APPDATA%\p2pchat\` (Windows). L'historique et les clés Tor sont dans le dossier de données de l'application
(`~/.local/share/p2pchat/p2pchat/`, `~/Library/Application Support/p2pchat/p2pchat/`…).
Supprimer le sous-dossier `tor/hs` change votre adresse onion.
