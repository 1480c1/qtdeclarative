/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: http://www.qt.io/licensing/
**
** This file is part of the demonstration applications of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:BSD$
** You may use this file under the terms of the BSD license as follows:
**
** "Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions are
** met:
**   * Redistributions of source code must retain the above copyright
**     notice, this list of conditions and the following disclaimer.
**   * Redistributions in binary form must reproduce the above copyright
**     notice, this list of conditions and the following disclaimer in
**     the documentation and/or other materials provided with the
**     distribution.
**   * Neither the name of The Qt Company Ltd nor the names of its
**     contributors may be used to endorse or promote products derived
**     from this software without specific prior written permission.
**
**
** THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
** "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
** LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
** A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
** OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
** SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
** LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
** OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE."
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include <QGuiApplication>
#include <QThread>
#include <QQuickView>
#include <QQmlEngine>
#include <QQmlContext>

class Helper : public QObject
{
    Q_OBJECT

public:
    Q_INVOKABLE void sleep(int ms) {
        QThread::msleep(ms);
    }
};

int main(int argc, char **argv)
{
    qputenv("QT_QUICK_BACKEND", "d3d12");

    QGuiApplication app(argc, argv);

    qDebug("Available tests:");
    qDebug("  [R] - Rectangles");
    qDebug("  [4] - A lot of rectangles (perf)");
    qDebug("  [I] - Images");
    qDebug("  [T] - Text");
    qDebug("  [A] - Render thread Animator");
    qDebug("\nPress S to stop the currently running test\n");

    Helper helper;
    QQuickView view;
    if (app.arguments().contains(QLatin1String("--multisample"))) {
        qDebug("Requesting sample count 4");
        QSurfaceFormat fmt;
        fmt.setSamples(4);
        view.setFormat(fmt);
    }
    view.engine()->rootContext()->setContextProperty(QLatin1String("helper"), &helper);
    view.setResizeMode(QQuickView::SizeRootObjectToView);
    view.resize(1024, 768);
    view.setSource(QUrl("qrc:/main.qml"));
    view.show();

    return app.exec();
}

#include "nodetypes.moc"
