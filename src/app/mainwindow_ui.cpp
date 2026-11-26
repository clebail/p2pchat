#include "mainwindow.h"

#include "appstyle.h"
#include "chatview.h"
#include "composer.h"
#include "mentioncompleter.h"
#include "spellchecker.h"
#ifdef P2P_TERMINAL
#  include "terminalsession.h"
#  include "terminalsharing.h"
#endif

#include <QApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMimeData>
#include <QPushButton>
#include <QShortcut>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>

// Construction de la fenêtre : colonne des conversations, barre du haut et menu ⋮, recherche, page d'accueil,
// bandeaux au-dessus de la saisie, zone de saisie (le comportement est dans mainwindow.cpp)

void MainWindow::buildUi()
{
    setWindowTitle(tr("P2P Chat"));
    resize(820, 720);
    setStyleSheet(AppStyle::styleSheet());

    // Colonne des conversations à gauche, conversation affichée à droite
    auto *window = new QWidget(this);
    window->setObjectName("central");
    auto *columns = new QHBoxLayout(window);
    columns->setContentsMargins(0, 0, 0, 0);
    columns->setSpacing(0);
    m_sidebar = buildSidebar(window);
    columns->addWidget(m_sidebar);
    auto *central = new QWidget(window);
    central->setObjectName("central");
    columns->addWidget(central, 1);
    auto *layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    layout->addWidget(buildTopBar(central));
    layout->addWidget(buildSearchBar(central));
    m_pages = new QStackedWidget(central);
    m_home = buildHome(m_pages);
    m_pages->addWidget(m_home); // puis une vue par conversation (openConversation)
    layout->addWidget(m_pages, 1);

    // Bandeau « Réponse à … » (réponse avec citation en cours d'écriture)
    m_replyLabel = new QLabel;
    m_replyLabel->setObjectName("replyLabel");
    m_replyLabel->setTextFormat(Qt::RichText);
    m_replyLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); // extrait long : pas d'élargissement
    m_replyBar = buildClosableBar(central, {m_replyLabel}, tr("Annuler la réponse (Échap)"), &MainWindow::cancelReply);
    layout->addWidget(m_replyBar);

    // Aperçu de l'image jointe au message en cours d'écriture (même aspect que le bandeau de réponse)
    m_imagePreview = new QLabel;
    m_imagePreview->setObjectName("imagePreview");
    m_imagePreview->setFixedSize(56, 56);
    m_imagePreview->setAlignment(Qt::AlignCenter);
    m_imageInfo = new QLabel;
    m_imageInfo->setObjectName("replyLabel");
    m_imageBar = buildClosableBar(central, {m_imagePreview, m_imageInfo}, tr("Retirer l'image"), &MainWindow::cancelImage);
    layout->addWidget(m_imageBar);

    layout->addWidget(buildComposerBar(central));
    setAcceptDrops(true);
    setCentralWidget(window);
}

// Barre du haut : bouton ☰, avatar et nom de la conversation, membres en ligne, menu ⋮
QWidget *MainWindow::buildTopBar(QWidget *parent)
{
    auto *topBar = new QWidget(parent);
    topBar->setObjectName("topBar");
    topBar->setAttribute(Qt::WA_StyledBackground);
    auto *top = new QHBoxLayout(topBar);
    top->setContentsMargins(8, 10, 8, 10);
    m_sidebarButton = new QToolButton(topBar);
    m_sidebarButton->setObjectName("menuButton");
    m_sidebarButton->setText(QStringLiteral("☰"));
    m_sidebarButton->setFixedSize(40, 40);
    m_sidebarButton->setCursor(Qt::PointingHandCursor);
    m_sidebarButton->setToolTip(tr("Afficher ou masquer les conversations"));
    connect(m_sidebarButton, &QToolButton::clicked, this, [this] {
        m_sidebarWanted = !m_sidebar->isVisible();
        setSidebarVisible(m_sidebarWanted);
    });
    top->addWidget(m_sidebarButton, 0, Qt::AlignTop);
    top->addSpacing(4);
    m_titleAvatar = new QLabel(topBar);
    m_titleAvatar->setFixedSize(TitleAvatarSide, TitleAvatarSide);
    top->addWidget(m_titleAvatar, 0, Qt::AlignTop);
    top->addSpacing(4);
    auto *titles = new QVBoxLayout;
    titles->setSpacing(1);
    m_title = new QLabel(tr("P2P Chat"), topBar);
    m_title->setObjectName("title");
    m_title->setTextFormat(Qt::PlainText); // contient des pseudos choisis par les autres
    m_subtitle = new QLabel(topBar);
    m_subtitle->setObjectName("subtitle");
    m_subtitle->setTextFormat(Qt::RichText);
    m_subtitle->setWordWrap(true);
    titles->addWidget(m_title);
    titles->addWidget(m_subtitle);
    top->addLayout(titles, 1);


    auto *menuButton = new QToolButton(topBar);
    menuButton->setObjectName("menuButton");
    menuButton->setIcon(AppStyle::moreIcon());
    menuButton->setIconSize(QSize(24, 24));
    menuButton->setFixedSize(40, 40);
    menuButton->setPopupMode(QToolButton::InstantPopup);
    menuButton->setMenu(buildMainMenu(menuButton));
    top->addWidget(menuButton, 0, Qt::AlignTop);
    return topBar;
}

