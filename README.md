# P2P Chat

Micro messagerie de groupe pair-à-pair en Qt 6 / C++ (Linux, macOS, Windows) :
- **aucun serveur à héberger** ;
- **aucune configuration de box** : ça fonctionne derrière n'importe quelle box, en 4G, en CGNAT, etc. ;
- connexions sortantes uniquement vers les **ports 443/80** ;
- chiffrement de bout en bout ;
- **console partagée** (Linux, macOS) : ouvrir sa console à un autre membre pour qu'il voie, voire tape, des
  commandes exécutées sur son poste.

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

### Synchronisation de l'historique

À chaque connexion entre deux membres, chacun envoie la liste des identifiants de ses messages des
30 derniers jours, et reçoit en retour ceux qui lui manquent, avec la signature de leur auteur. Chaque
message rattrapé est vérifié, puis inséré à sa place chronologique.

- Un poste éteint récupère ce qui s'est dit pendant son absence dès qu'il croise n'importe quel membre.
- On peut écrire même si personne n'est en ligne : le message est transmis à la prochaine connexion,
  directement ou par un autre membre qui l'a déjà reçu.
- Un membre ne peut pas inventer ni modifier de messages, puisque les signatures sont vérifiées. Il peut
  seulement ne pas transmettre ce qu'il a.
- Un nouveau membre ne reçoit pas les messages antérieurs à son arrivée, sauf si l'invitant coche
  « Partager l'historique des 30 derniers jours » dans la fenêtre d'invitation (pratique pour ajouter un de
  ses propres appareils). L'autorisation vaut pour les personnes qui rejoignent dans les 7 jours.
- *Vider la conversation* est purement local : l'historique de ce poste est effacé et ne sera pas
  ré-téléchargé. Les autres gardent le leur, et on ne peut pas effacer un message chez eux.
- Les messages rattrapés donnent lieu à une seule notification (« n messages reçus pendant votre absence »).

### Interface

