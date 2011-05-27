/****************************************************************************
**
** Copyright (C) 2011 Nokia Corporation and/or its subsidiary(-ies).
** All rights reserved.
** Contact: Nokia Corporation (qt-info@nokia.com)
**
** This file is part of the QtDeclarative module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** GNU Lesser General Public License Usage
** This file may be used under the terms of the GNU Lesser General Public
** License version 2.1 as published by the Free Software Foundation and
** appearing in the file LICENSE.LGPL included in the packaging of this
** file. Please review the following information to ensure the GNU Lesser
** General Public License version 2.1 requirements will be met:
** http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Nokia gives you certain additional
** rights. These rights are described in the Nokia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU General
** Public License version 3.0 as published by the Free Software Foundation
** and appearing in the file LICENSE.GPL included in the packaging of this
** file. Please review the following information to ensure the GNU General
** Public License version 3.0 requirements will be met:
** http://www.gnu.org/copyleft/gpl.html.
**
** Other Usage
** Alternatively, this file may be used in accordance with the terms and
** conditions contained in a signed written agreement between you and Nokia.
**
**
**
**
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "QtDeclarative/private/qdeclarativeobserverservice_p.h"
#include "QtDeclarative/private/qdeclarativedebughelper_p.h"

#include "qdeclarativeviewobserver_p.h"
#include "qdeclarativeviewobserver_p_p.h"
#include "qdeclarativeobserverprotocol.h"

#include "editor/liveselectiontool_p.h"
#include "editor/zoomtool_p.h"
#include "editor/colorpickertool_p.h"
#include "editor/livelayeritem_p.h"
#include "editor/boundingrecthighlighter_p.h"
#include "editor/qmltoolbar_p.h"

#include <QtDeclarative/QDeclarativeItem>
#include <QtDeclarative/QDeclarativeEngine>
#include <QtDeclarative/QDeclarativeContext>
#include <QtDeclarative/QDeclarativeExpression>
#include <QtGui/QWidget>
#include <QtGui/QVBoxLayout>
#include <QtGui/QMouseEvent>
#include <QtGui/QGraphicsObject>
#include <QtGui/QApplication>
#include <QtCore/QSettings>

static inline void initEditorResource() { Q_INIT_RESOURCE(editor); }

QT_BEGIN_NAMESPACE

const char * const KEY_TOOLBOX_GEOMETRY = "toolBox/geometry";

const int SceneChangeUpdateInterval = 5000;


class ToolBox : public QWidget
{
    Q_OBJECT

public:
    ToolBox(QWidget *parent = 0);
    ~ToolBox();

    QmlToolBar *toolBar() const { return m_toolBar; }

private:
    QSettings m_settings;
    QmlToolBar *m_toolBar;
};

ToolBox::ToolBox(QWidget *parent)
    : QWidget(parent, Qt::Tool)
    , m_settings(QLatin1String("Nokia"), QLatin1String("QmlObserver"), this)
    , m_toolBar(new QmlToolBar)
{
    setWindowFlags((windowFlags() & ~Qt::WindowCloseButtonHint) | Qt::CustomizeWindowHint);
    setWindowTitle(tr("Qt Quick Toolbox"));

    QVBoxLayout *verticalLayout = new QVBoxLayout;
    verticalLayout->setMargin(0);
    verticalLayout->addWidget(m_toolBar);
    setLayout(verticalLayout);

    restoreGeometry(m_settings.value(QLatin1String(KEY_TOOLBOX_GEOMETRY)).toByteArray());
}

ToolBox::~ToolBox()
{
    m_settings.setValue(QLatin1String(KEY_TOOLBOX_GEOMETRY), saveGeometry());
}


QDeclarativeViewObserverPrivate::QDeclarativeViewObserverPrivate(QDeclarativeViewObserver *q) :
    q(q),
    designModeBehavior(false),
    showAppOnTop(false),
    animationPaused(false),
    slowDownFactor(1.0f),
    toolBox(0)
{
}

QDeclarativeViewObserverPrivate::~QDeclarativeViewObserverPrivate()
{
}

QDeclarativeViewObserver::QDeclarativeViewObserver(QDeclarativeView *view,
                                                   QObject *parent) :
    QObject(parent),
    data(new QDeclarativeViewObserverPrivate(this))
{
    initEditorResource();

    data->view = view;
    data->manipulatorLayer = new LiveLayerItem(view->scene());
    data->selectionTool = new LiveSelectionTool(this);
    data->zoomTool = new ZoomTool(this);
    data->colorPickerTool = new ColorPickerTool(this);
    data->boundingRectHighlighter = new BoundingRectHighlighter(this);
    data->currentTool = data->selectionTool;

    // to capture ChildRemoved event when viewport changes
    data->view->installEventFilter(this);

    data->setViewport(data->view->viewport());

    data->debugService = QDeclarativeObserverService::instance();
    connect(data->debugService, SIGNAL(gotMessage(QByteArray)),
            this, SLOT(handleMessage(QByteArray)));

    connect(data->view, SIGNAL(statusChanged(QDeclarativeView::Status)),
            data.data(), SLOT(_q_onStatusChanged(QDeclarativeView::Status)));

    connect(data->colorPickerTool, SIGNAL(selectedColorChanged(QColor)),
            SIGNAL(selectedColorChanged(QColor)));
    connect(data->colorPickerTool, SIGNAL(selectedColorChanged(QColor)),
            this, SLOT(sendColorChanged(QColor)));

    data->_q_changeToSingleSelectTool();
}

QDeclarativeViewObserver::~QDeclarativeViewObserver()
{
}

void QDeclarativeViewObserverPrivate::_q_setToolBoxVisible(bool visible)
{
#if !defined(Q_OS_SYMBIAN) && !defined(Q_WS_MAEMO_5) && !defined(Q_WS_SIMULATOR)
    if (!toolBox && visible)
        createToolBox();
    if (toolBox)
        toolBox->setVisible(visible);
#else
    Q_UNUSED(visible)
#endif
}

void QDeclarativeViewObserverPrivate::_q_reloadView()
{
    clearHighlight();
    emit q->reloadRequested();
}

void QDeclarativeViewObserverPrivate::setViewport(QWidget *widget)
{
    if (viewport.data() == widget)
        return;

    if (viewport)
        viewport.data()->removeEventFilter(q);

    viewport = widget;
    if (viewport) {
        // make sure we get mouse move events
        viewport.data()->setMouseTracking(true);
        viewport.data()->installEventFilter(q);
    }
}

void QDeclarativeViewObserverPrivate::clearEditorItems()
{
    clearHighlight();
    setSelectedItems(QList<QGraphicsItem*>());
}

bool QDeclarativeViewObserver::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == data->view) {
        // Event from view
        if (event->type() == QEvent::ChildRemoved) {
            // Might mean that viewport has changed
            if (data->view->viewport() != data->viewport.data())
                data->setViewport(data->view->viewport());
        }
        return QObject::eventFilter(obj, event);
    }

    // Event from viewport
    switch (event->type()) {
    case QEvent::Leave: {
        if (leaveEvent(event))
            return true;
        break;
    }
    case QEvent::MouseButtonPress: {
        if (mousePressEvent(static_cast<QMouseEvent*>(event)))
            return true;
        break;
    }
    case QEvent::MouseMove: {
        if (mouseMoveEvent(static_cast<QMouseEvent*>(event)))
            return true;
        break;
    }
    case QEvent::MouseButtonRelease: {
        if (mouseReleaseEvent(static_cast<QMouseEvent*>(event)))
            return true;
        break;
    }
    case QEvent::KeyPress: {
        if (keyPressEvent(static_cast<QKeyEvent*>(event)))
            return true;
        break;
    }
    case QEvent::KeyRelease: {
        if (keyReleaseEvent(static_cast<QKeyEvent*>(event)))
            return true;
        break;
    }
    case QEvent::MouseButtonDblClick: {
        if (mouseDoubleClickEvent(static_cast<QMouseEvent*>(event)))
            return true;
        break;
    }
    case QEvent::Wheel: {
        if (wheelEvent(static_cast<QWheelEvent*>(event)))
            return true;
        break;
    }
    default: {
        break;
    }
    } //switch

    // standard event processing
    return QObject::eventFilter(obj, event);
}

bool QDeclarativeViewObserver::leaveEvent(QEvent * /*event*/)
{
    if (!data->designModeBehavior)
        return false;
    data->clearHighlight();
    return true;
}

