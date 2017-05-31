/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of the QtQuick module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 3 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL3 included in the
** packaging of this file. Please review the following information to
** ensure the GNU Lesser General Public License version 3 requirements
** will be met: https://www.gnu.org/licenses/lgpl-3.0.html.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 2.0 or (at your option) the GNU General
** Public license version 3 or any later version approved by the KDE Free
** Qt Foundation. The licenses are as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL2 and LICENSE.GPL3
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-2.0.html and
** https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "qquickshape_p.h"
#include "qquickshape_p_p.h"
#include "qquickshapegenericrenderer_p.h"
#include "qquickshapenvprrenderer_p.h"
#include "qquickshapesoftwarerenderer_p.h"
#include <private/qsgtexture_p.h>
#include <private/qquicksvgparser_p.h>
#include <QtGui/private/qdrawhelper_p.h>
#include <QOpenGLFunctions>

#include <private/qv4engine_p.h>
#include <private/qv4object_p.h>
#include <private/qv4qobjectwrapper_p.h>
#include <private/qv4mm_p.h>
#include <private/qqmlengine_p.h>

QT_BEGIN_NAMESPACE

/*!
    \qmlmodule QtQuick.Shapes 1.0
    \title Qt Quick Shapes QML Types
    \ingroup qmlmodules
    \brief Provides QML types for drawing stroked and filled shapes

    To use the types in this module, import the module with the following line:

    \code
    import QtQuick.Shapes 1.0
    \endcode
*/

QQuickShapeStrokeFillParams::QQuickShapeStrokeFillParams()
    : strokeColor(Qt::white),
      strokeWidth(1),
      fillColor(Qt::white),
      fillRule(QQuickShapePath::OddEvenFill),
      joinStyle(QQuickShapePath::BevelJoin),
      miterLimit(2),
      capStyle(QQuickShapePath::SquareCap),
      strokeStyle(QQuickShapePath::SolidLine),
      dashOffset(0),
      fillGradient(nullptr)
{
    dashPattern << 4 << 2; // 4 * strokeWidth dash followed by 2 * strokeWidth space
}

QPainterPath QQuickShapePathCommands::toPainterPath() const
{
    QPainterPath p;
    int coordIdx = 0;
    for (int i = 0; i < cmd.count(); ++i) {
        switch (cmd[i]) {
        case QQuickShapePathCommands::MoveTo:
            p.moveTo(coords[coordIdx], coords[coordIdx + 1]);
            coordIdx += 2;
            break;
        case QQuickShapePathCommands::LineTo:
            p.lineTo(coords[coordIdx], coords[coordIdx + 1]);
            coordIdx += 2;
            break;
        case QQuickShapePathCommands::QuadTo:
            p.quadTo(coords[coordIdx], coords[coordIdx + 1],
                    coords[coordIdx + 2], coords[coordIdx + 3]);
            coordIdx += 4;
            break;
        case QQuickShapePathCommands::CubicTo:
            p.cubicTo(coords[coordIdx], coords[coordIdx + 1],
                    coords[coordIdx + 2], coords[coordIdx + 3],
                    coords[coordIdx + 4], coords[coordIdx + 5]);
            coordIdx += 6;
            break;
        case QQuickShapePathCommands::ArcTo:
            // does not map to the QPainterPath API; reuse the helper code from QQuickSvgParser
            QQuickSvgParser::pathArc(p,
                                     coords[coordIdx], coords[coordIdx + 1], // radius
                                     coords[coordIdx + 2], // xAxisRotation
                                     !qFuzzyIsNull(coords[coordIdx + 6]), // useLargeArc
                                     !qFuzzyIsNull(coords[coordIdx + 5]), // sweep flag
                                     coords[coordIdx + 3], coords[coordIdx + 4], // end
                                     p.currentPosition().x(), p.currentPosition().y());
            coordIdx += 7;
            break;
        default:
            qWarning("Unknown JS path command: %d", cmd[i]);
            break;
        }
    }
    return p;
}

/*!
    \qmltype ShapePath
    \instantiates QQuickShapePath
    \inqmlmodule QtQuick.Shapes
    \ingroup qtquick-paths
    \ingroup qtquick-views
    \inherits Object
    \brief Describes a Path and associated properties for stroking and filling
    \since 5.10

    A Shape contains one or more ShapePath elements. At least one
    ShapePath is necessary in order to have a Shape output anything
    visible. A ShapeItem in turn contains a Path and properties describing the
    stroking and filling parameters, such as the stroke width and color, the
    fill color or gradient, join and cap styles, and so on. Finally, the Path
    object contains a list of path elements like PathMove, PathLine, PathCubic,
    PathQuad, PathArc.

    Any property changes in these data sets will be bubble up and change the
    output of the Shape. This means that it is simple and easy to change, or
    even animate, the starting and ending position, control points, or any
    stroke or fill parameters using the usual QML bindings and animation types
    like NumberAnimation.

    In the following example the line join style changes automatically based on
    the value of joinStyleIndex:

    \code
    ShapePath {
        strokeColor: "black"
        strokeWidth: 16
        fillColor: "transparent"
        capStyle: ShapePath.RoundCap

        property int joinStyleIndex: 0
        property variant styles: [ ShapePath.BevelJoin, ShapePath.MiterJoin, ShapePath.RoundJoin ]

        joinStyle: styles[joinStyleIndex]

        Path {
            startX: 30
            startY: 30
            PathLine { x: 100; y: 100 }
            PathLine { x: 30; y: 100 }
        }
    }
    \endcode

    Once associated with a Shape, here is the output with a joinStyleIndex
    of 2 (ShapePath.RoundJoin):

    \image visualpath-code-example.png
 */

