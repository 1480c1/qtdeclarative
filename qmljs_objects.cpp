/****************************************************************************
**
** Copyright (C) 2012 Digia Plc and/or its subsidiary(-ies).
** Contact: http://www.qt-project.org/legal
**
** This file is part of the V4VM module of the Qt Toolkit.
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

#include "qmljs_objects.h"
#include "qv4ir_p.h"
#include "qv4isel_p.h"
#include "qv4ecmaobjects_p.h"
#include "qv4mm.h"

#include <private/qqmljsengine_p.h>
#include <private/qqmljslexer_p.h>
#include <private/qqmljsparser_p.h>
#include <private/qqmljsast_p.h>
#include <qv4ir_p.h>
#include <qv4codegen_p.h>

#include <QtCore/qmath.h>
#include <QtCore/QDebug>
#include <cassert>
#include <typeinfo>
#include <iostream>

using namespace QQmlJS::VM;


Managed::~Managed()
{
}

void *Managed::operator new(size_t size, MemoryManager *mm)
{
    assert(mm);

    return mm->allocManaged(size);
}

void Managed::operator delete(void *ptr)
{
    if (!ptr)
        return;

    Managed *m = reinterpret_cast<Managed *>(ptr);
    assert(m->mm);
    m->mm->deallocManaged(m);
}

//
// Object
//
Object::~Object()
{
    delete members;
}

void Object::__put__(ExecutionContext *ctx, const QString &name, const Value &value)
{
    __put__(ctx, ctx->engine->identifier(name), value);
}

Value Object::getValue(ExecutionContext *ctx, PropertyDescriptor *p) const
{
    if (p->isData())
        return p->value;
    if (!p->get)
        return Value::undefinedValue();

    return p->get->call(ctx, Value::fromObject(const_cast<Object *>(this)), 0, 0);
}

bool Object::inplaceBinOp(Value rhs, String *name, BinOp op, ExecutionContext *ctx)
{
    PropertyDescriptor to_fill;
    PropertyDescriptor *pd = __getPropertyDescriptor__(ctx, name, &to_fill);
    if (!pd)
        return false;
    Value result = op(getValue(ctx, pd), rhs, ctx);
    __put__(ctx, name, result);
    return true;
}

bool Object::inplaceBinOp(Value rhs, Value index, BinOp op, ExecutionContext *ctx)
{
    String *name = index.toString(ctx);
    assert(name);
    return inplaceBinOp(rhs, name, op, ctx);
}

void Object::defineDefaultProperty(String *name, Value value)
{
    if (!members)
        members = new PropertyTable();
    PropertyDescriptor *pd = members->insert(name);
    pd->type = PropertyDescriptor::Data;
    pd->writable = PropertyDescriptor::Set;
    pd->enumberable = PropertyDescriptor::Unset;
    pd->configurable = PropertyDescriptor::Set;
    pd->value = value;
}

void Object::defineDefaultProperty(ExecutionContext *context, const QString &name, Value value)
{
    defineDefaultProperty(context->engine->identifier(name), value);
}

void Object::defineDefaultProperty(ExecutionContext *context, const QString &name, Value (*code)(ExecutionContext *), int count)
{
    Q_UNUSED(count);
    // ### FIX count
    String *s = context->engine->identifier(name);
    defineDefaultProperty(s, Value::fromObject(context->engine->newNativeFunction(context, s, code)));
}

void Object::defineReadonlyProperty(ExecutionEngine *engine, const QString &name, Value value)
{
    if (!members)
        members = new PropertyTable();
    PropertyDescriptor *pd = members->insert(engine->identifier(name));
    pd->type = PropertyDescriptor::Data;
    pd->writable = PropertyDescriptor::Unset;
    pd->enumberable = PropertyDescriptor::Unset;
    pd->configurable = PropertyDescriptor::Unset;
    pd->value = value;
}

void Object::getCollectables(QVector<Object *> &objects)
{
    if (prototype)
        objects.append(prototype);

    if (members) {
        for (PropertyTable::iterator it = members->begin(), eit = members->end(); it < eit; ++it) {
            if ((*it)->descriptor.isData())
                if (Object *o = (*it)->descriptor.value.asObject())
                    objects.append(o);
        }
    }
}

// Section 8.12.1
PropertyDescriptor *Object::__getOwnProperty__(ExecutionContext *, String *name)
{
    if (members)
        return members->find(name);
    return 0;
}

// Section 8.12.2
PropertyDescriptor *Object::__getPropertyDescriptor__(ExecutionContext *ctx, String *name, PropertyDescriptor *to_fill)
{
    if (PropertyDescriptor *p = __getOwnProperty__(ctx, name))
        return p;

    if (prototype)
        return prototype->__getPropertyDescriptor__(ctx, name, to_fill);
    return 0;
}

// Section 8.12.3
Value Object::__get__(ExecutionContext *ctx, String *name)
{
    if (name->isEqualTo(ctx->engine->id___proto__))
        return Value::fromObject(prototype);

    PropertyDescriptor tmp;
    if (PropertyDescriptor *p = __getPropertyDescriptor__(ctx, name, &tmp))
        return getValue(ctx, p);

    return Value::undefinedValue();
}

// Section 8.12.4
bool Object::__canPut__(ExecutionContext *ctx, String *name)
{
    if (PropertyDescriptor *p = __getOwnProperty__(ctx, name)) {
        if (p->isAccessor())
            return p->set != 0;
        return p->isWritable();
    }

    if (! prototype)
        return extensible;

    PropertyDescriptor tmp;
    if (PropertyDescriptor *p = prototype->__getPropertyDescriptor__(ctx, name, &tmp)) {
        if (p->isAccessor())
            return p->set != 0;
        if (!extensible)
            return false;
        return p->isWritable();
    } else {
        return extensible;
    }
    return true;
}

// Section 8.12.5
void Object::__put__(ExecutionContext *ctx, String *name, Value value)
{
    // clause 1
    if (!__canPut__(ctx, name))
        goto reject;

    if (!members)
        members = new PropertyTable();

    {
        // Clause 2
        PropertyDescriptor *pd = __getOwnProperty__(ctx, name);
        // Clause 3
        if (pd && pd->isData()) {
            // spec says to call [[DefineOwnProperty]] with { [[Value]]: value }

            // ### to simplify and speed up we should expand the relevant parts here (clauses 6,7,9,10,12,13)
            PropertyDescriptor desc = PropertyDescriptor::fromValue(value);
            __defineOwnProperty__(ctx, name, &desc);
            return;
        }

        // clause 4
        PropertyDescriptor tmp;
        if (!pd && prototype)
            pd = prototype->__getPropertyDescriptor__(ctx, name, &tmp);

        // Clause 5
        if (pd && pd->isAccessor()) {
            assert(pd->set != 0);

            Value args[1];
            args[0] = value;
            pd->set->call(ctx, Value::fromObject(this), args, 1);
            return;
        }

        PropertyDescriptor *p = members->insert(name);
        *p = PropertyDescriptor::fromValue(value);
        p->configurable = PropertyDescriptor::Set;
        p->enumberable = PropertyDescriptor::Set;
        p->writable = PropertyDescriptor::Set;
        return;
    }

  reject:
    if (ctx->strictMode)
        __qmljs_throw_type_error(ctx);
}

// Section 8.12.6
bool Object::__hasProperty__(const ExecutionContext *ctx, String *name) const
{
    if (members && members->find(name) != 0)
        return true;

    return prototype ? prototype->__hasProperty__(ctx, name) : false;
}

// Section 8.12.7
bool Object::__delete__(ExecutionContext *ctx, String *name)
{
    if (members) {
        if (PropertyTableEntry *entry = members->findEntry(name)) {
            if (entry->descriptor.isConfigurable()) {
                members->remove(entry);
                return true;
            }
            if (ctx->strictMode)
                __qmljs_throw_type_error(ctx);
            return false;
        }
    }
    return true;
}

// Section 8.12.9
bool Object::__defineOwnProperty__(ExecutionContext *ctx, String *name, PropertyDescriptor *desc)
{
    if (!members)
        members = new PropertyTable();

    // Clause 1
    PropertyDescriptor *current = __getOwnProperty__(ctx, name);
    if (!current) {
        // clause 3
        if (!extensible)
            goto reject;
        // clause 4
        PropertyDescriptor *pd = members->insert(name);
        *pd = *desc;
        pd->fullyPopulated();
        return true;
    }

    // clause 5
    if (desc->isEmpty())
        return true;

    // clause 6
    if (desc->isSubset(current))
        return true;

    // clause 7
    if (!current->isConfigurable()) {
        if (desc->isConfigurable())
            goto reject;
        if (desc->enumberable != PropertyDescriptor::Undefined && desc->enumberable != current->enumberable)
            goto reject;
    }

    // clause 8
    if (desc->isGeneric())
        goto accept;

    // clause 9
    if (current->isData() != desc->isData()) {
        // 9a
        if (!current->isConfigurable())
            goto reject;
        if (current->isData()) {
            // 9b
            current->type = PropertyDescriptor::Accessor;
            current->writable = PropertyDescriptor::Undefined;
            current->get = 0;
            current->set = 0;
        } else {
            // 9c
            current->type = PropertyDescriptor::Data;
            current->writable = PropertyDescriptor::Unset;
            current->value = Value::undefinedValue();
        }
    } else if (current->isData() && desc->isData()) { // clause 10
        if (!current->isConfigurable() && !current->isWritable()) {
            if (desc->isWritable() || !current->value.sameValue(desc->value))
                goto reject;
        }
    } else { // clause 10
        assert(current->isAccessor() && desc->isAccessor());
        if (!current->isConfigurable()) {
            if ((desc->get && current->get != desc->get) ||
                (desc->set && current->set != desc->set))
                goto reject;
        }
    }

  accept:

    *current += *desc;
    return true;
  reject:
    qDebug() << "___put__ rejected" << name->toQString();
    if (ctx->strictMode)
        __qmljs_throw_type_error(ctx);
    return false;
}

bool Object::__defineOwnProperty__(ExecutionContext *ctx, const QString &name, PropertyDescriptor *desc)
{
    return __defineOwnProperty__(ctx, ctx->engine->identifier(name), desc);
}


Value Object::call(ExecutionContext *context, Value , Value *, int)
{
    context->throwTypeError();
    return Value::undefinedValue();
}

String *ForEachIteratorObject::nextPropertyName()
{
    PropertyTableEntry *p = 0;
    while (1) {
        if (!current)
            return 0;

        // ### index array data as well
        ++tableIndex;
        if (!current->members || tableIndex > current->members->_propertyCount) {
            current = current->prototype;
            tableIndex = -1;
            continue;
        }
        p = current->members->_properties[tableIndex];
        // ### check that it's not a repeated attribute
        if (p && p->descriptor.isEnumerable())
            return p->name;
    }
}

void ForEachIteratorObject::getCollectables(QVector<Object *> &objects)
{
    Object::getCollectables(objects);
    if (object)
        objects.append(object);
    if (current)
        objects.append(current);
}

Value ArrayObject::__get__(ExecutionContext *ctx, String *name)
{
    if (name->isEqualTo(ctx->engine->id_length))
        return Value::fromDouble(value.size());
    return Object::__get__(ctx, name);
}

bool ArrayObject::inplaceBinOp(Value rhs, Value index, BinOp op, ExecutionContext *ctx)
{
    if (index.isNumber()) {
        const quint32 idx = index.toUInt32(ctx);
        Value v = value.at(idx);
        v = op(v, rhs, ctx);
        value.assign(idx, v);
        return true;
    }
    return Object::inplaceBinOp(rhs, index, op, ctx);
}

void ArrayObject::getCollectables(QVector<Object *> &objects)
{
    Object::getCollectables(objects);
    value.getCollectables(objects);
}

Function::~Function()
{
    delete[] codeData;
}

bool FunctionObject::hasInstance(ExecutionContext *ctx, const Value &value)
{
    if (! value.isObject()) {
        ctx->throwTypeError();
        return false;
    }

    Value o = __get__(ctx, ctx->engine->id_prototype);
    if (! o.isObject()) {
        ctx->throwTypeError();
        return false;
    }

    Object *v = value.objectValue();
    while (v) {
        v = v->prototype;

        if (! v)
            break;
        else if (o.objectValue() == v)
            return true;
    }

    return false;
}

Value FunctionObject::construct(ExecutionContext *context, Value *args, int argc)
{
    ExecutionContext k;
    ExecutionContext *ctx = needsActivation ? context->engine->newContext() : &k;
    ctx->initCallContext(context, Value::undefinedValue(), this, args, argc);
    Value result = construct(ctx);
    ctx->wireUpPrototype();
    ctx->leaveCallContext();
    if (ctx != &k)
        delete ctx;
    return result;
}

Value FunctionObject::call(ExecutionContext *context, Value thisObject, Value *args, int argc)
{
    ExecutionContext k;
    ExecutionContext *ctx = needsActivation ? context->engine->newContext() : &k;

    ctx->initCallContext(context, thisObject, this, args, argc);
    Value result = call(ctx);
    ctx->leaveCallContext();
    return result;
}

Value FunctionObject::call(ExecutionContext *ctx)
{
    Q_UNUSED(ctx);
    return Value::undefinedValue();
}

Value FunctionObject::construct(ExecutionContext *ctx)
{
    ctx->thisObject = Value::fromObject(ctx->engine->newObject());
    call(ctx);
    return ctx->thisObject;
}

ScriptFunction::ScriptFunction(ExecutionContext *scope, VM::Function *function)
    : FunctionObject(scope)
    , function(function)
{
    assert(function);
    assert(function->code);

    // global function
    if (!scope)
        return;

    if (!function->name.isEmpty())
        name = scope->engine->identifier(function->name);
    needsActivation = function->needsActivation();
    strictMode = function->isStrict;
    formalParameterCount = function->formals.size();
    if (formalParameterCount) {
        formalParameterList = new String*[formalParameterCount];
        for (unsigned int i = 0; i < formalParameterCount; ++i) {
            formalParameterList[i] = scope->engine->identifier(function->formals.at(i));
        }
    }

    varCount = function->locals.size();
    if (varCount) {
        varList = new String*[varCount];
        for (unsigned int i = 0; i < varCount; ++i) {
            varList[i] = scope->engine->identifier(function->locals.at(i));
        }
    }
}

ScriptFunction::~ScriptFunction()
{
    delete[] formalParameterList;
    delete[] varList;
}

Value ScriptFunction::call(VM::ExecutionContext *ctx)
{
    assert(function->code);
    return function->code(ctx, function->codeData);
}


Value EvalFunction::call(ExecutionContext *context, Value /*thisObject*/, Value *args, int argc)
{
    if (argc < 1)
        return Value::undefinedValue();

    if (!args[0].isString())
        return args[0];

    // ### how to determine this correctly?
    bool directCall = true;

    const QString code = args[0].stringValue()->toQString();
    QQmlJS::VM::Function *f = parseSource(context, QStringLiteral("eval code"), code, QQmlJS::Codegen::EvalCode);
    if (!f)
        return Value::undefinedValue();

    bool strict = f->isStrict || context->strictMode;

    ExecutionContext k, *ctx;
    if (!directCall) {
        qDebug() << "!direct";
        // ###
    } else if (strict) {
        ctx = &k;
        ctx->initCallContext(context, context->thisObject, this, args, argc);
    } else {
        // use the surrounding context
        ctx = context;
    }

    // set the correct strict mode flag on the context
    bool cstrict = ctx->strictMode;
    ctx->strictMode = strict;

    Value result = f->code(ctx, f->codeData);

    ctx->strictMode = cstrict;

    if (strict)
        ctx->leaveCallContext();

    return result;
}

