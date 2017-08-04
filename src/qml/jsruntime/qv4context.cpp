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

#include <QString>
#include "qv4debugging_p.h"
#include <qv4context_p.h>
#include <qv4object_p.h>
#include <qv4objectproto_p.h>
#include <private/qv4mm_p.h>
#include <qv4argumentsobject_p.h>
#include "qv4function_p.h"
#include "qv4errorobject_p.h"
#include "qv4string_p.h"
#include "qv4qmlcontext_p.h"
#include "qv4profiling_p.h"
#include <private/qqmljavascriptexpression_p.h>

using namespace QV4;

DEFINE_MANAGED_VTABLE(ExecutionContext);
DEFINE_MANAGED_VTABLE(CallContext);
DEFINE_MANAGED_VTABLE(CatchContext);

Heap::CallContext *ExecutionContext::newCallContext(Function *function, CallData *callData)
{
    uint localsAndFormals = function->compiledFunction->nLocals + sizeof(CallData)/sizeof(Value) - 1 + qMax(static_cast<uint>(callData->argc), function->nFormals);
    size_t requiredMemory = sizeof(CallContext::Data) - sizeof(Value) + sizeof(Value) * (localsAndFormals);

    ExecutionEngine *v4 = engine();
    Heap::CallContext *c = v4->memoryManager->allocManaged<CallContext>(requiredMemory, function->internalClass);
    c->init(Heap::ExecutionContext::Type_CallContext);

    c->v4Function = function;

    c->strictMode = function->isStrict();
    c->outer.set(v4, this->d());

    const CompiledData::Function *compiledFunction = function->compiledFunction;
    uint nLocals = compiledFunction->nLocals;
    c->locals.size = nLocals;
    c->locals.alloc = localsAndFormals;
#if QT_POINTER_SIZE == 8
    // memory allocated from the JS heap is 0 initialized, so skip the std::fill() below
    Q_ASSERT(Primitive::undefinedValue().asReturnedValue() == 0);
#else
    if (nLocals)
        std::fill(c->locals.values, c->locals.values + nLocals, Primitive::undefinedValue());
#endif

    c->callData = reinterpret_cast<CallData *>(c->locals.values + nLocals);
    ::memcpy(c->callData, callData, sizeof(CallData) - sizeof(Value) + static_cast<uint>(callData->argc) * sizeof(Value));
    if (callData->argc < static_cast<int>(compiledFunction->nFormals))
        std::fill(c->callData->args + c->callData->argc, c->callData->args + compiledFunction->nFormals, Primitive::undefinedValue());

    return c;
}

Heap::ExecutionContext *ExecutionContext::newWithContext(Heap::Object *with)
{
    Heap::ExecutionContext *c = engine()->memoryManager->alloc<ExecutionContext>(Heap::ExecutionContext::Type_WithContext);
    c->outer.set(engine(), d());
    c->activation.set(engine(), with);

    c->callData = d()->callData;
    c->v4Function = d()->v4Function;

    return c;
}

Heap::CatchContext *ExecutionContext::newCatchContext(Heap::String *exceptionVarName, ReturnedValue exceptionValue)
{
    Scope scope(this);
    ScopedValue e(scope, exceptionValue);
    return engine()->memoryManager->alloc<CatchContext>(d(), exceptionVarName, e);
}

void ExecutionContext::createMutableBinding(String *name, bool deletable)
{
    Scope scope(this);

    // find the right context to create the binding on
    ScopedObject activation(scope);
    ScopedContext ctx(scope, this);
    while (ctx) {
        switch (ctx->d()->type) {
        case Heap::ExecutionContext::Type_CallContext:
        case Heap::ExecutionContext::Type_SimpleCallContext:
            if (!activation) {
                Heap::CallContext *c = static_cast<Heap::CallContext *>(ctx->d());
                if (!c->activation)
                    c->activation.set(scope.engine, scope.engine->newObject());
                activation = c->activation;
            }
            break;
        case Heap::ExecutionContext::Type_QmlContext: {
            // this is ugly, as it overrides the inner callcontext, but has to stay as long
            // as bindings still get their own callcontext
            activation = ctx->d()->activation;
            break;
        }
        case Heap::ExecutionContext::Type_GlobalContext: {
            Q_ASSERT(scope.engine->globalObject->d() == ctx->d()->activation);
            if (!activation)
                activation = ctx->d()->activation;
            break;
        }
        default:
            break;
        }
        ctx = ctx->d()->outer;
    }

    if (activation->hasOwnProperty(name))
        return;
    ScopedProperty desc(scope);
    PropertyAttributes attrs(Attr_Data);
    attrs.setConfigurable(deletable);
    activation->__defineOwnProperty__(scope.engine, name, desc, attrs);
}