QQuickShapePathPrivate::QQuickShapePathPrivate()
    : path(nullptr),
      dirty(DirtyAll)
{
}

QQuickShapePath::QQuickShapePath(QObject *parent)
    : QObject(*(new QQuickShapePathPrivate), parent)
{
}

QQuickShapePath::~QQuickShapePath()
{
}

/*!
    \qmlproperty Path QtQuick.Shapes::ShapePath::path

    This property holds the Path object.

    \default
 */

QQuickPath *QQuickShapePath::path() const
{
    Q_D(const QQuickShapePath);
    return d->path;
}

void QQuickShapePath::setPath(QQuickPath *path)
{
    Q_D(QQuickShapePath);
    if (d->path == path)
        return;

    if (d->path)
        qmlobject_disconnect(d->path, QQuickPath, SIGNAL(changed()),
                             this, QQuickShapePath, SLOT(_q_pathChanged()));
    d->path = path;
    qmlobject_connect(d->path, QQuickPath, SIGNAL(changed()),
                      this, QQuickShapePath, SLOT(_q_pathChanged()));

    d->dirty |= QQuickShapePathPrivate::DirtyPath;
    emit pathChanged();
    emit changed();
}

void QQuickShapePathPrivate::_q_pathChanged()
{
    Q_Q(QQuickShapePath);
    dirty |= DirtyPath;
    emit q->changed();
}

/*!
    \qmlproperty color QtQuick.Shapes::ShapePath::strokeColor

    This property holds the stroking color.

    When set to \c transparent, no stroking occurs.

    The default value is \c white.
 */

QColor QQuickShapePath::strokeColor() const
{
    Q_D(const QQuickShapePath);
    return d->sfp.strokeColor;
}

void QQuickShapePath::setStrokeColor(const QColor &color)
{
    Q_D(QQuickShapePath);
    if (d->sfp.strokeColor != color) {
        d->sfp.strokeColor = color;
        d->dirty |= QQuickShapePathPrivate::DirtyStrokeColor;
        emit strokeColorChanged();
        emit changed();
    }
}

/*!
    \qmlproperty color QtQuick.Shapes::ShapePath::strokeWidth

    This property holds the stroke width.

    When set to a negative value, no stroking occurs.

    The default value is 1.
 */

qreal QQuickShapePath::strokeWidth() const
{
    Q_D(const QQuickShapePath);
    return d->sfp.strokeWidth;
}

void QQuickShapePath::setStrokeWidth(qreal w)
{
    Q_D(QQuickShapePath);
    if (d->sfp.strokeWidth != w) {
        d->sfp.strokeWidth = w;
        d->dirty |= QQuickShapePathPrivate::DirtyStrokeWidth;
        emit strokeWidthChanged();
        emit changed();
    }
}

/*!
    \qmlproperty color QtQuick.Shapes::ShapePath::fillColor

    This property holds the fill color.

    When set to \c transparent, no filling occurs.

    The default value is \c white.
 */

QColor QQuickShapePath::fillColor() const
{
    Q_D(const QQuickShapePath);
    return d->sfp.fillColor;
}

void QQuickShapePath::setFillColor(const QColor &color)
{
    Q_D(QQuickShapePath);
    if (d->sfp.fillColor != color) {
        d->sfp.fillColor = color;
        d->dirty |= QQuickShapePathPrivate::DirtyFillColor;
        emit fillColorChanged();
        emit changed();
    }
}

/*!
    \qmlproperty enumeration QtQuick.Shapes::ShapePath::fillRule

    This property holds the fill rule. The default value is
    ShapePath.OddEvenFill. For an example on fill rules, see
    QPainterPath::setFillRule().

    \list
    \li ShapePath.OddEvenFill
    \li ShapePath.WindingFill
    \endlist
 */

QQuickShapePath::FillRule QQuickShapePath::fillRule() const
{
    Q_D(const QQuickShapePath);
    return d->sfp.fillRule;
}

void QQuickShapePath::setFillRule(FillRule fillRule)
{
    Q_D(QQuickShapePath);
    if (d->sfp.fillRule != fillRule) {
        d->sfp.fillRule = fillRule;
        d->dirty |= QQuickShapePathPrivate::DirtyFillRule;
        emit fillRuleChanged();
        emit changed();
    }
}

/*!
    \qmlproperty enumeration QtQuick.Shapes::ShapePath::joinStyle

    This property defines how joins between two connected lines are drawn. The
    default value is ShapePath.BevelJoin.

    \list
    \li ShapePath.MiterJoin - The outer edges of the lines are extended to meet at an angle, and this area is filled.
    \li ShapePath.BevelJoin - The triangular notch between the two lines is filled.
    \li ShapePath.RoundJoin - A circular arc between the two lines is filled.
    \endlist
 */

QQuickShapePath::JoinStyle QQuickShapePath::joinStyle() const
{
    Q_D(const QQuickShapePath);
    return d->sfp.joinStyle;
}

void QQuickShapePath::setJoinStyle(JoinStyle style)
{
    Q_D(QQuickShapePath);
    if (d->sfp.joinStyle != style) {
        d->sfp.joinStyle = style;
        d->dirty |= QQuickShapePathPrivate::DirtyStyle;
        emit joinStyleChanged();
        emit changed();
    }
}

/*!
    \qmlproperty int QtQuick.Shapes::ShapePath::miterLimit

    When ShapePath.joinStyle is set to ShapePath.MiterJoin, this property
    specifies how far the miter join can extend from the join point.

    The default value is 2.
 */

int QQuickShapePath::miterLimit() const
{
    Q_D(const QQuickShapePath);
    return d->sfp.miterLimit;
}

