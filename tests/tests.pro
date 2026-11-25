# Tests unitaires : qmake ../tests/tests.pro && make && make check
TEMPLATE = subdirs
SUBDIRS += \
    tst_crypto \
    tst_invitation \
    tst_protocol \
    tst_groupstore \
    tst_history \
    tst_connectionpolicy \
    tst_peerconnection \
    tst_chatmodel
