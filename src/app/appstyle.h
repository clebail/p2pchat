#pragma once
#include <QIcon>
#include <QString>

// Apparence de la fenêtre principale : feuille de style (couleurs Material) et icônes dessinées
namespace AppStyle {

QString styleSheet();
QIcon attachIcon(); // trombone
QIcon sendIcon();   // avion en papier, en blanc
QIcon moreIcon();   // « ⋮ »

} // namespace AppStyle