bool QDeclarativeViewObserver::mousePressEvent(QMouseEvent *event)
{
    if (!data->designModeBehavior)
        return false;
    data->cursorPos = event->pos();
    data->currentTool->mousePressEvent(event);
    return true;
}

bool QDeclarativeViewObserver::mouseMoveEvent(QMouseEvent *event)
{
    if (!data->designModeBehavior) {
        data->clearEditorItems();
        return false;
    }
    data->cursorPos = event->pos();

    QList<QGraphicsItem*> selItems = data->selectableItems(event->pos());
    if (!selItems.isEmpty()) {
        declarativeView()->setToolTip(data->currentTool->titleForItem(selItems.first()));
    } else {
        declarativeView()->setToolTip(QString());
    }
    if (event->buttons()) {
        data->currentTool->mouseMoveEvent(event);
    } else {
        data->currentTool->hoverMoveEvent(event);
    }
    return true;
}

bool QDeclarativeViewObserver::mouseReleaseEvent(QMouseEvent *event)
{
    if (!data->designModeBehavior)
        return false;

    data->cursorPos = event->pos();
    data->currentTool->mouseReleaseEvent(event);
    return true;
}

bool QDeclarativeViewObserver::keyPressEvent(QKeyEvent *event)
{
    if (!data->designModeBehavior)
        return false;

    data->currentTool->keyPressEvent(event);
    return true;
}