EvalFunction::EvalFunction(ExecutionContext *scope)
    : FunctionObject(scope)
{
    name = scope->engine->newString(QLatin1String("eval"));
}

QQmlJS::VM::Function *EvalFunction::parseSource(QQmlJS::VM::ExecutionContext *ctx,
                                                const QString &fileName, const QString &source,
                                                QQmlJS::Codegen::Mode mode)
{
    using namespace QQmlJS;

    MemoryManager::GCBlocker gcBlocker(ctx->engine->memoryManager);

    VM::ExecutionEngine *vm = ctx->engine;
    IR::Module module;
    VM::Function *globalCode = 0;

    {
        QQmlJS::Engine ee, *engine = &ee;
        Lexer lexer(engine);
        lexer.setCode(source, 1, false);
        Parser parser(engine);

        const bool parsed = parser.parseProgram();

        VM::DiagnosticMessage *error = 0, **errIt = &error;
        foreach (const QQmlJS::DiagnosticMessage &m, parser.diagnosticMessages()) {
            if (m.isError()) {
                *errIt = new VM::DiagnosticMessage;
                (*errIt)->fileName = fileName;
                (*errIt)->offset = m.loc.offset;
                (*errIt)->length = m.loc.length;
                (*errIt)->startLine = m.loc.startLine;
                (*errIt)->startColumn = m.loc.startColumn;
                (*errIt)->type = VM::DiagnosticMessage::Error;
                (*errIt)->message = m.message;
                errIt = &(*errIt)->next;
            } else {
                std::cerr << qPrintable(fileName) << ':' << m.loc.startLine << ':' << m.loc.startColumn
                          << ": warning: " << qPrintable(m.message) << std::endl;
            }
        }
        if (error)
            ctx->throwSyntaxError(error);

        if (parsed) {
            using namespace AST;
            Program *program = AST::cast<Program *>(parser.rootNode());
            if (!program) {
                // if parsing was successful, and we have no program, then
                // we're done...:
                return 0;
            }

            Codegen cg(ctx);
            IR::Function *globalIRCode = cg(fileName, program, &module, mode);
            QScopedPointer<EvalInstructionSelection> isel(ctx->engine->iselFactory->create(vm, &module));
            if (globalIRCode)
                globalCode = isel->vmFunction(globalIRCode);
        }

        if (! globalCode)
            // ### should be a syntax error
            __qmljs_throw_type_error(ctx);
    }

    return globalCode;
}


