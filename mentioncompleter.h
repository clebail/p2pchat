#pragma once
#include <QObject>
#include <QStringList>

class QCompleter;
class QLineEdit;
class QStringListModel;

// Zone de saisie : taper « @ » propose les pseudos des membres (et « tous »).
// Flèches pour choisir, Entrée ou Tab pour insérer, Échap pour fermer.
// Liste fermée, Tab complète comme dans un terminal : le pseudo s'il n'y en a qu'un possible,
// sinon ouvre la liste.
class MentionCompleter : public QObject
{
    Q_OBJECT
public:
    explicit MentionCompleter(QLineEdit *edit);
    void setNames(const QStringList &names);
    bool isPopupVisible() const;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    bool completeWithTab(); // false : pas de mention en cours, Tab garde son rôle habituel
    void update();
    void insert(const QString &name);

    QLineEdit *m_edit;
    QCompleter *m_completer;
    QStringListModel *m_model;
    QStringList m_names;
};