void QQuickShapePath::setMiterLimit(int limit)
{
    Q_D(QQuickShapePath);
    if (d->sfp.miterLimit != limit) {
        d->sfp.miterLimit = limit;
        d->dirty |= QQuickShapePathPrivate::DirtyStyle;
        emit miterLimitChanged();
        emit changed();
    }
}

/*!
    \qmlproperty enumeration QtQuick.Shapes::ShapePath::capStyle

    This property defines how the end points of lines are drawn. The
    default value is ShapePath.SquareCap.

    \list
    \li ShapePath.FlatCap - A square line end that does not cover the end point of the line.
    \li ShapePath.SquareCap - A square line end that covers the end point and extends beyond it by half the line width.
    \li ShapePath.RoundCap - A rounded line end.
    \endlist
 */

QQuickShapePath::CapStyle QQuickShapePath::capStyle() const
{
    Q_D(const QQuickShapePath);
    return d->sfp.capStyle;
}

void QQuickShapePath::setCapStyle(CapStyle style)
{
    Q_D(QQuickShapePath);
    if (d->sfp.capStyle != style) {
        d->sfp.capStyle = style;
        d->dirty |= QQuickShapePathPrivate::DirtyStyle;
        emit capStyleChanged();
        emit changed();
    }
}

/*!
    \qmlproperty enumeration QtQuick.Shapes::ShapePath::strokeStyle

    This property defines the style of stroking. The default value is
    ShapePath.SolidLine.

    \list
    \li ShapePath.SolidLine - A plain line.
    \li ShapePath.DashLine - Dashes separated by a few pixels.
    \endlist
 */

QQuickShapePath::StrokeStyle QQuickShapePath::strokeStyle() const
{
    Q_D(const QQuickShapePath);
    return d->sfp.strokeStyle;
}

void QQuickShapePath::setStrokeStyle(StrokeStyle style)
{
    Q_D(QQuickShapePath);
    if (d->sfp.strokeStyle != style) {
        d->sfp.strokeStyle = style;
        d->dirty |= QQuickShapePathPrivate::DirtyDash;
        emit strokeStyleChanged();
        emit changed();
    }
}

/*!
    \qmlproperty real QtQuick.Shapes::ShapePath::dashOffset

    This property defines the starting point on the dash pattern, measured in
    units used to specify the dash pattern.

    The default value is 0.

    \sa QPen::setDashOffset()
 */

qreal QQuickShapePath::dashOffset() const
{
    Q_D(const QQuickShapePath);
    return d->sfp.dashOffset;
}

void QQuickShapePath::setDashOffset(qreal offset)
{
    Q_D(QQuickShapePath);
    if (d->sfp.dashOffset != offset) {
        d->sfp.dashOffset = offset;
        d->dirty |= QQuickShapePathPrivate::DirtyDash;
        emit dashOffsetChanged();
        emit changed();
    }
}

/*!
    \qmlproperty list<real> QtQuick.Shapes::ShapePath::dashPattern

    This property defines the dash pattern when ShapePath.strokeStyle is set
    to ShapePath.DashLine. The pattern must be specified as an even number of
    positive entries where the entries 1, 3, 5... are the dashes and 2, 4, 6...
    are the spaces. The pattern is specified in units of the pen's width.

    The default value is (4, 2), meaning a dash of 4 * ShapePath.strokeWidth
    pixels followed by a space of 2 * ShapePath.strokeWidth pixels.

    \sa QPen::setDashPattern()
 */

QVector<qreal> QQuickShapePath::dashPattern() const
{
    Q_D(const QQuickShapePath);
    return d->sfp.dashPattern;
}

void QQuickShapePath::setDashPattern(const QVector<qreal> &array)
{
    Q_D(QQuickShapePath);
    if (d->sfp.dashPattern != array) {
        d->sfp.dashPattern = array;
        d->dirty |= QQuickShapePathPrivate::DirtyDash;
        emit dashPatternChanged();
        emit changed();
    }
}

/*!
    \qmlproperty ShapeGradient QtQuick.Shapes::ShapePath::fillGradient

    This property defines the fill gradient. By default no gradient is enabled
    and the value is \c null. In this case the fill uses a solid color based on
    the value of ShapePath.fillColor.

    When set, ShapePath.fillColor is ignored and filling is done using one of
    the ShapeGradient subtypes.
 */

QQuickShapeGradient *QQuickShapePath::fillGradient() const
{
    Q_D(const QQuickShapePath);
    return d->sfp.fillGradient;
}

void QQuickShapePath::setFillGradient(QQuickShapeGradient *gradient)
{
    Q_D(QQuickShapePath);
    if (d->sfp.fillGradient != gradient) {
        if (d->sfp.fillGradient)
            qmlobject_disconnect(d->sfp.fillGradient, QQuickShapeGradient, SIGNAL(updated()),
                                 this, QQuickShapePath, SLOT(_q_fillGradientChanged()));
        d->sfp.fillGradient = gradient;
        if (d->sfp.fillGradient)
            qmlobject_connect(d->sfp.fillGradient, QQuickShapeGradient, SIGNAL(updated()),
                              this, QQuickShapePath, SLOT(_q_fillGradientChanged()));
        d->dirty |= QQuickShapePathPrivate::DirtyFillGradient;
        emit changed();
    }
}

void QQuickShapePathPrivate::_q_fillGradientChanged()
{
    Q_Q(QQuickShapePath);
    dirty |= DirtyFillGradient;
    emit q->changed();
}

void QQuickShapePath::resetFillGradient()
{
    setFillGradient(nullptr);
}

