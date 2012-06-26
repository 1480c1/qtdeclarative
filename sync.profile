%modules = ( # path to module name map
    "QtQml" => "$basedir/src/qml",
    "QtQuick" => "$basedir/src/quick",
    "QtQuickParticles" => "$basedir/src/particles",
    "QtQuickTest" => "$basedir/src/qmltest",
    "QtQmlDevTools" => "$basedir/src/qmldevtools",
);
%moduleheaders = ( # restrict the module headers to those found in relative path
    "QtQmlDevTools" => "../qml/qml/parser",
);
%deprecatedheaders = (
    "QtQml" => {
        "QQmlImageProvider" => "QtQuick/QQuickImageProvider",
        "qqmlimageprovider.h" => "QtQuick/qquickimageprovider.h",
    },
);
# Module dependencies.
# Every module that is required to build this module should have one entry.
# Each of the module version specifiers can take one of the following values:
#   - A specific Git revision.
#   - any git symbolic ref resolvable from the module's repository (e.g. "refs/heads/master" to track master branch)
#
%dependencies = (
        "qtbase" => "refs/heads/master",
        "qtxmlpatterns" => "refs/heads/master",
        "qtjsbackend" => "refs/heads/master",
);