bool QDeclarativeViewObserver::keyReleaseEvent(QKeyEvent *event)
{
    if (!data->designModeBehavior)
        return false;

    switch (event->key()) {
    case Qt::Key_V:
        data->_q_changeToSingleSelectTool();
        break;
// disabled because multiselection does not do anything useful without design mode
//    case Qt::Key_M:
//        data->_q_changeToMarqueeSelectTool();
//        break;
    case Qt::Key_I:
        data->_q_changeToColorPickerTool();
        break;
    case Qt::Key_Z:
        data->_q_changeToZoomTool();
        break;
    case Qt::Key_Space:
        setAnimationPaused(!data->animationPaused);
        break;
    default:
        break;
    }

    data->currentTool->keyReleaseEvent(event);
    return true;
}

void QDeclarativeViewObserverPrivate::_q_createQmlObject(const QString &qml, QObject *parent,
                                                         const QStringList &importList,
                                                         const QString &filename)
{
    if (!parent)
        return;

    QString imports;
    foreach (const QString &s, importList) {
        imports += s;
        imports += QLatin1Char('\n');
    }

    QDeclarativeContext *parentContext = view->engine()->contextForObject(parent);
    QDeclarativeComponent component(view->engine(), q);
    QByteArray constructedQml = QString(imports + qml).toLatin1();

    component.setData(constructedQml, QUrl::fromLocalFile(filename));
    QObject *newObject = component.create(parentContext);
    if (newObject) {
        newObject->setParent(parent);
        QDeclarativeItem *parentItem = qobject_cast<QDeclarativeItem*>(parent);
        QDeclarativeItem *newItem    = qobject_cast<QDeclarativeItem*>(newObject);
        if (parentItem && newItem)
            newItem->setParentItem(parentItem);
    }
}

void QDeclarativeViewObserverPrivate::_q_reparentQmlObject(QObject *object, QObject *newParent)
{
    if (!newParent)
        return;

    object->setParent(newParent);
    QDeclarativeItem *newParentItem = qobject_cast<QDeclarativeItem*>(newParent);
    QDeclarativeItem *item    = qobject_cast<QDeclarativeItem*>(object);
    if (newParentItem && item)
        item->setParentItem(newParentItem);
}