/*!
    \qmltype Shape
    \instantiates QQuickShape
    \inqmlmodule QtQuick.Shapes
    \ingroup qtquick-paths
    \ingroup qtquick-views
    \inherits Item
    \brief Renders a path
    \since 5.10

    Renders a path either by generating geometry via QPainterPath and manual
    triangulation or by using a GPU vendor extension like \c{GL_NV_path_rendering}.

    This approach is different from rendering shapes via QQuickPaintedItem or
    the 2D Canvas because the path never gets rasterized in software. Therefore
    Shape is suitable for creating shapes spreading over larger areas of the
    screen, avoiding the performance penalty for texture uploads or framebuffer
    blits. In addition, the declarative API allows manipulating, binding to,
    and even animating the path element properties like starting and ending
    position, the control points, etc.

    The types for specifying path elements are shared between \l PathView and
    Shape. However, not all Shape implementations support all path
    element types, while some may not make sense for PathView. Shape's
    currently supported subset is: PathMove, PathLine, PathQuad, PathCubic,
    PathArc, PathSvg.

    See \l Path for a detailed overview of the supported path elements.

    \code
    Shape {
        width: 200
        height: 150
        anchors.centerIn: parent
        ShapePath {
            strokeWidth: 4
            strokeColor: "red"
            fillGradient: ShapeLinearGradient {
                x1: 20; y1: 20
                x2: 180; y2: 130
                ShapeGradientStop { position: 0; color: "blue" }
                ShapeGradientStop { position: 0.2; color: "green" }
                ShapeGradientStop { position: 0.4; color: "red" }
                ShapeGradientStop { position: 0.6; color: "yellow" }
                ShapeGradientStop { position: 1; color: "cyan" }
            }
            strokeStyle: ShapePath.DashLine
            dashPattern: [ 1, 4 ]
            Path {
                startX: 20; startY: 20
                PathLine { x: 180; y: 130 }
                PathLine { x: 20; y: 130 }
                PathLine { x: 20; y: 20 }
            }
        }
    }
    \endcode

    \image pathitem-code-example.png

    \note It is important to be aware of performance implications, in particular
    when the application is running on the generic Shape implementation due to
    not having support for accelerated path rendering.  The geometry generation
    happens entirely on the CPU in this case, and this is potentially
    expensive. Changing the set of path elements, changing the properties of
    these elements, or changing certain properties of the Shape itself all lead
    to retriangulation of the affected elements on every change. Therefore,
    applying animation to such properties can affect performance on less
    powerful systems. If animating properties other than stroke and fill colors
    is a must, it is recommended to target systems providing
    \c{GL_NV_path_rendering} where the cost of path property changes is much
    smaller.

    \note However, the data-driven, declarative nature of the Shape API often
    means better cacheability for the underlying CPU and GPU resources. A
    property change in one ShapePath will only lead to reprocessing the affected
    ShapePath, leaving other parts of the Shape unchanged. Therefore, a heavily
    changing (for example, animating) property can often result in a lower
    overall system load than with imperative painting approaches (for example,
    QPainter).

    The following list summarizes the available Shape rendering approaches:

    \list

    \li When running with the default, OpenGL backend of Qt Quick, both the
    generic, triangulation-based and the NVIDIA-specific
    \c{GL_NV_path_rendering} methods are available. The choice is made at
    runtime, depending on the graphics driver's capabilities. When this is not
    desired, applications can force using the generic method by setting the
    Shape.enableVendorExtensions property to \c false.

    \li The \c software backend is fully supported. The path is rendered via
    QPainter::strokePath() and QPainter::fillPath() in this case.

    \li The Direct 3D 12 backend is not currently supported.

    \li The OpenVG backend is not currently supported.

    \endlist

    \sa Path, PathMove, PathLine, PathQuad, PathCubic, PathArc, PathSvg
*/

QQuickShapePrivate::QQuickShapePrivate()
    : componentComplete(true),
      spChanged(false),
      rendererType(QQuickShape::UnknownRenderer),
      async(false),
      status(QQuickShape::Null),
      renderer(nullptr),
      enableVendorExts(true)
{
}

QQuickShapePrivate::~QQuickShapePrivate()
{
    delete renderer;
}

void QQuickShapePrivate::_q_shapePathChanged()
{
    Q_Q(QQuickShape);
    spChanged = true;
    q->polish();
}

void QQuickShapePrivate::setStatus(QQuickShape::Status newStatus)
{
    Q_Q(QQuickShape);
    if (status != newStatus) {
        status = newStatus;
        emit q->statusChanged();
    }
}

QQuickShape::QQuickShape(QQuickItem *parent)
  : QQuickItem(*(new QQuickShapePrivate), parent)
{
    setFlag(ItemHasContents);
}

QQuickShape::~QQuickShape()
{
}

/*!
    \qmlproperty enumeration QtQuick.Shapes::Shape::rendererType

    This property determines which path rendering backend is active.

    \list

    \li Shape.UnknownRenderer - The renderer is unknown.

    \li Shape.GeometryRenderer - The generic, driver independent solution
    for OpenGL. Uses the same CPU-based triangulation approach as QPainter's
    OpenGL 2 paint engine. This is the default on non-NVIDIA hardware when the
    default, OpenGL Qt Quick scenegraph backend is in use.

    \li Shape.NvprRenderer - Path items are rendered by performing OpenGL
    calls using the \c{GL_NV_path_rendering} extension. This is the default on
    NVIDIA hardware when the default, OpenGL Qt Quick scenegraph backend is in
    use.

    \li Shape.SoftwareRenderer - Pure QPainter drawing using the raster
    paint engine. This is the default, and only, option when the Qt Quick
    scenegraph is running with the \c software backend.

    \endlist
*/

QQuickShape::RendererType QQuickShape::rendererType() const
{
    Q_D(const QQuickShape);
    return d->rendererType;
}

