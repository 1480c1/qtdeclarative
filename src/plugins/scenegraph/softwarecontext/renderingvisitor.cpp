/****************************************************************************
**
** Copyright (C) 2015 The Qt Company Ltd.
** Contact: http://www.qt.io/licensing/
**
** This file is part of Qt Quick 2d Renderer module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see http://www.qt.io/terms-conditions. For further
** information use the contact form at http://www.qt.io/contact-us.
** $QT_END_LICENSE$
**
****************************************************************************/

#include "renderingvisitor.h"

#include "imagenode.h"
#include "rectanglenode.h"
#include "glyphnode.h"
#include "ninepatchnode.h"
#include "painternode.h"
#include "pixmaptexture.h"

#include <QtQuick/QSGSimpleRectNode>
#include <QtQuick/qsgsimpletexturenode.h>
#include <private/qsgtexture_p.h>
#include <private/qquickshadereffectnode_p.h>

RenderingVisitor::RenderingVisitor(QPainter *painter)
    : painter(painter)
{

}

bool RenderingVisitor::visit(QSGTransformNode *node)
{
    painter->save();
    painter->setTransform(node->matrix().toTransform(), /*combine*/true);
    return true;
}

void RenderingVisitor::endVisit(QSGTransformNode *)
{
    painter->restore();
}

bool RenderingVisitor::visit(QSGClipNode *node)
{
    painter->save();
    painter->setClipRect(node->clipRect(), Qt::IntersectClip);
    return true;
}

void RenderingVisitor::endVisit(QSGClipNode *)
{
    painter->restore();
}

bool RenderingVisitor::visit(QSGGeometryNode *node)
{
    if (QSGSimpleRectNode *rectNode = dynamic_cast<QSGSimpleRectNode *>(node)) {
        if (!rectNode->material()->flags() & QSGMaterial::Blending)
            painter->setCompositionMode(QPainter::CompositionMode_Source);
        painter->fillRect(rectNode->rect(), rectNode->color());
        painter->setCompositionMode(QPainter::CompositionMode_SourceOver);
    } else if (QSGSimpleTextureNode *tn = dynamic_cast<QSGSimpleTextureNode *>(node)) {
        QSGTexture *texture = tn->texture();
        if (PixmapTexture *pt = dynamic_cast<PixmapTexture *>(texture)) {
            const QPixmap &pm = pt->pixmap();
#if QT_VERSION >= QT_VERSION_CHECK(5, 5, 0)
            painter->drawPixmap(tn->rect(), pm, tn->sourceRect());
#else
            painter->drawPixmap(tn->rect(), pm, QRectF(0, 0, pm.width(), pm.height()));
#endif
        } else if (QSGPlainTexture *pt = dynamic_cast<QSGPlainTexture *>(texture)) {
            const QImage &im = pt->image();
            painter->drawImage(tn->rect(), im, QRectF(0, 0, im.width(), im.height()));
        } else {
            //Do nothing
        }
    } else if (QQuickShaderEffectNode *sn = dynamic_cast<QQuickShaderEffectNode *>(node)) {
        Q_UNUSED(sn)
    } else {
        //Do nothing
    }
    return true;
}

void RenderingVisitor::endVisit(QSGGeometryNode *)
{
}

bool RenderingVisitor::visit(QSGOpacityNode *node)
{
    painter->save();

    const qreal newOpacity = painter->opacity() * node->opacity();
    if (qFuzzyIsNull(newOpacity))
        return false;

    painter->setOpacity(newOpacity);
    return true;
}

void RenderingVisitor::endVisit(QSGOpacityNode *)
{
    painter->restore();
}

bool RenderingVisitor::visit(QSGImageNode *node)
{
    static_cast<ImageNode*>(node)->paint(painter);
    return true;
}

void RenderingVisitor::endVisit(QSGImageNode *)
{
}

bool RenderingVisitor::visit(QSGPainterNode *node)
{
    static_cast<PainterNode*>(node)->paint(painter);
    return true;
}

void RenderingVisitor::endVisit(QSGPainterNode *)
{
}

bool RenderingVisitor::visit(QSGRectangleNode *node)
{
    static_cast<RectangleNode*>(node)->paint(painter);
    return true;
}

void RenderingVisitor::endVisit(QSGRectangleNode *)
{
}

bool RenderingVisitor::visit(QSGGlyphNode *node)
{
    static_cast<GlyphNode*>(node)->paint(painter);
    return true;
}

void RenderingVisitor::endVisit(QSGGlyphNode *)
{
}

bool RenderingVisitor::visit(QSGNinePatchNode *node)
{
    static_cast<NinePatchNode*>(node)->paint(painter);
    return true;
}

void RenderingVisitor::endVisit(QSGNinePatchNode *)
{
}

bool RenderingVisitor::visit(QSGRootNode *)
{
    return true;
}

void RenderingVisitor::endVisit(QSGRootNode *)
{
}