void QDeclarativeViewObserverPrivate::_q_clearComponentCache()
{
    view->engine()->clearComponentCache();
}

void QDeclarativeViewObserverPrivate::_q_removeFromSelection(QObject *obj)
{
    QList<QGraphicsItem*> items = selectedItems();
    if (QGraphicsItem *item = qobject_cast<QGraphicsObject*>(obj))
        items.removeOne(item);
    setSelectedItems(items);
}

bool QDeclarativeViewObserver::mouseDoubleClickEvent(QMouseEvent * /*event*/)
{
    if (!data->designModeBehavior)
        return false;

    return true;
}

bool QDeclarativeViewObserver::wheelEvent(QWheelEvent *event)
{
    if (!data->designModeBehavior)
        return false;
    data->currentTool->wheelEvent(event);
    return true;
}

void QDeclarativeViewObserver::setDesignModeBehavior(bool value)
{
    emit designModeBehaviorChanged(value);

    if (data->toolBox)
        data->toolBox->toolBar()->setDesignModeBehavior(value);
    sendDesignModeBehavior(value);

    data->designModeBehavior = value;

    if (!data->designModeBehavior)
        data->clearEditorItems();
}

bool QDeclarativeViewObserver::designModeBehavior()
{
    return data->designModeBehavior;
}

bool QDeclarativeViewObserver::showAppOnTop() const
{
    return data->showAppOnTop;
}

void QDeclarativeViewObserver::setShowAppOnTop(bool appOnTop)
{
    if (data->view) {
        QWidget *window = data->view->window();
        Qt::WindowFlags flags = window->windowFlags();
        if (appOnTop)
            flags |= Qt::WindowStaysOnTopHint;
        else
            flags &= ~Qt::WindowStaysOnTopHint;

        window->setWindowFlags(flags);
        window->show();
    }

    data->showAppOnTop = appOnTop;
    sendShowAppOnTop(appOnTop);

    emit showAppOnTopChanged(appOnTop);
}

void QDeclarativeViewObserverPrivate::changeTool(Constants::DesignTool tool,
                                                 Constants::ToolFlags /*flags*/)
{
    switch (tool) {
    case Constants::SelectionToolMode:
        _q_changeToSingleSelectTool();
        break;
    case Constants::NoTool:
    default:
        currentTool = 0;
        break;
    }
}

void QDeclarativeViewObserverPrivate::setSelectedItemsForTools(const QList<QGraphicsItem *> &items)
{
    foreach (const QWeakPointer<QGraphicsObject> &obj, currentSelection) {
        if (QGraphicsItem *item = obj.data()) {
            if (!items.contains(item)) {
                QObject::disconnect(obj.data(), SIGNAL(destroyed(QObject*)),
                                    this, SLOT(_q_removeFromSelection(QObject*)));
                currentSelection.removeOne(obj);
            }
        }
    }

    foreach (QGraphicsItem *item, items) {
        if (item) {
            if (QGraphicsObject *obj = item->toGraphicsObject()) {
                QObject::connect(obj, SIGNAL(destroyed(QObject*)),
                                 this, SLOT(_q_removeFromSelection(QObject*)));
                currentSelection.append(obj);
            }
        }
    }

    currentTool->updateSelectedItems();
}

void QDeclarativeViewObserverPrivate::setSelectedItems(const QList<QGraphicsItem *> &items)
{
    QList<QWeakPointer<QGraphicsObject> > oldList = currentSelection;
    setSelectedItemsForTools(items);
    if (oldList != currentSelection) {
        QList<QObject*> objectList;
        foreach (const QWeakPointer<QGraphicsObject> &graphicsObject, currentSelection) {
            if (graphicsObject)
                objectList << graphicsObject.data();
        }

        q->sendCurrentObjects(objectList);
    }
}

QList<QGraphicsItem *> QDeclarativeViewObserverPrivate::selectedItems() const
{
    QList<QGraphicsItem *> selection;
    foreach (const QWeakPointer<QGraphicsObject> &selectedObject, currentSelection) {
        if (selectedObject.data())
            selection << selectedObject.data();
    }

    return selection;
}