- Bulles façon messagerie mobile, avatars, regroupement des messages, séparateurs de date
- **Au survol d'un message**, une petite barre apparaît au coin de la bulle : réactions rapides et ↩ *Répondre*.
- **Réactions** (👍 ❤️ 😂 😮 😢 🎉) : clic droit sur un message → *Réagir*, ou clic sur une pastille existante pour
  ajouter ou retirer la vôtre ; survoler une pastille montre qui a réagi. Chaque réaction est signée par son
  auteur, synchronisée et relayée comme les messages (pas envoyée aux versions précédentes, qui l'ignoreraient).
- **Saisie sur plusieurs lignes** : Entrée envoie, Maj+Entrée va à la ligne ; la zone grandit jusqu'à 6 lignes.
  `` `code` `` et les blocs ` ``` ` s'affichent en police à chasse fixe (les accents graves sont masqués, mais
  gardés quand on copie le message).
- **Notifications** (menu ⋮ → *Notifications*) : tous les messages, mentions et réponses seulement, ou aucune ;
  « ne pas déranger » pendant 1 h ou jusqu'au lendemain 8 h. Le compteur de non-lus reste à jour.
- **Nouveaux messages** : en revenant sur la fenêtre, un séparateur rouge marque le premier message reçu en votre
  absence, et la liste s'y place. Si vous avez remonté l'historique, un bouton « ↓ » en bas à droite indique le
  nombre de messages arrivés depuis et ramène en bas.
- **« Alice écrit… »** s'affiche sous le titre pendant qu'un membre relié directement tape un message (signal
  envoyé au plus toutes les 3 s, jamais enregistré ni relayé).
- **Recherche** (Ctrl+F) : occurrences surlignées, sans tenir compte des majuscules ni des accents ; Entrée pour
  le résultat précédent (plus ancien), Maj+Entrée pour le suivant, Échap pour fermer.
- **Liens** cliquables (http, https, www.). Ils s'ouvrent dans le navigateur, donc **hors Tor** : une confirmation
  le rappelle la première fois.
- **Images** : bouton 🖼 à gauche de la zone de saisie, glisser-déposer d'un fichier ou d'une image n'importe où
  sur la fenêtre, ou collage (Ctrl+V) d'une capture d'écran. Un aperçu apparaît au-dessus de la zone de saisie
  (✕ pour retirer) ; on peut ajouter une légende avant d'envoyer. L'image est réduite (1600 px au plus) et
  recompressée en JPEG de 150 Ko au plus, puis envoyée dans le message lui-même : chiffrée, signée par son auteur,
  synchronisée et relayée comme le texte. Clic sur une image : affichage en grand, « Enregistrer… », « Copier ».
  Un **GIF animé** de 150 Ko au plus part tel quel et s'anime dans la conversation (badge « GIF ») ; plus lourd,
  il part figé (l'aperçu le signale). Seuls les GIF visibles s'animent.
  Les versions précédentes de P2P Chat affichent « 📷 Image (mettez à jour P2P Chat pour la voir) ».
- Clic droit sur un message → copier, ou **répondre** : un bandeau « Réponse à … » apparaît au-dessus de la zone
  de saisie (✕ ou Échap pour annuler). La réponse affiche une citation du message d'origine ; un clic dessus y
  ramène. La citation porte sa propre signature de l'auteur : personne ne peut la changer ni en ajouter une
  en relayant. Les versions précédentes de P2P Chat affichent les réponses sans la citation (et les relaient
  intactes) ; l'infobulle de la ligne d'état signale les membres qui utilisent une ancienne version. Une
  réponse à l'un de vos messages est mise en avant et notifiée (« Alice vous a répondu »).
- **Mentions** : taper `@` propose les pseudos des membres (flèches puis Entrée ou Tab), `@tous` pour tout le
  monde. Liste fermée, Tab complète comme dans un terminal (le pseudo s'il est seul possible, sinon la liste). Les mentions apparaissent en gras ; un message qui vous mentionne a une bulle jaune, et sa notification
  l'indique (« Alice vous a mentionné(e) »). Elles sont retrouvées dans le texte signé : rien ne change dans le
  protocole, et personne ne peut en ajouter en relayant un message.
- Menu ⋮ : inviter, rejoindre, se reconnecter, **partager ma console**, changer de pseudo,
  **vider la conversation** (efface l'historique local), quitter le groupe
- Survoler la ligne d'état sous le titre affiche l'état de chaque membre (dernière erreur, heure du prochain essai).
  Un membre pas encore relié directement, mais connecté à un autre membre, apparaît « en ligne (via …) » : les
  messages passent par celui-ci.
- Arrivées et départs regroupés : les « X est en ligne » / « X s'est déconnecté(e) » consécutifs tiennent sur une
  seule ligne (un clic la déplie). Une coupure n'est annoncée que si le membre n'est pas revenu au bout de 60 s,
  et qu'aucun autre membre ne le voit encore en ligne.
- **Une seule instance** : relancer P2P Chat alors qu'il est déjà ouvert (par exemple caché dans la zone de
  notification) affiche simplement la fenêtre existante.
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

### Présence et reconnexion

Un membre est « en ligne » quand une connexion directe et authentifiée est ouverte avec lui. Chaque côté
envoie un ping chiffré toutes les 30 s ; sans nouvelles pendant 100 s, la connexion est fermée.

Chacun annonce aussi aux autres la liste des membres avec qui il est connecté directement. Un membre que l'on
n'a pas encore joint, mais qu'un autre voit en ligne, est affiché « en ligne (via …) » : les messages lui
parviennent par relais.

Les membres hors ligne sont rappelés de plus en plus espacé (10 s, 20 s, 40 s… jusqu'à 90 s), pour ne pas
solliciter Tor en permanence. Pendant les 2 minutes qui suivent le démarrage de Tor, un retour du réseau ou une
sortie de veille, les essais restent toutes les 10 s. Le délai repart de zéro quand le membre se connecte à nous,
quand un message de lui nous parvient par relais, quand un autre membre le voit en ligne, dès qu'une connexion
aboutit (Tor fonctionne), quand le réseau revient (Qt 6.4 et plus), à la sortie de veille, ou avec
⋮ → *Se reconnecter maintenant*. Après une veille de plus de 110 s, les connexions sont rouvertes sans attendre
le délai d'inactivité.

### Console partagée (Linux, macOS)

Pour dépanner quelqu'un ou travailler à deux sur une machine : A ouvre une console sur son poste et la
partage avec un membre B. Les deux voient la même chose ; les commandes ne s'exécutent que chez A.

1. A choisit ⋮ → **Partager ma console…**, sélectionne un membre en ligne et coche, ou non,
   « Autoriser cette personne à taper des commandes ».
2. B reçoit la proposition (avec une notification si la fenêtre est cachée) et choisit
   **Ouvrir la console** ou **Refuser**. Sans réponse, la proposition expire au bout de 2 minutes.
3. Une fenêtre de terminal s'ouvre de chaque côté. Le shell de A y démarre ; le titre de la fenêtre et un
   message en tête de console rappellent le mode (contrôle ou lecture seule).

Pendant la session :
- A peut **donner ou retirer le contrôle** à tout moment (menu ⋮). En lecture seule, ce que tape B est ignoré,
  et c'est le poste de A qui le vérifie.
- La taille de la console est la plus petite des deux fenêtres.
- Pour arrêter : `exit` dans la console, fermer sa fenêtre, ou ⋮ → *Arrêter le partage* / *Quitter la console*.
- **Coupure de la liaison** : la session reste ouverte jusqu'à 2 minutes, le temps que la connexion se rétablisse.
  A continue de travailler normalement ; à la reprise, B reçoit ce qui s'est affiché entre-temps (jusqu'à 1 Mo,
  au-delà un message signale la partie perdue). Ce que B tape pendant la coupure est ignoré, jamais exécuté
  plus tard à l'aveugle. Un message perdu en route est redemandé automatiquement.
- Une seule console partagée à la fois, avec un seul invité. B ne voit que ce qui s'affiche après son arrivée.
- Via Tor, l'écho des frappes de B prend environ une demi-seconde à une seconde : confortable pour des
  commandes, moins pour un éditeur plein écran.

Fenêtre de terminal utilisée :
- **Linux** : konsole, gnome-terminal, xfce4-terminal, kitty, alacritty, foot, wezterm, x-terminal-emulator ou
  xterm, dans cet ordre. La variable `P2PCHAT_TERMINAL` en impose un autre (appelé avec `-e commande`).
- **macOS** : iTerm2 s'il est installé, sinon Terminal. `P2PCHAT_TERMINAL=Ghostty` (nom d'application) en
  impose un autre. Le shell est lancé en shell de connexion, comme dans Terminal (`~/.zprofile` est lu).
- **Windows** : pas encore disponible ; une proposition reçue est refusée automatiquement.

Dans le shell partagé, la variable `P2PCHAT_SHARED_WITH` contient le pseudo de l'invité (pratique pour
l'afficher dans son invite de commande).

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

Dépendances : Qt 6 (Widgets, Network) et OpenSSL 3 (libcrypto). Qt 5.15 fonctionne aussi
(`qtbase5-dev`, puis `qmake` au lieu de `qmake6`).

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

### Tests unitaires

Qt Test (module `testlib`, inclus dans `qtbase`) : un exécutable par fichier `tests/tst_*/`.

```bash
mkdir build-tests && cd build-tests && qmake ../tests/tests.pro && make -j && make check
```

Sous AddressSanitizer et UndefinedBehaviorSanitizer (accès mémoire, fuites, comportements indéfinis) :
`tests/run-sanitizers.sh` (compile dans `build-asan/`). `tests/lsan.supp` écarte une fuite connue de Qt 5.

Ils couvrent la cryptographie, les codes d'invitation, la signature des messages, l'état du groupe (membres,
trousseau), l'historique et sa synchronisation, les règles de reconnexion, la poignée de main entre deux
`PeerConnection` reliées en local (sans Tor), des scénarios complets entre plusieurs membres
(`tst_groupnode` : maillage, relais, réactions, rattrapage après une absence, message falsifié, imposteur…),
la négociation de la console partagée (`tst_terminalsharing`, sans ouvrir de terminal), l'instance unique et le modèle de la liste de messages (rendu hors écran).

## Sécurité

**Vérifier l'identité d'un membre** (⋮ → *Vérifier l'identité d'un membre…*). La clé d'un membre est mémorisée
à la première rencontre : si quelqu'un avait modifié le code d'invitation en route, rien ne le signalerait. Le
code de sécurité (30 chiffres, empreinte des deux identités) est le même sur les deux postes : comparez-le de
vive voix, au téléphone ou en personne. S'il est identique, cliquez *Les codes sont identiques* : le membre est
marqué ✔ (à côté de son nom dans la conversation et dans l'état des membres).


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
  - les notifications affichent le texte des messages ;
  - **console partagée** : l'invité voit tout ce qui s'affiche dans la console et, avec le contrôle, exécute
    des commandes avec les droits de l'hôte. Ne donnez le contrôle qu'à quelqu'un de confiance. Seul le membre
    choisi peut se joindre à la session (identité vérifiée par sa clé Ed25519), et rien n'est accessible sans
    l'accord de l'hôte à chaque session.

## Fichiers

| Fichier | Rôle |
|---|---|
| `torservice.*` | Lancement de tor, torrc, progression, adresse onion |
| `peerconnection.*` | Trames, poignée de main, messages chiffrés, ping |
| `crypto.*` | X25519, Ed25519, HKDF, ChaCha20-Poly1305 (OpenSSL) |
| `protocol.*` | Formats communs (adresse onion, base64), messages signés |
| `invitation.*` | Encodage / décodage du code d'invitation |
| `attachment.*` | Images jointes : chargement, réduction et compression, décodage sûr à la réception |
| `filestore.*` | Fichiers joints : découpage en morceaux, empreintes, stockage, reprise, ménage à 30 jours |
| `reactions.*` | Réactions : enregistrements signés, état (le plus récent l'emporte) |
| `markup.*` | Repérage de `code` et des blocs ``` dans le texte |
| `notifications.*` | Réglage des notifications (lesquelles, pause) |
| `composer.*` | Zone de saisie sur plusieurs lignes (Entrée / Maj+Entrée, hauteur, collage d'images) |
| `safetynumber.*` | Code de sécurité entre deux membres |
| `links.*` | Détection des liens dans le texte des messages |
| `mentions.*`, `mentioncompleter.*` | Détection des « @pseudo », autocomplétion dans la zone de saisie |
| `groupstore.*` | État persistant : pseudo, identité, secret, membres, trousseau des clés |
| `history.*` | Historique local, inventaire et lots de synchronisation |
| `connectionpolicy.*` | Espacement des appels, appels croisés, présence relayée |
| `core.pri` | Liste des fichiers ci-dessus (sans interface), partagée par l'application et les tests |
| `groupnode.*` | Le groupe, sans interface : maillage, reconnexion, présence, synchronisation, messages, réactions, historique |
| `mainwindow.*` | Fenêtre : conversation, invitations, Tor, notifications, console partagée |
| `notifier.*`, `notifier_mac.mm` | Notifications natives (D-Bus sous Linux, UserNotifications sous macOS) |
| `chatmodel.*` | Contenu de la conversation : messages, lignes système et de présence, citations, réactions, recherche |
| `chatdelegate.*` | Dessin des bulles (citation, image, texte, réactions) et de ce qui y est cliquable |
| `chatstyle.*` | Couleurs, dimensions, polices et mise en page du texte des bulles |
| `chatview.*`, `hoverbar.h` | Liste des messages : défilement, séparateur, bouton ↓, barre de survol, GIF, images en grand |
| `ui.pri` | Composants de la conversation, partagés par l'application et les tests graphiques |
| `terminalsession.*` | Console partagée : shell dans un pseudo-terminal (hôte), miroir (invité), échanges avec l'autre membre |
| `terminalsharing.*` | Console partagée : proposition, réponse, annulation, une session à la fois |
| `singleinstance.*` | Une seule instance par utilisateur ; la suivante affiche la fenêtre existante |
| `terminalclient.*` | Mode `p2pchat --terminal` exécuté dans la fenêtre de terminal (mode brut, relais vers P2P Chat) |
| `tests/` | Tests unitaires (Qt Test) |
| `icons/` | Icône : `p2pchat.svg` (source), `.icns` (macOS), `.ico` (Windows), `.png` |

La configuration (`p2pchat.ini`) est dans `~/.config/p2pchat/` (Linux), `~/Library/Preferences/p2pchat/` (macOS)
ou `%APPDATA%\p2pchat\` (Windows). L'historique et les clés Tor sont dans le dossier de données de l'application
(`~/.local/share/p2pchat/p2pchat/`, `~/Library/Application Support/p2pchat/p2pchat/`…).
Supprimer le sous-dossier `tor/hs` change votre adresse onion.
