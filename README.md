# P2P Chat

Micro messagerie de groupe pair-à-pair en Qt 6 / C++ (Linux, macOS, Windows) :
- **aucun serveur à héberger** ;
- **aucune configuration de box** : ça fonctionne derrière n'importe quelle box, en 4G, en CGNAT, etc. ;
- connexions sortantes uniquement vers les **ports 443/80** ;
- chiffrement de bout en bout.

## Principe

Chaque programme lance sa propre instance de **Tor** et y publie un *service onion*, c'est-à-dire une
adresse `xxxx.onion` fixe, joignable de partout sans ouvrir de port. Le réseau Tor sert uniquement de
transport. Vous n'hébergez rien, et personne ne peut lire les messages.

### Mise en relation (une seule fois par personne)

1. Quelqu'un clique sur **Créer une conversation** (ou ⋮ → *Inviter quelqu'un…*). Un code `P2PCHAT-…`
   s'affiche : son adresse onion + le secret du groupe (128 bits). Il peut l'envoyer avec le bouton **Envoyer par mail**.
2. La personne invitée clique sur **J'ai reçu un code** et le colle.
3. À la connexion, chacun transmet la liste des membres qu'il connaît : le nouveau venu découvre tous les
   autres et se connecte à chacun d'eux (maillage complet, sans centre).

N'importe quel membre peut inviter quelqu'un d'autre. Une fois relié, le groupe ne dépend plus de celui qui
l'a créé : B et C peuvent discuter même si A est hors ligne.
Les messages sont aussi relayés par les membres (avec anti-doublon), ce qui couvre le cas d'un lien direct
pas encore établi.

Il n'y a pas de messages hors ligne : seuls les membres en ligne au moment de l'envoi reçoivent le message.
Chacun réessaie de joindre les membres absents toutes les 30 s.

### Interface

- Bulles façon messagerie mobile, avatars, regroupement des messages, séparateurs de date
- Clic droit sur un message → copier
- Menu ⋮ : inviter, rejoindre, changer de pseudo, **vider la conversation** (efface l'historique local), quitter le groupe
- Survoler la ligne d'état sous le titre affiche l'état de chaque membre
- **Notifications natives** avec l'icône de l'application quand un message arrive et que la fenêtre n'est pas
  au premier plan. Un clic sur la notification rouvre la fenêtre.
- **Linux / Windows** : icône dans la zone de notification, avec une pastille rouge s'il y a des messages non lus.
  La croix masque la fenêtre dans la zone de notification. Pour quitter : clic droit sur l'icône → *Quitter*,
  ou ⋮ → *Quitter P2P Chat*. Si le bureau n'a pas de zone de notification (GNOME sans l'extension
  AppIndicator), la croix quitte normalement.
- **macOS** : la croix masque la fenêtre sans quitter le programme. Un clic sur l'icône du Dock la rouvre,
  et Cmd+Q quitte. Le nombre de messages non lus s'affiche en pastille sur l'icône du Dock. Au premier lancement,
  macOS demande l'autorisation d'afficher des notifications. Les notifications ne fonctionnent que depuis le
  bundle `P2PChat.app`, pas en lançant le binaire directement.

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
make install   # facultatif : installe dans ~/.local (menu des applications + icône des notifications)
```

**macOS**
```bash
brew install qt openssl@3 tor     # macOS 11 minimum
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

- **Transport** : service onion Tor. Le trafic est chiffré de bout en bout, aucun relais ne connaît à la
  fois l'expéditeur et le destinataire, et les membres ne voient pas l'adresse IP des autres.
- **Canal** : à chaque connexion, échange de clés **X25519** éphémère, puis **HKDF-SHA256** salé avec le
  secret du groupe, puis **ChaCha20-Poly1305**, avec des compteurs qui empêchent rejeu et réordonnancement.
  Sans le secret, la connexion est refusée dès le premier message. Le trafic enregistré reste illisible même
  si le secret fuit plus tard.
- **Identité** : chaque poste a une clé **Ed25519**. Le code d'invitation contient celle de l'invitant, et
  les clés des autres membres sont mémorisées à la première rencontre (trousseau conservé même après avoir
  quitté le groupe). Chaque poste signe sa poignée de main (ses deux clés éphémères et son adresse onion) :
  quelqu'un qui possède le code ne peut pas se faire passer pour un autre membre.
- **Messages** : l'auteur signe chaque message (identifiant, adresse, pseudo, texte, heure). Un message
  modifié en route ou attribué à quelqu'un d'autre est rejeté.
- **Historique** : chaque ligne est enregistrée avec la signature de son auteur et revérifiée au chargement.
  « ⚠ signature invalide » signale une ligne modifiée ou inventée, et « ⚠ non signé » une ligne de l'ancien
  format. La suppression d'une ligne n'est pas détectée. L'historique n'est **pas chiffré** : il est protégé
  par les droits du fichier (600) et par le chiffrement du disque (FileVault / LUKS) s'il est activé.
- **Fichiers sensibles** : la configuration (secret du groupe, clé d'identité, trousseau) et l'historique
  sont lisibles par le seul utilisateur. Le serveur local n'écoute que sur `127.0.0.1`.
- **Limites** :
  - le code d'invitation permet d'entrer dans le groupe : envoyez-le par un canal de confiance ;
  - on ne peut pas exclure un seul membre : il faut que chacun quitte le groupe et en recrée un ;
  - les notifications affichent le texte des messages.

## Fichiers

| Fichier | Rôle |
|---|---|
| `torservice.*` | Lancement de tor, torrc, progression, adresse onion |
| `peerconnection.*` | Trames, poignée de main, messages chiffrés, ping |
| `crypto.*` | X25519, HKDF, ChaCha20-Poly1305 (OpenSSL) |
| `invitation.*` | Encodage / décodage du code d'invitation |
| `mainwindow.*` | Fenêtre, groupe (membres, maillage, relais), reconnexion automatique, historique |
| `notifier.*`, `notifier_mac.mm` | Notifications natives (D-Bus sous Linux, UserNotifications sous macOS) |
| `chatview.*` | Liste de messages en bulles (modèle + délégué de dessin) |
| `icons/` | Icône : `p2pchat.svg` (source), `.icns` (macOS), `.ico` (Windows), `.png` |

La configuration (`p2pchat.ini`) est dans `~/.config/p2pchat/` (Linux), `~/Library/Preferences/p2pchat/` (macOS)
ou `%APPDATA%\p2pchat\` (Windows). L'historique et les clés Tor sont dans le dossier de données de l'application
(`~/.local/share/p2pchat/p2pchat/`, `~/Library/Application Support/p2pchat/p2pchat/`…).
Supprimer le sous-dossier `tor/hs` change votre adresse onion.