void QDeclarativeViewObserver::setSelectedItems(QList<QGraphicsItem *> items)
{
    data->setSelectedItems(items);
}

QList<QGraphicsItem *> QDeclarativeViewObserver::selectedItems() const
{
    return data->selectedItems();
}

QDeclarativeView *QDeclarativeViewObserver::declarativeView()
{
    return data->view;
}

void QDeclarativeViewObserverPrivate::clearHighlight()
{
    boundingRectHighlighter->clear();
}

void QDeclarativeViewObserverPrivate::highlight(const QList<QGraphicsObject *> &items)
{
    if (items.isEmpty())
        return;

    QList<QGraphicsObject*> objectList;
    foreach (QGraphicsItem *item, items) {
        QGraphicsItem *child = item;

        if (child) {
            QGraphicsObject *childObject = child->toGraphicsObject();
            if (childObject)
                objectList << childObject;
        }
    }

    boundingRectHighlighter->highlight(objectList);
}

QList<QGraphicsItem*> QDeclarativeViewObserverPrivate::selectableItems(
    const QPointF &scenePos) const
{
    QList<QGraphicsItem*> itemlist = view->scene()->items(scenePos);
    return filterForSelection(itemlist);
}

QList<QGraphicsItem*> QDeclarativeViewObserverPrivate::selectableItems(const QPoint &pos) const
{
    QList<QGraphicsItem*> itemlist = view->items(pos);
    return filterForSelection(itemlist);
}

QList<QGraphicsItem*> QDeclarativeViewObserverPrivate::selectableItems(
    const QRectF &sceneRect, Qt::ItemSelectionMode selectionMode) const
{
    QList<QGraphicsItem*> itemlist = view->scene()->items(sceneRect, selectionMode);
    return filterForSelection(itemlist);
}

void QDeclarativeViewObserverPrivate::_q_changeToSingleSelectTool()
{
    currentToolMode = Constants::SelectionToolMode;
    selectionTool->setRubberbandSelectionMode(false);

    changeToSelectTool();

    emit q->selectToolActivated();
    q->sendCurrentTool(Constants::SelectionToolMode);
}

void QDeclarativeViewObserverPrivate::changeToSelectTool()
{
    if (currentTool == selectionTool)
        return;

    currentTool->clear();
    currentTool = selectionTool;
    currentTool->clear();
    currentTool->updateSelectedItems();
}

void QDeclarativeViewObserverPrivate::_q_changeToMarqueeSelectTool()
{
    changeToSelectTool();
    currentToolMode = Constants::MarqueeSelectionToolMode;
    selectionTool->setRubberbandSelectionMode(true);

    emit q->marqueeSelectToolActivated();
    q->sendCurrentTool(Constants::MarqueeSelectionToolMode);
}

void QDeclarativeViewObserverPrivate::_q_changeToZoomTool()
{
    currentToolMode = Constants::ZoomMode;
    currentTool->clear();
    currentTool = zoomTool;
    currentTool->clear();

    emit q->zoomToolActivated();
    q->sendCurrentTool(Constants::ZoomMode);
}

void QDeclarativeViewObserverPrivate::_q_changeToColorPickerTool()
{
    if (currentTool == colorPickerTool)
        return;

    currentToolMode = Constants::ColorPickerMode;
    currentTool->clear();
    currentTool = colorPickerTool;
    currentTool->clear();

    emit q->colorPickerActivated();
    q->sendCurrentTool(Constants::ColorPickerMode);
}

void QDeclarativeViewObserver::setAnimationSpeed(qreal slowDownFactor)
{
    Q_ASSERT(slowDownFactor > 0);
    if (data->slowDownFactor == slowDownFactor)
        return;

    animationSpeedChangeRequested(slowDownFactor);
    sendAnimationSpeed(slowDownFactor);
}

void QDeclarativeViewObserver::setAnimationPaused(bool paused)
{
    if (data->animationPaused == paused)
        return;

    animationPausedChangeRequested(paused);
    sendAnimationPaused(paused);
}

