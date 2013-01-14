/****************************************************************************
**
** Copyright (C) 2012 Digia Plc and/or its subsidiary(-ies).
** Contact: http://www.qt-project.org/legal
**
** This file is part of the QtCore module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Digia.  For licensing terms and
** conditions see http://qt.digia.com/licensing.  For further information
** use the contact form at http://qt.digia.com/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Digia gives you certain additional
** rights.  These rights are described in the Digia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3.0 as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU General Public License version 3.0 requirements will be
** met: http://www.gnu.org/copyleft/gpl.html.
**
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "qv4array.h"
#include "qmljs_runtime.h"
#include "qmljs_objects.h"
#include <stdlib.h>

#ifdef QT_QMAP_DEBUG
# include <qstring.h>
# include <qvector.h>
#endif

namespace QQmlJS {
namespace VM {

bool ArrayElementLessThan::operator()(const PropertyDescriptor &p1, const PropertyDescriptor &p2) const
{
    if (p1.type == PropertyDescriptor::Generic)
        return false;
    if (p2.type == PropertyDescriptor::Generic)
        return true;
    Value v1 = thisObject->getValue(m_context, &p1);
    Value v2 = thisObject->getValue(m_context, &p2);

    if (v1.isUndefined())
        return false;
    if (v2.isUndefined())
        return true;
    if (!m_comparefn.isUndefined()) {
        Value args[] = { v1, v2 };
        Value result = __qmljs_call_value(m_context, Value::undefinedValue(), m_comparefn, args, 2);
        return result.toNumber(m_context) <= 0;
    }
    return v1.toString(m_context)->toQString() < v2.toString(m_context)->toQString();
}


const SparseArrayNode *SparseArrayNode::nextNode() const
{
    const SparseArrayNode *n = this;
    if (n->right) {
        n = n->right;
        while (n->left)
            n = n->left;
    } else {
        const SparseArrayNode *y = n->parent();
        while (y && n == y->right) {
            n = y;
            y = n->parent();
        }
        n = y;
    }
    return n;
}

const SparseArrayNode *SparseArrayNode::previousNode() const
{
    const SparseArrayNode *n = this;
    if (n->left) {
        n = n->left;
        while (n->right)
            n = n->right;
    } else {
        const SparseArrayNode *y = n->parent();
        while (y && n == y->left) {
            n = y;
            y = n->parent();
        }
        n = y;
    }
    return n;
}

SparseArrayNode *SparseArrayNode::copy(SparseArray *d) const
{
    SparseArrayNode *n = d->createNode(size_left, 0, false);
    n->value = value;
    n->setColor(color());
    if (left) {
        n->left = left->copy(d);
        n->left->setParent(n);
    } else {
        n->left = 0;
    }
    if (right) {
        n->right = right->copy(d);
        n->right->setParent(n);
    } else {
        n->right = 0;
    }
    return n;
}

/*
     x              y
      \            / \
       y    -->   x   b
      / \          \
     a   b          a
*/
void SparseArray::rotateLeft(SparseArrayNode *x)
{
    SparseArrayNode *&root = header.left;
    SparseArrayNode *y = x->right;
    x->right = y->left;
    if (y->left != 0)
        y->left->setParent(x);
    y->setParent(x->parent());
    if (x == root)
        root = y;
    else if (x == x->parent()->left)
        x->parent()->left = y;
    else
        x->parent()->right = y;
    y->left = x;
    x->setParent(y);
    y->size_left += x->size_left;
}


/*
         x          y
        /          / \
       y    -->   a   x
      / \            /
     a   b          b
*/
void SparseArray::rotateRight(SparseArrayNode *x)
{
    SparseArrayNode *&root = header.left;
    SparseArrayNode *y = x->left;
    x->left = y->right;
    if (y->right != 0)
        y->right->setParent(x);
    y->setParent(x->parent());
    if (x == root)
        root = y;
    else if (x == x->parent()->right)
        x->parent()->right = y;
    else
        x->parent()->left = y;
    y->right = x;
    x->setParent(y);
    x->size_left -= y->size_left;
}


