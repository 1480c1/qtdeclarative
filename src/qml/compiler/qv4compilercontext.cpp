/****************************************************************************
**
** Copyright (C) 2017 The Qt Company Ltd.
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

#include "qv4compilercontext_p.h"
#include "qv4compilercontrolflow_p.h"
#include "qv4bytecodegenerator_p.h"

QT_USE_NAMESPACE
using namespace QV4;
using namespace QV4::Compiler;
using namespace QQmlJS::AST;

QT_BEGIN_NAMESPACE

Context *Module::newContext(Node *node, Context *parent, ContextType contextType)
{
    Q_ASSERT(!contextMap.contains(node));

    Context *c = new Context(parent, contextType);
    if (node) {
        SourceLocation loc = node->firstSourceLocation();
        c->line = loc.startLine;
        c->column = loc.startColumn;
    }

    contextMap.insert(node, c);

    if (!parent)
        rootContext = c;
    else {
        parent->nestedContexts.append(c);
        c->isStrict = parent->isStrict;
    }

    return c;
}

bool Context::addLocalVar(const QString &name, Context::MemberType type, VariableScope scope, FunctionExpression *function)
{
    // ### can this happen?
    if (name.isEmpty())
        return true;

    if (type != FunctionDefinition) {
        if (formals && formals->containsName(name))
            return (scope == VariableScope::Var);
    }
    if (!isCatchBlock || name != caughtVariable) {
        MemberMap::iterator it = members.find(name);
        if (it != members.end()) {
            if (scope != VariableScope::Var || (*it).scope != VariableScope::Var)
                return false;
            if ((*it).type <= type) {
                (*it).type = type;
                (*it).function = function;
            }
            return true;
        }
    }

    // hoist var declarations to the function level
    if (contextType == ContextType::Block && (scope == VariableScope::Var && type != MemberType::FunctionDefinition))
        return parent->addLocalVar(name, type, scope, function);

    Member m;
    m.type = type;
    m.function = function;
    m.scope = scope;
    members.insert(name, m);
    return true;
}

Context::ResolvedName Context::resolveName(const QString &name)
{
    int scope = 0;
    Context *c = this;

    ResolvedName result;

    while (c) {
        if (c->isWithBlock)
            return result;

        Context::Member m = c->findMember(name);
        if (!c->parent && m.index < 0)
            break;

        if (m.type != Context::UndefinedMember) {
            result.type = m.canEscape ? ResolvedName::Local : ResolvedName::Stack;
            result.scope = scope;
            result.index = m.index;
            result.isConst = (m.scope == VariableScope::Const);
            if (c->isStrict && (name == QLatin1String("arguments") || name == QLatin1String("eval")))
                result.isArgOrEval = true;
            return result;
        }
        const int argIdx = c->findArgument(name);
        if (argIdx != -1) {
            if (c->argumentsCanEscape) {
                result.index = argIdx + c->locals.size();
                result.scope = scope;
                result.type = ResolvedName::Local;
                result.isConst = false;
                return result;
            } else {
                result.index = argIdx + sizeof(CallData)/sizeof(Value) - 1;
                result.scope = 0;
                result.type = ResolvedName::Stack;
                result.isConst = false;
                return result;
            }
        }
        if (c->hasDirectEval) {
            Q_ASSERT(!c->isStrict && c->contextType != ContextType::Block);
            return result;
        }

        if (c->requiresExecutionContext)
            ++scope;
        c = c->parent;
    }

    if (c && c->contextType == ContextType::ESModule) {
        for (int i = 0; i < c->importEntries.count(); ++i) {
            if (c->importEntries.at(i).localName == name) {
                result.index = i;
                result.type = ResolvedName::Import;
                result.isConst = true;
                return result;
            }
        }
    }

    // ### can we relax the restrictions here?
    if (contextType == ContextType::Eval || c->contextType == ContextType::Binding)
        return result;

    result.type = ResolvedName::Global;
    return result;
}

void Context::emitBlockHeader(Codegen *codegen)
{
    using Instruction = Moth::Instruction;
    Moth::BytecodeGenerator *bytecodeGenerator = codegen->generator();

    setupFunctionIndices(bytecodeGenerator);

    if (requiresExecutionContext) {
        if (blockIndex < 0) {
            codegen->module()->blocks.append(this);
            blockIndex = codegen->module()->blocks.count() - 1;
        }

        if (contextType == ContextType::Global) {
            Instruction::PushScriptContext scriptContext;
            scriptContext.index = blockIndex;
            bytecodeGenerator->addInstruction(scriptContext);
        } else if (contextType == ContextType::Block || (contextType == ContextType::Eval && !isStrict)) {
            if (isCatchBlock) {
                Instruction::PushCatchContext catchContext;
                catchContext.index = blockIndex;
                catchContext.name = codegen->registerString(caughtVariable);
                bytecodeGenerator->addInstruction(catchContext);
            } else {
                Instruction::PushBlockContext blockContext;
                blockContext.index = blockIndex;
                bytecodeGenerator->addInstruction(blockContext);
            }
        } else {
            Instruction::CreateCallContext createContext;
            bytecodeGenerator->addInstruction(createContext);
        }
    }

    if (usesThis) {
        Q_ASSERT(!isStrict);
        // make sure we convert this to an object
        Instruction::ConvertThisToObject convert;
        bytecodeGenerator->addInstruction(convert);
    }

    if (contextType == ContextType::Global || (contextType == ContextType::Eval && !isStrict)) {
        // variables in global code are properties of the global context object, not locals as with other functions.
        for (Context::MemberMap::const_iterator it = members.constBegin(), cend = members.constEnd(); it != cend; ++it) {
            if (it->isLexicallyScoped())
                continue;
            const QString &local = it.key();

            Instruction::DeclareVar declareVar;
            declareVar.isDeletable = (contextType == ContextType::Eval);
            declareVar.varName = codegen->registerString(local);
            bytecodeGenerator->addInstruction(declareVar);
        }
    }

    if (contextType == ContextType::Function || contextType == ContextType::Binding || contextType == ContextType::ESModule) {
        for (Context::MemberMap::iterator it = members.begin(), end = members.end(); it != end; ++it) {
            if (it->canEscape && it->type == Context::ThisFunctionName) {
                // move the function from the stack to the call context
                Instruction::LoadReg load;
                load.reg = CallData::Function;
                bytecodeGenerator->addInstruction(load);
                Instruction::StoreLocal store;
                store.index = it->index;
                bytecodeGenerator->addInstruction(store);
            }
        }
    }

    if (usesArgumentsObject == Context::ArgumentsObjectUsed) {
        Q_ASSERT(contextType != ContextType::Block);
        if (isStrict || (formals && !formals->isSimpleParameterList())) {
            Instruction::CreateUnmappedArgumentsObject setup;
            bytecodeGenerator->addInstruction(setup);
        } else {
            Instruction::CreateMappedArgumentsObject setup;
            bytecodeGenerator->addInstruction(setup);
        }
        codegen->referenceForName(QStringLiteral("arguments"), false).storeConsumeAccumulator();
    }

    for (const Context::Member &member : qAsConst(members)) {
        if (member.function) {
            const int function = codegen->defineFunction(member.function->name.toString(), member.function, member.function->formals, member.function->body);
            codegen->loadClosure(function);
            Codegen::Reference r = codegen->referenceForName(member.function->name.toString(), true);
            r.storeConsumeAccumulator();
        }
    }
}

void Context::emitBlockFooter(Codegen *codegen)
{
    using Instruction = Moth::Instruction;
    Moth::BytecodeGenerator *bytecodeGenerator = codegen->generator();

    if (!requiresExecutionContext)
        return;

QT_WARNING_PUSH
QT_WARNING_DISABLE_GCC("-Wmaybe-uninitialized") // the loads below are empty structs.
    if (contextType == ContextType::Global)
        bytecodeGenerator->addInstruction(Instruction::PopScriptContext());
    else
        bytecodeGenerator->addInstruction(Instruction::PopContext());
QT_WARNING_POP
}

void Context::setupFunctionIndices(Moth::BytecodeGenerator *bytecodeGenerator)
{
    if (registerOffset != -1) {
        // already computed, check for consistency
        Q_ASSERT(registerOffset == bytecodeGenerator->currentRegister());
        bytecodeGenerator->newRegisterArray(nRegisters);
        return;
    }
    Q_ASSERT(locals.size() == 0);
    Q_ASSERT(nRegisters == 0);
    registerOffset = bytecodeGenerator->currentRegister();

    switch (contextType) {
    case ContextType::ESModule:
    case ContextType::Block:
    case ContextType::Function:
    case ContextType::Binding: {
        for (Context::MemberMap::iterator it = members.begin(), end = members.end(); it != end; ++it) {
            const QString &local = it.key();
            if (it->canEscape) {
                it->index = locals.size();
                locals.append(local);
            } else {
                if (it->type == Context::ThisFunctionName)
                    it->index = CallData::Function;
                else
                    it->index = bytecodeGenerator->newRegister();
            }
        }
        break;
    }
    case ContextType::Global:
    case ContextType::Eval:
        for (Context::MemberMap::iterator it = members.begin(), end = members.end(); it != end; ++it) {
            if (!it->isLexicallyScoped() && (contextType == ContextType::Global || !isStrict))
                continue;
            if (it->canEscape) {
                it->index = locals.size();
                locals.append(it.key());
            } else {
                it->index = bytecodeGenerator->newRegister();
            }
        }
        break;
    }
    nRegisters = bytecodeGenerator->currentRegister() - registerOffset;
}

QT_END_NAMESPACE