void QDeclarativeViewObserver::animationSpeedChangeRequested(qreal factor)
{
    if (data->slowDownFactor != factor) {
        data->slowDownFactor = factor;
        emit animationSpeedChanged(factor);
    }

    const float effectiveFactor = data->animationPaused ? 0 : factor;
    QDeclarativeDebugHelper::setAnimationSlowDownFactor(effectiveFactor);
}

void QDeclarativeViewObserver::animationPausedChangeRequested(bool paused)
{
    if (data->animationPaused != paused) {
        data->animationPaused = paused;
        emit animationPausedChanged(paused);
    }

    const float effectiveFactor = paused ? 0 : data->slowDownFactor;
    QDeclarativeDebugHelper::setAnimationSlowDownFactor(effectiveFactor);
}


void QDeclarativeViewObserverPrivate::_q_applyChangesFromClient()
{
}


QList<QGraphicsItem*> QDeclarativeViewObserverPrivate::filterForSelection(
    QList<QGraphicsItem*> &itemlist) const
{
    foreach (QGraphicsItem *item, itemlist) {
        if (isEditorItem(item))
            itemlist.removeOne(item);
    }

    return itemlist;
}

bool QDeclarativeViewObserverPrivate::isEditorItem(QGraphicsItem *item) const
{
    return (item->type() == Constants::EditorItemType
            || item->type() == Constants::ResizeHandleItemType
            || item->data(Constants::EditorItemDataKey).toBool());
}

void QDeclarativeViewObserverPrivate::_q_onStatusChanged(QDeclarativeView::Status status)
{
    if (status == QDeclarativeView::Ready)
        q->sendReloaded();
}

void QDeclarativeViewObserverPrivate::_q_onCurrentObjectsChanged(QList<QObject*> objects)
{
    QList<QGraphicsItem*> items;
    QList<QGraphicsObject*> gfxObjects;
    foreach (QObject *obj, objects) {
        if (QDeclarativeItem *declarativeItem = qobject_cast<QDeclarativeItem*>(obj)) {
            items << declarativeItem;
            gfxObjects << declarativeItem;
        }
    }
    if (designModeBehavior) {
        setSelectedItemsForTools(items);
        clearHighlight();
        highlight(gfxObjects);
    }
}

// adjusts bounding boxes on edges of screen to be visible
QRectF QDeclarativeViewObserver::adjustToScreenBoundaries(const QRectF &boundingRectInSceneSpace)
{
    int marginFromEdge = 1;
    QRectF boundingRect(boundingRectInSceneSpace);
    if (qAbs(boundingRect.left()) - 1 < 2)
        boundingRect.setLeft(marginFromEdge);

    QRect rect = data->view->rect();

    if (boundingRect.right() >= rect.right())
        boundingRect.setRight(rect.right() - marginFromEdge);

    if (qAbs(boundingRect.top()) - 1 < 2)
        boundingRect.setTop(marginFromEdge);

    if (boundingRect.bottom() >= rect.bottom())
        boundingRect.setBottom(rect.bottom() - marginFromEdge);

    return boundingRect;
}