void Heap::CatchContext::init(ExecutionContext *outerContext, String *exceptionVarName,
                              const Value &exceptionValue)
{
    Heap::ExecutionContext::init(Heap::ExecutionContext::Type_CatchContext);
    outer.set(internalClass->engine, outerContext);
    strictMode = outer->strictMode;
    callData = outer->callData;
    v4Function = outer->v4Function;

    this->exceptionVarName.set(internalClass->engine, exceptionVarName);
    this->exceptionValue.set(internalClass->engine, exceptionValue);
}

Identifier * const *CallContext::formals() const
{
    return d()->v4Function ? d()->internalClass->nameMap.constData() : 0;
}

unsigned int CallContext::formalCount() const
{
    return d()->v4Function ? d()->v4Function->nFormals : 0;
}

Identifier * const *CallContext::variables() const
{
    return d()->v4Function ? d()->internalClass->nameMap.constData() + d()->v4Function->nFormals : 0;
}

unsigned int CallContext::variableCount() const
{
    return d()->v4Function ? d()->v4Function->compiledFunction->nLocals : 0;
}



bool ExecutionContext::deleteProperty(String *name)
{
    name->makeIdentifier();
    Identifier *id = name->identifier();

    Scope scope(this);
    ScopedContext ctx(scope, this);
    for (; ctx; ctx = ctx->d()->outer) {
        switch (ctx->d()->type) {
        case Heap::ExecutionContext::Type_CatchContext: {
            Heap::CatchContext *c = static_cast<Heap::CatchContext *>(ctx->d());
            if (c->exceptionVarName->isEqualTo(name->d()))
                return false;
            break;
        }
        case Heap::ExecutionContext::Type_CallContext: {
            Heap::CallContext *c = static_cast<Heap::CallContext *>(ctx->d());
            uint index = c->internalClass->find(id);
            if (index < UINT_MAX)
                // ### throw in strict mode?
                return false;
            Q_FALLTHROUGH();
        }
        case Heap::ExecutionContext::Type_WithContext:
        case Heap::ExecutionContext::Type_GlobalContext:
        case Heap::ExecutionContext::Type_SimpleCallContext: {
            ScopedObject object(scope, ctx->d()->activation);
            if (object && object->hasProperty(name))
                return object->deleteProperty(name);
            break;
        }
        case Heap::ExecutionContext::Type_QmlContext:
            // can't delete properties on qml objects
            break;
        }
    }

    if (d()->strictMode)
        engine()->throwSyntaxError(QStringLiteral("Can't delete property %1").arg(name->toQString()));
    return true;
}

// Do a standard call with this execution context as the outer scope
ReturnedValue ExecutionContext::call(Scope &scope, CallData *callData, Function *function, const FunctionObject *f)
{
    ExecutionContextSaver ctxSaver(scope);

    Scoped<CallContext> ctx(scope, newCallContext(function, callData));
    if (f)
        ctx->d()->function.set(scope.engine, f->d());
    scope.engine->pushContext(ctx);

    ReturnedValue res = Q_V4_PROFILE(scope.engine, function);

    if (function->hasQmlDependencies)
        QQmlPropertyCapture::registerQmlDependencies(function->compiledFunction, scope);

    return res;
}