void SparseArray::rebalance(SparseArrayNode *x)
{
    SparseArrayNode *&root = header.left;
    x->setColor(SparseArrayNode::Red);
    while (x != root && x->parent()->color() == SparseArrayNode::Red) {
        if (x->parent() == x->parent()->parent()->left) {
            SparseArrayNode *y = x->parent()->parent()->right;
            if (y && y->color() == SparseArrayNode::Red) {
                x->parent()->setColor(SparseArrayNode::Black);
                y->setColor(SparseArrayNode::Black);
                x->parent()->parent()->setColor(SparseArrayNode::Red);
                x = x->parent()->parent();
            } else {
                if (x == x->parent()->right) {
                    x = x->parent();
                    rotateLeft(x);
                }
                x->parent()->setColor(SparseArrayNode::Black);
                x->parent()->parent()->setColor(SparseArrayNode::Red);
                rotateRight (x->parent()->parent());
            }
        } else {
            SparseArrayNode *y = x->parent()->parent()->left;
            if (y && y->color() == SparseArrayNode::Red) {
                x->parent()->setColor(SparseArrayNode::Black);
                y->setColor(SparseArrayNode::Black);
                x->parent()->parent()->setColor(SparseArrayNode::Red);
                x = x->parent()->parent();
            } else {
                if (x == x->parent()->left) {
                    x = x->parent();
                    rotateRight(x);
                }
                x->parent()->setColor(SparseArrayNode::Black);
                x->parent()->parent()->setColor(SparseArrayNode::Red);
                rotateLeft(x->parent()->parent());
            }
        }
    }
    root->setColor(SparseArrayNode::Black);
}

void SparseArray::deleteNode(SparseArrayNode *z)
{
    SparseArrayNode *&root = header.left;
    SparseArrayNode *y = z;
    SparseArrayNode *x;
    SparseArrayNode *x_parent;
    if (y->left == 0) {
        x = y->right;
        if (y == mostLeftNode) {
            if (x)
                mostLeftNode = x; // It cannot have (left) children due the red black invariant.
            else
                mostLeftNode = y->parent();
        }
    } else {
        if (y->right == 0) {
            x = y->left;
        } else {
            y = y->right;
            while (y->left != 0)
                y = y->left;
            x = y->right;
        }
    }
    if (y != z) {
        z->left->setParent(y);
        y->left = z->left;
        if (y != z->right) {
            x_parent = y->parent();
            if (x)
                x->setParent(y->parent());
            y->parent()->left = x;
            y->right = z->right;
            z->right->setParent(y);
        } else {
            x_parent = y;
        }
        if (root == z)
            root = y;
        else if (z->parent()->left == z)
            z->parent()->left = y;
        else
            z->parent()->right = y;
        y->setParent(z->parent());
        // Swap the colors
        SparseArrayNode::Color c = y->color();
        y->setColor(z->color());
        z->setColor(c);
        y = z;
    } else {
        x_parent = y->parent();
        if (x)
            x->setParent(y->parent());
        if (root == z)
            root = x;
        else if (z->parent()->left == z)
            z->parent()->left = x;
        else
            z->parent()->right = x;
    }
    if (y->color() != SparseArrayNode::Red) {
        while (x != root && (x == 0 || x->color() == SparseArrayNode::Black)) {
            if (x == x_parent->left) {
                SparseArrayNode *w = x_parent->right;
                if (w->color() == SparseArrayNode::Red) {
                    w->setColor(SparseArrayNode::Black);
                    x_parent->setColor(SparseArrayNode::Red);
                    rotateLeft(x_parent);
                    w = x_parent->right;
                }
                if ((w->left == 0 || w->left->color() == SparseArrayNode::Black) &&
                    (w->right == 0 || w->right->color() == SparseArrayNode::Black)) {
                    w->setColor(SparseArrayNode::Red);
                    x = x_parent;
                    x_parent = x_parent->parent();
                } else {
                    if (w->right == 0 || w->right->color() == SparseArrayNode::Black) {
                        if (w->left)
                            w->left->setColor(SparseArrayNode::Black);
                        w->setColor(SparseArrayNode::Red);
                        rotateRight(w);
                        w = x_parent->right;
                    }
                    w->setColor(x_parent->color());
                    x_parent->setColor(SparseArrayNode::Black);
                    if (w->right)
                        w->right->setColor(SparseArrayNode::Black);
                    rotateLeft(x_parent);
                    break;
                }
            } else {
            SparseArrayNode *w = x_parent->left;
            if (w->color() == SparseArrayNode::Red) {
                w->setColor(SparseArrayNode::Black);
                x_parent->setColor(SparseArrayNode::Red);
                rotateRight(x_parent);
                w = x_parent->left;
            }
            if ((w->right == 0 || w->right->color() == SparseArrayNode::Black) &&
                (w->left == 0 || w->left->color() == SparseArrayNode::Black)) {
                w->setColor(SparseArrayNode::Red);
                x = x_parent;
                x_parent = x_parent->parent();
            } else {
                if (w->left == 0 || w->left->color() == SparseArrayNode::Black) {
                    if (w->right)
                        w->right->setColor(SparseArrayNode::Black);
                    w->setColor(SparseArrayNode::Red);
                    rotateLeft(w);
                    w = x_parent->left;
                }
                w->setColor(x_parent->color());
                x_parent->setColor(SparseArrayNode::Black);
                if (w->left)
                    w->left->setColor(SparseArrayNode::Black);
                rotateRight(x_parent);
                break;
            }
        }
    }
    if (x)
        x->setColor(SparseArrayNode::Black);
    }
    free(y);
    --numEntries;
}