/*!
    \qmlproperty bool QtQuick.Shapes::Shape::asynchronous

    When Shape.rendererType is Shape.GeometryRenderer, the input path is
    triangulated on the CPU during the polishing phase of the Shape. This is
    potentially expensive. To offload this work to separate worker threads, set
    this property to \c true.

    When enabled, making a Shape visible will not wait for the content to
    become available. Instead, the gui/main thread is not blocked and the
    results of the path rendering are shown only when all the asynchronous work
    has been finished.

    The default value is \c false.
 */

bool QQuickShape::asynchronous() const
{
    Q_D(const QQuickShape);
    return d->async;
}

void QQuickShape::setAsynchronous(bool async)
{
    Q_D(QQuickShape);
    if (d->async != async) {
        d->async = async;
        emit asynchronousChanged();
        if (d->componentComplete)
            d->_q_shapePathChanged();
    }
}

/*!
    \qmlproperty bool QtQuick.Shapes::Shape::enableVendorExtensions

    This property controls the usage of non-standard OpenGL extensions like
    GL_NV_path_rendering. To disable Shape.NvprRenderer and force a uniform
    behavior regardless of the graphics card and drivers, set this property to
    \c false.

    The default value is \c true.
 */

bool QQuickShape::enableVendorExtensions() const
{
    Q_D(const QQuickShape);
    return d->enableVendorExts;
}

void QQuickShape::setEnableVendorExtensions(bool enable)
{
    Q_D(QQuickShape);
    if (d->enableVendorExts != enable) {
        d->enableVendorExts = enable;
        emit enableVendorExtensionsChanged();
    }
}

/*!
    \qmlproperty enumeration QtQuick.Shapes::Shape::status

    This property determines the status of the Shape and is relevant when
    Shape.asynchronous is set to \c true.

    \list

    \li Shape.Null - Not yet initialized.

    \li Shape.Ready - The Shape has finished processing.

    \li Shape.Processing - The path is being processed.

    \endlist
 */

QQuickShape::Status QQuickShape::status() const
{
    Q_D(const QQuickShape);
    return d->status;
}

static QQuickShapePath *vpe_at(QQmlListProperty<QQuickShapePath> *property, int index)
{
    QQuickShapePrivate *d = QQuickShapePrivate::get(static_cast<QQuickShape *>(property->object));
    return d->qmlData.sp.at(index);
}

static void vpe_append(QQmlListProperty<QQuickShapePath> *property, QQuickShapePath *obj)
{
    QQuickShape *item = static_cast<QQuickShape *>(property->object);
    QQuickShapePrivate *d = QQuickShapePrivate::get(item);
    d->qmlData.sp.append(obj);

    if (d->componentComplete) {
        QObject::connect(obj, SIGNAL(changed()), item, SLOT(_q_shapePathChanged()));
        d->_q_shapePathChanged();
    }
}

static int vpe_count(QQmlListProperty<QQuickShapePath> *property)
{
    QQuickShapePrivate *d = QQuickShapePrivate::get(static_cast<QQuickShape *>(property->object));
    return d->qmlData.sp.count();
}

static void vpe_clear(QQmlListProperty<QQuickShapePath> *property)
{
    QQuickShape *item = static_cast<QQuickShape *>(property->object);
    QQuickShapePrivate *d = QQuickShapePrivate::get(item);

    for (QQuickShapePath *p : d->qmlData.sp)
        QObject::disconnect(p, SIGNAL(changed()), item, SLOT(_q_shapePathChanged()));

    d->qmlData.sp.clear();

    if (d->componentComplete)
        d->_q_shapePathChanged();
}

/*!
    \qmlproperty list<ShapePath> QtQuick.Shapes::Shape::elements

    This property holds the ShapePath objects that define the contents of the
    Shape.

    \default
 */

QQmlListProperty<QQuickShapePath> QQuickShape::elements()
{
    return QQmlListProperty<QQuickShapePath>(this,
                                              nullptr,
                                              vpe_append,
                                              vpe_count,
                                              vpe_at,
                                              vpe_clear);
}

void QQuickShape::classBegin()
{
    Q_D(QQuickShape);
    d->componentComplete = false;
}

void QQuickShape::componentComplete()
{
    Q_D(QQuickShape);
    d->componentComplete = true;

    for (QQuickShapePath *p : d->qmlData.sp)
        connect(p, SIGNAL(changed()), this, SLOT(_q_shapePathChanged()));

    d->_q_shapePathChanged();
}

void QQuickShape::updatePolish()
{
    Q_D(QQuickShape);

    if (!d->spChanged)
        return;

    d->spChanged = false;

    if (!d->renderer) {
        d->createRenderer();
        if (!d->renderer)
            return;
        emit rendererChanged();
    }

    // endSync() is where expensive calculations may happen (or get kicked off
    // on worker threads), depending on the backend. Therefore do this only
    // when the item is visible.
    if (isVisible())
        d->sync();

    update();
}

void QQuickShape::itemChange(ItemChange change, const ItemChangeData &data)
{
    Q_D(QQuickShape);

    // sync may have been deferred; do it now if the item became visible
    if (change == ItemVisibleHasChanged && data.boolValue)
        d->_q_shapePathChanged();

    QQuickItem::itemChange(change, data);
}

QSGNode *QQuickShape::updatePaintNode(QSGNode *node, UpdatePaintNodeData *)
{
    // Called on the render thread, with the gui thread blocked. We can now
    // safely access gui thread data.

    Q_D(QQuickShape);
    if (d->renderer) {
        if (!node)
            node = d->createNode();
        d->renderer->updateNode();
    }
    return node;
}