// Menu ⋮ : conversations, console partagée, réglages, membres, profil
QMenu *MainWindow::buildMainMenu(QWidget *parent)
{
    auto *menu = new QMenu(parent);
    QAction *inviteAct = menu->addAction(tr("Inviter quelqu'un…"), this, &MainWindow::invite);
    menu->addAction(tr("Nouvelle conversation…"), this, &MainWindow::startConversation);
    QAction *withAct = menu->addAction(tr("Nouvelle conversation avec…"), this, &MainWindow::chooseConversationMembers);
    menu->addAction(tr("Rejoindre avec un code…"), this, &MainWindow::join);
    menu->addAction(tr("Se reconnecter maintenant"), this, &MainWindow::reconnectNow);
#ifdef P2P_TERMINAL
    menu->addSeparator();
    m_termShareAct = menu->addAction(tr("Partager ma console…"), this, &MainWindow::shareTerminal);
    m_termCtlAct = menu->addAction(QString(), this, [this] { m_sharing->toggleControl(); });
    m_termStopAct = menu->addAction(QString(), this, [this] { m_sharing->stop(); });
    connect(menu, &QMenu::aboutToShow, this, [this] {
        const TerminalSession *term = m_sharing->session();
        const bool host = term && term->role() == TerminalSession::Host;
        m_termShareAct->setEnabled(m_sharing->canShare() && m_node->hasConnections());
        m_termCtlAct->setVisible(host);
        if (host)
            m_termCtlAct->setText(term->control() ? tr("Retirer le contrôle à %1").arg(term->peerNick())
                                                  : tr("Donner le contrôle à %1").arg(term->peerNick()));
        m_termStopAct->setVisible(term || m_sharing->hasOutgoingOffer());
        m_termStopAct->setText(!term  ? tr("Annuler la proposition de console")
                               : host ? tr("Arrêter le partage de console")
                                      : tr("Quitter la console de %1").arg(term->peerNick()));
    });
#endif
    menu->addSeparator();
    // Sous-menus reconstruits à chaque ouverture (état courant) ; remplis d'avance : vide, il ne s'ouvrirait pas
    auto liveMenu = [this, menu](const QString &title, void (MainWindow::*build)(QMenu *)) {
        QMenu *sub = menu->addMenu(title);
        connect(sub, &QMenu::aboutToShow, this, [this, sub, build] { (this->*build)(sub); });
        (this->*build)(sub);
    };
    liveMenu(tr("Notifications"), &MainWindow::buildNotificationMenu);
    buildLanguageMenu(menu->addMenu(tr("Langue")));
    liveMenu(tr("Correcteur orthographique"), &MainWindow::buildSpellingMenu);
    menu->addAction(tr("Membres…"), this, &MainWindow::showMembers);
    menu->addAction(tr("Vérifier l'identité d'un membre…"), this, &MainWindow::verifyMember);
    menu->addAction(tr("Changer de pseudo…"), this, &MainWindow::changeNickname);
    menu->addAction(tr("Changer d'avatar…"), this, &MainWindow::changeAvatar);
    QAction *renameAct = menu->addAction(tr("Renommer la conversation…"), this, &MainWindow::renameConversation);
    QAction *convAvatarAct =
        menu->addAction(tr("Avatar de la conversation…"), this, &MainWindow::changeConversationAvatar);
    menu->addAction(tr("Vider la conversation"), this, &MainWindow::clearConversation);
    menu->addSeparator();
    QAction *leaveAct = menu->addAction(tr("Quitter la conversation…"), this, &MainWindow::resetAll);
    QAction *deleteAct = menu->addAction(tr("Supprimer la conversation…"), this, &MainWindow::deleteConversation);
    connect(menu, &QMenu::aboutToShow, this, [this, inviteAct, leaveAct, deleteAct, withAct, renameAct, convAvatarAct] {
        withAct->setEnabled(m_group->inGroup() && !m_group->members().isEmpty());
        // Réservé aux administrateurs (le nom vaut pour tous)
        renameAct->setEnabled(m_node->isAdmin() && !m_node->groupId().isEmpty());
        convAvatarAct->setEnabled(renameAct->isEnabled());
        inviteAct->setText(m_group->inGroup() ? tr("Inviter quelqu'un…") : tr("Créer la conversation et inviter…"));
        leaveAct->setVisible(m_group->inGroup());
        deleteAct->setVisible(!m_group->inGroup());
    });
#ifndef Q_OS_MACOS // sous macOS : menu de l'application / Cmd+Q
    menu->addAction(tr("Quitter P2P Chat"), qApp, &QCoreApplication::quit);
#endif
    return menu;
}