void SparseArray::recalcMostLeftNode()
{
    mostLeftNode = &header;
    while (mostLeftNode->left)
        mostLeftNode = mostLeftNode->left;
}

static inline int qMapAlignmentThreshold()
{
    // malloc on 32-bit platforms should return pointers that are 8-byte
    // aligned or more while on 64-bit platforms they should be 16-byte aligned
    // or more
    return 2 * sizeof(void*);
}

static inline void *qMapAllocate(int alloc, int alignment)
{
    return alignment > qMapAlignmentThreshold()
        ? qMallocAligned(alloc, alignment)
        : ::malloc(alloc);
}

static inline void qMapDeallocate(SparseArrayNode *node, int alignment)
{
    if (alignment > qMapAlignmentThreshold())
        qFreeAligned(node);
    else
        ::free(node);
}

SparseArrayNode *SparseArray::createNode(uint sl, SparseArrayNode *parent, bool left)
{
    SparseArrayNode *node = static_cast<SparseArrayNode *>(qMapAllocate(sizeof(SparseArrayNode), Q_ALIGNOF(SparseArrayNode)));
    Q_CHECK_PTR(node);

    node->p = (quintptr)parent;
    node->left = 0;
    node->right = 0;
    node->size_left = sl;
    node->value = UINT_MAX;
    ++numEntries;

    if (parent) {
        if (left) {
            parent->left = node;
            if (parent == mostLeftNode)
                mostLeftNode = node;
        } else {
            parent->right = node;
        }
        node->setParent(parent);
        rebalance(node);
    }
    return node;
}

void SparseArray::freeTree(SparseArrayNode *root, int alignment)
{
    if (root->left)
        freeTree(root->left, alignment);
    if (root->right)
        freeTree(root->right, alignment);
    qMapDeallocate(root, alignment);
}

SparseArray::SparseArray()
    : numEntries(0)
{
    header.p = 0;
    header.left = 0;
    header.right = 0;
    mostLeftNode = &header;
}

SparseArray::SparseArray(const SparseArray &other)
{
    header.p = 0;
    header.right = 0;
    if (other.header.left) {
        header.left = other.header.left->copy(this);
        header.left->setParent(&header);
        recalcMostLeftNode();
    }
}

SparseArrayNode *SparseArray::insert(uint akey)
{
    SparseArrayNode *n = root();
    SparseArrayNode *y = end();
    bool  left = true;
    uint s = akey;
    while (n) {
        y = n;
        if (s == n->size_left) {
            return n;
        } else if (s < n->size_left) {
            left = true;
            n = n->left;
        } else {
            left = false;
            s -= n->size_left;
            n = n->right;
        }
    }

    return createNode(s, y, left);
}

Array::Array(const Array &other)
    : len(other.len)
    , lengthProperty(0)
    , values(other.values)
    , sparse(0)
{
    freeList = other.freeList;
    if (other.sparse)
        sparse = new SparseArray(*other.sparse);
}