/// isNaN [15.1.2.4]
IsNaNFunction::IsNaNFunction(ExecutionContext *scope)
    : FunctionObject(scope)
{
    name = scope->engine->newString(QLatin1String("isNaN"));
}

Value IsNaNFunction::call(ExecutionContext *context, Value /*thisObject*/, Value *args, int /*argc*/)
{
    const Value &v = args[0];
    if (v.integerCompatible())
        return Value::fromBoolean(false);

    double d = v.toNumber(context);
    return Value::fromBoolean(std::isnan(d));
}

/// isFinite [15.1.2.5]
IsFiniteFunction::IsFiniteFunction(ExecutionContext *scope)
    : FunctionObject(scope)
{
    name = scope->engine->newString(QLatin1String("isFinite"));
}

Value IsFiniteFunction::call(ExecutionContext *context, Value /*thisObject*/, Value *args, int /*argc*/)
{
    const Value &v = args[0];
    if (v.integerCompatible())
        return Value::fromBoolean(true);

    double d = v.toNumber(context);
    return Value::fromBoolean(std::isfinite(d));
}


Value RegExpObject::__get__(ExecutionContext *ctx, String *name)
{
    QString n = name->toQString();
    if (n == QLatin1String("source"))
        return Value::fromString(ctx, value.pattern());
    else if (n == QLatin1String("global"))
        return Value::fromBoolean(global);
    else if (n == QLatin1String("ignoreCase"))
        return Value::fromBoolean(value.patternOptions() & QRegularExpression::CaseInsensitiveOption);
    else if (n == QLatin1String("multiline"))
        return Value::fromBoolean(value.patternOptions() & QRegularExpression::MultilineOption);
    else if (n == QLatin1String("lastIndex"))
        return lastIndex;
    return Object::__get__(ctx, name);
}

