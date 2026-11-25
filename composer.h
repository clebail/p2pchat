#pragma once
#include <QPlainTextEdit>

class QMimeData;

// Zone de saisie des messages, sur plusieurs lignes :
//  - Entrée envoie (returnPressed), Maj+Entrée va à la ligne ;
//  - la hauteur suit le texte, de 1 à MaxLines lignes (ascenseur au-delà) ;
//  - une image collée ou déposée n'est pas insérée : imageInserted la transmet ;
//  - le texte collé l'est sans mise en forme.
class Composer : public QPlainTextEdit
{
    Q_OBJECT
public:
    static constexpr int MaxLines = 6;

    explicit Composer(QWidget *parent = nullptr);

    QString text() const { return toPlainText(); }
    void setText(const QString &text); // n'émet pas textEdited
    int cursorPosition() const;
    void setCursorPosition(int position);
    void setMaxLength(int length) { m_maxLength = length; }
    int maxLength() const { return m_maxLength; }
    void setMinimumHeight(int h) { m_minHeight = h; } // hauteur d'une ligne, alignée sur les boutons
    int visibleLines() const; // lignes affichées (1 à MaxLines)
    // Liste de complétion (mentions) : ouverte, Entrée et Tab lui reviennent
    void setCompletionPopup(QWidget *popup) { m_popup = popup; }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    void returnPressed();
    void textEdited(const QString &text);    // modification faite par l'utilisateur (frappe, collage)
    void imageInserted(const QMimeData *mime); // image collée ou déposée

protected:
    void keyPressEvent(QKeyEvent *event) override;
    bool canInsertFromMimeData(const QMimeData *source) const override;
    void insertFromMimeData(const QMimeData *source) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void onTextChanged();
    void updateHeight();
    int heightForLines(int lines) const;
    int totalLines() const;

    int m_maxLength = 4000;
    int m_minHeight = 0;
    QWidget *m_popup = nullptr;
    bool m_programmatic = false;
};
