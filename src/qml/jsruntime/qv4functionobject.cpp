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

#include "qv4object_p.h"
#include "qv4objectproto_p.h"
#include "qv4stringobject_p.h"
#include "qv4function_p.h"
#include "qv4symbol_p.h"
#include <private/qv4mm_p.h>

#include "qv4arrayobject_p.h"
#include "qv4scopedvalue_p.h"
#include "qv4argumentsobject_p.h"

#include <private/qqmljsengine_p.h>
#include <private/qqmljslexer_p.h>
#include <private/qqmljsparser_p.h>
#include <private/qqmljsast_p.h>
#include <private/qqmljavascriptexpression_p.h>
#include <private/qqmlengine_p.h>
#include <qv4runtimecodegen_p.h>
#include "private/qlocale_tools_p.h"
#include "private/qqmlbuiltinfunctions_p.h"
#include <private/qv4jscall_p.h>

#include <QtCore/QDebug>
#include <algorithm>
#include "qv4alloca_p.h"
#include "qv4profiling_p.h"

using namespace QV4;


DEFINE_OBJECT_VTABLE(FunctionObject);

void Heap::FunctionObject::init(QV4::ExecutionContext *scope, QV4::String *name,
                                ReturnedValue (*code)(const QV4::FunctionObject *, const Value *thisObject, const Value *argv, int argc))
{
    jsCall = code;
    jsConstruct = QV4::FunctionObject::callAsConstructor;

    Object::init();
    this->scope.set(scope->engine(), scope->d());
    Scope s(scope->engine());
    ScopedFunctionObject f(s, this);
    if (name)
        f->setName(name);
}

void Heap::FunctionObject::init(QV4::ExecutionContext *scope, QV4::String *name, bool createProto)
{
    jsCall = vtable()->call;
    jsConstruct = vtable()->callAsConstructor;

    Object::init();
    this->scope.set(scope->engine(), scope->d());
    Scope s(scope->engine());
    ScopedFunctionObject f(s, this);
    if (name)
        f->setName(name);

    if (createProto)
        f->createDefaultPrototypeProperty(Heap::FunctionObject::Index_Prototype, Heap::FunctionObject::Index_ProtoConstructor);
}



void Heap::FunctionObject::init(QV4::ExecutionContext *scope, Function *function, bool createProto)
{
    jsCall = vtable()->call;
    jsConstruct = vtable()->callAsConstructor;

    Object::init();
    setFunction(function);
    this->scope.set(scope->engine(), scope->d());
    Scope s(scope->engine());
    ScopedString name(s, function->name());
    ScopedFunctionObject f(s, this);
    if (name)
        f->setName(name);

    if (createProto)
        f->createDefaultPrototypeProperty(Heap::FunctionObject::Index_Prototype, Heap::FunctionObject::Index_ProtoConstructor);
}

void Heap::FunctionObject::init(QV4::ExecutionContext *scope, const QString &name, bool createProto)
{
    Scope valueScope(scope);
    ScopedString s(valueScope, valueScope.engine->newString(name));
    init(scope, s, createProto);
}

void Heap::FunctionObject::init()
{
    jsCall = vtable()->call;
    jsConstruct = vtable()->callAsConstructor;

    Object::init();
    this->scope.set(internalClass->engine, internalClass->engine->rootContext()->d());
    Q_ASSERT(internalClass && internalClass->find(internalClass->engine->id_prototype()->propertyKey()) == Index_Prototype);
    setProperty(internalClass->engine, Index_Prototype, Primitive::undefinedValue());
}

void Heap::FunctionObject::setFunction(Function *f)
{
    if (f) {
        function = f;
        function->compilationUnit->addref();
    }
}
void Heap::FunctionObject::destroy()
{
    if (function)
        function->compilationUnit->release();
    Object::destroy();
}

void FunctionObject::createDefaultPrototypeProperty(uint protoSlot, uint protoConstructorSlot)
{
    Scope s(this);

    Q_ASSERT(internalClass() && internalClass()->find(s.engine->id_prototype()->propertyKey()) == protoSlot);
    Q_ASSERT(s.engine->internalClasses(EngineBase::Class_ObjectProto)->find(s.engine->id_constructor()->propertyKey()) == protoConstructorSlot);

    ScopedObject proto(s, s.engine->newObject(s.engine->internalClasses(EngineBase::Class_ObjectProto)));
    proto->setProperty(protoConstructorSlot, d());
    setProperty(protoSlot, proto);
}