// Barre de recherche (Ctrl+F), cachée jusqu'à son ouverture
QWidget *MainWindow::buildSearchBar(QWidget *parent)
{
    m_searchBar = new QWidget(parent);
    m_searchBar->setObjectName("searchBar");
    m_searchBar->setAttribute(Qt::WA_StyledBackground);
    auto *searchRow = new QHBoxLayout(m_searchBar);
    searchRow->setContentsMargins(12, 6, 8, 6);
    searchRow->setSpacing(4);
    m_searchEdit = new QLineEdit(m_searchBar);
    m_searchEdit->setObjectName("searchEdit");
    m_searchEdit->setPlaceholderText(tr("Rechercher dans la conversation"));
    m_searchEdit->setClearButtonEnabled(true);
    m_searchCount = new QLabel(m_searchBar);
    m_searchCount->setObjectName("searchCount");
    auto searchButton = [this](const QString &text, const QString &tip) {
        auto *b = new QToolButton(m_searchBar);
        b->setObjectName("searchButton");
        b->setText(text);
        b->setToolTip(tip);
        b->setFixedSize(32, 32);
        b->setCursor(Qt::PointingHandCursor);
        return b;
    };
    QToolButton *older = searchButton(QStringLiteral("▲"), tr("Résultat précédent, plus ancien (Entrée)"));
    QToolButton *newer = searchButton(QStringLiteral("▼"), tr("Résultat suivant, plus récent (Maj+Entrée)"));
    QToolButton *closeSearch = searchButton(QStringLiteral("✕"), tr("Fermer la recherche (Échap)"));
    searchRow->addWidget(m_searchEdit, 1);
    searchRow->addWidget(m_searchCount);
    searchRow->addWidget(older);
    searchRow->addWidget(newer);
    searchRow->addWidget(closeSearch);
    m_searchBar->setVisible(false);
    connect(m_searchEdit, &QLineEdit::textChanged, this, [this](const QString &t) { m_chat->setSearchTerm(t); });
    connect(m_searchEdit, &QLineEdit::returnPressed, this, [this] {
        m_chat->searchNext(!(QGuiApplication::keyboardModifiers() & Qt::ShiftModifier));
    });
    connect(older, &QToolButton::clicked, this, [this] { m_chat->searchNext(true); });
    connect(newer, &QToolButton::clicked, this, [this] { m_chat->searchNext(false); });
    connect(closeSearch, &QToolButton::clicked, this, &MainWindow::closeSearch);
    auto *searchEscape = new QShortcut(QKeySequence(Qt::Key_Escape), m_searchEdit, nullptr, nullptr,
                                       Qt::WidgetShortcut);
    connect(searchEscape, &QShortcut::activated, this, &MainWindow::closeSearch);
    auto *find = new QShortcut(QKeySequence::Find, this);
    connect(find, &QShortcut::activated, this, &MainWindow::openSearch);
    return m_searchBar;
}