// the renderer object lives on the gui thread
void QQuickShapePrivate::createRenderer()
{
    Q_Q(QQuickShape);
    QSGRendererInterface *ri = q->window()->rendererInterface();
    if (!ri)
        return;

    switch (ri->graphicsApi()) {
#ifndef QT_NO_OPENGL
    case QSGRendererInterface::OpenGL:
        if (enableVendorExts && QQuickShapeNvprRenderNode::isSupported()) {
            rendererType = QQuickShape::NvprRenderer;
            renderer = new QQuickShapeNvprRenderer;
        } else {
            rendererType = QQuickShape::GeometryRenderer;
            renderer = new QQuickShapeGenericRenderer(q);
        }
        break;
#endif
    case QSGRendererInterface::Software:
        rendererType = QQuickShape::SoftwareRenderer;
        renderer = new QQuickShapeSoftwareRenderer;
        break;
    default:
        qWarning("No path backend for this graphics API yet");
        break;
    }
}

// the node lives on the render thread
QSGNode *QQuickShapePrivate::createNode()
{
    Q_Q(QQuickShape);
    QSGNode *node = nullptr;
    if (!q->window())
        return node;
    QSGRendererInterface *ri = q->window()->rendererInterface();
    if (!ri)
        return node;

    switch (ri->graphicsApi()) {
#ifndef QT_NO_OPENGL
    case QSGRendererInterface::OpenGL:
        if (enableVendorExts && QQuickShapeNvprRenderNode::isSupported()) {
            node = new QQuickShapeNvprRenderNode;
            static_cast<QQuickShapeNvprRenderer *>(renderer)->setNode(
                static_cast<QQuickShapeNvprRenderNode *>(node));
        } else {
            node = new QQuickShapeGenericNode;
            static_cast<QQuickShapeGenericRenderer *>(renderer)->setRootNode(
                static_cast<QQuickShapeGenericNode *>(node));
        }
        break;
#endif
    case QSGRendererInterface::Software:
        node = new QQuickShapeSoftwareRenderNode(q);
        static_cast<QQuickShapeSoftwareRenderer *>(renderer)->setNode(
                    static_cast<QQuickShapeSoftwareRenderNode *>(node));
        break;
    default:
        qWarning("No path backend for this graphics API yet");
        break;
    }

    return node;
}

static void q_asyncShapeReady(void *data)
{
    QQuickShapePrivate *self = static_cast<QQuickShapePrivate *>(data);
    self->setStatus(QQuickShape::Ready);
}

void QQuickShapePrivate::sync()
{
    const bool useAsync = async && renderer->flags().testFlag(QQuickAbstractPathRenderer::SupportsAsync);
    if (useAsync) {
        setStatus(QQuickShape::Processing);
        renderer->setAsyncCallback(q_asyncShapeReady, this);
    }

    if (!jsData.isValid()) {
        // Standard route: The path and stroke/fill parameters are provided via
        // QML elements.
        const int count = qmlData.sp.count();
        renderer->beginSync(count);

        for (int i = 0; i < count; ++i) {
            QQuickShapePath *p = qmlData.sp[i];
            int &dirty(QQuickShapePathPrivate::get(p)->dirty);

            if (dirty & QQuickShapePathPrivate::DirtyPath)
                renderer->setPath(i, p->path());
            if (dirty & QQuickShapePathPrivate::DirtyStrokeColor)
                renderer->setStrokeColor(i, p->strokeColor());
            if (dirty & QQuickShapePathPrivate::DirtyStrokeWidth)
                renderer->setStrokeWidth(i, p->strokeWidth());
            if (dirty & QQuickShapePathPrivate::DirtyFillColor)
                renderer->setFillColor(i, p->fillColor());
            if (dirty & QQuickShapePathPrivate::DirtyFillRule)
                renderer->setFillRule(i, p->fillRule());
            if (dirty & QQuickShapePathPrivate::DirtyStyle) {
                renderer->setJoinStyle(i, p->joinStyle(), p->miterLimit());
                renderer->setCapStyle(i, p->capStyle());
            }
            if (dirty & QQuickShapePathPrivate::DirtyDash)
                renderer->setStrokeStyle(i, p->strokeStyle(), p->dashOffset(), p->dashPattern());
            if (dirty & QQuickShapePathPrivate::DirtyFillGradient)
                renderer->setFillGradient(i, p->fillGradient());

            dirty = 0;
        }

        renderer->endSync(useAsync);
    } else {

        // ### there is no public API to reach this code path atm
        Q_UNREACHABLE();

        // Path and stroke/fill params provided from JavaScript. This avoids
        // QObjects at the expense of not supporting changes afterwards.
        const int count = jsData.paths.count();
        renderer->beginSync(count);

        for (int i = 0; i < count; ++i) {
            renderer->setJSPath(i, jsData.paths[i]);
            const QQuickShapeStrokeFillParams sfp(jsData.sfp[i]);
            renderer->setStrokeColor(i, sfp.strokeColor);
            renderer->setStrokeWidth(i, sfp.strokeWidth);
            renderer->setFillColor(i, sfp.fillColor);
            renderer->setFillRule(i, sfp.fillRule);
            renderer->setJoinStyle(i, sfp.joinStyle, sfp.miterLimit);
            renderer->setCapStyle(i, sfp.capStyle);
            renderer->setStrokeStyle(i, sfp.strokeStyle, sfp.dashOffset, sfp.dashPattern);
            renderer->setFillGradient(i, sfp.fillGradient);
        }

        renderer->endSync(useAsync);
    }

    if (!useAsync)
        setStatus(QQuickShape::Ready);
}

// ***** gradient support *****

/*!
    \qmltype ShapeGradientStop
    \instantiates QQuickShapeGradientStop
    \inqmlmodule QtQuick.Shapes
    \ingroup qtquick-paths
    \ingroup qtquick-views
    \inherits Object
    \brief Defines a color at a position in a gradient
    \since 5.10
 */