ReturnedValue FunctionObject::name() const
{
    return get(scope()->internalClass->engine->id_name());
}

ReturnedValue FunctionObject::callAsConstructor(const FunctionObject *f, const Value *, int)
{
    return f->engine()->throwTypeError();
}

ReturnedValue FunctionObject::call(const FunctionObject *, const Value *, const Value *, int)
{
    return Encode::undefined();
}

Heap::FunctionObject *FunctionObject::createScriptFunction(ExecutionContext *scope, Function *function)
{
    return scope->engine()->memoryManager->allocate<ScriptFunction>(scope, function);
}

Heap::FunctionObject *FunctionObject::createConstructorFunction(ExecutionContext *scope, Function *function)
{
    return scope->engine()->memoryManager->allocate<ConstructorFunction>(scope, function);
}

Heap::FunctionObject *FunctionObject::createMemberFunction(ExecutionContext *scope, Function *function)
{
    return scope->engine()->memoryManager->allocate<MemberFunction>(scope, function);
}

Heap::FunctionObject *FunctionObject::createBuiltinFunction(ExecutionEngine *engine, StringOrSymbol *nameOrSymbol, VTable::Call code, int argumentCount)
{
    Scope scope(engine);
    ScopedString name(scope, nameOrSymbol);
    if (!name)
        name = engine->newString(QChar::fromLatin1('[') + nameOrSymbol->toQString().midRef(1) + QChar::fromLatin1(']'));

    ScopedFunctionObject function(scope, engine->memoryManager->allocate<FunctionObject>(engine->rootContext(), name, code));
    function->defineReadonlyConfigurableProperty(engine->id_length(), Primitive::fromInt32(argumentCount));
    return function->d();
}

bool FunctionObject::isBinding() const
{
    return d()->vtable() == QQmlBindingFunction::staticVTable();
}

bool FunctionObject::isBoundFunction() const
{
    return d()->vtable() == BoundFunction::staticVTable();
}

QQmlSourceLocation FunctionObject::sourceLocation() const
{
    return d()->function->sourceLocation();
}

DEFINE_OBJECT_VTABLE(FunctionCtor);

void Heap::FunctionCtor::init(QV4::ExecutionContext *scope)
{
    Heap::FunctionObject::init(scope, QStringLiteral("Function"));
}

// 15.3.2
QQmlRefPointer<CompiledData::CompilationUnit> FunctionCtor::parse(ExecutionEngine *engine, const Value *argv, int argc, Type t)
{
    QString arguments;
    QString body;
    if (argc > 0) {
        for (int i = 0, ei = argc - 1; i < ei; ++i) {
            if (i)
                arguments += QLatin1String(", ");
            arguments += argv[i].toQString();
        }
        body = argv[argc - 1].toQString();
    }
    if (engine->hasException)
        return nullptr;

    QString function = (t == Type_Function ? QLatin1String("function anonymous(") : QLatin1String("function* anonymous(")) + arguments + QLatin1String("\n){") + body + QLatin1String("\n}");

    QQmlJS::Engine ee;
    QQmlJS::Lexer lexer(&ee);
    lexer.setCode(function, 1, false);
    QQmlJS::Parser parser(&ee);

    const bool parsed = parser.parseExpression();

    if (!parsed) {
        engine->throwSyntaxError(QLatin1String("Parse error"));
        return nullptr;
    }

    QQmlJS::AST::FunctionExpression *fe = QQmlJS::AST::cast<QQmlJS::AST::FunctionExpression *>(parser.rootNode());
    if (!fe) {
        engine->throwSyntaxError(QLatin1String("Parse error"));
        return nullptr;
    }

    Compiler::Module module(engine->debugger() != nullptr);

    Compiler::JSUnitGenerator jsGenerator(&module);
    RuntimeCodegen cg(engine, &jsGenerator, false);
    cg.generateFromFunctionExpression(QString(), function, fe, &module);

    if (engine->hasException)
        return nullptr;

    return cg.generateCompilationUnit();
}

ReturnedValue FunctionCtor::callAsConstructor(const FunctionObject *f, const Value *argv, int argc)
{
    ExecutionEngine *engine = f->engine();

    QQmlRefPointer<CompiledData::CompilationUnit> compilationUnit = parse(engine, argv, argc, Type_Function);
    if (engine->hasException)
        return Encode::undefined();

    Function *vmf = compilationUnit->linkToEngine(engine);
    ExecutionContext *global = engine->scriptContext();
    return Encode(FunctionObject::createScriptFunction(global, vmf));
}