// Do a simple, fast call with this execution context as the outer scope
ReturnedValue QV4::ExecutionContext::simpleCall(Scope &scope, CallData *callData, Function *function)
{
    Q_ASSERT(function->canUseSimpleFunction());

    ExecutionContextSaver ctxSaver(scope);

    CallContext::Data *ctx = scope.engine->memoryManager->allocSimpleCallContext();

    ctx->strictMode = function->isStrict();
    ctx->callData = callData;
    ctx->v4Function = function;
    ctx->outer.set(scope.engine, this->d());
    for (int i = callData->argc; i < (int)function->nFormals; ++i)
        callData->args[i] = Encode::undefined();

    scope.engine->pushContext(ctx);
    Q_ASSERT(scope.engine->current == ctx);

    ReturnedValue res = Q_V4_PROFILE(scope.engine, function);

    if (function->hasQmlDependencies)
        QQmlPropertyCapture::registerQmlDependencies(function->compiledFunction, scope);
    scope.engine->memoryManager->freeSimpleCallContext();

    return res;
}

void ExecutionContext::setProperty(String *name, const Value &value)
{
    name->makeIdentifier();
    Identifier *id = name->identifier();

    Scope scope(this);
    ScopedContext ctx(scope, this);
    ScopedObject activation(scope);

    for (; ctx; ctx = ctx->d()->outer) {
        activation = (Object *)0;
        switch (ctx->d()->type) {
        case Heap::ExecutionContext::Type_CatchContext: {
            Heap::CatchContext *c = static_cast<Heap::CatchContext *>(ctx->d());
            if (c->exceptionVarName->isEqualTo(name->d())) {
                    c->exceptionValue.set(scope.engine, value);
                    return;
            }
            break;
        }
        case Heap::ExecutionContext::Type_WithContext: {
            // the semantics are different from the setProperty calls of other activations
            ScopedObject w(scope, ctx->d()->activation);
            if (w->hasProperty(name)) {
                w->put(name, value);
                return;
            }
            break;
        }
        case Heap::ExecutionContext::Type_CallContext:
        case Heap::ExecutionContext::Type_SimpleCallContext: {
            Heap::CallContext *c = static_cast<Heap::CallContext *>(ctx->d());
            if (c->v4Function) {
                uint index = c->internalClass->find(id);
                if (index < UINT_MAX) {
                    if (index < c->v4Function->nFormals) {
                        c->callData->args[c->v4Function->nFormals - index - 1] = value;
                    } else {
                        Q_ASSERT(c->type == Heap::ExecutionContext::Type_CallContext);
                        index -= c->v4Function->nFormals;
                        static_cast<Heap::CallContext *>(c)->locals.set(scope.engine, index, value);
                    }
                    return;
                }
            }
        }
            Q_FALLTHROUGH();
        case Heap::ExecutionContext::Type_GlobalContext:
            activation = ctx->d()->activation;
            break;
        case Heap::ExecutionContext::Type_QmlContext: {
            activation = ctx->d()->activation;
            activation->put(name, value);
            return;
        }
        }

        if (activation) {
            uint member = activation->internalClass()->find(id);
            if (member < UINT_MAX) {
                activation->putValue(member, value);
                return;
            }
        }
    }

    if (d()->strictMode || name->equals(engine()->id_this())) {
        ScopedValue n(scope, name->asReturnedValue());
        engine()->throwReferenceError(n);
        return;
    }
    engine()->globalObject->put(name, value);
}

