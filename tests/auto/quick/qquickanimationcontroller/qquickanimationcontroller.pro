QT += core-private gui-private qml-private
TEMPLATE=app
TARGET=tst_qquickanimationcontroller

CONFIG += qmltestcase
CONFIG += parallel_test
SOURCES += tst_qquickanimationcontroller.cpp

TESTDATA = data/*