// 15.3.1: This is equivalent to new Function(...)
ReturnedValue FunctionCtor::call(const FunctionObject *f, const Value *, const Value *argv, int argc)
{
    return callAsConstructor(f, argv, argc);
}

DEFINE_OBJECT_VTABLE(FunctionPrototype);

void Heap::FunctionPrototype::init()
{
    Heap::FunctionObject::init();
}

void FunctionPrototype::init(ExecutionEngine *engine, Object *ctor)
{
    Scope scope(engine);
    ScopedObject o(scope);

    ctor->defineReadonlyConfigurableProperty(engine->id_length(), Primitive::fromInt32(1));
    ctor->defineReadonlyProperty(engine->id_prototype(), (o = this));

    defineReadonlyConfigurableProperty(engine->id_name(), *engine->id_empty());
    defineReadonlyConfigurableProperty(engine->id_length(), Primitive::fromInt32(0));
    defineDefaultProperty(QStringLiteral("constructor"), (o = ctor));
    defineDefaultProperty(engine->id_toString(), method_toString, 0);
    defineDefaultProperty(QStringLiteral("apply"), method_apply, 2);
    defineDefaultProperty(QStringLiteral("call"), method_call, 1);
    defineDefaultProperty(QStringLiteral("bind"), method_bind, 1);
    defineDefaultProperty(engine->symbol_hasInstance(), method_hasInstance, 1, Attr_ReadOnly);
}

ReturnedValue FunctionPrototype::method_toString(const FunctionObject *b, const Value *thisObject, const Value *, int)
{
    ExecutionEngine *v4 = b->engine();
    const FunctionObject *fun = thisObject->as<FunctionObject>();
    if (!fun)
        return v4->throwTypeError();

    const Scope scope(fun->engine());
    const ScopedString scopedFunctionName(scope, fun->name());
    const QString functionName(scopedFunctionName ? scopedFunctionName->toQString() : QString());
    QString functionAsString = QStringLiteral("function");

    // If fun->name() is empty, then there is no function name
    // to append because the function is anonymous.
    if (!functionName.isEmpty())
        functionAsString.append(QLatin1Char(' ') + functionName);

    functionAsString.append(QStringLiteral("() { [code] }"));

    return Encode(v4->newString(functionAsString));
}

ReturnedValue FunctionPrototype::method_apply(const QV4::FunctionObject *b, const Value *thisObject, const Value *argv, int argc)
{
    ExecutionEngine *v4 = b->engine();
    const FunctionObject *f = thisObject->as<FunctionObject>();
    if (!f)
        return v4->throwTypeError();
    thisObject = argc ? argv : nullptr;
    if (argc < 2 || argv[1].isNullOrUndefined())
        return f->call(thisObject, argv, 0);

    Object *arr = argv[1].objectValue();
    if (!arr)
        return v4->throwTypeError();

    uint len = arr->getLength();

    Scope scope(v4);
    Value *arguments = scope.alloc(len);
    if (len) {
        if (ArgumentsObject::isNonStrictArgumentsObject(arr) && !arr->cast<ArgumentsObject>()->fullyCreated()) {
            QV4::ArgumentsObject *a = arr->cast<ArgumentsObject>();
            int l = qMin(len, (uint)a->d()->context->argc());
            memcpy(arguments, a->d()->context->args(), l*sizeof(Value));
            for (quint32 i = l; i < len; ++i)
                arguments[i] = Primitive::undefinedValue();
        } else if (arr->arrayType() == Heap::ArrayData::Simple && !arr->protoHasArray()) {
            auto sad = static_cast<Heap::SimpleArrayData *>(arr->arrayData());
            uint alen = sad ? sad->values.size : 0;
            if (alen > len)
                alen = len;
            for (uint i = 0; i < alen; ++i)
                arguments[i] = sad->data(i);
            for (quint32 i = alen; i < len; ++i)
                arguments[i] = Primitive::undefinedValue();
        } else {
            for (quint32 i = 0; i < len; ++i)
                arguments[i] = arr->get(i);
        }
    }

    return f->call(thisObject, arguments, len);
}

ReturnedValue FunctionPrototype::method_call(const QV4::FunctionObject *b, const Value *thisObject, const Value *argv, int argc)
{
    if (!thisObject->isFunctionObject())
        return b->engine()->throwTypeError();

    const FunctionObject *f = static_cast<const FunctionObject *>(thisObject);

    thisObject = argc ? argv : nullptr;
    if (argc) {
        ++argv;
        --argc;
    }
    return f->call(thisObject, argv, argc);
}