void Array::splice(double start, double deleteCount,
                   const QVector<Value> &/*items*/,
                   Array &/*other*/)
{
    initSparse();
    uint len = length();
    if (start < 0)
        start = qMax(len + start, double(0));
    else if (start > len)
        start = len;
    deleteCount = qMax(qMin(deleteCount, len - start), double(0));

    // ###
//    const uint st = uint(start);
//    const uint dc = uint(deleteCount);
//    other.resize(dc);

//    const uint itemsSize = uint(items.size());

//    for (uint i = 0; i < dc; ++i)
//        other.assign(i, to_vector.at(st + i));
//    if (itemsSize > dc)
//        to_vector.insert(to_vector.begin() + st, itemsSize - dc, Value::undefinedValue());
//    else if (itemsSize < dc)
//        to_vector.erase(to_vector.begin() + st, to_vector.begin() + (dc - itemsSize));
//    for (uint i = 0; i < itemsSize; ++i)
    //        (*to_vector)[st + i] = items.at(i);
}

Value Array::indexOf(Value v, uint fromIndex, uint endIndex, ExecutionContext *ctx, Object *o)
{
    bool protoHasArray = false;
    Object *p = o;
    while ((p = p->prototype))
        if (p->array.length())
            protoHasArray = true;

    if (protoHasArray) {
        // lets be safe and slow
        for (uint i = fromIndex; i < endIndex; ++i) {
            bool exists;
            Value value = o->__get__(ctx, i, &exists);
            if (exists && __qmljs_strict_equal(value, v))
                return Value::fromDouble(i);
        }
    } else if (sparse) {
        for (SparseArrayNode *n = sparse->findNode(fromIndex); n && n->key() < endIndex; n = n->nextNode()) {
            bool exists;
            Value value = o->getValueChecked(ctx, descriptor(n->value), &exists);
            if (exists && __qmljs_strict_equal(value, v))
                return Value::fromDouble(n->key());
        }
    } else {
        if (endIndex > len)
            endIndex = len;
        PropertyDescriptor *pd = values.data() + offset;
        PropertyDescriptor *end = pd + endIndex;
        pd += fromIndex;
        while (pd < end) {
            bool exists;
            Value value = o->getValueChecked(ctx, pd, &exists);
            if (exists && __qmljs_strict_equal(value, v))
                return Value::fromDouble(pd - offset - values.constData());
            ++pd;
        }
    }
    return Value::fromInt32(-1);
}

void Array::concat(const Array &other)
{
    initSparse();
    int newLen = len + other.length();
    if (other.sparse)
        initSparse();
    if (sparse) {
        if (other.sparse) {
            for (const SparseArrayNode *it = other.sparse->begin(); it != other.sparse->end(); it = it->nextNode())
                set(len + it->key(), other.descriptor(it->value));
        } else {
            int oldSize = values.size();
            values.resize(oldSize + other.length());
            memcpy(values.data() + oldSize, other.values.constData() + other.offset, other.length()*sizeof(PropertyDescriptor));
            for (uint i = 0; i < other.length(); ++i) {
                SparseArrayNode *n = sparse->insert(len + i);
                n->value = oldSize + i;
            }
        }
    } else {
        int oldSize = values.size();
        values.resize(oldSize + other.length());
        memcpy(values.data() + oldSize, other.values.constData() + other.offset, other.length()*sizeof(PropertyDescriptor));
    }
    setLengthUnchecked(newLen);
}

void Array::sort(ExecutionContext *context, Object *thisObject, const Value &comparefn)
{
    if (!sparse)
        return;

    ArrayElementLessThan lessThan(context, thisObject, comparefn);
    std::sort(values.begin(), values.end(), lessThan);
    delete sparse;
}


void Array::initSparse()
{
    if (!sparse) {
        sparse = new SparseArray;
        for (int i = offset; i < values.size(); ++i) {
            SparseArrayNode *n = sparse->insert(i - offset);
            n->value = i;
        }

        if (offset) {
            int o = offset;
            for (int i = 0; i < o - 1; ++i) {
                values[i].type = PropertyDescriptor::Generic;
                values[i].value = Value::fromInt32(i + 1);
            }
            values[o - 1].type = PropertyDescriptor::Generic;
            values[o - 1].value = Value::fromInt32(values.size());
            freeList = 0;
        } else {
            freeList = values.size();
        }
    }
}


}
}