ReturnedValue ExecutionContext::getProperty(String *name)
{
    Scope scope(this);
    ScopedValue v(scope);
    name->makeIdentifier();

    if (name->equals(engine()->id_this()))
        return thisObject().asReturnedValue();

    ScopedContext ctx(scope, this);
    for (; ctx; ctx = ctx->d()->outer) {
        switch (ctx->d()->type) {
        case Heap::ExecutionContext::Type_CatchContext: {
            Heap::CatchContext *c = static_cast<Heap::CatchContext *>(ctx->d());
            if (c->exceptionVarName->isEqualTo(name->d()))
                return c->exceptionValue.asReturnedValue();
            break;
        }
        case Heap::ExecutionContext::Type_CallContext: {
            Heap::CallContext *c = static_cast<Heap::CallContext *>(ctx->d());
            name->makeIdentifier();
            Identifier *id = name->identifier();

            uint index = c->internalClass->find(id);
            if (index < UINT_MAX) {
                if (index < c->v4Function->nFormals)
                    return c->callData->args[c->v4Function->nFormals - index - 1].asReturnedValue();
                Q_ASSERT(c->type == Heap::ExecutionContext::Type_CallContext);
                return c->locals[index - c->v4Function->nFormals].asReturnedValue();
            }
            if (c->v4Function->isNamedExpression()) {
                if (c->function && name->equals(ScopedString(scope, c->v4Function->name())))
                    return c->function->asReturnedValue();
            }
            Q_FALLTHROUGH();
        }
        case Heap::ExecutionContext::Type_WithContext:
        case Heap::ExecutionContext::Type_GlobalContext:
        case Heap::ExecutionContext::Type_QmlContext:
        case Heap::ExecutionContext::Type_SimpleCallContext: {
            ScopedObject activation(scope, ctx->d()->activation);
            if (activation) {
                bool hasProperty = false;
                v = activation->get(name, &hasProperty);
                if (hasProperty)
                    return v->asReturnedValue();
            }
            break;
        }
        }
    }
    ScopedValue n(scope, name);
    return engine()->throwReferenceError(n);
}

ReturnedValue ExecutionContext::getPropertyAndBase(String *name, Value *base)
{
    Scope scope(this);
    ScopedValue v(scope);
    base->setM(0);
    name->makeIdentifier();

    if (name->equals(engine()->id_this()))
        return thisObject().asReturnedValue();

    ScopedContext ctx(scope, this);
    for (; ctx; ctx = ctx->d()->outer) {
        switch (ctx->d()->type) {
        case Heap::ExecutionContext::Type_CatchContext: {
            Heap::CatchContext *c = static_cast<Heap::CatchContext *>(ctx->d());
            if (c->exceptionVarName->isEqualTo(name->d()))
                return c->exceptionValue.asReturnedValue();
            break;
        }
        case Heap::ExecutionContext::Type_CallContext: {
            Heap::CallContext *c = static_cast<Heap::CallContext *>(ctx->d());
            name->makeIdentifier();
            Identifier *id = name->identifier();

            uint index = c->internalClass->find(id);
            if (index < UINT_MAX) {
                if (index < c->v4Function->nFormals)
                    return c->callData->args[c->v4Function->nFormals - index - 1].asReturnedValue();
                return c->locals[index - c->v4Function->nFormals].asReturnedValue();
            }
            if (c->v4Function->isNamedExpression()) {
                if (c->function && name->equals(ScopedString(scope, c->v4Function->name())))
                    return c->function->asReturnedValue();
            }
            Q_FALLTHROUGH();
        }
        case Heap::ExecutionContext::Type_GlobalContext:
        case Heap::ExecutionContext::Type_SimpleCallContext: {
            ScopedObject activation(scope, ctx->d()->activation);
            if (activation) {
                bool hasProperty = false;
                v = activation->get(name, &hasProperty);
                if (hasProperty)
                    return v->asReturnedValue();
            }
            break;
        }
        case Heap::ExecutionContext::Type_WithContext:
        case Heap::ExecutionContext::Type_QmlContext: {
            ScopedObject o(scope, ctx->d()->activation);
            bool hasProperty = false;
            v = o->get(name, &hasProperty);
            if (hasProperty) {
                base->setM(o->d());
                return v->asReturnedValue();
            }
            break;
        }
        }
    }
    ScopedValue n(scope, name);
    return engine()->throwReferenceError(n);
}

Function *ExecutionContext::getFunction() const
{
    Scope scope(engine());
    ScopedContext it(scope, this->d());
    for (; it; it = it->d()->outer) {
        if (const CallContext *callCtx = it->asCallContext())
            return callCtx->d()->v4Function;
        else if (it->d()->type == Heap::ExecutionContext::Type_CatchContext ||
                 it->d()->type == Heap::ExecutionContext::Type_WithContext)
            continue; // look in the parent context for a FunctionObject
        else
            break;
    }

    return 0;
}