ReturnedValue FunctionPrototype::method_bind(const FunctionObject *b, const Value *thisObject, const Value *argv, int argc)
{
    QV4::Scope scope(b);
    ScopedFunctionObject target(scope, thisObject);
    if (!target || target->isBinding())
        return scope.engine->throwTypeError();

    ScopedValue boundThis(scope, argc ? argv[0] : Primitive::undefinedValue());
    Scoped<MemberData> boundArgs(scope, (Heap::MemberData *)nullptr);

    int nArgs = (argc - 1 >= 0) ? argc - 1 : 0;
    if (target->isBoundFunction()) {
        BoundFunction *bound = static_cast<BoundFunction *>(target.getPointer());
        Scoped<MemberData> oldArgs(scope, bound->boundArgs());
        boundThis = bound->boundThis();
        int oldSize = !oldArgs ? 0 : oldArgs->size();
        if (oldSize + nArgs) {
            boundArgs = MemberData::allocate(scope.engine, oldSize + nArgs);
            boundArgs->d()->values.size = oldSize + nArgs;
            for (uint i = 0; i < static_cast<uint>(oldSize); ++i)
                boundArgs->set(scope.engine, i, oldArgs->data()[i]);
            for (uint i = 0; i < static_cast<uint>(nArgs); ++i)
                boundArgs->set(scope.engine, oldSize + i, argv[i + 1]);
        }
        target = bound->target();
    } else if (nArgs) {
        boundArgs = MemberData::allocate(scope.engine, nArgs);
        boundArgs->d()->values.size = nArgs;
        for (uint i = 0, ei = static_cast<uint>(nArgs); i < ei; ++i)
            boundArgs->set(scope.engine, i, argv[i + 1]);
    }

    ScopedContext ctx(scope, target->scope());
    Heap::BoundFunction *bound = BoundFunction::create(ctx, target, boundThis, boundArgs);
    bound->setFunction(target->function());
    return bound->asReturnedValue();
}

ReturnedValue FunctionPrototype::method_hasInstance(const FunctionObject *f, const Value *thisObject, const Value *argv, int argc)
{
    if (!argc)
        return false;
    const Object *o = thisObject->as<Object>();
    if (!o)
        return f->engine()->throwTypeError();

    return Object::instanceOf(o, argv[0]);
}

DEFINE_OBJECT_VTABLE(ScriptFunction);

ReturnedValue ScriptFunction::callAsConstructor(const FunctionObject *fo, const Value *argv, int argc)
{
    ExecutionEngine *v4 = fo->engine();
    const ScriptFunction *f = static_cast<const ScriptFunction *>(fo);

    Scope scope(v4);
    ScopedValue thisObject(scope, v4->memoryManager->allocObject<Object>(f->classForConstructor()));

    ReturnedValue result = Moth::VME::exec(fo, thisObject, argv, argc);

    if (Q_UNLIKELY(v4->hasException))
        return Encode::undefined();
    else if (!Value::fromReturnedValue(result).isObject())
        return thisObject->asReturnedValue();
    return result;
}

ReturnedValue ScriptFunction::call(const FunctionObject *fo, const Value *thisObject, const Value *argv, int argc)
{
    return Moth::VME::exec(fo, thisObject, argv, argc);
}

void Heap::ScriptFunction::init(QV4::ExecutionContext *scope, Function *function)
{
    FunctionObject::init();
    this->scope.set(scope->engine(), scope->d());

    setFunction(function);
    Q_ASSERT(function);

    Scope s(scope);
    ScopedFunctionObject f(s, this);

    ScopedString name(s, function->name());
    if (name)
        f->setName(name);
    f->createDefaultPrototypeProperty(Heap::FunctionObject::Index_Prototype, Heap::FunctionObject::Index_ProtoConstructor);

    Q_ASSERT(internalClass && internalClass->find(s.engine->id_length()->propertyKey()) == Index_Length);
    setProperty(s.engine, Index_Length, Primitive::fromInt32(int(function->compiledFunction->length)));
}

Heap::InternalClass *ScriptFunction::classForConstructor() const
{
    const Object *o = d()->protoProperty();
    if (d()->cachedClassForConstructor && d()->cachedClassForConstructor->prototype == o->d())
        return d()->cachedClassForConstructor;

    Scope scope(engine());
    Scoped<InternalClass> ic(scope, engine()->internalClasses(EngineBase::Class_Object));
    if (o)
        ic = ic->changePrototype(o->d());
    d()->cachedClassForConstructor.set(scope.engine, ic->d());

    return ic->d();
}

