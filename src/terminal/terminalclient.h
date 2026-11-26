#pragma once

// Mode « console » : `p2pchat --terminal <socket> [titre] [langue]`, lancé dans une fenêtre de terminal
// (konsole, gnome-terminal…) par une session de console partagée.
//
// Le terminal passe en mode brut ; les frappes partent vers P2P Chat par le socket local et
// ce que P2P Chat renvoie s'affiche tel quel. Trames : type (1 octet) + longueur (4 octets,
// big-endian) + contenu.
//   console -> P2P Chat : 'D' frappes, 'R' taille (colonnes, lignes : 2 x 16 bits)
//   P2P Chat -> console : 'D' affichage, 'T' titre de la fenêtre, 'M' avis, 'E' fin (motif)
// language : "en" pour les messages en anglais (sans application Qt : pas de traductions chargées), sinon français
int runTerminalClient(const char *socketPath, const char *title, const char *language);
