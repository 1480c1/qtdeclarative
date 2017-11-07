/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of the QtQml module of the Qt Toolkit.
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

#include "qv4booleanobject_p.h"
#include "qv4string_p.h"

using namespace QV4;

DEFINE_OBJECT_VTABLE(BooleanCtor);
DEFINE_OBJECT_VTABLE(BooleanObject);

void Heap::BooleanCtor::init(QV4::ExecutionContext *scope)
{
    Heap::FunctionObject::init(scope, QStringLiteral("Boolean"));
}

ReturnedValue BooleanCtor::callAsConstructor(const Managed *that, CallData *callData)
{
    bool n = callData->argc() ? callData->args[0].toBoolean() : false;
    return Encode(that->engine()->newBooleanObject(n));
}

ReturnedValue BooleanCtor::call(const Managed *, CallData *callData)
{
    bool value = callData->argc() ? callData->args[0].toBoolean() : 0;
    return Encode(value);
}

void BooleanPrototype::init(ExecutionEngine *engine, Object *ctor)
{
    Scope scope(engine);
    ScopedObject o(scope);
    ctor->defineReadonlyProperty(engine->id_length(), Primitive::fromInt32(1));
    ctor->defineReadonlyProperty(engine->id_prototype(), (o = this));
    defineDefaultProperty(QStringLiteral("constructor"), (o = ctor));
    defineDefaultProperty(engine->id_toString(), method_toString);
    defineDefaultProperty(engine->id_valueOf(), method_valueOf);
}

ReturnedValue BooleanPrototype::method_toString(const BuiltinFunction *b, CallData *callData)
{
    ExecutionEngine *v4 = b->engine();
    bool result;
    if (callData->thisObject.isBoolean()) {
        result = callData->thisObject.booleanValue();
    } else {
        const BooleanObject *thisObject = callData->thisObject.as<BooleanObject>();
        if (!thisObject)
            return v4->throwTypeError();
        result = thisObject->value();
    }

    return Encode(result ? v4->id_true() : v4->id_false());
}

ReturnedValue BooleanPrototype::method_valueOf(const BuiltinFunction *b, CallData *callData)
{
    if (callData->thisObject.isBoolean())
        return callData->thisObject.asReturnedValue();

    const BooleanObject *thisObject = callData->thisObject.as<BooleanObject>();
    if (!thisObject)
        return b->engine()->throwTypeError();

    return Encode(thisObject->value());
}
