/****************************************************************************
**
** Copyright (C) 2018 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of the QML preview debug service.
**
** $QT_BEGIN_LICENSE:GPL-EXCEPT$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 as published by the Free Software
** Foundation with exceptions as appearing in the file LICENSE.GPL3-EXCEPT
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "qqmlpreviewhandler.h"

#include <QtCore/qtimer.h>
#include <QtCore/qsettings.h>
#include <QtCore/qlibraryinfo.h>
#include <QtCore/qtranslator.h>

#include <QtGui/qwindow.h>
#include <QtGui/qguiapplication.h>
#include <QtQuick/qquickwindow.h>
#include <QtQuick/qquickitem.h>
#include <QtQml/qqmlcomponent.h>

#include <private/qquickpixmapcache_p.h>
#include <private/qquickview_p.h>
#include <private/qhighdpiscaling_p.h>

QT_BEGIN_NAMESPACE

struct QuitLockDisabler
{
    const bool quitLockEnabled;

    QuitLockDisabler() : quitLockEnabled(QCoreApplication::isQuitLockEnabled())
    {
        QCoreApplication::setQuitLockEnabled(false);
    }

    ~QuitLockDisabler()
    {
        QCoreApplication::setQuitLockEnabled(quitLockEnabled);
    }
};

QQmlPreviewHandler::QQmlPreviewHandler(QObject *parent) : QObject(parent)
{
    m_dummyItem.reset(new QQuickItem);

    // TODO: Is there a better way to determine this? We want to keep the window alive when possible
    //       as otherwise it will reappear in a different place when (re)loading a file. However,
    //       the file we load might create another window, in which case the eglfs plugin (and
    //       others?) will do a qFatal as it only supports a single window.
    const QString platformName = QGuiApplication::platformName();
    m_supportsMultipleWindows = (platformName == QStringLiteral("windows")
                                 || platformName == QStringLiteral("cocoa")
                                 || platformName == QStringLiteral("xcb")
                                 || platformName == QStringLiteral("wayland"));

    QCoreApplication::instance()->installEventFilter(this);

    m_fpsTimer.setInterval(1000);
    connect(&m_fpsTimer, &QTimer::timeout, this, &QQmlPreviewHandler::fpsTimerHit);
}

QQmlPreviewHandler::~QQmlPreviewHandler()
{
    removeTranslators();
    clear();
}

static void closeAllWindows()
{
    const QWindowList windows = QGuiApplication::allWindows();
    for (QWindow *window : windows)
        window->close();
}

bool QQmlPreviewHandler::eventFilter(QObject *obj, QEvent *event)
{
    if (event->type() == QEvent::Show) {
        if (QWindow *window = qobject_cast<QQuickWindow*>(obj)) {
            m_lastPosition.initLastSavedWindowPosition(window);
        }
    }
    if ((event->type() == QEvent::Move || event->type() == QEvent::Resize) &&
            qobject_cast<QQuickWindow*>(obj) == m_currentWindow) {
        // we always start with factor 1 so calculate and save the origin as it would be not scaled
        m_lastPosition.setPosition(m_currentWindow->framePosition() *
                                    QHighDpiScaling::factor(m_currentWindow));
    }

    return QObject::eventFilter(obj, event);
}

void QQmlPreviewHandler::addEngine(QQmlEngine *qmlEngine)
{
    m_engines.append(qmlEngine);
}

void QQmlPreviewHandler::removeEngine(QQmlEngine *qmlEngine)
{
    const bool found = m_engines.removeOne(qmlEngine);
    Q_ASSERT(found);
    for (QObject *obj : m_createdObjects)
    if (obj && QtQml::qmlEngine(obj) == qmlEngine)
      delete obj;
    m_createdObjects.removeAll(nullptr);
}

void QQmlPreviewHandler::loadUrl(const QUrl &url)
{
    QSharedPointer<QuitLockDisabler> disabler(new QuitLockDisabler);

    clear();
    m_component.reset(nullptr);
    QQuickPixmap::purgeCache();

    const int numEngines = m_engines.count();
    if (numEngines > 1) {
        emit error(QString::fromLatin1("%1 QML engines available. We cannot decide which one "
                                       "should load the component.").arg(numEngines));
        return;
    } else if (numEngines == 0) {
        emit error(QLatin1String("No QML engines found."));
        return;
    }
    m_lastPosition.loadWindowPositionSettings(url);

    QQmlEngine *engine = m_engines.front();
    engine->clearComponentCache();
    m_component.reset(new QQmlComponent(engine, url, this));

    auto onStatusChanged = [disabler, this](QQmlComponent::Status status) {
        switch (status) {
        case QQmlComponent::Null:
        case QQmlComponent::Loading:
            return true; // try again later
        case QQmlComponent::Ready:
            tryCreateObject();
            break;
        case QQmlComponent::Error:
            emit error(m_component->errorString());
            break;
        default:
            Q_UNREACHABLE();
            break;
        }

        disconnect(m_component.data(), &QQmlComponent::statusChanged, this, nullptr);
        return false; // we're done
    };

    if (onStatusChanged(m_component->status()))
        connect(m_component.data(), &QQmlComponent::statusChanged, this, onStatusChanged);
}

void QQmlPreviewHandler::rerun()
{
    if (m_component.isNull() || !m_component->isReady())
        emit error(QLatin1String("Component is not ready."));

    QuitLockDisabler disabler;
    Q_UNUSED(disabler);
    clear();
    tryCreateObject();
}

void QQmlPreviewHandler::zoom(qreal newFactor)
{
    if (!m_currentWindow)
        return;
    if (qFuzzyIsNull(newFactor)) {
        emit error(QString::fromLatin1("Zooming with factor: %1 will result in nothing " \
                                       "so it will be ignored.").arg(newFactor));
        return;
    }
    QString errorMessage;
    bool resetZoom = false;

    if (newFactor < 0) {
        resetZoom = true;
        newFactor = 1.0;
    }

    // On single-window devices we allow any scale factor as the window will adapt to the screen.
    if (m_supportsMultipleWindows) {
        const QSize newAvailableScreenSize = QQmlPreviewPosition::currentScreenSize(m_currentWindow)
                * QHighDpiScaling::factor(m_currentWindow) / newFactor;
        if (m_currentWindow->size().width() > newAvailableScreenSize.width()) {
            errorMessage = QString::fromLatin1(
                        "Zooming with factor: "
                        "%1 will result in a too wide preview.").arg(newFactor);
        }
        if (m_currentWindow->size().height() > newAvailableScreenSize.height()) {
            errorMessage = QString::fromLatin1(
                        "Zooming with factor: "
                        "%1 will result in a too heigh preview.").arg(newFactor);
        }
    }

    if (errorMessage.isEmpty()) {
        const QPoint newToOriginMappedPosition = m_currentWindow->position() *
                QHighDpiScaling::factor(m_currentWindow) / newFactor;
        m_currentWindow->destroy();
        QHighDpiScaling::setScreenFactor(m_currentWindow->screen(), newFactor);
        if (resetZoom)
            QHighDpiScaling::updateHighDpiScaling();
        m_currentWindow->setPosition(newToOriginMappedPosition);
        m_currentWindow->show();
    } else {
        emit error(errorMessage);
    }
}

void QQmlPreviewHandler::removeTranslators()
{
    if (!m_qtTranslator.isNull()) {
        QCoreApplication::removeTranslator(m_qtTranslator.get());
        m_qtTranslator.reset();
    }

    if (m_qmlTranslator.isNull()) {
        QCoreApplication::removeTranslator(m_qmlTranslator.get());
        m_qmlTranslator.reset();
    }
}

void QQmlPreviewHandler::language(const QUrl &context, const QString &locale)
{
    removeTranslators();

    m_qtTranslator.reset(new QTranslator(this));
    if (m_qtTranslator->load(QLatin1String("qt_") + locale,
                           QLibraryInfo::location(QLibraryInfo::TranslationsPath))) {
        QCoreApplication::installTranslator(m_qtTranslator.get());
    }

    m_qmlTranslator.reset(new QTranslator(this));
    if (m_qmlTranslator->load(QLatin1String("qml_" ) + locale,
                              context.toLocalFile() + QLatin1String("/i18n"))) {
        QCoreApplication::installTranslator(m_qmlTranslator.get());
    }

    for (QQmlEngine *engine : qAsConst(m_engines))
        engine->retranslate();
}

void QQmlPreviewHandler::clear()
{
    qDeleteAll(m_createdObjects);
    m_createdObjects.clear();
    setCurrentWindow(nullptr);
}

Qt::WindowFlags fixFlags(Qt::WindowFlags flags)
{
    // If only the type flag is given, some other window flags are automatically assumed. When we
    // add a flag, we need to make those explicit.
    switch (flags) {
    case Qt::Window:
        return flags | Qt::WindowTitleHint | Qt::WindowSystemMenuHint | Qt::WindowCloseButtonHint
                | Qt::WindowMinimizeButtonHint | Qt::WindowMaximizeButtonHint;
    case Qt::Dialog:
    case Qt::Tool:
        return flags | Qt::WindowTitleHint | Qt::WindowSystemMenuHint | Qt::WindowCloseButtonHint;
    default:
        return flags;
    }
}

void QQmlPreviewHandler::showObject(QObject *object)
{
    if (QWindow *window = qobject_cast<QWindow *>(object)) {
        setCurrentWindow(qobject_cast<QQuickWindow *>(window));
        for (QWindow *otherWindow : QGuiApplication::allWindows()) {
            if (QQuickWindow *quickWindow = qobject_cast<QQuickWindow *>(otherWindow)) {
                if (quickWindow == m_currentWindow)
                    continue;
                quickWindow->setVisible(false);
                quickWindow->setFlags(quickWindow->flags() & ~Qt::WindowStaysOnTopHint);
            }
        }
    } else if (QQuickItem *item = qobject_cast<QQuickItem *>(object)) {
        setCurrentWindow(nullptr);
        for (QWindow *window : QGuiApplication::allWindows()) {
            if (QQuickWindow *quickWindow = qobject_cast<QQuickWindow *>(window)) {
                if (m_currentWindow != nullptr) {
                    emit error(QLatin1String("Multiple QQuickWindows available. We cannot "
                                             "decide which one to use."));
                    return;
                }
                setCurrentWindow(quickWindow);
            } else {
                window->setVisible(false);
                window->setFlag(Qt::WindowStaysOnTopHint, false);
            }
        }

        if (m_currentWindow == nullptr) {
            setCurrentWindow(new QQuickWindow);
            m_createdObjects.append(m_currentWindow.data());
        }

        for (QQuickItem *oldItem : m_currentWindow->contentItem()->childItems())
            oldItem->setParentItem(m_dummyItem.data());

        // Special case for QQuickView, as that keeps a "root" pointer around, and uses it to
        // automatically resize the window or the item.
        if (QQuickView *view = qobject_cast<QQuickView *>(m_currentWindow))
            QQuickViewPrivate::get(view)->setRootObject(item);
        else
            item->setParentItem(m_currentWindow->contentItem());

        m_currentWindow->resize(item->size().toSize());
    } else {
        emit error(QLatin1String("Created object is neither a QWindow nor a QQuickItem."));
    }

    if (m_currentWindow) {
        m_lastPosition.initLastSavedWindowPosition(m_currentWindow);
        m_currentWindow->setFlags(fixFlags(m_currentWindow->flags()) | Qt::WindowStaysOnTopHint);
        m_currentWindow->setVisible(true);
    }
}

void QQmlPreviewHandler::setCurrentWindow(QQuickWindow *window)
{
    if (window == m_currentWindow.data())
        return;

    if (m_currentWindow) {
        disconnect(m_currentWindow.data(), &QQuickWindow::frameSwapped,
                   this, &QQmlPreviewHandler::frameSwapped);
        m_fpsTimer.stop();
        m_frames = 0;
    }

    m_currentWindow = window;

    if (m_currentWindow) {
        connect(m_currentWindow.data(), &QQuickWindow::frameSwapped,
                this, &QQmlPreviewHandler::frameSwapped);
        m_fpsTimer.start();
    }
}

void QQmlPreviewHandler::frameSwapped()
{
    ++m_frames;
}

void QQmlPreviewHandler::fpsTimerHit()
{
    emit fps(m_frames);
    m_frames = 0;
}

void QQmlPreviewHandler::tryCreateObject()
{
    if (!m_supportsMultipleWindows)
        closeAllWindows();
    QObject *object = m_component->create();
    m_createdObjects.append(object);
    showObject(object);
}

QT_END_NAMESPACE
