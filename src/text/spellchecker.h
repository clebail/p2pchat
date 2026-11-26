#pragma once
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

#include <memory>

// Correcteur orthographique (Hunspell). Les dictionnaires (<nom>.aff + <nom>.dic, ceux de LibreOffice) sont
// cherchés à côté de l'application, dans le dossier de données, puis dans ceux du système.
// Compilé sans Hunspell (P2P_SPELLCHECK absent) : supported() est faux et aucun dictionnaire ne se charge.
class SpellChecker
{
public:
    SpellChecker();
    ~SpellChecker();
    SpellChecker(const SpellChecker &) = delete;
    SpellChecker &operator=(const SpellChecker &) = delete;

    static bool supported();
    static QStringList searchPaths();
    // Fichier .dic d'une langue ("fr", "en") dans ces dossiers, vide si aucun (le .aff doit être à côté)
    static QString findDictionary(const QString &language, const QStringList &dirs = searchPaths());

    bool load(const QString &dicPath); // remplace le dictionnaire chargé ; false si illisible
    bool isLoaded() const;
    QString language() const { return m_language; } // celle demandée à l'appelant, pour mémoire
    void setLanguage(const QString &language) { m_language = language; }

    bool isCorrect(const QString &word) const; // sans dictionnaire : tout est correct
    QStringList suggestions(const QString &word, int max = 6) const;
    void addWord(const QString &word); // accepté jusqu'au prochain chargement (le dictionnaire personnel est à l'appelant)

    // Mots à souligner dans un texte : (début, longueur). Ne sont pas vérifiés : le code (`…`, ```…```), les
    // liens, les mentions (@pseudo), les mots avec des chiffres et les sigles en majuscules.
    QList<QPair<int, int>> misspelled(const QString &text) const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
    QString m_language;
};
