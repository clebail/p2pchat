# Tests unitaires : qmake ../tests/tests.pro && make && make check
TEMPLATE = subdirs
SUBDIRS += \
    tst_attachment \
    tst_avatar \
    tst_composer \
    tst_crypto \
    tst_edits \
    tst_invitation \
    tst_language \
    tst_links \
    tst_markup \
    tst_mentions \
    tst_mentioncompleter \
    tst_notifications \
    tst_protocol \
    tst_reactions \
    tst_safetynumber \
    tst_singleinstance \
    tst_torservice \
    tst_spellchecker \
    tst_filestore \
    tst_governance \
    tst_groupnode \
    tst_groupstore \
    tst_history \
    tst_connectionpolicy \
    tst_peerconnection \
    tst_chatmodel \
    tst_conversation \
    tst_mainwindow

# Console partagée : Linux et macOS seulement ; de bout en bout (vrai shell, faux terminal) : Linux
unix: SUBDIRS += tst_terminalsharing
linux: SUBDIRS += tst_terminalsession
