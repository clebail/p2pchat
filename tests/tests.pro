# Tests unitaires : qmake ../tests/tests.pro && make && make check
TEMPLATE = subdirs
SUBDIRS += \
    tst_attachment \
    tst_composer \
    tst_crypto \
    tst_invitation \
    tst_links \
    tst_markup \
    tst_mentions \
    tst_mentioncompleter \
    tst_notifications \
    tst_protocol \
    tst_reactions \
    tst_safetynumber \
    tst_singleinstance \
    tst_groupnode \
    tst_groupstore \
    tst_history \
    tst_connectionpolicy \
    tst_peerconnection \
    tst_chatmodel

# Console partagée : Linux et macOS seulement
unix: SUBDIRS += tst_terminalsharing
