QT += qml quick

CONFIG += qmltypes
QML_IMPORT_NAME = Charts
QML_IMPORT_MAJOR_VERSION = 1

HEADERS += piechart.h
SOURCES += piechart.cpp \
           main.cpp

RESOURCES += chapter1-basics.qrc

DESTPATH = $$[QT_INSTALL_EXAMPLES]/qml/tutorials/extending-qml/chapter1-basics
target.path = $$DESTPATH

qml.files = *.qml
qml.path = $$DESTPATH

INSTALLS += target qml