QQuickShapeGradientStop::QQuickShapeGradientStop(QObject *parent)
    : QObject(parent),
      m_position(0),
      m_color(Qt::black)
{
}

/*!
    \qmlproperty real QtQuick.Shapes::ShapeGradientStop::position

    The position and color properties describe the color used at a given
    position in a gradient, as represented by a gradient stop.

    The default value is 0.
 */

qreal QQuickShapeGradientStop::position() const
{
    return m_position;
}

void QQuickShapeGradientStop::setPosition(qreal position)
{
    if (m_position != position) {
        m_position = position;
        if (QQuickShapeGradient *grad = qobject_cast<QQuickShapeGradient *>(parent()))
            emit grad->updated();
    }
}

/*!
    \qmlproperty real QtQuick.Shapes::ShapeGradientStop::color

    The position and color properties describe the color used at a given
    position in a gradient, as represented by a gradient stop.

    The default value is \c black.
 */

QColor QQuickShapeGradientStop::color() const
{
    return m_color;
}

void QQuickShapeGradientStop::setColor(const QColor &color)
{
    if (m_color != color) {
        m_color = color;
        if (QQuickShapeGradient *grad = qobject_cast<QQuickShapeGradient *>(parent()))
            emit grad->updated();
    }
}

/*!
    \qmltype ShapeGradient
    \instantiates QQuickShapeGradient
    \inqmlmodule QtQuick.Shapes
    \ingroup qtquick-paths
    \ingroup qtquick-views
    \inherits Object
    \brief Base type of Shape fill gradients
    \since 5.10

    This is an abstract base class for gradients like ShapeLinearGradient and
    cannot be created directly.
 */

QQuickShapeGradient::QQuickShapeGradient(QObject *parent)
    : QObject(parent),
      m_spread(PadSpread)
{
}

int QQuickShapeGradient::countStops(QQmlListProperty<QObject> *list)
{
    QQuickShapeGradient *grad = qobject_cast<QQuickShapeGradient *>(list->object);
    Q_ASSERT(grad);
    return grad->m_stops.count();
}

QObject *QQuickShapeGradient::atStop(QQmlListProperty<QObject> *list, int index)
{
    QQuickShapeGradient *grad = qobject_cast<QQuickShapeGradient *>(list->object);
    Q_ASSERT(grad);
    return grad->m_stops.at(index);
}

void QQuickShapeGradient::appendStop(QQmlListProperty<QObject> *list, QObject *stop)
{
    QQuickShapeGradientStop *sstop = qobject_cast<QQuickShapeGradientStop *>(stop);
    if (!sstop) {
        qWarning("Gradient stop list only supports QQuickShapeGradientStop elements");
        return;
    }
    QQuickShapeGradient *grad = qobject_cast<QQuickShapeGradient *>(list->object);
    Q_ASSERT(grad);
    sstop->setParent(grad);
    grad->m_stops.append(sstop);
}

/*!
    \qmlproperty list<Object> QtQuick.Shapes::ShapeGradient::stops
    \default

    The list of ShapeGradientStop objects defining the colors at given positions
    in the gradient.
 */

QQmlListProperty<QObject> QQuickShapeGradient::stops()
{
    return QQmlListProperty<QObject>(this, nullptr,
                                     &QQuickShapeGradient::appendStop,
                                     &QQuickShapeGradient::countStops,
                                     &QQuickShapeGradient::atStop,
                                     nullptr);
}

QGradientStops QQuickShapeGradient::sortedGradientStops() const
{
    QGradientStops result;
    for (int i = 0; i < m_stops.count(); ++i) {
        QQuickShapeGradientStop *s = static_cast<QQuickShapeGradientStop *>(m_stops[i]);
        int j = 0;
        while (j < result.count() && result[j].first < s->position())
            ++j;
        result.insert(j, QGradientStop(s->position(), s->color()));
    }
    return result;
}

/*!
    \qmlproperty enumeration QtQuick.Shapes::ShapeGradient::spred

    Specifies how the area outside the gradient area should be filled. The
    default value is ShapeGradient.PadSpread.

    \list
    \li ShapeGradient.PadSpread - The area is filled with the closest stop color.
    \li ShapeGradient.RepeatSpread - The gradient is repeated outside the gradient area.
    \li ShapeGradient.ReflectSpread - The gradient is reflected outside the gradient area.
    \endlist
 */

QQuickShapeGradient::SpreadMode QQuickShapeGradient::spread() const
{
    return m_spread;
}

void QQuickShapeGradient::setSpread(SpreadMode mode)
{
    if (m_spread != mode) {
        m_spread = mode;
        emit spreadChanged();
        emit updated();
    }
}

/*!
    \qmltype ShapeLinearGradient
    \instantiates QQuickShapeLinearGradient
    \inqmlmodule QtQuick.Shapes
    \ingroup qtquick-paths
    \ingroup qtquick-views
    \inherits ShapeGradient
    \brief Linear gradient
    \since 5.10

    Linear gradients interpolate colors between start and end points. Outside
    these points the gradient is either padded, reflected or repeated depending
    on the spread type.

    \sa QLinearGradient
 */

QQuickShapeLinearGradient::QQuickShapeLinearGradient(QObject *parent)
    : QQuickShapeGradient(parent)
{
}

/*!
    \qmlproperty real QtQuick.Shapes::ShapeLinearGradient::x1
    \qmlproperty real QtQuick.Shapes::ShapeLinearGradient::y1
    \qmlproperty real QtQuick.Shapes::ShapeLinearGradient::x2
    \qmlproperty real QtQuick.Shapes::ShapeLinearGradient::y2

    These properties define the start and end points between which color
    interpolation occurs. By default both the stard and end points are set to
    (0, 0).
 */