DEFINE_OBJECT_VTABLE(ConstructorFunction);

ReturnedValue ConstructorFunction::call(const FunctionObject *f, const Value *, const Value *, int)
{
    return f->engine()->throwTypeError(QStringLiteral("Cannot call a class constructor without |new|"));
}

DEFINE_OBJECT_VTABLE(MemberFunction);

ReturnedValue MemberFunction::callAsConstructor(const FunctionObject *f, const Value *, int)
{
    return f->engine()->throwTypeError(QStringLiteral("Function is not a constructor."));
}

DEFINE_OBJECT_VTABLE(DefaultClassConstructorFunction);

ReturnedValue DefaultClassConstructorFunction::callAsConstructor(const FunctionObject *f, const Value *, int)
{
    Scope scope(f);
    ScopedObject proto(scope, f->get(scope.engine->id_prototype()));
    ScopedObject c(scope, scope.engine->newObject());
    c->setPrototypeUnchecked(proto);
    return c->asReturnedValue();
}

ReturnedValue DefaultClassConstructorFunction::call(const FunctionObject *f, const Value *, const Value *, int)
{
    return f->engine()->throwTypeError(QStringLiteral("Cannot call a class constructor without |new|"));
}

DEFINE_OBJECT_VTABLE(IndexedBuiltinFunction);

DEFINE_OBJECT_VTABLE(BoundFunction);

void Heap::BoundFunction::init(QV4::ExecutionContext *scope, QV4::FunctionObject *target,
                               const Value &boundThis, QV4::MemberData *boundArgs)
{
    Scope s(scope);
    Heap::FunctionObject::init(scope, QStringLiteral("__bound function__"));
    this->target.set(s.engine, target->d());
    this->boundArgs.set(s.engine, boundArgs ? boundArgs->d() : nullptr);
    this->boundThis.set(scope->engine(), boundThis);

    ScopedObject f(s, this);

    ScopedValue l(s, target->get(s.engine->id_length()));
    int len = l->toUInt32();
    if (boundArgs)
        len -= boundArgs->size();
    if (len < 0)
        len = 0;
    f->defineReadonlyConfigurableProperty(s.engine->id_length(), Primitive::fromInt32(len));

    ScopedProperty pd(s);
    pd->value = s.engine->thrower();
    pd->set = s.engine->thrower();
    f->insertMember(s.engine->id_arguments(), pd, Attr_Accessor|Attr_NotConfigurable|Attr_NotEnumerable);
    f->insertMember(s.engine->id_caller(), pd, Attr_Accessor|Attr_NotConfigurable|Attr_NotEnumerable);
}

ReturnedValue BoundFunction::call(const FunctionObject *fo, const Value *, const Value *argv, int argc)
{
    const BoundFunction *f = static_cast<const BoundFunction *>(fo);
    Scope scope(f->engine());

    if (scope.hasException())
        return Encode::undefined();

    Scoped<MemberData> boundArgs(scope, f->boundArgs());
    ScopedFunctionObject target(scope, f->target());
    JSCallData jsCallData(scope, (boundArgs ? boundArgs->size() : 0) + argc);
    *jsCallData->thisObject = f->boundThis();
    Value *argp = jsCallData->args;
    if (boundArgs) {
        memcpy(argp, boundArgs->data(), boundArgs->size()*sizeof(Value));
        argp += boundArgs->size();
    }
    memcpy(argp, argv, argc*sizeof(Value));
    return target->call(jsCallData);
}

ReturnedValue BoundFunction::callAsConstructor(const FunctionObject *fo, const Value *argv, int argc)
{
    const BoundFunction *f = static_cast<const BoundFunction *>(fo);
    Scope scope(f->engine());

    if (scope.hasException())
        return Encode::undefined();

    Scoped<MemberData> boundArgs(scope, f->boundArgs());
    ScopedFunctionObject target(scope, f->target());
    JSCallData jsCallData(scope, (boundArgs ? boundArgs->size() : 0) + argc);
    Value *argp = jsCallData->args;
    if (boundArgs) {
        memcpy(argp, boundArgs->data(), boundArgs->size()*sizeof(Value));
        argp += boundArgs->size();
    }
    memcpy(argp, argv, argc*sizeof(Value));
    return target->callAsConstructor(jsCallData);
}