void QDeclarativeViewObserverPrivate::createToolBox()
{
    toolBox = new ToolBox(q->declarativeView());

    QmlToolBar *toolBar = toolBox->toolBar();

    QObject::connect(q, SIGNAL(selectedColorChanged(QColor)),
                     toolBar, SLOT(setColorBoxColor(QColor)));

    QObject::connect(q, SIGNAL(designModeBehaviorChanged(bool)),
                     toolBar, SLOT(setDesignModeBehavior(bool)));

    QObject::connect(toolBar, SIGNAL(designModeBehaviorChanged(bool)),
                     q, SLOT(setDesignModeBehavior(bool)));
    QObject::connect(toolBar, SIGNAL(animationSpeedChanged(qreal)), q, SLOT(setAnimationSpeed(qreal)));
    QObject::connect(toolBar, SIGNAL(animationPausedChanged(bool)), q, SLOT(setAnimationPaused(bool)));
    QObject::connect(toolBar, SIGNAL(colorPickerSelected()), this, SLOT(_q_changeToColorPickerTool()));
    QObject::connect(toolBar, SIGNAL(zoomToolSelected()), this, SLOT(_q_changeToZoomTool()));
    QObject::connect(toolBar, SIGNAL(selectToolSelected()), this, SLOT(_q_changeToSingleSelectTool()));
    QObject::connect(toolBar, SIGNAL(marqueeSelectToolSelected()),
                     this, SLOT(_q_changeToMarqueeSelectTool()));

    QObject::connect(toolBar, SIGNAL(applyChangesFromQmlFileSelected()),
                     this, SLOT(_q_applyChangesFromClient()));

    QObject::connect(q, SIGNAL(animationSpeedChanged(qreal)), toolBar, SLOT(setAnimationSpeed(qreal)));
    QObject::connect(q, SIGNAL(animationPausedChanged(bool)), toolBar, SLOT(setAnimationPaused(bool)));

    QObject::connect(q, SIGNAL(selectToolActivated()), toolBar, SLOT(activateSelectTool()));

    // disabled features
    //connect(d->m_toolBar, SIGNAL(applyChangesToQmlFileSelected()), SLOT(applyChangesToClient()));
    //connect(q, SIGNAL(resizeToolActivated()), d->m_toolBar, SLOT(activateSelectTool()));
    //connect(q, SIGNAL(moveToolActivated()),   d->m_toolBar, SLOT(activateSelectTool()));

    QObject::connect(q, SIGNAL(colorPickerActivated()), toolBar, SLOT(activateColorPicker()));
    QObject::connect(q, SIGNAL(zoomToolActivated()), toolBar, SLOT(activateZoom()));
    QObject::connect(q, SIGNAL(marqueeSelectToolActivated()),
                     toolBar, SLOT(activateMarqueeSelectTool()));
}

void QDeclarativeViewObserver::handleMessage(const QByteArray &message)
{
    QDataStream ds(message);

    ObserverProtocol::Message type;
    ds >> type;

    switch (type) {
    case ObserverProtocol::SetCurrentObjects: {
        int itemCount = 0;
        ds >> itemCount;

        QList<QObject*> selectedObjects;
        for (int i = 0; i < itemCount; ++i) {
            int debugId = -1;
            ds >> debugId;
            if (QObject *obj = QDeclarativeDebugService::objectForId(debugId))
                selectedObjects << obj;
        }

        data->_q_onCurrentObjectsChanged(selectedObjects);
        break;
    }
    case ObserverProtocol::Reload: {
        data->_q_reloadView();
        break;
    }
    case ObserverProtocol::SetAnimationSpeed: {
        qreal speed;
        ds >> speed;
        animationSpeedChangeRequested(speed);
        break;
    }
    case ObserverProtocol::SetAnimationPaused: {
        bool paused;
        ds >> paused;
        animationPausedChangeRequested(paused);
        break;
    }
    case ObserverProtocol::ChangeTool: {
        ObserverProtocol::Tool tool;
        ds >> tool;
        switch (tool) {
        case ObserverProtocol::ColorPickerTool:
            data->_q_changeToColorPickerTool();
            break;
        case ObserverProtocol::SelectTool:
            data->_q_changeToSingleSelectTool();
            break;
        case ObserverProtocol::SelectMarqueeTool:
            data->_q_changeToMarqueeSelectTool();
            break;
        case ObserverProtocol::ZoomTool:
            data->_q_changeToZoomTool();
            break;
        default:
            qWarning() << "Warning: Unhandled tool:" << tool;
        }
        break;
    }
    case ObserverProtocol::SetDesignMode: {
        bool inDesignMode;
        ds >> inDesignMode;
        setDesignModeBehavior(inDesignMode);
        break;
    }
    case ObserverProtocol::ShowAppOnTop: {
        bool showOnTop;
        ds >> showOnTop;
        setShowAppOnTop(showOnTop);
        break;
    }
    case ObserverProtocol::CreateObject: {
        QString qml;
        int parentId;
        QString filename;
        QStringList imports;
        ds >> qml >> parentId >> imports >> filename;
        data->_q_createQmlObject(qml, QDeclarativeDebugService::objectForId(parentId),
                                 imports, filename);
        break;
    }
    case ObserverProtocol::DestroyObject: {
        int debugId;
        ds >> debugId;
        if (QObject* obj = QDeclarativeDebugService::objectForId(debugId))
            obj->deleteLater();
        break;
    }
    case ObserverProtocol::MoveObject: {
        int debugId, newParent;
        ds >> debugId >> newParent;
        data->_q_reparentQmlObject(QDeclarativeDebugService::objectForId(debugId),
                                   QDeclarativeDebugService::objectForId(newParent));
        break;
    }
    case ObserverProtocol::ObjectIdList: {
        int itemCount;
        ds >> itemCount;
        data->stringIdForObjectId.clear();
        for (int i = 0; i < itemCount; ++i) {
            int itemDebugId;
            QString itemIdString;
            ds >> itemDebugId
               >> itemIdString;

            data->stringIdForObjectId.insert(itemDebugId, itemIdString);
        }
        break;
    }
    case ObserverProtocol::ClearComponentCache: {
        data->_q_clearComponentCache();
        break;
    }
    default:
        qWarning() << "Warning: Not handling message:" << type;
    }
}