qreal QQuickShapeLinearGradient::x1() const
{
    return m_start.x();
}

void QQuickShapeLinearGradient::setX1(qreal v)
{
    if (m_start.x() != v) {
        m_start.setX(v);
        emit x1Changed();
        emit updated();
    }
}

qreal QQuickShapeLinearGradient::y1() const
{
    return m_start.y();
}

void QQuickShapeLinearGradient::setY1(qreal v)
{
    if (m_start.y() != v) {
        m_start.setY(v);
        emit y1Changed();
        emit updated();
    }
}

qreal QQuickShapeLinearGradient::x2() const
{
    return m_end.x();
}

void QQuickShapeLinearGradient::setX2(qreal v)
{
    if (m_end.x() != v) {
        m_end.setX(v);
        emit x2Changed();
        emit updated();
    }
}

qreal QQuickShapeLinearGradient::y2() const
{
    return m_end.y();
}

void QQuickShapeLinearGradient::setY2(qreal v)
{
    if (m_end.y() != v) {
        m_end.setY(v);
        emit y2Changed();
        emit updated();
    }
}

#ifndef QT_NO_OPENGL

// contexts sharing with each other get the same cache instance
class QQuickShapeGradientCacheWrapper
{
public:
    QQuickShapeGradientCache *get(QOpenGLContext *context)
    {
        return m_resource.value<QQuickShapeGradientCache>(context);
    }

private:
    QOpenGLMultiGroupSharedResource m_resource;
};

QQuickShapeGradientCache *QQuickShapeGradientCache::currentCache()
{
    static QQuickShapeGradientCacheWrapper qt_path_gradient_caches;
    return qt_path_gradient_caches.get(QOpenGLContext::currentContext());
}

// let QOpenGLContext manage the lifetime of the cached textures
QQuickShapeGradientCache::~QQuickShapeGradientCache()
{
    m_cache.clear();
}

void QQuickShapeGradientCache::invalidateResource()
{
    m_cache.clear();
}

void QQuickShapeGradientCache::freeResource(QOpenGLContext *)
{
    qDeleteAll(m_cache);
    m_cache.clear();
}

static void generateGradientColorTable(const QQuickShapeGradientCache::GradientDesc &gradient,
                                       uint *colorTable, int size, float opacity)
{
    int pos = 0;
    const QGradientStops &s = gradient.stops;
    const bool colorInterpolation = true;

    uint alpha = qRound(opacity * 256);
    uint current_color = ARGB_COMBINE_ALPHA(s[0].second.rgba(), alpha);
    qreal incr = 1.0 / qreal(size);
    qreal fpos = 1.5 * incr;
    colorTable[pos++] = ARGB2RGBA(qPremultiply(current_color));

    while (fpos <= s.first().first) {
        colorTable[pos] = colorTable[pos - 1];
        pos++;
        fpos += incr;
    }

    if (colorInterpolation)
        current_color = qPremultiply(current_color);

    const int sLast = s.size() - 1;
    for (int i = 0; i < sLast; ++i) {
        qreal delta = 1/(s[i+1].first - s[i].first);
        uint next_color = ARGB_COMBINE_ALPHA(s[i + 1].second.rgba(), alpha);
        if (colorInterpolation)
            next_color = qPremultiply(next_color);

        while (fpos < s[i+1].first && pos < size) {
            int dist = int(256 * ((fpos - s[i].first) * delta));
            int idist = 256 - dist;
            if (colorInterpolation)
                colorTable[pos] = ARGB2RGBA(INTERPOLATE_PIXEL_256(current_color, idist, next_color, dist));
            else
                colorTable[pos] = ARGB2RGBA(qPremultiply(INTERPOLATE_PIXEL_256(current_color, idist, next_color, dist)));
            ++pos;
            fpos += incr;
        }
        current_color = next_color;
    }

    Q_ASSERT(s.size() > 0);

    uint last_color = ARGB2RGBA(qPremultiply(ARGB_COMBINE_ALPHA(s[sLast].second.rgba(), alpha)));
    for ( ; pos < size; ++pos)
        colorTable[pos] = last_color;

    colorTable[size-1] = last_color;
}

QSGTexture *QQuickShapeGradientCache::get(const GradientDesc &grad)
{
    QSGPlainTexture *tx = m_cache[grad];
    if (!tx) {
        QOpenGLFunctions *f = QOpenGLContext::currentContext()->functions();
        GLuint id;
        f->glGenTextures(1, &id);
        f->glBindTexture(GL_TEXTURE_2D, id);
        static const uint W = 1024; // texture size is 1024x1
        uint buf[W];
        generateGradientColorTable(grad, buf, W, 1.0f);
        f->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, buf);
        tx = new QSGPlainTexture;
        tx->setTextureId(id);
        switch (grad.spread) {
        case QQuickShapeGradient::PadSpread:
            tx->setHorizontalWrapMode(QSGTexture::ClampToEdge);
            tx->setVerticalWrapMode(QSGTexture::ClampToEdge);
            break;
        case QQuickShapeGradient::RepeatSpread:
            tx->setHorizontalWrapMode(QSGTexture::Repeat);
            tx->setVerticalWrapMode(QSGTexture::Repeat);
            break;
        case QQuickShapeGradient::ReflectSpread:
            tx->setHorizontalWrapMode(QSGTexture::MirroredRepeat);
            tx->setVerticalWrapMode(QSGTexture::MirroredRepeat);
            break;
        default:
            qWarning("Unknown gradient spread mode %d", grad.spread);
            break;
        }
        m_cache[grad] = tx;
    }
    return tx;
}

#endif // QT_NO_OPENGL

QT_END_NAMESPACE

#include "moc_qquickshape_p.cpp"