Value ErrorObject::__get__(ExecutionContext *ctx, String *name)
{
    QString n = name->toQString();
    if (n == QLatin1String("message"))
        return value;
    return Object::__get__(ctx, name);
}

void ErrorObject::setNameProperty(ExecutionContext *ctx)
{
    defineDefaultProperty(ctx, QLatin1String("name"), Value::fromString(ctx, className()));
}

void ErrorObject::getCollectables(QVector<Object *> &objects)
{
    Object::getCollectables(objects);
    if (Object *o = value.asObject())
        objects.append(o);
}

SyntaxErrorObject::SyntaxErrorObject(ExecutionContext *ctx, DiagnosticMessage *message)
    : ErrorObject(ctx->argument(0))
    , msg(message)
{
    if (message)
        value = Value::fromString(message->buildFullMessage(ctx));
    setNameProperty(ctx);
}


Value ScriptFunction::construct(VM::ExecutionContext *ctx)
{
    Object *obj = ctx->engine->newObject();
    Value proto = __get__(ctx, ctx->engine->id_prototype);
    if (proto.isObject())
        obj->prototype = proto.objectValue();
    ctx->thisObject = Value::fromObject(obj);
    function->code(ctx, function->codeData);
    return ctx->thisObject;
}

Value ArgumentsObject::__get__(ExecutionContext *ctx, String *name)
{
    if (name->isEqualTo(ctx->engine->id_length))
        return Value::fromInt32(context->argumentCount);
    return Object::__get__(ctx, name);
}

PropertyDescriptor *ArgumentsObject::__getPropertyDescriptor__(ExecutionContext *ctx, String *name, PropertyDescriptor *to_fill)
{
    if (context) {
        const quint32 i = Value::fromString(name).toUInt32(ctx);
        if (i < context->argumentCount) {
            *to_fill = PropertyDescriptor::fromValue(context->argument(i));
            to_fill->writable = PropertyDescriptor::Unset;
            to_fill->enumberable = PropertyDescriptor::Unset;
            return to_fill;
        }
    }

    return Object::__getPropertyDescriptor__(ctx, name, to_fill);
}

NativeFunction::NativeFunction(ExecutionContext *scope, String *name, Value (*code)(ExecutionContext *))
    : FunctionObject(scope)
    , code(code)
{
    this->name = name;
}
