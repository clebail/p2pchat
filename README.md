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

- **Plusieurs conversations**, chacune avec ses membres, son code d'invitation et son historique, listées dans
  une colonne à gauche (nom, dernier message, non-lus) ; elle se replie quand la fenêtre est étroite (bouton ☰).
  ＋ → *Nouvelle conversation…* ou *Rejoindre avec un code…* (sans quitter les autres). Une même identité et un
  seul service Tor pour toutes. ⋮ → *Quitter la conversation…*, puis *Supprimer la conversation…* pour
  effacer son historique.
- **Invitation directe** : ⋮ → *Nouvelle conversation avec…* (cocher des membres de la conversation affichée),
  ou *Membres…* → *Conversation privée*. Pas de code à recopier : l'invitation passe par la connexion chiffrée
  déjà établie avec chacun, qui l'accepte ou non. Elle n'est acceptée que si elle vient bien de son auteur
  (son adresse et sa clé d'identité). Une conversation privée déjà ouverte avec ce membre est réutilisée.
- **Nom de la conversation** (⋮ → *Renommer la conversation…*) : réservé aux administrateurs, il vaut pour
  tous les membres (signé, le plus récent l'emporte), y compris ceux qui rejoignent ensuite. Après un départ,
  la conversation garde le dernier nom connu.
- **Avatar de la conversation** (⋮ → *Avatar de la conversation…*) : réservé aux administrateurs, partagé avec
  tous les membres comme le nom. Il s'affiche dans la colonne des conversations et dans l'en-tête ; sans image,
  un rond avec l'initiale du nom de la conversation, de la même couleur chez tous les membres (sans nom choisi
  par un administrateur, chacun voit les pseudos des autres, donc une autre initiale). Il disparaît quand on
  quitte le groupe.
- Bulles façon messagerie mobile, avatars, regroupement des messages, séparateurs de date
- **Avatar** (⋮ → *Changer d'avatar…*) : une image recadrée en carré et réduite (96 px), commune à toutes vos
  conversations et visible par leurs membres. Sans image, un rond coloré avec l'initiale du pseudo. Signé par son
  auteur et relayé par les autres membres, comme le nom de la conversation ; *Retirer* l'efface aussi chez eux.
  Les versions précédentes de P2P Chat ne l'affichent pas.
- **Au survol d'un message**, une petite barre apparaît au coin de la bulle : réactions rapides et ↩ *Répondre*.
- **Modifier ou supprimer un message** (clic droit → *Modifier*, *Supprimer*) : la modification est signée,
  synchronisée avec les absents, et le message affiche « modifié » ; supprimé, il devient « Message supprimé »,
  sans image ni fichier. Un administrateur peut aussi modifier ou supprimer les messages des autres (son nom
  apparaît alors : « modifié par X », « Message supprimé par X »). Les versions antérieures à la 13 ne voient
  pas ces changements, et rien ne garantit qu'un message supprimé disparaisse de tous les disques.
- **Réactions** (👍 ❤️ 😂 😮 😢 🎉) : clic droit sur un message → *Réagir*, ou clic sur une pastille existante pour
  ajouter ou retirer la vôtre ; survoler une pastille montre qui a réagi. Chaque réaction est signée par son
  auteur, synchronisée et relayée comme les messages (pas envoyée aux versions précédentes, qui l'ignoreraient).
- **Saisie sur plusieurs lignes** : Entrée envoie, Maj+Entrée va à la ligne ; la zone grandit jusqu'à 6 lignes.
  `` `code` `` et les blocs ` ``` ` s'affichent en police à chasse fixe (les accents graves sont masqués, mais
  gardés quand on copie le message).
- **Notifications** (menu ⋮ → *Notifications*, propres à chaque conversation) : tous les messages, mentions et
  réponses seulement, ou aucune ; « ne pas déranger » (pour toutes) pendant 1 h ou jusqu'au lendemain 8 h. Le
  compteur de non-lus reste à jour.
- **Nouveaux messages** : en revenant sur la fenêtre, un séparateur rouge marque le premier message reçu en votre
  absence, et la liste s'y place. Si vous avez remonté l'historique, un bouton « ↓ » en bas à droite indique le
  nombre de messages arrivés depuis et ramène en bas.
- **« Alice écrit… »** s'affiche sous le titre pendant qu'un membre relié directement tape un message (signal
  envoyé au plus toutes les 3 s, jamais enregistré ni relayé).
- **Recherche** (Ctrl+F) : occurrences surlignées, sans tenir compte des majuscules ni des accents ; Entrée pour
  le résultat précédent (plus ancien), Maj+Entrée pour le suivant, Échap pour fermer.
- **Liens** cliquables (http, https, www.). Ils s'ouvrent dans le navigateur, donc **hors Tor** : une confirmation
  le rappelle la première fois.
- **Fichiers** (100 Mo au plus) : bouton 📎, glisser-déposer ou collage d'un fichier ; une image reste une
  image. La bulle montre le type, le nom, la taille et l'état. Les fichiers de 2 Mo au plus sont téléchargés
  automatiquement, les autres d'un clic sur « Télécharger » ; un téléchargement interrompu reprend tout seul.
  N'importe quel membre qui a le fichier peut le fournir, même en partie : il est demandé à plusieurs membres
  à la fois (jusqu'à 4, deux morceaux en vol chacun pour masquer la latence de Tor), les morceaux les plus rares
  d'abord, et celui qui télécharge annonce sa progression pour servir de source à son tour. Les derniers
  morceaux sont aussi demandés aux sources libres, et une source lente est moins sollicitée. La bulle indique
  « depuis Alice, Bob ». Chaque morceau est vérifié grâce à l'annonce signée de l'auteur (une source qui en
  fournit un faux est écartée) ; ils voyagent en trames binaires, sans base64. Clic sur un fichier reçu : *Ouvrir* (avertissement avant un programme ou un script) ; clic
  droit : *Enregistrer sous…*, *Annuler le téléchargement*. Fichiers conservés 30 jours, effacés avec la
  conversation.
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
- Menu ⋮ : inviter, rejoindre, se reconnecter, **partager ma console**, changer de pseudo ou d'avatar,
  **vider la conversation** (efface l'historique local), quitter le groupe
- Survoler la ligne d'état sous le titre affiche l'état de chaque membre (dernière erreur, heure du prochain essai).
  Un membre pas encore relié directement, mais connecté à un autre membre, apparaît « en ligne (via …) » : les
  messages passent par celui-ci.
- Arrivées et départs regroupés : les « X est en ligne » / « X s'est déconnecté(e) » consécutifs tiennent sur une
  seule ligne (un clic la déplie). Une coupure n'est annoncée que si le membre n'est pas revenu au bout de 60 s,
  et qu'aucun autre membre ne le voit encore en ligne.
- **Correcteur orthographique** (⋮ → *Correcteur orthographique*) : les mots inconnus sont soulignés en
  rouge dans la zone de saisie, une fois le mot terminé. Le clic droit propose des corrections et *Ajouter au
  dictionnaire* (dictionnaire personnel, conservé). Langue : comme l'interface (par défaut), français, anglais,
  ou désactivé. Le code, les liens, les mentions, les nombres et les sigles ne sont pas vérifiés. Utilise
  Hunspell et les dictionnaires de LibreOffice (cf. *Compiler*).
- **Langue** (⋮ → *Langue*) : français ou anglais. *Automatique* (par défaut) suit le système : français si
  c'est sa langue préférée, anglais sinon. Le changement s'applique au prochain démarrage. Les dates et les
  tailles de fichiers suivent la langue choisie.
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

Dépendances : Qt 6 (Widgets, Network, et `lrelease` pour compiler les traductions) et OpenSSL 3 (libcrypto).
Qt 5.15 fonctionne aussi (`qtbase5-dev qttools5-dev-tools`, puis `qmake` au lieu de `qmake6`).
Facultatif : Hunspell pour le correcteur orthographique (sans lui, l'application se compile sans correcteur).
qmake le trouve par pkg-config (Linux, Homebrew), ou dans `HUNSPELL_DIR` (dossier avec `include/` et `lib/`,
celui de vcpkg sous Windows).

**Dictionnaires** (`<langue>.aff` + `<langue>.dic`, ceux de LibreOffice : `fr`/`fr_FR`, `en_US`/`en_GB`/`en`),
cherchés dans cet ordre : dossier `dictionaries/` à côté de l'exécutable (sous macOS :
`P2PChat.app/Contents/Resources/dictionaries/`), dossier de données de P2P Chat (`…/dictionaries/`), puis
ceux du système (`/usr/share/hunspell` sous Linux ; `$(brew --prefix)/share/hunspell` et `~/Library/Spelling`
sous macOS). Pour une version autonome sous Windows ou macOS, copier les dictionnaires dans `dictionaries/`
(dépôt `LibreOffice/dictionaries` : `fr_FR/fr.aff`, `fr_FR/fr.dic`, `en/en_US.aff`, `en/en_US.dic`).

**Linux**
```bash
sudo apt install qt6-base-dev qt6-l10n-tools libssl-dev libhunspell-dev hunspell-fr hunspell-en-us tor
mkdir build && cd build && qmake6 ../p2pchat.pro && make -j
./p2pchat
make install   # facultatif : installe dans ~/.local (menu des applications + icône des notifications)
```

**macOS**
```bash
brew install qt openssl@3 hunspell pkgconf tor     # macOS 11 minimum
mkdir build && cd build && qmake ../p2pchat.pro && make -j
macdeployqt P2PChat.app           # rend l'application autonome
open P2PChat.app
```
Le `.pro` trouve OpenSSL dans `/opt/homebrew/opt/openssl@3` (Apple Silicon) ou `/usr/local/opt/openssl@3` (Intel).
Sinon, définir `OPENSSL_DIR`. L'application n'étant pas signée, au premier lancement il faut faire
clic droit → **Ouvrir**.

**Windows** (MSYS2 / MinGW)
```bash
pacman -S mingw-w64-x86_64-qt6-base mingw-w64-x86_64-qt6-tools mingw-w64-x86_64-openssl mingw-w64-x86_64-hunspell
mkdir build && cd build && qmake6 ../p2pchat.pro && mingw32-make
windeployqt release/p2pchat.exe   # + copier libcrypto-3-x64.dll et le dossier tor\
```

### Traductions

Les textes sont écrits en français, dans le code (`tr()`). La traduction anglaise est dans
`translations/p2pchat_en.ts`, compilée par `lrelease` pendant le build et embarquée dans l'application. Après
avoir ajouté ou modifié des textes :

```bash
lupdate -no-obsolete -locations none p2pchat.pro   # met à jour translations/p2pchat_en.ts
linguist translations/p2pchat_en.ts                # traduire les nouveaux textes
```

`tst_language` échoue tant qu'un texte n'est pas traduit, ou qu'une traduction perd un `%1`.

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
(`tst_groupnode` : maillage, relais, réactions, rattrapage après une absence, message falsifié, imposteur,
fichiers téléchargés automatiquement ou à la demande, reprise après coupure, source au fichier abîmé…),
la négociation de la console partagée (`tst_terminalsharing`, sans ouvrir de terminal), la console de bout en bout
(`tst_terminalsession`, Linux : vrai shell dans un pseudo-terminal, faux terminal, contrôle donné ou non), le lancement
de tor (`tst_torservice` : torrc, progression, adresse onion, erreurs, avec un faux tor), l'instance unique, le modèle
de la liste de messages, ce qu'une conversation affiche (`tst_conversation` : nom, aperçu, notifications, ligne d'état ;
rendu hors écran) et la fenêtre principale (`tst_mainwindow` : actions indisponibles grisées, sous-menus, recherche,
colonne repliée quand la fenêtre est étroite), la langue, le correcteur orthographique et les avatars.

**Intégration continue** (`.github/workflows/ci.yml`, GitHub Actions) : à chaque push et pull request, compilation
de l'application et des tests sous Linux (gcc, avec un second passage sous sanitizers), macOS (clang, Qt et OpenSSL
de Homebrew) et Windows (MSVC, Qt 5.15.2, OpenSSL de vcpkg), puis `make check`.

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
- **Postes liés** : une personne peut utiliser plusieurs postes, rattachés à une clé de personne commune par un
  certificat signé deux fois, par la personne et par le poste. Sans la clé du poste, personne ne peut réclamer le
  poste d'un autre. Le premier certificat vu pour un poste fixe sa personne : un poste qui prétend ensuite
  appartenir à quelqu'un d'autre est refusé (« identité refusée »). Le code de sécurité entre deux personnes est
  tiré de leurs clés de personne : une seule vérification couvre tous leurs postes.
- **Avatars** : signés par leur auteur (celui d'une conversation, par un administrateur, et lié au groupe),
  acceptés seulement pour les membres de la conversation. Une image reçue
  n'est décodée que si c'est un JPEG d'au plus 16 Ko et 192 × 192 pixels. Un membre ne reçoit que les avatars
  des membres de la conversation, rien sur les contacts de vos autres conversations.
- **Exclusion et secret** (⋮ → *Membres…* : rôles, présence, *Nommer administrateur*, *Exclure…*,
  *Renouveler le secret…*) : le créateur du groupe en est le premier administrateur ; un administrateur peut en
  nommer d'autres, retirer ce rôle ou s'en retirer, tant qu'il en reste un (sans changer le secret). Les décisions (exclusions,
  nouveau secret) forment une chaîne d'enregistrements signés par un administrateur, que tous les membres
  vérifient et se transmettent ; l'empreinte du premier identifie le groupe et figure dans les codes
  d'invitation. Exclure un membre renouvelle le secret : le nouveau ne voyage que par les connexions
  chiffrées, vers les membres restants. L'ancien reste accepté 30 jours, mais seulement pour les membres déjà
  connus et non exclus, qui reçoivent alors le nouveau ; un ancien code d'invitation ne fait plus entrer
  personne. Le membre exclu en est averti. Quitter le groupe est annoncé aux autres (annonce signée).
- **Historique** : chaque ligne est enregistrée avec la signature de son auteur et revérifiée au chargement.
  « ⚠ signature invalide » signale une ligne modifiée ou inventée, et « ⚠ non signé » une ligne de l'ancien
  format. La suppression d'une ligne n'est pas détectée. L'historique n'est **pas chiffré** : il est protégé
  par les droits du fichier (600) et par le chiffrement du disque (FileVault / LUKS) s'il est activé.
- **Fichiers sensibles** : la configuration (secret du groupe, clé d'identité, trousseau) et l'historique
  sont lisibles par le seul utilisateur. Le serveur local n'écoute que sur `127.0.0.1`.
- **Limites** :
  - le code d'invitation permet d'entrer dans le groupe : envoyez-le par un canal de confiance ;
  - un membre exclu garde l'historique déjà reçu ; les membres d'une version antérieure à la 11 ne suivent
    pas un renouvellement du secret et doivent être réinvités ;
  - les notifications affichent le texte des messages ;
  - **console partagée** : l'invité voit tout ce qui s'affiche dans la console et, avec le contrôle, exécute
    des commandes avec les droits de l'hôte. Ne donnez le contrôle qu'à quelqu'un de confiance. Seul le membre
    choisi peut se joindre à la session (identité vérifiée par sa clé Ed25519), et rien n'est accessible sans
    l'accord de l'hôte à chaque session.

## Fichiers

Les sources sont dans `src/`, un dossier par fonctionnalité. Les `#include` n'ont pas de chemin : chaque dossier est
dans le chemin d'inclusion (`src/core.pri`, `src/ui.pri`).

**`src/app/`** — Application

| Fichier | Rôle |
|---|---|
| `main.cpp` | Démarrage : langue, instance unique, pseudo, fenêtre |
| `mainwindow.*` | Fenêtre : colonne des conversations, zone de saisie, Tor, notifications |
| `mainwindow_ui.cpp` | Construction de la fenêtre : colonne, barre du haut et menu ⋮, recherche, accueil, bandeaux, zone de saisie |
| `mainwindow_dialogs.cpp` | Dialogues de la fenêtre : invitation, membres et gouvernance, code de sécurité, console partagée |
| `appstyle.*` | Feuille de style et icônes dessinées de la fenêtre |
| `language.*` | Langue de l'interface (réglage, système), traductions (`translations/`) de P2P Chat et de Qt |
| `singleinstance.*` | Une seule instance par utilisateur ; la suivante affiche la fenêtre existante |

**`src/chat/`** — Conversation affichée

| Fichier | Rôle |
|---|---|
| `conversation.*` | Une conversation affichable : son groupe et sa vue, ce que la fenêtre en montre (nom, aperçu, état, non-lus, notifications) |
| `chatview.*`, `hoverbar.h` | Liste des messages : défilement, séparateur, bouton ↓, barre de survol, GIF, images en grand |
| `chatmodel.*` | Contenu de la conversation : messages, lignes système et de présence, citations, réactions, recherche |
| `chatdelegate.*` | Dessin des bulles (citation, image, texte, réactions) et de ce qui y est cliquable |
| `chatstyle.*` | Couleurs, dimensions, polices et mise en page du texte des bulles |
| `composer.*` | Zone de saisie sur plusieurs lignes (Entrée / Maj+Entrée, hauteur, collage d'images) |
| `mentioncompleter.*` | Autocomplétion des « @pseudo » dans la zone de saisie |

**`src/group/`** — Groupe

| Fichier | Rôle |
|---|---|
| `groupnode.*` | Le groupe, sans interface : maillage, reconnexion, présence, synchronisation, messages, réactions, historique |
| `groupnode_files.cpp` | `GroupNode`, fichiers joints : annonces, morceaux, sources, reprise |
| `groupnode_governance.cpp` | `GroupNode`, gouvernance (administrateurs, exclusions, secret), nom et avatar de la conversation, invitations directes, avatars des membres |
| `groupnode_p.h` | Commun aux fichiers de `GroupNode` |
| `groupstore.*` | État persistant : pseudo, avatars, identité et trousseau (communs), secret et membres (par conversation) |
| `governance.*` | Gouvernance du groupe : administrateurs, exclusions, secret en vigueur (enregistrements signés) |
| `invitation.*` | Encodage / décodage du code d'invitation |
| `person.*` | Postes liés : certificat de poste, signé par la personne et par le poste |
| `history.*` | Historique local, inventaire et lots de synchronisation |
| `conversations.*` | Liste des conversations, leurs dossiers, reprise de l'ancienne disposition (un seul groupe) |

**`src/network/`** — Réseau

| Fichier | Rôle |
|---|---|
| `protocol.*` | Formats communs (adresse onion, base64), messages signés |
| `peerconnection.*` | Trames, poignée de main, messages chiffrés, ping |
| `switchboard.*` | Écoute locale commune : chaque connexion entrante va à la conversation dont elle présente le secret |
| `connectionpolicy.*` | Espacement des appels, appels croisés, présence relayée |
| `torservice.*` | Lancement de tor, torrc, progression, adresse onion |

**`src/crypto/`** — Cryptographie

| Fichier | Rôle |
|---|---|
| `crypto.*` | X25519, Ed25519, HKDF, ChaCha20-Poly1305 (OpenSSL) |
| `safetynumber.*` | Code de sécurité entre deux membres |

**`src/messages/`** — Contenu des messages

| Fichier | Rôle |
|---|---|
| `attachment.*` | Images jointes : chargement, réduction et compression, décodage sûr à la réception |
| `avatar.*` | Avatars : image recadrée et réduite, enregistrement signé, décodage prudent d'une image reçue |
| `edits.*` | Modification et suppression des messages : enregistrements signés |
| `reactions.*` | Réactions : enregistrements signés, état (le plus récent l'emporte) |

**`src/text/`** — Texte

| Fichier | Rôle |
|---|---|
| `markup.*` | Repérage de `code` et des blocs ``` dans le texte |
| `links.*` | Détection des liens dans le texte des messages |
| `mentions.*` | Détection des « @pseudo » dans le texte |
| `spellchecker.*` | Correcteur orthographique (Hunspell) : dictionnaires, mots à souligner, suggestions |

**`src/files/`** — Fichiers joints

| Fichier | Rôle |
|---|---|
| `filestore.*` | Fichiers joints : découpage en morceaux, empreintes, stockage, reprise, ménage à 30 jours |

**`src/notifications/`** — Notifications

| Fichier | Rôle |
|---|---|
| `notifications.*` | Réglage des notifications (lesquelles, pause) |
| `notifier.*`, `notifier_mac.mm` | Notifications natives (D-Bus sous Linux, UserNotifications sous macOS) |

**`src/terminal/`** — Console partagée (Linux, macOS)

| Fichier | Rôle |
|---|---|
| `terminalsession.*` | Console partagée : shell dans un pseudo-terminal (hôte), miroir (invité), échanges avec l'autre membre |
| `terminalsharing.*` | Console partagée : proposition, réponse, annulation, une session à la fois |
| `terminalclient.*` | Mode `p2pchat --terminal` exécuté dans la fenêtre de terminal (mode brut, relais vers P2P Chat) |

**Autour**

| Fichier | Rôle |
|---|---|
| `src/core.pri` | Fichiers sans interface (protocole, crypto, groupe, historique…), partagés par l'application et les tests ; OpenSSL et Hunspell |
| `src/ui.pri` | Composants de la conversation, partagés par l'application et les tests graphiques |
| `resources/` | Icône (`icons/` : `p2pchat.svg` source, `.icns` macOS, `.ico` Windows, `.png`), entrée de menu Linux (`p2pchat.desktop`), ressources Qt |
| `translations/` | Traduction anglaise (cf. *Traductions*) |
| `tests/` | Tests unitaires (Qt Test) |

La configuration (`p2pchat.ini`) est dans `~/.config/p2pchat/` (Linux), `~/Library/Preferences/p2pchat/` (macOS)
ou `%APPDATA%\p2pchat\` (Windows). Les clés Tor et les conversations sont dans le dossier de données de
l'application (`~/.local/share/p2pchat/p2pchat/`, `~/Library/Application Support/p2pchat/p2pchat/`…) : un
sous-dossier `conversations/<identifiant>/` par conversation, avec son historique et ses fichiers.
Supprimer le sous-dossier `tor/hs` change votre adresse onion.
