TEMPLATE = subdirs

METATYPETESTS += \
    qqmlmetatype

PUBLICTESTS += \
    parserstress \
    qjsvalueiterator \
    qjsonbinding \
    qmlmin \
    qmlplugindump \
    qqmlcomponent \
    qqmlconsole \
    qqmlengine \
    qqmlerror \
    qqmlincubator \
    qqmlinfo \
    qqmllistreference \
    qqmllocale \
    qqmlmetaobject \
    qqmlmoduleplugin \
    qqmlqt \
    qqmltranslation \
    qqmlxmlhttprequest \
    qqmlparser \
    qquickfolderlistmodel

PRIVATETESTS += \
    animation \
    qqmlcpputils \
    qqmlecmascript \
    qqmlcontext \
    qqmlexpression \
    qqmlglobal \
    qqmlinstruction \
    qqmllanguage \
    qqmlproperty \
    qqmlpropertymap \
    qqmlsqldatabase \
    qqmlvaluetypes \
    qqmlvaluetypeproviders \
    qquickbinding \
    qquickchangeset \
    qquickconnection \
    qquicklistcompositor \
    qquicklistmodel \
    qquicklistmodelworkerscript \
    qquickworkerscript \
    qqmlbundle \
    qrcqml \
    v4

!contains(QT_CONFIG, no-widgets) {
    PUBLICTESTS += \
        qjsengine \
        qjsvalue
}

SUBDIRS += $$PUBLICTESTS
SUBDIRS += $$METATYPETESTS
SUBDIRS += debugger

contains(QT_CONFIG, private_tests) {
    SUBDIRS += $$PRIVATETESTS
}