void QDeclarativeViewObserver::sendDesignModeBehavior(bool inDesignMode)
{
    QByteArray message;
    QDataStream ds(&message, QIODevice::WriteOnly);

    ds << ObserverProtocol::SetDesignMode
       << inDesignMode;

    data->debugService->sendMessage(message);
}

void QDeclarativeViewObserver::sendCurrentObjects(const QList<QObject*> &objects)
{
    QByteArray message;
    QDataStream ds(&message, QIODevice::WriteOnly);

    ds << ObserverProtocol::CurrentObjectsChanged
       << objects.length();

    foreach (QObject *object, objects) {
        int id = QDeclarativeDebugService::idForObject(object);
        ds << id;
    }

    data->debugService->sendMessage(message);
}

void QDeclarativeViewObserver::sendCurrentTool(Constants::DesignTool toolId)
{
    QByteArray message;
    QDataStream ds(&message, QIODevice::WriteOnly);

    ds << ObserverProtocol::ToolChanged
       << toolId;

    data->debugService->sendMessage(message);
}

void QDeclarativeViewObserver::sendAnimationSpeed(qreal slowDownFactor)
{
    QByteArray message;
    QDataStream ds(&message, QIODevice::WriteOnly);

    ds << ObserverProtocol::AnimationSpeedChanged
       << slowDownFactor;

    data->debugService->sendMessage(message);
}

void QDeclarativeViewObserver::sendAnimationPaused(bool paused)
{
    QByteArray message;
    QDataStream ds(&message, QIODevice::WriteOnly);

    ds << ObserverProtocol::AnimationPausedChanged
       << paused;

    data->debugService->sendMessage(message);
}

void QDeclarativeViewObserver::sendReloaded()
{
    QByteArray message;
    QDataStream ds(&message, QIODevice::WriteOnly);

    ds << ObserverProtocol::Reloaded;

    data->debugService->sendMessage(message);
}

void QDeclarativeViewObserver::sendShowAppOnTop(bool showAppOnTop)
{
    QByteArray message;
    QDataStream ds(&message, QIODevice::WriteOnly);

    ds << ObserverProtocol::ShowAppOnTop << showAppOnTop;

    data->debugService->sendMessage(message);
}

void QDeclarativeViewObserver::sendColorChanged(const QColor &color)
{
    QByteArray message;
    QDataStream ds(&message, QIODevice::WriteOnly);

    ds << ObserverProtocol::ColorChanged
       << color;

    data->debugService->sendMessage(message);
}

QString QDeclarativeViewObserver::idStringForObject(QObject *obj) const
{
    int id = QDeclarativeDebugService::idForObject(obj);
    QString idString = data->stringIdForObjectId.value(id, QString());
    return idString;
}

QT_END_NAMESPACE

#include "qdeclarativeviewobserver.moc"