// Page d'accueil (conversation vierge) : créer une conversation, ou rejoindre avec un code
QWidget *MainWindow::buildHome(QWidget *parent)
{
    auto *home = new QWidget(parent);
    home->setObjectName("home");
    auto *hl = new QVBoxLayout(home);
    hl->setContentsMargins(32, 32, 32, 32);
    hl->addStretch(2);
    auto *logo = new QLabel(home);
    logo->setPixmap(QIcon(":/icons/p2pchat.png").pixmap(112, 112));
    logo->setAlignment(Qt::AlignCenter);
    hl->addWidget(logo);
    hl->addSpacing(16);
    auto *homeTitle = new QLabel(tr("Bienvenue !"), home);
    homeTitle->setObjectName("homeTitle");
    homeTitle->setAlignment(Qt::AlignCenter);
    hl->addWidget(homeTitle);
    auto *homeText = new QLabel(tr("Créez une conversation et envoyez le code à vos proches,\n"
                                   "ou collez le code que l'on vous a envoyé."), home);
    homeText->setObjectName("homeText");
    homeText->setAlignment(Qt::AlignCenter);
    homeText->setWordWrap(true);
    hl->addWidget(homeText);
    hl->addSpacing(24);
    auto *inviteBtn = new QPushButton(tr("Créer une conversation"), home);
    inviteBtn->setObjectName("primary");
    inviteBtn->setCursor(Qt::PointingHandCursor);
    auto *joinBtn = new QPushButton(tr("J'ai reçu un code"), home);
    joinBtn->setObjectName("tonal");
    joinBtn->setCursor(Qt::PointingHandCursor);
    connect(inviteBtn, &QPushButton::clicked, this, &MainWindow::invite);
    connect(joinBtn, &QPushButton::clicked, this, &MainWindow::join);
    for (QPushButton *b : {inviteBtn, joinBtn}) {
        auto *row = new QHBoxLayout;
        row->addStretch();
        row->addWidget(b);
        row->addStretch();
        hl->addLayout(row);
        hl->addSpacing(8);
    }
    hl->addStretch(3);
    return home;
}

// Bandeau au-dessus de la saisie (réponse en cours, image jointe) : contenu, puis ✕ qui appelle onClose.
// Caché au départ.
QWidget *MainWindow::buildClosableBar(QWidget *parent, const QList<QWidget *> &content, const QString &closeTip,
                                      void (MainWindow::*onClose)())
{
    auto *bar = new QWidget(parent);
    auto *rowLayout = new QHBoxLayout(bar);
    rowLayout->setContentsMargins(12, 6, 12, 0);
    auto *box = new QWidget(bar);
    box->setObjectName("replyBar");
    box->setAttribute(Qt::WA_StyledBackground);
    auto *boxLayout = new QHBoxLayout(box);
    boxLayout->setContentsMargins(content.size() > 1 ? 8 : 12, 6, 6, 6);
    for (QWidget *w : content) {
        w->setParent(box);
        boxLayout->addWidget(w, w == content.last() ? 1 : 0);
    }
    auto *close = new QToolButton(box);
    close->setObjectName("replyClose");
    close->setText(QStringLiteral("✕"));
    close->setFixedSize(24, 24);
    close->setCursor(Qt::PointingHandCursor);
    close->setToolTip(closeTip);
    connect(close, &QToolButton::clicked, this, onClose);
    boxLayout->addWidget(close, 0, Qt::AlignTop);
    rowLayout->addWidget(box);
    bar->setVisible(false);
    return bar;
}

// Zone de saisie : 📎, texte (mentions, correcteur), envoyer
QWidget *MainWindow::buildComposerBar(QWidget *parent)
{
    auto *composerBar = new QWidget(parent);
    composerBar->setObjectName("composerBar");
    composerBar->setAttribute(Qt::WA_StyledBackground);
    auto *row = new QHBoxLayout(composerBar);
    row->setContentsMargins(12, 8, 12, 12);
    row->setSpacing(8);
    m_input = new Composer(composerBar);
    m_input->setObjectName("composer");
    m_input->setMaxLength(4000);
    m_input->setMinimumHeight(48);
    m_input->setPlaceholderText(tr("Message"));
    m_input->setToolTip(tr("Entrée : envoyer · Maj+Entrée : aller à la ligne · `code`, ```bloc```"));
    connect(m_input, &Composer::wordAdded, this, [this](const QString &word) {
        QStringList words = m_settings.value("spelling/words").toStringList();
        if (!words.contains(word))
            m_settings.setValue("spelling/words", words << word);
        if (m_spell)
            m_spell->addWord(word);
        m_input->recheckSpelling();
    });
    m_mentions = new MentionCompleter(m_input);
    connect(m_input, &Composer::attachmentInserted, this, &MainWindow::attachFromMime); // collage, dépôt
    m_attachButton = new QToolButton(composerBar);
    m_attachButton->setObjectName("attachButton");
    m_attachButton->setIcon(AppStyle::attachIcon());
    m_attachButton->setIconSize(QSize(24, 24));
    m_attachButton->setFixedSize(48, 48);
    m_attachButton->setCursor(Qt::PointingHandCursor);
    m_attachButton->setToolTip(tr("Joindre une image ou un fichier (ou le glisser dans la fenêtre, ou le coller)"));
    connect(m_attachButton, &QToolButton::clicked, this, &MainWindow::chooseAttachment);
    m_sendButton = new QToolButton(composerBar);
    m_sendButton->setObjectName("sendButton");
    m_sendButton->setIcon(AppStyle::sendIcon());
    m_sendButton->setIconSize(QSize(22, 22));
    m_sendButton->setFixedSize(48, 48);
    m_sendButton->setCursor(Qt::PointingHandCursor);
    m_sendButton->setToolTip(tr("Envoyer"));
    row->addWidget(m_attachButton, 0, Qt::AlignBottom); // la zone de saisie grandit vers le haut
    row->addWidget(m_input, 1);
    row->addWidget(m_sendButton, 0, Qt::AlignBottom);

    connect(m_input, &Composer::returnPressed, this, &MainWindow::sendCurrent);
    // Échap annule la réponse en cours (la liste des mentions, si elle est ouverte, se ferme d'abord)
    auto *escape = new QShortcut(QKeySequence(Qt::Key_Escape), m_input, nullptr, nullptr, Qt::WidgetShortcut);
    connect(escape, &QShortcut::activated, this, &MainWindow::cancelReply);
    connect(m_input, &Composer::textEdited, this, &MainWindow::onTyping);
    connect(m_sendButton, &QToolButton::clicked, this, &MainWindow::sendCurrent);
    return composerBar;
}

QWidget *MainWindow::buildSidebar(QWidget *parent)
{
    auto *side = new QWidget(parent);
    side->setObjectName("sidebar");
    side->setAttribute(Qt::WA_StyledBackground);
    side->setFixedWidth(260);
    auto *lay = new QVBoxLayout(side);
    lay->setContentsMargins(0, 14, 0, 8);
    lay->setSpacing(6);
    auto *head = new QHBoxLayout;
    head->setContentsMargins(18, 0, 10, 0);
    auto *title = new QLabel(tr("Conversations"), side);
    title->setObjectName("sidebarTitle");
    head->addWidget(title, 1);
    auto *add = new QToolButton(side);
    add->setObjectName("menuButton");
    add->setText(QStringLiteral("＋"));
    add->setFixedSize(36, 36);
    add->setCursor(Qt::PointingHandCursor);
    add->setToolTip(tr("Nouvelle conversation, ou rejoindre avec un code"));
    add->setPopupMode(QToolButton::InstantPopup);
    auto *addMenu = new QMenu(add);
    addMenu->addAction(tr("Nouvelle conversation…"), this, &MainWindow::startConversation);
    addMenu->addAction(tr("Rejoindre avec un code…"), this, &MainWindow::join);
    add->setMenu(addMenu);
    head->addWidget(add);
    lay->addLayout(head);
    m_convList = new QListWidget(side);
    m_convList->setObjectName("convList");
    m_convList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_convList->setTextElideMode(Qt::ElideRight);
    m_convList->setIconSize(QSize(ConvAvatarSide, ConvAvatarSide));
    lay->addWidget(m_convList, 1);
    connect(m_convList, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        showConversation(conversationById(item->data(Qt::UserRole).toString()));
    });
    return side;
}
