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

#include "qv4codegen_p.h"
#include "qv4util_p.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QStringList>
#include <QtCore/QSet>
#include <QtCore/QBuffer>
#include <QtCore/QBitArray>
#include <QtCore/QLinkedList>
#include <QtCore/QStack>
#include <private/qqmljsast_p.h>
#include <private/qv4string_p.h>
#include <private/qv4value_p.h>
#include <private/qv4compilercontext_p.h>
#include <private/qv4compilercontrolflow_p.h>
#include <private/qv4bytecodegenerator_p.h>
#include <private/qv4compilerscanfunctions_p.h>

#include <cmath>
#include <iostream>

static const bool disable_lookups = false;

#ifdef CONST
#undef CONST
#endif

QT_USE_NAMESPACE
using namespace QV4;
using namespace QV4::Compiler;
using namespace QQmlJS::AST;

static inline void setJumpOutLocation(QV4::Moth::BytecodeGenerator *bytecodeGenerator,
                                      const Statement *body, const SourceLocation &fallback)
{
    switch (body->kind) {
    // Statements where we might never execute the last line.
    // Use the fallback.
    case Statement::Kind_ConditionalExpression:
    case Statement::Kind_ForEachStatement:
    case Statement::Kind_ForStatement:
    case Statement::Kind_IfStatement:
    case Statement::Kind_WhileStatement:
        bytecodeGenerator->setLocation(fallback);
        break;
    default:
        bytecodeGenerator->setLocation(body->lastSourceLocation());
        break;
    }
}

Codegen::Codegen(QV4::Compiler::JSUnitGenerator *jsUnitGenerator, bool strict)
    : _module(nullptr)
    , _returnAddress(-1)
    , _context(nullptr)
    , _labelledStatement(nullptr)
    , jsUnitGenerator(jsUnitGenerator)
    , _strictMode(strict)
    , _fileNameIsUrl(false)
    , hasError(false)
{
    jsUnitGenerator->codeGeneratorName = QStringLiteral("moth");
}

void Codegen::generateFromProgram(const QString &fileName,
                                  const QString &finalUrl,
                                  const QString &sourceCode,
                                  Program *node,
                                  Module *module,
                                  ContextType contextType)
{
    Q_ASSERT(node);

    _module = module;
    _context = nullptr;

    // ### should be set on the module outside of this method
    _module->fileName = fileName;
    _module->finalUrl = finalUrl;

    ScanFunctions scan(this, sourceCode, contextType);
    scan(node);

    if (hasError)
        return;

    defineFunction(QStringLiteral("%entry"), node, nullptr, node->statements);
}

void Codegen::enterContext(Node *node)
{
    _context = _module->contextMap.value(node);
    Q_ASSERT(_context);
}

int Codegen::leaveContext()
{
    Q_ASSERT(_context);
    int functionIndex = _context->functionIndex;
    _context = _context->parent;
    return functionIndex;
}

Context *Codegen::enterBlock(Node *node)
{
    enterContext(node);
    return _context;
}

Codegen::Reference Codegen::unop(UnaryOperation op, const Reference &expr)
{
    if (hasError)
        return _expr.result();

#ifndef V4_BOOTSTRAP
    if (expr.isConst()) {
        auto v = Value::fromReturnedValue(expr.constant);
        if (v.isNumber()) {
            switch (op) {
            case Not:
                return Reference::fromConst(this, Encode(!v.toBoolean()));
            case UMinus:
                return Reference::fromConst(this, Runtime::method_uMinus(v));
            case UPlus:
                return expr;
            case Compl:
                return Reference::fromConst(this, Encode((int)~v.toInt32()));
            default:
                break;
            }
        }
    }
#endif // V4_BOOTSTRAP

    switch (op) {
    case UMinus: {
        expr.loadInAccumulator();
        Instruction::UMinus uminus;
        bytecodeGenerator->addInstruction(uminus);
        return Reference::fromAccumulator(this);
    }
    case UPlus: {
        expr.loadInAccumulator();
        Instruction::UPlus uplus;
        bytecodeGenerator->addInstruction(uplus);
        return Reference::fromAccumulator(this);
    }
    case Not: {
        expr.loadInAccumulator();
        Instruction::UNot unot;
        bytecodeGenerator->addInstruction(unot);
        return Reference::fromAccumulator(this);
    }
    case Compl: {
        expr.loadInAccumulator();
        Instruction::UCompl ucompl;
        bytecodeGenerator->addInstruction(ucompl);
        return Reference::fromAccumulator(this);
    }
    case PostIncrement:
        if (!_expr.accept(nx) || requiresReturnValue) {
            Reference e = expr.asLValue();
            e.loadInAccumulator();
            Instruction::UPlus uplus;
            bytecodeGenerator->addInstruction(uplus);
            Reference originalValue = Reference::fromStackSlot(this).storeRetainAccumulator();
            Instruction::Increment inc;
            bytecodeGenerator->addInstruction(inc);
            e.storeConsumeAccumulator();
            return originalValue;
        } else {
            // intentionally fall-through: the result is never used, so it's equivalent to
            // "expr += 1", which is what a pre-increment does as well.
            Q_FALLTHROUGH();
        }
    case PreIncrement: {
        Reference e = expr.asLValue();
        e.loadInAccumulator();
        Instruction::Increment inc;
        bytecodeGenerator->addInstruction(inc);
        if (_expr.accept(nx))
            return e.storeConsumeAccumulator();
        else
            return e.storeRetainAccumulator();
    }
    case PostDecrement:
        if (!_expr.accept(nx) || requiresReturnValue) {
            Reference e = expr.asLValue();
            e.loadInAccumulator();
            Instruction::UPlus uplus;
            bytecodeGenerator->addInstruction(uplus);
            Reference originalValue = Reference::fromStackSlot(this).storeRetainAccumulator();
            Instruction::Decrement dec;
            bytecodeGenerator->addInstruction(dec);
            e.storeConsumeAccumulator();
            return originalValue;
        } else {
            // intentionally fall-through: the result is never used, so it's equivalent to
            // "expr -= 1", which is what a pre-decrement does as well.
            Q_FALLTHROUGH();
        }
    case PreDecrement: {
        Reference e = expr.asLValue();
        e.loadInAccumulator();
        Instruction::Decrement dec;
        bytecodeGenerator->addInstruction(dec);
        if (_expr.accept(nx))
            return e.storeConsumeAccumulator();
        else
            return e.storeRetainAccumulator();
    }
    }

    Q_UNREACHABLE();
}

void Codegen::addCJump()
{
    bytecodeGenerator->addCJumpInstruction(_expr.trueBlockFollowsCondition(),
                                           _expr.iftrue(), _expr.iffalse());
}

void Codegen::accept(Node *node)
{
    if (hasError)
        return;

    if (node)
        node->accept(this);
}

void Codegen::statement(Statement *ast)
{
    RegisterScope scope(this);

    bytecodeGenerator->setLocation(ast->firstSourceLocation());

    VolatileMemoryLocations vLocs = scanVolatileMemoryLocations(ast);
    qSwap(_volatileMemoryLocations, vLocs);
    accept(ast);
    qSwap(_volatileMemoryLocations, vLocs);
}

void Codegen::statement(ExpressionNode *ast)
{
    RegisterScope scope(this);

    if (! ast) {
        return;
    } else {
        Result r(nx);
        qSwap(_expr, r);
        VolatileMemoryLocations vLocs = scanVolatileMemoryLocations(ast);
        qSwap(_volatileMemoryLocations, vLocs);

        accept(ast);

        qSwap(_volatileMemoryLocations, vLocs);
        qSwap(_expr, r);

        if (hasError)
            return;
        if (r.result().loadTriggersSideEffect())
            r.result().loadInAccumulator(); // triggers side effects
    }
}

void Codegen::condition(ExpressionNode *ast, const BytecodeGenerator::Label *iftrue,
                        const BytecodeGenerator::Label *iffalse, bool trueBlockFollowsCondition)
{
    if (hasError)
        return;

    if (!ast)
        return;

    Result r(iftrue, iffalse, trueBlockFollowsCondition);
    qSwap(_expr, r);
    accept(ast);
    qSwap(_expr, r);

    if (hasError)
        return;

    if (r.format() == ex) {
        Q_ASSERT(iftrue == r.iftrue());
        Q_ASSERT(iffalse == r.iffalse());
        Q_ASSERT(r.result().isValid());
        bytecodeGenerator->setLocation(ast->firstSourceLocation());
        r.result().loadInAccumulator();
        if (r.trueBlockFollowsCondition())
            bytecodeGenerator->jumpFalse().link(*r.iffalse());
        else
            bytecodeGenerator->jumpTrue().link(*r.iftrue());
    }
}

Codegen::Reference Codegen::expression(ExpressionNode *ast)
{
    Result r;
    if (ast) {
        qSwap(_expr, r);
        accept(ast);
        qSwap(_expr, r);
    }
    return r.result();
}

void Codegen::program(Program *ast)
{
    if (ast) {
        statementList(ast->statements);
    }
}

enum class CompletionState {
    Empty,
    EmptyAbrupt,
    NonEmpty
};

static CompletionState completionState(StatementList *list)
{
    for (StatementList *it = list; it; it = it->next) {
        if (it->statement->kind == Statement::Kind_BreakStatement ||
            it->statement->kind == Statement::Kind_ContinueStatement)
            return CompletionState::EmptyAbrupt;
        if (it->statement->kind == Statement::Kind_EmptyStatement ||
            it->statement->kind == Statement::Kind_VariableDeclaration ||
            it->statement->kind == Statement::Kind_FunctionDeclaration)
            continue;
        if (it->statement->kind == Statement::Kind_Block) {
            CompletionState subState = completionState(static_cast<Block *>(it->statement)->statements);
            if (subState != CompletionState::Empty)
                return subState;
            continue;
        }
        return CompletionState::NonEmpty;
    }
    return CompletionState::Empty;
}

static Node *completionStatement(StatementList *list)
{
    Node *completionStatement = nullptr;
    for (StatementList *it = list; it; it = it->next) {
        if (it->statement->kind == Statement::Kind_BreakStatement ||
            it->statement->kind == Statement::Kind_ContinueStatement)
            return completionStatement;
        if (it->statement->kind == Statement::Kind_ThrowStatement ||
            it->statement->kind == Statement::Kind_ReturnStatement)
            return it->statement;
        if (it->statement->kind == Statement::Kind_EmptyStatement ||
            it->statement->kind == Statement::Kind_VariableStatement ||
            it->statement->kind == Statement::Kind_FunctionDeclaration)
            continue;
        if (it->statement->kind == Statement::Kind_Block) {
            CompletionState state = completionState(static_cast<Block *>(it->statement)->statements);
            switch (state) {
            case CompletionState::Empty:
                continue;
            case CompletionState::EmptyAbrupt:
                return it->statement;
            case CompletionState::NonEmpty:
                break;
            }
        }
        completionStatement = it->statement;
    }
    return completionStatement;
}

void Codegen::statementList(StatementList *ast)
{
    if (!ast)
        return;

    bool _requiresReturnValue = requiresReturnValue;
    // ### the next line is pessimizing a bit too much, as there are many cases, where the complietion from the break
    // statement will not be used, but it's at least spec compliant
    if (!controlFlow || !controlFlow->hasLoop())
        requiresReturnValue = false;

    Node *needsCompletion = nullptr;

    if (_requiresReturnValue && !requiresReturnValue)
        needsCompletion = completionStatement(ast);

    if (requiresReturnValue && !needsCompletion && !insideSwitch) {
        // break or continue is the first real statement, set the return value to undefined
        Reference::fromConst(this, Encode::undefined()).storeOnStack(_returnAddress);
    }

    bool _insideSwitch = insideSwitch;
    insideSwitch = false;

    for (StatementList *it = ast; it; it = it->next) {
        if (it->statement == needsCompletion)
            requiresReturnValue = true;
        if (Statement *s = it->statement->statementCast())
            statement(s);
        else
            statement(static_cast<ExpressionNode *>(it->statement));
        if (it->statement == needsCompletion)
            requiresReturnValue = false;
        if (it->statement->kind == Statement::Kind_ThrowStatement ||
            it->statement->kind == Statement::Kind_BreakStatement ||
            it->statement->kind == Statement::Kind_ContinueStatement ||
            it->statement->kind == Statement::Kind_ReturnStatement)
            // any code after those statements is unreachable
            break;
    }
    requiresReturnValue = _requiresReturnValue;
    insideSwitch = _insideSwitch;
}

void Codegen::variableDeclaration(PatternElement *ast)
{
    RegisterScope scope(this);

    if (!ast->initializer)
        return;
    initializeAndDestructureBindingElement(ast, Reference());
}

void Codegen::variableDeclarationList(VariableDeclarationList *ast)
{
    for (VariableDeclarationList *it = ast; it; it = it->next) {
        variableDeclaration(it->declaration);
    }
}

Codegen::Reference Codegen::targetForPatternElement(AST::PatternElement *p)
{
    if (!p->bindingIdentifier.isNull())
        return referenceForName(p->bindingIdentifier, true);
    if (!p->bindingTarget || p->destructuringPattern())
        return Codegen::Reference::fromStackSlot(this);
    Reference lhs = expression(p->bindingTarget);
    if (hasError)
        return lhs;
    lhs = lhs.asLValue();
    return lhs;
}

void Codegen::initializeAndDestructureBindingElement(AST::PatternElement *e, const Reference &base)
{
    Q_ASSERT(e->type == AST::PatternElement::Binding || e->type == AST::PatternElement::RestElement);
    RegisterScope scope(this);
    Reference baseRef = (base.isAccumulator()) ? base.storeOnStack() : base;
    Reference varToStore = targetForPatternElement(e);
    if (hasError)
        return;

    if (e->initializer) {
        if (!baseRef.isValid()) {
            // assignment
            Reference expr = expression(e->initializer);
            if (hasError)
                return;
            expr.loadInAccumulator();
            varToStore.storeConsumeAccumulator();
        } else if (baseRef == varToStore) {
            baseRef.loadInAccumulator();
            BytecodeGenerator::Jump jump = bytecodeGenerator->jumpNotUndefined();
            Reference expr = expression(e->initializer);
            if (hasError) {
                jump.link();
                return;
            }
            expr.loadInAccumulator();
            varToStore.storeConsumeAccumulator();
            jump.link();
        } else {
            baseRef.loadInAccumulator();
            BytecodeGenerator::Jump jump = bytecodeGenerator->jumpNotUndefined();
            Reference expr = expression(e->initializer);
            if (hasError) {
                jump.link();
                return;
            }
            expr.loadInAccumulator();
            jump.link();
            varToStore.storeConsumeAccumulator();
        }
    } else if (baseRef != varToStore && baseRef.isValid()) {
        baseRef.loadInAccumulator();
        varToStore.storeConsumeAccumulator();
    }
    Pattern *p = e->destructuringPattern();
    if (!p)
        return;

    if (!varToStore.isStackSlot())
        varToStore = varToStore.storeOnStack();
    if (PatternElementList *l = e->elementList()) {
        destructureElementList(varToStore, l);
    } else if (PatternPropertyList *p = e->propertyList()) {
        destructurePropertyList(varToStore, p);
    } else if (e->bindingTarget) {
        // empty binding pattern. For spec compatibility, try to coerce the argument to an object
        varToStore.loadInAccumulator();
        Instruction::ToObject toObject;
        bytecodeGenerator->addInstruction(toObject);
        return;
    }
}

void Codegen::destructurePropertyList(const Codegen::Reference &object, PatternPropertyList *bindingList)
{
    RegisterScope scope(this);

    for (PatternPropertyList *it = bindingList; it; it = it->next) {
        PatternProperty *p = it->property;
        RegisterScope scope(this);
        AST::ComputedPropertyName *cname = AST::cast<AST::ComputedPropertyName *>(p->name);
        Reference property;
        if (cname) {
            Reference computedName = expression(cname->expression);
            if (hasError)
                return;
            computedName = computedName.storeOnStack();
            property = Reference::fromSubscript(object, computedName).asLValue();
        } else {
            QString propertyName = p->name->asString();
            property = Reference::fromMember(object, propertyName);
        }
        initializeAndDestructureBindingElement(p, property);
        if (hasError)
            return;
    }
}

void Codegen::destructureElementList(const Codegen::Reference &array, PatternElementList *bindingList)
{
    RegisterScope scope(this);

    Reference iterator = Reference::fromStackSlot(this);
    Reference iteratorValue = Reference::fromStackSlot(this);
    Reference iteratorDone = Reference::fromStackSlot(this);

    array.loadInAccumulator();
    Instruction::GetIterator iteratorObjInstr;
    iteratorObjInstr.iterator = 1; // ForEachType::Of
    bytecodeGenerator->addInstruction(iteratorObjInstr);
    iterator.storeConsumeAccumulator();

    bool hadNext = false;
    bool hasRest = false;

    BytecodeGenerator::Label end = bytecodeGenerator->newLabel();

    for (PatternElementList *p = bindingList; p; p = p->next) {
        PatternElement *e = p->element;
        for (Elision *elision = p->elision; elision; elision = elision->next) {
            iterator.loadInAccumulator();
            Instruction::IteratorNext next;
            next.value = iteratorValue.stackSlot();
            bytecodeGenerator->addInstruction(next);
            hadNext = true;
            bool last = !elision->next && !e && !p->next;
            if (last)
                iteratorDone.storeConsumeAccumulator();
        }

        if (!e)
            continue;

        hadNext = true;
        RegisterScope scope(this);
        iterator.loadInAccumulator();

        if (e->type == PatternElement::RestElement) {
            bytecodeGenerator->addInstruction(Instruction::DestructureRestElement());
            initializeAndDestructureBindingElement(e, Reference::fromAccumulator(this));
            hasRest = true;
        } else {
            Instruction::IteratorNext next;
            next.value = iteratorValue.stackSlot();
            bytecodeGenerator->addInstruction(next);
            bool last = !p->next || (!p->next->elision && !p->next->element);
            if (last)
                iteratorDone.storeConsumeAccumulator();
            initializeAndDestructureBindingElement(e, iteratorValue);
            if (hasError) {
                end.link();
                return;
            }
        }
    }

    if (!hadNext) {
        Reference::storeConstOnStack(this, Encode(false), iteratorDone.stackSlot());
    }

    if (!hasRest) {
        iterator.loadInAccumulator();
        Instruction::IteratorClose close;
        close.done = iteratorDone.stackSlot();
        bytecodeGenerator->addInstruction(close);
    }

    end.link();
}

void Codegen::destructurePattern(Pattern *p, const Reference &rhs)
{
    RegisterScope scope(this);
    if (auto *o = AST::cast<ObjectPattern *>(p))
        destructurePropertyList(rhs, o->properties);
    else if (auto *a = AST::cast<ArrayPattern *>(p))
        destructureElementList(rhs, a->elements);
    else
        Q_UNREACHABLE();
}


bool Codegen::visit(ArgumentList *)
{
    Q_UNREACHABLE();
    return false;
}

bool Codegen::visit(CaseBlock *)
{
    Q_UNREACHABLE();
    return false;
}

bool Codegen::visit(CaseClause *)
{
    Q_UNREACHABLE();
    return false;
}

bool Codegen::visit(CaseClauses *)
{
    Q_UNREACHABLE();
    return false;
}

bool Codegen::visit(Catch *)
{
    Q_UNREACHABLE();
    return false;
}

bool Codegen::visit(DefaultClause *)
{
    Q_UNREACHABLE();
    return false;
}

bool Codegen::visit(Elision *)
{
    Q_UNREACHABLE();
    return false;
}

bool Codegen::visit(Finally *)
{
    Q_UNREACHABLE();
    return false;
}

bool Codegen::visit(FormalParameterList *)
{
    Q_UNREACHABLE();
    return false;
}

bool Codegen::visit(Program *)
{
    Q_UNREACHABLE();
    return false;
}

bool Codegen::visit(PatternElement *)
{
    Q_UNREACHABLE();
    return false;
}

bool Codegen::visit(PatternElementList *)
{
    Q_UNREACHABLE();
    return false;
}

bool Codegen::visit(PatternProperty *)
{
    Q_UNREACHABLE();
    return false;
}

bool Codegen::visit(PatternPropertyList *)
{
    Q_UNREACHABLE();
    return false;
}

bool Codegen::visit(StatementList *)
{
    Q_UNREACHABLE();
    return false;
}

bool Codegen::visit(UiArrayMemberList *)
{
    Q_UNREACHABLE();
    return false;
}

bool Codegen::visit(UiImport *)
{
    Q_UNREACHABLE();
    return false;
}

bool Codegen::visit(UiHeaderItemList *)
{
    Q_UNREACHABLE();
    return false;
}

bool Codegen::visit(UiPragma *)
{
    Q_UNREACHABLE();
    return false;
}

bool Codegen::visit(UiObjectInitializer *)
{
    Q_UNREACHABLE();
    return false;
}

bool Codegen::visit(UiObjectMemberList *)
{
    Q_UNREACHABLE();
    return false;
}

bool Codegen::visit(UiParameterList *)
{
    Q_UNREACHABLE();
    return false;
}

bool Codegen::visit(UiProgram *)
{
    Q_UNREACHABLE();
    return false;
}

bool Codegen::visit(UiQualifiedId *)
{
    Q_UNREACHABLE();
    return false;
}

bool Codegen::visit(UiQualifiedPragmaId *)
{
    Q_UNREACHABLE();
    return false;
}

bool Codegen::visit(VariableDeclarationList *)
{
    Q_UNREACHABLE();
    return false;
}

bool Codegen::visit(ClassExpression *ast)
{
    throwSyntaxError(ast->firstSourceLocation(), QLatin1String("Support for 'class' is unimplemented."));
    return false;
}

bool Codegen::visit(Expression *ast)
{
    if (hasError)
        return false;

    statement(ast->left);
    accept(ast->right);
    return false;
}

bool Codegen::visit(ArrayPattern *ast)
{
    if (hasError)
        return false;

    RegisterScope scope(this);

    int argc = 0;
    int args = -1;
    auto push = [this, &argc, &args](AST::ExpressionNode *arg) {
        int temp = bytecodeGenerator->newRegister();
        if (args == -1)
            args = temp;
        if (!arg) {
            auto c = Reference::fromConst(this, Primitive::emptyValue().asReturnedValue());
            (void) c.storeOnStack(temp);
        } else {
            RegisterScope scope(this);
            Reference r = expression(arg);
            if (hasError)
                return;
            (void) r.storeOnStack(temp);
        }
        ++argc;
    };

    for (PatternElementList *it = ast->elements; it; it = it->next) {
        for (Elision *elision = it->elision; elision; elision = elision->next)
            push(nullptr);

        PatternElement *e = it->element;
        if (!e)
            continue;
        if (e->type == PatternElement::RestElement) {
            throwSyntaxError(it->firstSourceLocation(), QLatin1String("'...' in ArrayLiterals is unimplementd."));
            return false;
        }

        push(e->initializer);
        if (hasError)
            return false;
    }

    if (args == -1) {
        Q_ASSERT(argc == 0);
        args = 0;
    }

    Instruction::DefineArray call;
    call.argc = argc;
    call.args = Moth::StackSlot::createRegister(args);
    bytecodeGenerator->addInstruction(call);
    _expr.setResult(Reference::fromAccumulator(this));

    return false;
}

bool Codegen::visit(ArrayMemberExpression *ast)
{
    if (hasError)
        return false;

    Reference base = expression(ast->base);
    if (hasError)
        return false;
    base = base.storeOnStack();
    if (AST::StringLiteral *str = AST::cast<AST::StringLiteral *>(ast->expression)) {
        QString s = str->value.toString();
        uint arrayIndex = QV4::String::toArrayIndex(s);
        if (arrayIndex == UINT_MAX) {
            _expr.setResult(Reference::fromMember(base, str->value.toString()));
            return false;
        }
        Reference index = Reference::fromConst(this, QV4::Encode(arrayIndex));
        _expr.setResult(Reference::fromSubscript(base, index));
        return false;
    }
    Reference index = expression(ast->expression);
    _expr.setResult(Reference::fromSubscript(base, index));
    return false;
}

static QSOperator::Op baseOp(int op)
{
    switch ((QSOperator::Op) op) {
    case QSOperator::InplaceAnd: return QSOperator::BitAnd;
    case QSOperator::InplaceSub: return QSOperator::Sub;
    case QSOperator::InplaceDiv: return QSOperator::Div;
    case QSOperator::InplaceAdd: return QSOperator::Add;
    case QSOperator::InplaceLeftShift: return QSOperator::LShift;
    case QSOperator::InplaceMod: return QSOperator::Mod;
    case QSOperator::InplaceExp: return QSOperator::Exp;
    case QSOperator::InplaceMul: return QSOperator::Mul;
    case QSOperator::InplaceOr: return QSOperator::BitOr;
    case QSOperator::InplaceRightShift: return QSOperator::RShift;
    case QSOperator::InplaceURightShift: return QSOperator::URShift;
    case QSOperator::InplaceXor: return QSOperator::BitXor;
    default: return QSOperator::Invalid;
    }
}

bool Codegen::visit(BinaryExpression *ast)
{
    if (hasError)
        return false;

    if (ast->op == QSOperator::And) {
        if (_expr.accept(cx)) {
            auto iftrue = bytecodeGenerator->newLabel();
            condition(ast->left, &iftrue, _expr.iffalse(), true);
            iftrue.link();
            condition(ast->right, _expr.iftrue(), _expr.iffalse(), _expr.trueBlockFollowsCondition());
        } else {
            auto iftrue = bytecodeGenerator->newLabel();
            auto endif = bytecodeGenerator->newLabel();

            Reference left = expression(ast->left);
            if (hasError)
                return false;
            left.loadInAccumulator();

            bytecodeGenerator->setLocation(ast->operatorToken);
            bytecodeGenerator->jumpFalse().link(endif);
            iftrue.link();

            Reference right = expression(ast->right);
            if (hasError)
                return false;
            right.loadInAccumulator();

            endif.link();

            _expr.setResult(Reference::fromAccumulator(this));
        }
        return false;
    } else if (ast->op == QSOperator::Or) {
        if (_expr.accept(cx)) {
            auto iffalse = bytecodeGenerator->newLabel();
            condition(ast->left, _expr.iftrue(), &iffalse, false);
            iffalse.link();
            condition(ast->right, _expr.iftrue(), _expr.iffalse(), _expr.trueBlockFollowsCondition());
        } else {
            auto iffalse = bytecodeGenerator->newLabel();
            auto endif = bytecodeGenerator->newLabel();

            Reference left = expression(ast->left);
            if (hasError)
                return false;
            left.loadInAccumulator();

            bytecodeGenerator->setLocation(ast->operatorToken);
            bytecodeGenerator->jumpTrue().link(endif);
            iffalse.link();

            Reference right = expression(ast->right);
            if (hasError)
                return false;
            right.loadInAccumulator();

            endif.link();

            _expr.setResult(Reference::fromAccumulator(this));
        }
        return false;
    } else if (ast->op == QSOperator::Assign) {
        if (AST::Pattern *p = ast->left->patternCast()) {
            RegisterScope scope(this);
            Reference right = expression(ast->right).storeOnStack();
            destructurePattern(p, right);
            if (!_expr.accept(nx)) {
                right.loadInAccumulator();
                _expr.setResult(Reference::fromAccumulator(this));
            }
            return false;
        }
        Reference left = expression(ast->left);
        if (hasError)
            return false;

        if (!left.isLValue()) {
            throwReferenceError(ast->operatorToken, QStringLiteral("left-hand side of assignment operator is not an lvalue"));
            return false;
        }
        left = left.asLValue();
        if (throwSyntaxErrorOnEvalOrArgumentsInStrictMode(left, ast->left->lastSourceLocation()))
            return false;
        Reference r = expression(ast->right);
        if (hasError)
            return false;
        r.loadInAccumulator();
        if (_expr.accept(nx))
            _expr.setResult(left.storeConsumeAccumulator());
        else
            _expr.setResult(left.storeRetainAccumulator());
        return false;
    }

    Reference left = expression(ast->left);
    if (hasError)
        return false;

    switch (ast->op) {
    case QSOperator::Or:
    case QSOperator::And:
    case QSOperator::Assign:
        Q_UNREACHABLE(); // handled separately above
        break;

    case QSOperator::InplaceAnd:
    case QSOperator::InplaceSub:
    case QSOperator::InplaceDiv:
    case QSOperator::InplaceAdd:
    case QSOperator::InplaceLeftShift:
    case QSOperator::InplaceMod:
    case QSOperator::InplaceExp:
    case QSOperator::InplaceMul:
    case QSOperator::InplaceOr:
    case QSOperator::InplaceRightShift:
    case QSOperator::InplaceURightShift:
    case QSOperator::InplaceXor: {
        if (throwSyntaxErrorOnEvalOrArgumentsInStrictMode(left, ast->left->lastSourceLocation()))
            return false;

        if (!left.isLValue()) {
            throwSyntaxError(ast->operatorToken, QStringLiteral("left-hand side of inplace operator is not an lvalue"));
            return false;
        }
        left = left.asLValue();

        Reference tempLeft = left.storeOnStack();
        Reference right = expression(ast->right);

        if (hasError)
            return false;

        binopHelper(baseOp(ast->op), tempLeft, right).loadInAccumulator();
        _expr.setResult(left.storeRetainAccumulator());

        break;
    }

    case QSOperator::BitAnd:
    case QSOperator::BitOr:
    case QSOperator::BitXor:
        if (left.isConst()) {
            Reference right = expression(ast->right);
            if (hasError)
                return false;
            _expr.setResult(binopHelper(static_cast<QSOperator::Op>(ast->op), right, left));
            break;
        }
        // intentional fall-through!
    case QSOperator::In:
    case QSOperator::InstanceOf:
    case QSOperator::Equal:
    case QSOperator::NotEqual:
    case QSOperator::Ge:
    case QSOperator::Gt:
    case QSOperator::Le:
    case QSOperator::Lt:
    case QSOperator::StrictEqual:
    case QSOperator::StrictNotEqual:
    case QSOperator::Add:
    case QSOperator::Div:
    case QSOperator::Exp:
    case QSOperator::Mod:
    case QSOperator::Mul:
    case QSOperator::Sub:
    case QSOperator::LShift:
    case QSOperator::RShift:
    case QSOperator::URShift: {
        Reference right;
        if (AST::NumericLiteral *rhs = AST::cast<AST::NumericLiteral *>(ast->right)) {
            visit(rhs);
            right = _expr.result();
        } else {
            left = left.storeOnStack(); // force any loads of the lhs, so the rhs won't clobber it
            right = expression(ast->right);
        }
        if (hasError)
            return false;

        _expr.setResult(binopHelper(static_cast<QSOperator::Op>(ast->op), left, right));

        break;
    }

    } // switch

    return false;
}

Codegen::Reference Codegen::binopHelper(QSOperator::Op oper, Reference &left, Reference &right)
{
    switch (oper) {
    case QSOperator::Add: {
        //### Todo: when we add type hints, we can generate an Increment when both the lhs is a number and the rhs == 1
        left = left.storeOnStack();
        right.loadInAccumulator();
        Instruction::Add add;
        add.lhs = left.stackSlot();
        bytecodeGenerator->addInstruction(add);
        break;
    }
    case QSOperator::Sub: {
        if (right.isConst() && right.constant == Encode(int(1))) {
            left.loadInAccumulator();
            bytecodeGenerator->addInstruction(Instruction::Decrement());
        } else {
            left = left.storeOnStack();
            right.loadInAccumulator();
            Instruction::Sub sub;
            sub.lhs = left.stackSlot();
            bytecodeGenerator->addInstruction(sub);
        }
        break;
    }
    case QSOperator::Exp: {
        left = left.storeOnStack();
        right.loadInAccumulator();
        Instruction::Exp exp;
        exp.lhs = left.stackSlot();
        bytecodeGenerator->addInstruction(exp);
        break;
    }
    case QSOperator::Mul: {
        left = left.storeOnStack();
        right.loadInAccumulator();
        Instruction::Mul mul;
        mul.lhs = left.stackSlot();
        bytecodeGenerator->addInstruction(mul);
        break;
    }
    case QSOperator::Div: {
        left = left.storeOnStack();
        right.loadInAccumulator();
        Instruction::Div div;
        div.lhs = left.stackSlot();
        bytecodeGenerator->addInstruction(div);
        break;
    }
    case QSOperator::Mod: {
        left = left.storeOnStack();
        right.loadInAccumulator();
        Instruction::Mod mod;
        mod.lhs = left.stackSlot();
        bytecodeGenerator->addInstruction(mod);
        break;
    }
    case QSOperator::BitAnd:
        if (right.isConst()) {
            int rightAsInt = Primitive::fromReturnedValue(right.constant).toInt32();
            if (left.isConst()) {
                int result = Primitive::fromReturnedValue(left.constant).toInt32() & rightAsInt;
                return Reference::fromConst(this, Encode(result));
            }
            left.loadInAccumulator();
            Instruction::BitAndConst bitAnd;
            bitAnd.rhs = rightAsInt;
            bytecodeGenerator->addInstruction(bitAnd);
        } else {
            right.loadInAccumulator();
            Instruction::BitAnd bitAnd;
            bitAnd.lhs = left.stackSlot();
            bytecodeGenerator->addInstruction(bitAnd);
        }
        break;
    case QSOperator::BitOr:
        if (right.isConst()) {
            int rightAsInt = Primitive::fromReturnedValue(right.constant).toInt32();
            if (left.isConst()) {
                int result = Primitive::fromReturnedValue(left.constant).toInt32() | rightAsInt;
                return Reference::fromConst(this, Encode(result));
            }
            left.loadInAccumulator();
            Instruction::BitOrConst bitOr;
            bitOr.rhs = rightAsInt;
            bytecodeGenerator->addInstruction(bitOr);
        } else {
            right.loadInAccumulator();
            Instruction::BitOr bitOr;
            bitOr.lhs = left.stackSlot();
            bytecodeGenerator->addInstruction(bitOr);
        }
        break;
    case QSOperator::BitXor:
        if (right.isConst()) {
            int rightAsInt = Primitive::fromReturnedValue(right.constant).toInt32();
            if (left.isConst()) {
                int result = Primitive::fromReturnedValue(left.constant).toInt32() ^ rightAsInt;
                return Reference::fromConst(this, Encode(result));
            }
            left.loadInAccumulator();
            Instruction::BitXorConst bitXor;
            bitXor.rhs = rightAsInt;
            bytecodeGenerator->addInstruction(bitXor);
        } else {
            right.loadInAccumulator();
            Instruction::BitXor bitXor;
            bitXor.lhs = left.stackSlot();
            bytecodeGenerator->addInstruction(bitXor);
        }
        break;
    case QSOperator::URShift:
        if (right.isConst()) {
            left.loadInAccumulator();
            Instruction::UShrConst ushr;
            ushr.rhs = Primitive::fromReturnedValue(right.constant).toInt32() & 0x1f;
            bytecodeGenerator->addInstruction(ushr);
        } else {
            right.loadInAccumulator();
            Instruction::UShr ushr;
            ushr.lhs = left.stackSlot();
            bytecodeGenerator->addInstruction(ushr);
        }
        break;
    case QSOperator::RShift:
        if (right.isConst()) {
            left.loadInAccumulator();
            Instruction::ShrConst shr;
            shr.rhs = Primitive::fromReturnedValue(right.constant).toInt32() & 0x1f;
            bytecodeGenerator->addInstruction(shr);
        } else {
            right.loadInAccumulator();
            Instruction::Shr shr;
            shr.lhs = left.stackSlot();
            bytecodeGenerator->addInstruction(shr);
        }
        break;
    case QSOperator::LShift:
        if (right.isConst()) {
            left.loadInAccumulator();
            Instruction::ShlConst shl;
            shl.rhs = Primitive::fromReturnedValue(right.constant).toInt32() & 0x1f;
            bytecodeGenerator->addInstruction(shl);
        } else {
            right.loadInAccumulator();
            Instruction::Shl shl;
            shl.lhs = left.stackSlot();
            bytecodeGenerator->addInstruction(shl);
        }
        break;
    case QSOperator::InstanceOf: {
        Instruction::CmpInstanceOf binop;
        left = left.storeOnStack();
        right.loadInAccumulator();
        binop.lhs = left.stackSlot();
        bytecodeGenerator->addInstruction(binop);
        break;
    }
    case QSOperator::In: {
        Instruction::CmpIn binop;
        left = left.storeOnStack();
        right.loadInAccumulator();
        binop.lhs = left.stackSlot();
        bytecodeGenerator->addInstruction(binop);
        break;
    }
    case QSOperator::StrictEqual: {
        if (_expr.accept(cx))
            return jumpBinop(oper, left, right);

        Instruction::CmpStrictEqual cmp;
        left = left.storeOnStack();
        right.loadInAccumulator();
        cmp.lhs = left.stackSlot();
        bytecodeGenerator->addInstruction(cmp);
        break;
    }
    case QSOperator::StrictNotEqual: {
        if (_expr.accept(cx))
            return jumpBinop(oper, left, right);

        Instruction::CmpStrictNotEqual cmp;
        left = left.storeOnStack();
        right.loadInAccumulator();
        cmp.lhs = left.stackSlot();
        bytecodeGenerator->addInstruction(cmp);
        break;
    }
    case QSOperator::Equal: {
        if (_expr.accept(cx))
            return jumpBinop(oper, left, right);

        Instruction::CmpEq cmp;
        left = left.storeOnStack();
        right.loadInAccumulator();
        cmp.lhs = left.stackSlot();
        bytecodeGenerator->addInstruction(cmp);
        break;
    }
    case QSOperator::NotEqual: {
        if (_expr.accept(cx))
            return jumpBinop(oper, left, right);

        Instruction::CmpNe cmp;
        left = left.storeOnStack();
        right.loadInAccumulator();
        cmp.lhs = left.stackSlot();
        bytecodeGenerator->addInstruction(cmp);
        break;
    }
    case QSOperator::Gt: {
        if (_expr.accept(cx))
            return jumpBinop(oper, left, right);

        Instruction::CmpGt cmp;
        left = left.storeOnStack();
        right.loadInAccumulator();
        cmp.lhs = left.stackSlot();
        bytecodeGenerator->addInstruction(cmp);
        break;
    }
    case QSOperator::Ge: {
        if (_expr.accept(cx))
            return jumpBinop(oper, left, right);

        Instruction::CmpGe cmp;
        left = left.storeOnStack();
        right.loadInAccumulator();
        cmp.lhs = left.stackSlot();
        bytecodeGenerator->addInstruction(cmp);
        break;
    }
    case QSOperator::Lt: {
        if (_expr.accept(cx))
            return jumpBinop(oper, left, right);

        Instruction::CmpLt cmp;
        left = left.storeOnStack();
        right.loadInAccumulator();
        cmp.lhs = left.stackSlot();
        bytecodeGenerator->addInstruction(cmp);
        break;
    }
    case QSOperator::Le:
        if (_expr.accept(cx))
            return jumpBinop(oper, left, right);

        Instruction::CmpLe cmp;
        left = left.storeOnStack();
        right.loadInAccumulator();
        cmp.lhs = left.stackSlot();
        bytecodeGenerator->addInstruction(cmp);
        break;
    default:
        Q_UNREACHABLE();
    }

    return Reference::fromAccumulator(this);
}

static QSOperator::Op operatorForSwappedOperands(QSOperator::Op oper)
{
    switch (oper) {
    case QSOperator::StrictEqual: return QSOperator::StrictEqual;
    case QSOperator::StrictNotEqual: return QSOperator::StrictNotEqual;
    case QSOperator::Equal: return QSOperator::Equal;
    case QSOperator::NotEqual: return QSOperator::NotEqual;
    case QSOperator::Gt: return QSOperator::Le;
    case QSOperator::Ge: return QSOperator::Lt;
    case QSOperator::Lt: return QSOperator::Ge;
    case QSOperator::Le: return QSOperator::Gt;
    default: Q_UNIMPLEMENTED(); return QSOperator::Invalid;
    }
}

Codegen::Reference Codegen::jumpBinop(QSOperator::Op oper, Reference &left, Reference &right)
{
    if (left.isConst()) {
        oper = operatorForSwappedOperands(oper);
        qSwap(left, right);
    }

    if (right.isConst() && (oper == QSOperator::Equal || oper == QSOperator::NotEqual)) {
        Value c = Primitive::fromReturnedValue(right.constant);
        if (c.isNull() || c.isUndefined()) {
            left.loadInAccumulator();
            if (oper == QSOperator::Equal) {
                Instruction::CmpEqNull cmp;
                bytecodeGenerator->addInstruction(cmp);
                addCJump();
                return Reference();
            } else if (oper == QSOperator::NotEqual) {
                Instruction::CmpNeNull cmp;
                bytecodeGenerator->addInstruction(cmp);
                addCJump();
                return Reference();
            }
        } else if (c.isInt32()) {
            left.loadInAccumulator();
            if (oper == QSOperator::Equal) {
                Instruction::CmpEqInt cmp;
                cmp.lhs = c.int_32();
                bytecodeGenerator->addInstruction(cmp);
                addCJump();
                return Reference();
            } else if (oper == QSOperator::NotEqual) {
                Instruction::CmpNeInt cmp;
                cmp.lhs = c.int_32();
                bytecodeGenerator->addInstruction(cmp);
                addCJump();
                return Reference();
            }

        }
    }

    left = left.storeOnStack();
    right.loadInAccumulator();

    switch (oper) {
    case QSOperator::StrictEqual: {
        Instruction::CmpStrictEqual cmp;
        cmp.lhs = left.stackSlot();
        bytecodeGenerator->addInstruction(cmp);
        addCJump();
        break;
    }
    case QSOperator::StrictNotEqual: {
        Instruction::CmpStrictNotEqual cmp;
        cmp.lhs = left.stackSlot();
        bytecodeGenerator->addInstruction(cmp);
        addCJump();
        break;
    }
    case QSOperator::Equal: {
        Instruction::CmpEq cmp;
        cmp.lhs = left.stackSlot();
        bytecodeGenerator->addInstruction(cmp);
        addCJump();
        break;
    }
    case QSOperator::NotEqual: {
        Instruction::CmpNe cmp;
        cmp.lhs = left.stackSlot();
        bytecodeGenerator->addInstruction(cmp);
        addCJump();
        break;
    }
    case QSOperator::Gt: {
        Instruction::CmpGt cmp;
        cmp.lhs = left.stackSlot();
        bytecodeGenerator->addInstruction(cmp);
        addCJump();
        break;
    }
    case QSOperator::Ge: {
        Instruction::CmpGe cmp;
        cmp.lhs = left.stackSlot();
        bytecodeGenerator->addInstruction(cmp);
        addCJump();
        break;
    }
    case QSOperator::Lt: {
        Instruction::CmpLt cmp;
        cmp.lhs = left.stackSlot();
        bytecodeGenerator->addInstruction(cmp);
        addCJump();
        break;
    }
    case QSOperator::Le: {
        Instruction::CmpLe cmp;
        cmp.lhs = left.stackSlot();
        bytecodeGenerator->addInstruction(cmp);
        addCJump();
        break;
    }
    default:
        Q_UNREACHABLE();
    }
    return Reference();
}

bool Codegen::visit(CallExpression *ast)
{
    if (hasError)
        return false;

    RegisterScope scope(this);

    Reference base = expression(ast->base);
    if (hasError)
        return false;
    switch (base.type) {
    case Reference::Member:
    case Reference::Subscript:
    case Reference::QmlScopeObject:
    case Reference::QmlContextObject:
        base = base.asLValue();
        break;
    case Reference::Name:
        break;
    default:
        base = base.storeOnStack();
        break;
    }

    auto calldata = pushArgs(ast->arguments);
    if (hasError)
        return false;

    handleCall(base, calldata);
    return false;
}

void Codegen::handleCall(Reference &base, Arguments calldata)
{
    //### Do we really need all these call instructions? can's we load the callee in a temp?
    if (base.type == Reference::QmlScopeObject) {
        Instruction::CallScopeObjectProperty call;
        call.base = base.qmlBase.stackSlot();
        call.name = base.qmlCoreIndex;
        call.argc = calldata.argc;
        call.argv = calldata.argv;
        bytecodeGenerator->addInstruction(call);
    } else if (base.type == Reference::QmlContextObject) {
        Instruction::CallContextObjectProperty call;
        call.base = base.qmlBase.stackSlot();
        call.name = base.qmlCoreIndex;
        call.argc = calldata.argc;
        call.argv = calldata.argv;
        bytecodeGenerator->addInstruction(call);
    } else if (base.type == Reference::Member) {
        if (!disable_lookups && useFastLookups) {
            Instruction::CallPropertyLookup call;
            call.base = base.propertyBase.stackSlot();
            call.lookupIndex = registerGetterLookup(base.propertyNameIndex);
            call.argc = calldata.argc;
            call.argv = calldata.argv;
            bytecodeGenerator->addInstruction(call);
        } else {
            Instruction::CallProperty call;
            call.base = base.propertyBase.stackSlot();
            call.name = base.propertyNameIndex;
            call.argc = calldata.argc;
            call.argv = calldata.argv;
            bytecodeGenerator->addInstruction(call);
        }
    } else if (base.type == Reference::Subscript) {
        Instruction::CallElement call;
        call.base = base.elementBase;
        call.index = base.elementSubscript.stackSlot();
        call.argc = calldata.argc;
        call.argv = calldata.argv;
        bytecodeGenerator->addInstruction(call);
    } else if (base.type == Reference::Name) {
        if (base.name == QStringLiteral("eval")) {
            Instruction::CallPossiblyDirectEval call;
            call.argc = calldata.argc;
            call.argv = calldata.argv;
            bytecodeGenerator->addInstruction(call);
        } else if (!disable_lookups && useFastLookups && base.global) {
            Instruction::CallGlobalLookup call;
            call.index = registerGlobalGetterLookup(base.nameAsIndex());
            call.argc = calldata.argc;
            call.argv = calldata.argv;
            bytecodeGenerator->addInstruction(call);
        } else {
            Instruction::CallName call;
            call.name = base.nameAsIndex();
            call.argc = calldata.argc;
            call.argv = calldata.argv;
            bytecodeGenerator->addInstruction(call);
        }
    } else {
        Q_ASSERT(base.isStackSlot());
        Instruction::CallValue call;
        call.name = base.stackSlot();
        call.argc = calldata.argc;
        call.argv = calldata.argv;
        bytecodeGenerator->addInstruction(call);
    }

    _expr.setResult(Reference::fromAccumulator(this));
}

Codegen::Arguments Codegen::pushArgs(ArgumentList *args)
{
    int argc = 0;
    for (ArgumentList *it = args; it; it = it->next) {
        if (it->isSpreadElement) {
            throwSyntaxError(it->firstSourceLocation(), QLatin1String("'...' in argument lists is unimplemented."));
            return { 0, 0 };
        }
        ++argc;
    }

    if (!argc)
        return { 0, 0 };

    int calldata = bytecodeGenerator->newRegisterArray(argc);

    argc = 0;
    for (ArgumentList *it = args; it; it = it->next) {
        RegisterScope scope(this);
        Reference e = expression(it->expression);
        if (hasError)
            break;
        if (!argc && !it->next) {
            // avoid copy for functions taking a single argument
            if (e.isStackSlot())
                return { 1, e.stackSlot() };
        }
        (void) e.storeOnStack(calldata + argc);
        ++argc;
    }

    return { argc, calldata };
}

Codegen::Arguments Codegen::pushTemplateArgs(TemplateLiteral *args)
{
    int argc = 0;
    for (TemplateLiteral *it = args; it; it = it->next)
        ++argc;

    if (!argc)
        return { 0, 0 };

    int calldata = bytecodeGenerator->newRegisterArray(argc);

    argc = 0;
    for (TemplateLiteral *it = args; it && it->expression; it = it->next) {
        RegisterScope scope(this);
        Reference e = expression(it->expression);
        if (hasError)
            break;
        (void) e.storeOnStack(calldata + argc);
        ++argc;
    }

    return { argc, calldata };
}

bool Codegen::visit(ConditionalExpression *ast)
{
    if (hasError)
        return true;

    RegisterScope scope(this);

    BytecodeGenerator::Label iftrue = bytecodeGenerator->newLabel();
    BytecodeGenerator::Label iffalse = bytecodeGenerator->newLabel();
    condition(ast->expression, &iftrue, &iffalse, true);

    iftrue.link();
    Reference ok = expression(ast->ok);
    if (hasError)
        return false;
    ok.loadInAccumulator();
    BytecodeGenerator::Jump jump_endif = bytecodeGenerator->jump();

    iffalse.link();
    Reference ko = expression(ast->ko);
    if (hasError)
        return false;
    ko.loadInAccumulator();

    jump_endif.link();
    _expr.setResult(Reference::fromAccumulator(this));

    return false;
}

bool Codegen::visit(DeleteExpression *ast)
{
    if (hasError)
        return false;

    Reference expr = expression(ast->expression);
    if (hasError)
        return false;

    switch (expr.type) {
    case Reference::StackSlot:
        if (!expr.stackSlotIsLocalOrArgument)
            break;
        // fall through
    case Reference::ScopedLocal:
        // Trying to delete a function argument might throw.
        if (_context->isStrict) {
            throwSyntaxError(ast->deleteToken, QStringLiteral("Delete of an unqualified identifier in strict mode."));
            return false;
        }
        _expr.setResult(Reference::fromConst(this, QV4::Encode(false)));
        return false;
    case Reference::Name: {
        if (_context->isStrict) {
            throwSyntaxError(ast->deleteToken, QStringLiteral("Delete of an unqualified identifier in strict mode."));
            return false;
        }
        Instruction::DeleteName del;
        del.name = expr.nameAsIndex();
        bytecodeGenerator->addInstruction(del);
        _expr.setResult(Reference::fromAccumulator(this));
        return false;
    }
    case Reference::Member: {
        //### maybe add a variant where the base can be in the accumulator?
        expr = expr.asLValue();
        Instruction::DeleteMember del;
        del.base = expr.propertyBase.stackSlot();
        del.member = expr.propertyNameIndex;
        bytecodeGenerator->addInstruction(del);
        _expr.setResult(Reference::fromAccumulator(this));
        return false;
    }
    case Reference::Subscript: {
        //### maybe add a variant where the index can be in the accumulator?
        expr = expr.asLValue();
        Instruction::DeleteSubscript del;
        del.base = expr.elementBase;
        del.index = expr.elementSubscript.stackSlot();
        bytecodeGenerator->addInstruction(del);
        _expr.setResult(Reference::fromAccumulator(this));
        return false;
    }
    default:
        break;
    }
    // [[11.4.1]] Return true if it's not a reference
    _expr.setResult(Reference::fromConst(this, QV4::Encode(true)));
    return false;
}

bool Codegen::visit(FalseLiteral *)
{
    if (hasError)
        return false;

    _expr.setResult(Reference::fromConst(this, QV4::Encode(false)));
    return false;
}

bool Codegen::visit(SuperLiteral *ast)
{
    if (hasError)
        return false;

    throwSyntaxError(ast->superToken, QLatin1String("Support for 'super' keyword not implemented"));
    return false;
}

bool Codegen::visit(FieldMemberExpression *ast)
{
    if (hasError)
        return false;

    if (AST::IdentifierExpression *id = AST::cast<AST::IdentifierExpression *>(ast->base)) {
        if (id->name == QLatin1String("new")) {
            // new.target
            if (ast->name != QLatin1String("target")) {
                throwSyntaxError(ast->identifierToken, QLatin1String("Expected 'target' after 'new.'."));
                return false;
            }
            throwSyntaxError(ast->identifierToken, QLatin1String("Support for 'new.target' unimplemented."));
        }
    }

    Reference base = expression(ast->base);
    if (hasError)
        return false;
    _expr.setResult(Reference::fromMember(base, ast->name.toString()));
    return false;
}

bool Codegen::visit(TaggedTemplate *ast)
{
    if (hasError)
        return false;

    RegisterScope scope(this);

    Reference base = expression(ast->base);
    if (hasError)
        return false;
    switch (base.type) {
    case Reference::Member:
    case Reference::Subscript:
        base = base.asLValue();
        break;
    case Reference::Name:
        break;
    default:
        base = base.storeOnStack();
        break;
    }

    int arrayTemp = createTemplateArray(ast->templateLiteral);
    Q_UNUSED(arrayTemp);
    auto calldata = pushTemplateArgs(ast->templateLiteral);
    if (hasError)
        return false;
    ++calldata.argc;
    Q_ASSERT(calldata.argv == arrayTemp + 1);
    --calldata.argv;

    handleCall(base, calldata);
    return false;


    return false;
}

int Codegen::createTemplateArray(TemplateLiteral *t)
{
    int arrayTemp = bytecodeGenerator->newRegister();

    RegisterScope scope(this);

    int argc = 0;
    int args = -1;
    auto push = [this, &argc, &args](const QStringRef &arg) {
        int temp = bytecodeGenerator->newRegister();
        if (args == -1)
            args = temp;
        Instruction::LoadRuntimeString instr;
        instr.stringId = registerString(arg.toString());
        bytecodeGenerator->addInstruction(instr);
        Instruction::StoreReg store;
        store.reg = temp;
        bytecodeGenerator->addInstruction(store);

        ++argc;
    };

    for (TemplateLiteral *it = t; it; it = it->next)
        push(it->value);

    if (args == -1) {
        Q_ASSERT(argc == 0);
        args = 0;
    }

    Instruction::DefineArray call;
    call.argc = argc;
    call.args = Moth::StackSlot::createRegister(args);
    bytecodeGenerator->addInstruction(call);

    Instruction::StoreReg store;
    store.reg = arrayTemp;
    bytecodeGenerator->addInstruction(store);

    return arrayTemp;
}

bool Codegen::visit(FunctionExpression *ast)
{
    if (hasError)
        return false;

    RegisterScope scope(this);

    int function = defineFunction(ast->name.toString(), ast, ast->formals, ast->body);
    if (hasError)
        return false;
    loadClosure(function);
    _expr.setResult(Reference::fromAccumulator(this));
    return false;
}

Codegen::Reference Codegen::referenceForName(const QString &name, bool isLhs)
{
    Context::ResolvedName resolved = _context->resolveName(name);

    if (resolved.type == Context::ResolvedName::Local || resolved.type == Context::ResolvedName::Stack) {
        if (resolved.isArgOrEval && isLhs)
            // ### add correct source location
            throwSyntaxError(SourceLocation(), QStringLiteral("Variable name may not be eval or arguments in strict mode"));
        Reference r = (resolved.type == Context::ResolvedName::Local) ?
                    Reference::fromScopedLocal(this, resolved.index, resolved.scope) :
                    Reference::fromStackSlot(this, resolved.index, true /*isLocal*/);
        if (r.isStackSlot() && _volatileMemoryLocations.isVolatile(name))
            r.isVolatile = true;
        r.isArgOrEval = resolved.isArgOrEval;
        return r;
    }

    // This hook allows implementing QML lookup semantics
    Reference fallback = fallbackNameLookup(name);
    if (fallback.type != Reference::Invalid)
        return fallback;

    Reference r = Reference::fromName(this, name);
    r.global = (resolved.type == Context::ResolvedName::Global);
    return r;
}

void Codegen::loadClosure(int closureId)
{
    if (closureId >= 0) {
        Instruction::LoadClosure load;
        load.value = closureId;
        bytecodeGenerator->addInstruction(load);
    } else {
        Reference::fromConst(this, Encode::undefined()).loadInAccumulator();
    }
}

Codegen::Reference Codegen::fallbackNameLookup(const QString &name)
{
    Q_UNUSED(name)
    return Reference();
}

bool Codegen::visit(IdentifierExpression *ast)
{
    if (hasError)
        return false;

    _expr.setResult(referenceForName(ast->name.toString(), false));
    return false;
}

bool Codegen::visit(NestedExpression *ast)
{
    if (hasError)
        return false;

    accept(ast->expression);
    return false;
}

bool Codegen::visit(NewExpression *ast)
{
    if (hasError)
        return false;

    RegisterScope scope(this);

    Reference base = expression(ast->expression);
    if (hasError)
        return false;
    //### Maybe create a ConstructA that takes an accumulator?
    base = base.storeOnStack();

    Instruction::Construct create;
    create.func = base.stackSlot();
    create.argc = 0;
    create.argv = 0;
    bytecodeGenerator->addInstruction(create);
    _expr.setResult(Reference::fromAccumulator(this));
    return false;
}

bool Codegen::visit(NewMemberExpression *ast)
{
    if (hasError)
        return false;

    RegisterScope scope(this);

    Reference base = expression(ast->base);
    if (hasError)
        return false;
    base = base.storeOnStack();

    auto calldata = pushArgs(ast->arguments);
    if (hasError)
        return false;

    Instruction::Construct create;
    create.func = base.stackSlot();
    create.argc = calldata.argc;
    create.argv = calldata.argv;
    bytecodeGenerator->addInstruction(create);
    _expr.setResult(Reference::fromAccumulator(this));
    return false;
}

bool Codegen::visit(NotExpression *ast)
{
    if (hasError)
        return false;

    _expr.setResult(unop(Not, expression(ast->expression)));
    return false;
}

bool Codegen::visit(NullExpression *)
{
    if (hasError)
        return false;

    if (_expr.accept(cx))
        bytecodeGenerator->jump().link(*_expr.iffalse());
    else
        _expr.setResult(Reference::fromConst(this, Encode::null()));

    return false;
}

bool Codegen::visit(NumericLiteral *ast)
{
    if (hasError)
        return false;

    _expr.setResult(Reference::fromConst(this, QV4::Encode::smallestNumber(ast->value)));
    return false;
}

bool Codegen::visit(ObjectPattern *ast)
{
    if (hasError)
        return false;

    QVector<QPair<Reference, ObjectPropertyValue>> computedProperties;
    QMap<QString, ObjectPropertyValue> valueMap;

    RegisterScope scope(this);

    QStringList members;

    int argc = 0;
    int args = 0;
    auto push = [this, &args, &argc](const Reference &arg) {
        int temp = bytecodeGenerator->newRegister();
        if (argc == 0)
            args = temp;
        (void) arg.storeOnStack(temp);
        ++argc;
    };

    PatternPropertyList *it = ast->properties;
    for (; it; it = it->next) {
        PatternProperty *p = it->property;
        AST::ComputedPropertyName *cname = AST::cast<AST::ComputedPropertyName *>(p->name);
        if (cname || p->type == PatternProperty::Getter || p->type == PatternProperty::Setter)
            break;
        QString name = p->name->asString();
        uint arrayIndex = QV4::String::toArrayIndex(name);
        if (arrayIndex != UINT_MAX)
            break;
        if (members.contains(name))
            break;
        members.append(name);

        {
            RegisterScope innerScope(this);
            Reference value = expression(p->initializer);
            if (hasError)
                return false;
            value.loadInAccumulator();
        }
        push(Reference::fromAccumulator(this));
    }

    int classId = jsUnitGenerator->registerJSClass(members);

    // handle complex property setters
    for (; it; it = it->next) {
        PatternProperty *p = it->property;
        AST::ComputedPropertyName *cname = AST::cast<AST::ComputedPropertyName *>(p->name);
        ObjectLiteralArgument argType = ObjectLiteralArgument::Value;
        if (p->type == PatternProperty::Getter)
            argType = ObjectLiteralArgument::Getter;
        else if (p->type == PatternProperty::Setter)
            argType = ObjectLiteralArgument::Setter;

        Reference::fromConst(this, Encode(int(argType))).loadInAccumulator();
        push(Reference::fromAccumulator(this));

        if (cname) {
            RegisterScope innerScope(this);
            Reference name = expression(cname->expression);
            if (hasError)
                return false;
            name.loadInAccumulator();
        } else {
            QString name = p->name->asString();
#if 0
            uint arrayIndex = QV4::String::toArrayIndex(name);
            if (arrayIndex != UINT_MAX) {
                Reference::fromConst(this, Encode(arrayIndex)).loadInAccumulator();
            } else
#endif
            {
                Instruction::LoadRuntimeString instr;
                instr.stringId = registerString(name);
                bytecodeGenerator->addInstruction(instr);
            }
        }
        push(Reference::fromAccumulator(this));
        {
            RegisterScope innerScope(this);
            Reference value = expression(p->initializer);
            if (hasError)
                return false;
            value.loadInAccumulator();
        }
        push(Reference::fromAccumulator(this));
    }

    Instruction::DefineObjectLiteral call;
    call.internalClassId = classId;
    call.argc = argc;
    call.args = Moth::StackSlot::createRegister(args);
    bytecodeGenerator->addInstruction(call);
    Reference result = Reference::fromAccumulator(this);
    _expr.setResult(result);
    return false;
}

bool Codegen::visit(PostDecrementExpression *ast)
{
    if (hasError)
        return false;

    Reference expr = expression(ast->base);
    if (hasError)
        return false;
    if (!expr.isLValue()) {
        throwReferenceError(ast->base->lastSourceLocation(), QStringLiteral("Invalid left-hand side expression in postfix operation"));
        return false;
    }
    if (throwSyntaxErrorOnEvalOrArgumentsInStrictMode(expr, ast->decrementToken))
        return false;

    _expr.setResult(unop(PostDecrement, expr));

    return false;
}

bool Codegen::visit(PostIncrementExpression *ast)
{
    if (hasError)
        return false;

    Reference expr = expression(ast->base);
    if (hasError)
        return false;
    if (!expr.isLValue()) {
        throwReferenceError(ast->base->lastSourceLocation(), QStringLiteral("Invalid left-hand side expression in postfix operation"));
        return false;
    }
    if (throwSyntaxErrorOnEvalOrArgumentsInStrictMode(expr, ast->incrementToken))
        return false;

    _expr.setResult(unop(PostIncrement, expr));
    return false;
}

bool Codegen::visit(PreDecrementExpression *ast)
{    if (hasError)
        return false;

    Reference expr = expression(ast->expression);
    if (hasError)
        return false;
    if (!expr.isLValue()) {
        throwReferenceError(ast->expression->lastSourceLocation(), QStringLiteral("Prefix ++ operator applied to value that is not a reference."));
        return false;
    }

    if (throwSyntaxErrorOnEvalOrArgumentsInStrictMode(expr, ast->decrementToken))
        return false;
    _expr.setResult(unop(PreDecrement, expr));
    return false;
}

bool Codegen::visit(PreIncrementExpression *ast)
{
    if (hasError)
        return false;

    Reference expr = expression(ast->expression);
    if (hasError)
        return false;
    if (!expr.isLValue()) {
        throwReferenceError(ast->expression->lastSourceLocation(), QStringLiteral("Prefix ++ operator applied to value that is not a reference."));
        return false;
    }

    if (throwSyntaxErrorOnEvalOrArgumentsInStrictMode(expr, ast->incrementToken))
        return false;
    _expr.setResult(unop(PreIncrement, expr));
    return false;
}

bool Codegen::visit(RegExpLiteral *ast)
{
    if (hasError)
        return false;

    auto r = Reference::fromStackSlot(this);
    r.isReadonly = true;
    _expr.setResult(r);

    Instruction::MoveRegExp instr;
    instr.regExpId = jsUnitGenerator->registerRegExp(ast);
    instr.destReg = r.stackSlot();
    bytecodeGenerator->addInstruction(instr);
    return false;
}

bool Codegen::visit(StringLiteral *ast)
{
    if (hasError)
        return false;

    auto r = Reference::fromAccumulator(this);
    r.isReadonly = true;
    _expr.setResult(r);

    Instruction::LoadRuntimeString instr;
    instr.stringId = registerString(ast->value.toString());
    bytecodeGenerator->addInstruction(instr);
    return false;
}

bool Codegen::visit(TemplateLiteral *ast)
{
    if (hasError)
        return false;

    Instruction::LoadRuntimeString instr;
    instr.stringId = registerString(ast->value.toString());
    bytecodeGenerator->addInstruction(instr);

    if (ast->expression) {
        RegisterScope scope(this);
        int temp = bytecodeGenerator->newRegister();
        Instruction::StoreReg store;
        store.reg = temp;
        bytecodeGenerator->addInstruction(store);

        Reference expr = expression(ast->expression);

        if (ast->next) {
            int temp2 = bytecodeGenerator->newRegister();
            expr.storeOnStack(temp2);
            visit(ast->next);

            Instruction::Add instr;
            instr.lhs = temp2;
            bytecodeGenerator->addInstruction(instr);
        } else {
            expr.loadInAccumulator();
        }

        Instruction::Add instr;
        instr.lhs = temp;
        bytecodeGenerator->addInstruction(instr);
    }

    auto r = Reference::fromAccumulator(this);
    r.isReadonly = true;

    _expr.setResult(r);
    return false;

}

bool Codegen::visit(ThisExpression *)
{
    if (hasError)
        return false;

    _expr.setResult(Reference::fromThis(this));
    return false;
}

bool Codegen::visit(TildeExpression *ast)
{
    if (hasError)
        return false;

    _expr.setResult(unop(Compl, expression(ast->expression)));
    return false;
}

bool Codegen::visit(TrueLiteral *)
{
    if (hasError)
        return false;

    _expr.setResult(Reference::fromConst(this, QV4::Encode(true)));
    return false;
}

bool Codegen::visit(TypeOfExpression *ast)
{
    if (hasError)
        return false;

    RegisterScope scope(this);

    Reference expr = expression(ast->expression);
    if (hasError)
        return false;

    if (expr.type == Reference::Name) {
        // special handling as typeof doesn't throw here
        Instruction::TypeofName instr;
        instr.name = expr.nameAsIndex();
        bytecodeGenerator->addInstruction(instr);
    } else {
        expr.loadInAccumulator();
        Instruction::TypeofValue instr;
        bytecodeGenerator->addInstruction(instr);
    }
    _expr.setResult(Reference::fromAccumulator(this));

    return false;
}

bool Codegen::visit(UnaryMinusExpression *ast)
{
    if (hasError)
        return false;

    _expr.setResult(unop(UMinus, expression(ast->expression)));
    return false;
}

bool Codegen::visit(UnaryPlusExpression *ast)
{
    if (hasError)
        return false;

    _expr.setResult(unop(UPlus, expression(ast->expression)));
    return false;
}

bool Codegen::visit(VoidExpression *ast)
{
    if (hasError)
        return false;

    RegisterScope scope(this);

    statement(ast->expression);
    _expr.setResult(Reference::fromConst(this, Encode::undefined()));
    return false;
}

bool Codegen::visit(FunctionDeclaration * ast)
{
    if (hasError)
        return false;

    RegisterScope scope(this);

    if (_functionContext->contextType == ContextType::Binding)
        referenceForName(ast->name.toString(), true).loadInAccumulator();
    _expr.accept(nx);
    return false;
}

bool Codegen::visit(YieldExpression *ast)
{
    if (ast->isYieldStar) {
        throwSyntaxError(ast->firstSourceLocation(), QLatin1String("yield* is not currently supported"));
        return false;
    }
    if (inFormalParameterList) {
        throwSyntaxError(ast->firstSourceLocation(), QLatin1String("yield is not allowed inside parameter lists"));
        return false;
    }


    Reference result = ast->expression ? expression(ast->expression) : Reference::fromConst(this, Encode::undefined());
    if (hasError)
        return false;
    result.loadInAccumulator();
    Instruction::Yield yield;
    bytecodeGenerator->addInstruction(yield);
    Instruction::Resume resume;
    BytecodeGenerator::Jump jump = bytecodeGenerator->addJumpInstruction(resume);
    Reference acc = Reference::fromAccumulator(this);
    emitReturn(acc);
    jump.link();
    _expr.setResult(acc);
    return false;
}

static bool endsWithReturn(Module *module, Node *node)
{
    if (!node)
        return false;
    if (AST::cast<ReturnStatement *>(node))
        return true;
    if (AST::cast<ThrowStatement *>(node))
        return true;
    if (Program *p = AST::cast<Program *>(node))
        return endsWithReturn(module, p->statements);
    if (StatementList *sl = AST::cast<StatementList *>(node)) {
        while (sl->next)
            sl = sl->next;
        return endsWithReturn(module, sl->statement);
    }
    if (Block *b = AST::cast<Block *>(node)) {
        Context *blockContext = module->contextMap.value(node);
        if (blockContext->requiresExecutionContext)
            // we need to emit a return statement here, because of the
            // unwind handler
            return false;
        return endsWithReturn(module, b->statements);
    }
    if (IfStatement *is = AST::cast<IfStatement *>(node))
        return is->ko && endsWithReturn(module, is->ok) && endsWithReturn(module, is->ko);
    return false;
}

int Codegen::defineFunction(const QString &name, AST::Node *ast,
                            AST::FormalParameterList *formals,
                            AST::StatementList *body)
{
    enterContext(ast);

    if (_context->functionIndex >= 0)
        // already defined
        return leaveContext();

    _context->name = name;
    _module->functions.append(_context);
    _context->functionIndex = _module->functions.count() - 1;

    Context *savedFunctionContext = _functionContext;
    _functionContext = _context;
    ControlFlow *savedControlFlow = controlFlow;
    controlFlow = nullptr;

    if (_context->contextType == ContextType::Global) {
        _module->blocks.append(_context);
        _context->blockIndex = _module->blocks.count() - 1;
    }
    if (_module->debugMode) // allow the debugger to see overwritten arguments
        _context->argumentsCanEscape = true;

    // When a user writes the following QML signal binding:
    //    onSignal: function() { doSomethingUsefull }
    // we will generate a binding function that just returns the closure. However, that's not useful
    // at all, because if the onSignal is a signal handler, the user is actually making it explicit
    // that the binding is a function, so we should execute that. However, we don't know that during
    // AOT compilation, so mark the surrounding function as only-returning-a-closure.
    _context->returnsClosure = body && body->statement && cast<ExpressionStatement *>(body->statement) && cast<FunctionExpression *>(cast<ExpressionStatement *>(body->statement)->expression);

    BytecodeGenerator bytecode(_context->line, _module->debugMode);
    BytecodeGenerator *savedBytecodeGenerator;
    savedBytecodeGenerator = bytecodeGenerator;
    bytecodeGenerator = &bytecode;
    bytecodeGenerator->setLocation(ast->firstSourceLocation());
    BytecodeGenerator::Label *savedReturnLabel = _returnLabel;
    _returnLabel = nullptr;

    bool savedFunctionEndsWithReturn = functionEndsWithReturn;
    functionEndsWithReturn = endsWithReturn(_module, body);

    // reserve the js stack frame (Context & js Function & accumulator)
    bytecodeGenerator->newRegisterArray(sizeof(CallData)/sizeof(Value) - 1 + _context->arguments.size());

    bool _inFormalParameterList = false;
    qSwap(_inFormalParameterList, inFormalParameterList);

    int returnAddress = -1;
    bool _requiresReturnValue = _context->requiresImplicitReturnValue();
    qSwap(requiresReturnValue, _requiresReturnValue);
    returnAddress = bytecodeGenerator->newRegister();
    qSwap(_returnAddress, returnAddress);

    // register the lexical scope for global code
    if (!_context->parent && _context->requiresExecutionContext) {
        _module->blocks.append(_context);
        _context->blockIndex = _module->blocks.count() - 1;
    }

    RegisterScope registerScope(this);
    _context->emitBlockHeader(this);

    inFormalParameterList = true;
    int argc = 0;
    while (formals) {
        PatternElement *e = formals->element;
        if (!e) {
            if (!formals->next)
                // trailing comma
                break;
            Q_UNREACHABLE();
        }

        Reference arg = referenceForName(e->bindingIdentifier, true);
        if (e->type == PatternElement::RestElement) {
            Q_ASSERT(!formals->next);
            Instruction::CreateRestParameter rest;
            rest.argIndex = argc;
            bytecodeGenerator->addInstruction(rest);
            arg.storeConsumeAccumulator();
        } else {
            if (e->bindingTarget || e->initializer) {
                initializeAndDestructureBindingElement(e, arg);
                if (hasError)
                    break;
            }
        }
        formals = formals->next;
        ++argc;
    }
    inFormalParameterList = false;

    if (_context->isGenerator) {
        Instruction::Yield yield;
        bytecodeGenerator->addInstruction(yield);
    }

    beginFunctionBodyHook();

    statementList(body);

    bytecodeGenerator->setLocation(ast->lastSourceLocation());
    _context->emitBlockFooter(this);

    if (_returnLabel || hasError || !functionEndsWithReturn) {
        if (_returnLabel)
            _returnLabel->link();

        if (_returnLabel || requiresReturnValue) {
            Instruction::LoadReg load;
            load.reg = Moth::StackSlot::createRegister(_returnAddress);
            bytecodeGenerator->addInstruction(load);
        } else {
            Reference::fromConst(this, Encode::undefined()).loadInAccumulator();
        }

        bytecodeGenerator->addInstruction(Instruction::Ret());
    }

    Q_ASSERT(_context == _functionContext);
    bytecodeGenerator->finalize(_context);
    _context->registerCountInFunction = bytecodeGenerator->registerCount();
    static const bool showCode = qEnvironmentVariableIsSet("QV4_SHOW_BYTECODE");
    if (showCode) {
        qDebug() << "=== Bytecode for" << _context->name << "strict mode" << _context->isStrict
                 << "register count" << _context->registerCountInFunction << "implicit return" << requiresReturnValue;
        QV4::Moth::dumpBytecode(_context->code, _context->locals.size(), _context->arguments.size(),
                                _context->line, _context->lineNumberMapping);
        qDebug();
    }

    qSwap(_returnAddress, returnAddress);
    qSwap(requiresReturnValue, _requiresReturnValue);
    qSwap(_inFormalParameterList, inFormalParameterList);
    bytecodeGenerator = savedBytecodeGenerator;
    delete _returnLabel;
    _returnLabel = savedReturnLabel;
    controlFlow = savedControlFlow;
    functionEndsWithReturn = savedFunctionEndsWithReturn;
    _functionContext = savedFunctionContext;

    return leaveContext();
}

bool Codegen::visit(Block *ast)
{
    if (hasError)
        return false;

    RegisterScope scope(this);

    ControlFlowBlock controlFlow(this, ast);
    statementList(ast->statements);
    return false;
}

bool Codegen::visit(BreakStatement *ast)
{
    if (hasError)
        return false;

    if (!controlFlow) {
        throwSyntaxError(ast->lastSourceLocation(), QStringLiteral("Break outside of loop"));
        return false;
    }

    ControlFlow::UnwindTarget target = controlFlow->unwindTarget(ControlFlow::Break, ast->label.toString());
    if (!target.linkLabel.isValid()) {
        if (ast->label.isEmpty())
            throwSyntaxError(ast->lastSourceLocation(), QStringLiteral("Break outside of loop"));
        else
            throwSyntaxError(ast->lastSourceLocation(), QStringLiteral("Undefined label '%1'").arg(ast->label.toString()));
        return false;
    }

    bytecodeGenerator->unwindToLabel(target.unwindLevel, target.linkLabel);

    return false;
}

bool Codegen::visit(ContinueStatement *ast)
{
    if (hasError)
        return false;

    RegisterScope scope(this);

    if (!controlFlow) {
        throwSyntaxError(ast->lastSourceLocation(), QStringLiteral("Continue outside of loop"));
        return false;
    }

    ControlFlow::UnwindTarget target = controlFlow->unwindTarget(ControlFlow::Continue, ast->label.toString());
    if (!target.linkLabel.isValid()) {
        if (ast->label.isEmpty())
            throwSyntaxError(ast->lastSourceLocation(), QStringLiteral("Undefined label '%1'").arg(ast->label.toString()));
        else
            throwSyntaxError(ast->lastSourceLocation(), QStringLiteral("continue outside of loop"));
        return false;
    }

    bytecodeGenerator->unwindToLabel(target.unwindLevel, target.linkLabel);

    return false;
}

bool Codegen::visit(DebuggerStatement *)
{
    Q_UNIMPLEMENTED();
    return false;
}

bool Codegen::visit(DoWhileStatement *ast)
{
    if (hasError)
        return true;

    RegisterScope scope(this);

    BytecodeGenerator::Label body = bytecodeGenerator->label();
    BytecodeGenerator::Label cond = bytecodeGenerator->newLabel();
    BytecodeGenerator::Label end = bytecodeGenerator->newLabel();

    ControlFlowLoop flow(this, &end, &cond);

    statement(ast->statement);
    setJumpOutLocation(bytecodeGenerator, ast->statement, ast->semicolonToken);

    cond.link();

    if (!AST::cast<FalseLiteral *>(ast->expression)) {
        if (AST::cast<TrueLiteral *>(ast->expression))
            bytecodeGenerator->jump().link(body);
        else
            condition(ast->expression, &body, &end, false);
    }

    end.link();

    return false;
}

bool Codegen::visit(EmptyStatement *)
{
    if (hasError)
        return true;

    return false;
}

bool Codegen::visit(ExpressionStatement *ast)
{
    if (hasError)
        return true;

    RegisterScope scope(this);

    if (requiresReturnValue) {
        Reference e = expression(ast->expression);
        if (hasError)
            return false;
        (void) e.storeOnStack(_returnAddress);
    } else {
        statement(ast->expression);
    }
    return false;
}

bool Codegen::visit(ForEachStatement *ast)
{
    if (hasError)
        return true;

    RegisterScope scope(this);

    Reference iterator = Reference::fromStackSlot(this);
    Reference lhsValue = Reference::fromStackSlot(this);

    // There should be a temporal block, so that variables declared in lhs shadow outside vars.
    // This block should define a temporal dead zone for those variables, which is not yet implemented.
    {
        RegisterScope innerScope(this);
        ControlFlowBlock controlFlow(this, ast);
        Reference expr = expression(ast->expression);
        if (hasError)
            return true;

        expr.loadInAccumulator();
        Instruction::GetIterator iteratorObjInstr;
        iteratorObjInstr.iterator = (ast->type == ForEachType::Of) ? 1 : 0;
        bytecodeGenerator->addInstruction(iteratorObjInstr);
        iterator.storeConsumeAccumulator();
    }

    BytecodeGenerator::Label in = bytecodeGenerator->newLabel();
    BytecodeGenerator::Label end = bytecodeGenerator->newLabel();
    BytecodeGenerator::Label done = bytecodeGenerator->newLabel();

    {
        ControlFlowLoop flow(this, &end, &in, /*requiresUnwind*/ true);
        bytecodeGenerator->jump().link(in);

        BytecodeGenerator::Label body = bytecodeGenerator->label();

        // each iteration gets it's own context, as per spec
        {
            RegisterScope innerScope(this);
            ControlFlowBlock controlFlow(this, ast);

            if (ExpressionNode *e = ast->lhs->expressionCast()) {
                if (AST::Pattern *p = e->patternCast()) {
                    RegisterScope scope(this);
                    destructurePattern(p, lhsValue);
                } else {
                    Reference lhs = expression(e);
                    if (hasError)
                        goto error;
                    lhs = lhs.asLValue();
                    lhsValue.loadInAccumulator();
                    lhs.storeConsumeAccumulator();
                }
            } else if (PatternElement *p = AST::cast<PatternElement *>(ast->lhs)) {
                initializeAndDestructureBindingElement(p, lhsValue);
                if (hasError)
                    goto error;
            } else {
                Q_UNREACHABLE();
            }

            statement(ast->statement);
            setJumpOutLocation(bytecodeGenerator, ast->statement, ast->forToken);

        }

      error:
        in.link();
        iterator.loadInAccumulator();
        Instruction::IteratorNext next;
        next.value = lhsValue.stackSlot();
        bytecodeGenerator->addInstruction(next);
        bytecodeGenerator->addJumpInstruction(Instruction::JumpFalse()).link(body);
        bytecodeGenerator->jump().link(done);
    }

    end.link();

    if (ast->type == ForEachType::Of) {
        Reference iteratorDone = Reference::fromConst(this, Encode(false)).storeOnStack();
        iterator.loadInAccumulator();
        Instruction::IteratorClose close;
        close.done = iteratorDone.stackSlot();
        bytecodeGenerator->addInstruction(close);
    }

    done.link();

    return false;
}

bool Codegen::visit(ForStatement *ast)
{
    if (hasError)
        return true;

    RegisterScope scope(this);

    ControlFlowBlock controlFlow(this, ast);

    if (ast->initialiser)
        statement(ast->initialiser);
    else if (ast->declarations)
        variableDeclarationList(ast->declarations);

    BytecodeGenerator::Label cond = bytecodeGenerator->label();
    BytecodeGenerator::Label body = bytecodeGenerator->newLabel();
    BytecodeGenerator::Label step = bytecodeGenerator->newLabel();
    BytecodeGenerator::Label end = bytecodeGenerator->newLabel();

    ControlFlowLoop flow(this, &end, &step);

    condition(ast->condition, &body, &end, true);

    body.link();
    statement(ast->statement);
    setJumpOutLocation(bytecodeGenerator, ast->statement, ast->forToken);

    step.link();
    if (_context->requiresExecutionContext) {
        Instruction::CloneBlockContext clone;
        bytecodeGenerator->addInstruction(clone);
    }
    statement(ast->expression);
    bytecodeGenerator->jump().link(cond);

    end.link();

    return false;
}

bool Codegen::visit(IfStatement *ast)
{
    if (hasError)
        return true;

    RegisterScope scope(this);

    BytecodeGenerator::Label trueLabel = bytecodeGenerator->newLabel();
    BytecodeGenerator::Label falseLabel = bytecodeGenerator->newLabel();
    condition(ast->expression, &trueLabel, &falseLabel, true);

    trueLabel.link();
    statement(ast->ok);
    if (ast->ko) {
        if (endsWithReturn(_module, ast)) {
            falseLabel.link();
            statement(ast->ko);
        } else {
            BytecodeGenerator::Jump jump_endif = bytecodeGenerator->jump();
            falseLabel.link();
            statement(ast->ko);
            jump_endif.link();
        }
    } else {
        falseLabel.link();
    }

    return false;
}

bool Codegen::visit(LabelledStatement *ast)
{
    if (hasError)
        return true;

    RegisterScope scope(this);

    // check that no outer loop contains the label
    ControlFlow *l = controlFlow;
    while (l) {
        if (l->label() == ast->label) {
            QString error = QString(QStringLiteral("Label '%1' has already been declared")).arg(ast->label.toString());
            throwSyntaxError(ast->firstSourceLocation(), error);
            return false;
        }
        l = l->parent;
    }
    _labelledStatement = ast;

    if (AST::cast<AST::SwitchStatement *>(ast->statement) ||
            AST::cast<AST::WhileStatement *>(ast->statement) ||
            AST::cast<AST::DoWhileStatement *>(ast->statement) ||
            AST::cast<AST::ForStatement *>(ast->statement) ||
            AST::cast<AST::ForEachStatement *>(ast->statement)) {
        statement(ast->statement); // labelledStatement will be associated with the ast->statement's loop.
    } else {
        BytecodeGenerator::Label breakLabel = bytecodeGenerator->newLabel();
        ControlFlowLoop flow(this, &breakLabel);
        statement(ast->statement);
        breakLabel.link();
    }

    return false;
}

void Codegen::emitReturn(const Reference &expr)
{
    ControlFlow::UnwindTarget target = controlFlow ? controlFlow->unwindTarget(ControlFlow::Return) : ControlFlow::UnwindTarget();
    if (target.linkLabel.isValid() && target.unwindLevel) {
        Q_ASSERT(_returnAddress >= 0);
        (void) expr.storeOnStack(_returnAddress);
        bytecodeGenerator->unwindToLabel(target.unwindLevel, target.linkLabel);
    } else {
        expr.loadInAccumulator();
        bytecodeGenerator->addInstruction(Instruction::Ret());
    }
}

bool Codegen::visit(ReturnStatement *ast)
{
    if (hasError)
        return true;

    if (_functionContext->contextType != ContextType::Function && _functionContext->contextType != ContextType::Binding) {
        throwSyntaxError(ast->returnToken, QStringLiteral("Return statement outside of function"));
        return false;
    }
    Reference expr;
    if (ast->expression) {
         expr = expression(ast->expression);
        if (hasError)
            return false;
    } else {
        expr = Reference::fromConst(this, Encode::undefined());
    }

    emitReturn(expr);

    return false;
}

bool Codegen::visit(SwitchStatement *ast)
{
    if (hasError)
        return true;

    if (requiresReturnValue)
        Reference::fromConst(this, Encode::undefined()).storeOnStack(_returnAddress);

    RegisterScope scope(this);

    if (ast->block) {
        ControlFlowBlock controlFlow(this, ast->block);

        BytecodeGenerator::Label switchEnd = bytecodeGenerator->newLabel();

        Reference lhs = expression(ast->expression);
        if (hasError)
            return false;
        lhs = lhs.storeOnStack();

        // set up labels for all clauses
        QHash<Node *, BytecodeGenerator::Label> blockMap;
        for (CaseClauses *it = ast->block->clauses; it; it = it->next)
            blockMap[it->clause] = bytecodeGenerator->newLabel();
        if (ast->block->defaultClause)
            blockMap[ast->block->defaultClause] = bytecodeGenerator->newLabel();
        for (CaseClauses *it = ast->block->moreClauses; it; it = it->next)
            blockMap[it->clause] = bytecodeGenerator->newLabel();

        // do the switch conditions
        for (CaseClauses *it = ast->block->clauses; it; it = it->next) {
            CaseClause *clause = it->clause;
            Reference rhs = expression(clause->expression);
            if (hasError)
                return false;
            rhs.loadInAccumulator();
            bytecodeGenerator->jumpStrictEqual(lhs.stackSlot(), blockMap.value(clause));
        }

        for (CaseClauses *it = ast->block->moreClauses; it; it = it->next) {
            CaseClause *clause = it->clause;
            Reference rhs = expression(clause->expression);
            if (hasError)
                return false;
            rhs.loadInAccumulator();
            bytecodeGenerator->jumpStrictEqual(lhs.stackSlot(), blockMap.value(clause));
        }

        if (DefaultClause *defaultClause = ast->block->defaultClause)
            bytecodeGenerator->jump().link(blockMap.value(defaultClause));
        else
            bytecodeGenerator->jump().link(switchEnd);

        ControlFlowLoop flow(this, &switchEnd);

        insideSwitch = true;
        for (CaseClauses *it = ast->block->clauses; it; it = it->next) {
            CaseClause *clause = it->clause;
            blockMap[clause].link();

            statementList(clause->statements);
        }

        if (ast->block->defaultClause) {
            DefaultClause *clause = ast->block->defaultClause;
            blockMap[clause].link();

            statementList(clause->statements);
        }

        for (CaseClauses *it = ast->block->moreClauses; it; it = it->next) {
            CaseClause *clause = it->clause;
            blockMap[clause].link();

            statementList(clause->statements);
        }
        insideSwitch = false;

        switchEnd.link();

    }

    return false;
}

bool Codegen::visit(ThrowStatement *ast)
{
    if (hasError)
        return false;

    RegisterScope scope(this);

    Reference expr = expression(ast->expression);
    if (hasError)
        return false;

    expr.loadInAccumulator();
    Instruction::ThrowException instr;
    bytecodeGenerator->addInstruction(instr);
    return false;
}

void Codegen::handleTryCatch(TryStatement *ast)
{
    Q_ASSERT(ast);
    RegisterScope scope(this);
    BytecodeGenerator::Label noException = bytecodeGenerator->newLabel();
    {
        ControlFlowCatch catchFlow(this, ast->catchExpression);
        RegisterScope scope(this);
        statement(ast->statement);
        bytecodeGenerator->jump().link(noException);
    }
    noException.link();
}

void Codegen::handleTryFinally(TryStatement *ast)
{
    RegisterScope scope(this);
    ControlFlowFinally finally(this, ast->finallyExpression);

    if (ast->catchExpression) {
        handleTryCatch(ast);
    } else {
        RegisterScope scope(this);
        statement(ast->statement);
    }
}

bool Codegen::visit(TryStatement *ast)
{
    if (hasError)
        return true;

    RegisterScope scope(this);

    if (ast->finallyExpression && ast->finallyExpression->statement) {
        handleTryFinally(ast);
    } else {
        handleTryCatch(ast);
    }

    return false;
}

bool Codegen::visit(VariableStatement *ast)
{
    if (hasError)
        return true;

    variableDeclarationList(ast->declarations);
    return false;
}

bool Codegen::visit(WhileStatement *ast)
{
    if (hasError)
        return true;

    if (AST::cast<FalseLiteral *>(ast->expression))
        return false;

    RegisterScope scope(this);

    BytecodeGenerator::Label start = bytecodeGenerator->newLabel();
    BytecodeGenerator::Label end = bytecodeGenerator->newLabel();
    BytecodeGenerator::Label cond = bytecodeGenerator->label();
    ControlFlowLoop flow(this, &end, &cond);

    if (!AST::cast<TrueLiteral *>(ast->expression))
        condition(ast->expression, &start, &end, true);

    start.link();
    statement(ast->statement);
    setJumpOutLocation(bytecodeGenerator, ast->statement, ast->whileToken);
    bytecodeGenerator->jump().link(cond);

    end.link();
    return false;
}

bool Codegen::visit(WithStatement *ast)
{
    if (hasError)
        return true;

    RegisterScope scope(this);

    Reference src = expression(ast->expression);
    if (hasError)
        return false;
    src = src.storeOnStack(); // trigger load before we setup the exception handler, so exceptions here go to the right place
    src.loadInAccumulator();

    enterContext(ast);
    {
        ControlFlowWith flow(this);
        statement(ast->statement);
    }
    leaveContext();

    return false;
}

bool Codegen::visit(UiArrayBinding *)
{
    Q_UNIMPLEMENTED();
    return false;
}

bool Codegen::visit(UiObjectBinding *)
{
    Q_UNIMPLEMENTED();
    return false;
}

bool Codegen::visit(UiObjectDefinition *)
{
    Q_UNIMPLEMENTED();
    return false;
}

bool Codegen::visit(UiPublicMember *)
{
    Q_UNIMPLEMENTED();
    return false;
}

bool Codegen::visit(UiScriptBinding *)
{
    Q_UNIMPLEMENTED();
    return false;
}

bool Codegen::visit(UiSourceElement *)
{
    Q_UNIMPLEMENTED();
    return false;
}

bool Codegen::throwSyntaxErrorOnEvalOrArgumentsInStrictMode(const Reference &r, const SourceLocation& loc)
{
    if (!_context->isStrict)
        return false;
    bool isArgOrEval = false;
    if (r.type == Reference::Name) {
        QString str = jsUnitGenerator->stringForIndex(r.nameAsIndex());
        if (str == QLatin1String("eval") || str == QLatin1String("arguments")) {
            isArgOrEval = true;
        }
    } else if (r.type == Reference::ScopedLocal || r.isRegister()) {
        isArgOrEval = r.isArgOrEval;
    }
    if (isArgOrEval)
        throwSyntaxError(loc, QStringLiteral("Variable name may not be eval or arguments in strict mode"));
    return isArgOrEval;
}

void Codegen::throwSyntaxError(const SourceLocation &loc, const QString &detail)
{
    if (hasError)
        return;

    hasError = true;
    QQmlJS::DiagnosticMessage error;
    error.message = detail;
    error.loc = loc;
    _errors << error;
}

void Codegen::throwReferenceError(const SourceLocation &loc, const QString &detail)
{
    if (hasError)
        return;

    hasError = true;
    QQmlJS::DiagnosticMessage error;
    error.message = detail;
    error.loc = loc;
    _errors << error;
}

QList<QQmlJS::DiagnosticMessage> Codegen::errors() const
{
    return _errors;
}

QQmlRefPointer<CompiledData::CompilationUnit> Codegen::generateCompilationUnit(bool generateUnitData)
{
    CompiledData::CompilationUnit *compilationUnit = new CompiledData::CompilationUnit;
    if (generateUnitData)
        compilationUnit->data = jsUnitGenerator->generateUnit();

    QQmlRefPointer<CompiledData::CompilationUnit> unit;
    unit.adopt(compilationUnit);
    return unit;
}

QQmlRefPointer<CompiledData::CompilationUnit> Codegen::createUnitForLoading()
{
    QQmlRefPointer<CompiledData::CompilationUnit> result;
    result.adopt(new CompiledData::CompilationUnit);
    return result;
}

class Codegen::VolatileMemoryLocationScanner: protected QQmlJS::AST::Visitor
{
    VolatileMemoryLocations locs;

public:
    Codegen::VolatileMemoryLocations scan(AST::Node *s)
    {
        s->accept(this);
        return locs;
    }

    bool visit(ArrayMemberExpression *) override
    {
        locs.setAllVolatile();
        return false;
    }

    bool visit(FieldMemberExpression *) override
    {
        locs.setAllVolatile();
        return false;
    }

    bool visit(PostIncrementExpression *e) override
    {
        collectIdentifiers(locs.specificLocations, e->base);
        return false;
    }

    bool visit(PostDecrementExpression *e) override
    {
        collectIdentifiers(locs.specificLocations, e->base);
        return false;
    }

    bool visit(PreIncrementExpression *e) override
    {
        collectIdentifiers(locs.specificLocations, e->expression);
        return false;
    }

    bool visit(PreDecrementExpression *e) override
    {
        collectIdentifiers(locs.specificLocations, e->expression);
        return false;
    }

    bool visit(BinaryExpression *e) override
    {
        switch (e->op) {
        case QSOperator::InplaceAnd:
        case QSOperator::InplaceSub:
        case QSOperator::InplaceDiv:
        case QSOperator::InplaceAdd:
        case QSOperator::InplaceLeftShift:
        case QSOperator::InplaceMod:
        case QSOperator::InplaceMul:
        case QSOperator::InplaceOr:
        case QSOperator::InplaceRightShift:
        case QSOperator::InplaceURightShift:
        case QSOperator::InplaceXor:
            collectIdentifiers(locs.specificLocations, e);
            return false;

        default:
            return true;
        }
    }

private:
    void collectIdentifiers(QVector<QStringView> &ids, AST::Node *node) const {
        class Collector: public QQmlJS::AST::Visitor {
            QVector<QStringView> &ids;
        public:
            Collector(QVector<QStringView> &ids): ids(ids) {}
            virtual bool visit(IdentifierExpression *ie) {
                ids.append(ie->name);
                return false;
            }
        };
        Collector collector(ids);
        node->accept(&collector);
    }
};

Codegen::VolatileMemoryLocations Codegen::scanVolatileMemoryLocations(AST::Node *ast) const
{
    VolatileMemoryLocationScanner scanner;
    return scanner.scan(ast);
}


#ifndef V4_BOOTSTRAP

QList<QQmlError> Codegen::qmlErrors() const
{
    QList<QQmlError> qmlErrors;

    // Short circuit to avoid costly (de)heap allocation of QUrl if there are no errors.
    if (_errors.size() == 0)
        return qmlErrors;

    qmlErrors.reserve(_errors.size());

    QUrl url(_fileNameIsUrl ? QUrl(_module->fileName) : QUrl::fromLocalFile(_module->fileName));
    for (const QQmlJS::DiagnosticMessage &msg: qAsConst(_errors)) {
        QQmlError e;
        e.setUrl(url);
        e.setLine(msg.loc.startLine);
        e.setColumn(msg.loc.startColumn);
        e.setDescription(msg.message);
        qmlErrors << e;
    }

    return qmlErrors;
}

#endif // V4_BOOTSTRAP

bool Codegen::RValue::operator==(const RValue &other) const
{
    switch (type) {
    case Accumulator:
        return other.isAccumulator();
    case StackSlot:
        return other.isStackSlot() && theStackSlot == other.theStackSlot;
    case Const:
        return other.isConst() && constant == other.constant;
    default:
        return false;
    }
}

Codegen::RValue Codegen::RValue::storeOnStack() const
{
    switch (type) {
    case Accumulator:
        return RValue::fromStackSlot(codegen, Reference::fromAccumulator(codegen).storeOnStack().stackSlot());
    case StackSlot:
        return *this;
    case Const:
        return RValue::fromStackSlot(codegen, Reference::storeConstOnStack(codegen, constant).stackSlot());
    default:
        Q_UNREACHABLE();
    }
}

Codegen::Reference::Reference(const Codegen::Reference &other)
{
    *this = other;
}

Codegen::Reference &Codegen::Reference::operator =(const Reference &other)
{
    type = other.type;

    switch (type) {
    case Invalid:
    case Accumulator:
        break;
    case StackSlot:
        theStackSlot = other.theStackSlot;
        break;
    case ScopedLocal:
        index = other.index;
        scope = other.scope;
        break;
    case Name:
        name = other.name;
        break;
    case Member:
        propertyBase = other.propertyBase;
        propertyNameIndex = other.propertyNameIndex;
        break;
    case Subscript:
        elementBase = other.elementBase;
        elementSubscript = other.elementSubscript;
        break;
    case Const:
        constant = other.constant;
        break;
    case QmlScopeObject:
    case QmlContextObject:
        qmlBase = other.qmlBase;
        qmlCoreIndex = other.qmlCoreIndex;
        qmlNotifyIndex = other.qmlNotifyIndex;
        capturePolicy = other.capturePolicy;
        break;
    }

    // keep loaded reference
    isArgOrEval = other.isArgOrEval;
    codegen = other.codegen;
    isReadonly = other.isReadonly;
    stackSlotIsLocalOrArgument = other.stackSlotIsLocalOrArgument;
    isVolatile = other.isVolatile;
    global = other.global;
    return *this;
}

bool Codegen::Reference::operator==(const Codegen::Reference &other) const
{
    if (type != other.type)
        return false;
    switch (type) {
    case Invalid:
    case Accumulator:
        break;
    case StackSlot:
        return theStackSlot == other.theStackSlot;
    case ScopedLocal:
        return index == other.index && scope == other.scope;
    case Name:
        return nameAsIndex() == other.nameAsIndex();
    case Member:
        return propertyBase == other.propertyBase && propertyNameIndex == other.propertyNameIndex;
    case Subscript:
        return elementBase == other.elementBase && elementSubscript == other.elementSubscript;
    case Const:
        return constant == other.constant;
    case QmlScopeObject:
    case QmlContextObject:
        return qmlCoreIndex == other.qmlCoreIndex && qmlNotifyIndex == other.qmlNotifyIndex
                && capturePolicy == other.capturePolicy;
    }
    return true;
}

Codegen::RValue Codegen::Reference::asRValue() const
{
    switch (type) {
    case Invalid:
        Q_UNREACHABLE();
    case Accumulator:
        return RValue::fromAccumulator(codegen);
    case StackSlot:
        return RValue::fromStackSlot(codegen, stackSlot());
    case Const:
        return RValue::fromConst(codegen, constant);
    default:
        loadInAccumulator();
        return RValue::fromAccumulator(codegen);
    }
}

Codegen::Reference Codegen::Reference::asLValue() const
{
    switch (type) {
    case Invalid:
    case Accumulator:
        Q_UNREACHABLE();
    case Member:
        if (!propertyBase.isStackSlot()) {
            Reference r = *this;
            r.propertyBase = propertyBase.storeOnStack();
            return r;
        }
        return *this;
    case Subscript:
        if (!elementSubscript.isStackSlot()) {
            Reference r = *this;
            r.elementSubscript = elementSubscript.storeOnStack();
            return r;
        }
        return *this;
    default:
        return *this;
    }
}

Codegen::Reference Codegen::Reference::storeConsumeAccumulator() const
{
    storeAccumulator(); // it doesn't matter what happens here, just do it.
    return Reference();
}

Codegen::Reference Codegen::Reference::storeOnStack() const
{ return doStoreOnStack(-1); }

void Codegen::Reference::storeOnStack(int slotIndex) const
{ doStoreOnStack(slotIndex); }

Codegen::Reference Codegen::Reference::doStoreOnStack(int slotIndex) const
{
    if (isStackSlot() && slotIndex == -1 && !(stackSlotIsLocalOrArgument && isVolatile))
        return *this;

    if (isStackSlot()) { // temp-to-temp move
        Reference dest = Reference::fromStackSlot(codegen, slotIndex);
        Instruction::MoveReg move;
        move.srcReg = stackSlot();
        move.destReg = dest.stackSlot();
        codegen->bytecodeGenerator->addInstruction(move);
        return dest;
    }

    Reference slot = Reference::fromStackSlot(codegen, slotIndex);
    if (isConst()) {
        Instruction::MoveConst move;
        move.constIndex = codegen->registerConstant(constant);
        move.destTemp = slot.stackSlot();
        codegen->bytecodeGenerator->addInstruction(move);
    } else {
        loadInAccumulator();
        slot.storeConsumeAccumulator();
    }
    return slot;
}

Codegen::Reference Codegen::Reference::storeRetainAccumulator() const
{
    if (storeWipesAccumulator()) {
        // a store will
        auto tmp = Reference::fromStackSlot(codegen);
        tmp.storeAccumulator(); // this is safe, and won't destory the accumulator
        storeAccumulator();
        return tmp;
    } else {
        // ok, this is safe, just do the store.
        storeAccumulator();
        return *this;
    }
}

bool Codegen::Reference::storeWipesAccumulator() const
{
    switch (type) {
    default:
    case Invalid:
    case Const:
    case Accumulator:
        Q_UNREACHABLE();
        return false;
    case StackSlot:
    case ScopedLocal:
        return false;
    case Name:
    case Member:
    case Subscript:
    case QmlScopeObject:
    case QmlContextObject:
        return true;
    }
}

void Codegen::Reference::storeAccumulator() const
{
    switch (type) {
    case StackSlot: {
        Instruction::StoreReg store;
        store.reg = theStackSlot;
        codegen->bytecodeGenerator->addInstruction(store);
        return;
    }
    case ScopedLocal: {
        if (scope == 0) {
            Instruction::StoreLocal store;
            store.index = index;
            codegen->bytecodeGenerator->addInstruction(store);
        } else {
            Instruction::StoreScopedLocal store;
            store.index = index;
            store.scope = scope;
            codegen->bytecodeGenerator->addInstruction(store);
        }
        return;
    }
    case Name: {
        Context *c = codegen->currentContext();
        if (c->isStrict) {
            Instruction::StoreNameStrict store;
            store.name = nameAsIndex();
            codegen->bytecodeGenerator->addInstruction(store);
        } else {
            Instruction::StoreNameSloppy store;
            store.name = nameAsIndex();
            codegen->bytecodeGenerator->addInstruction(store);
        }
    } return;
    case Member:
        if (!disable_lookups && codegen->useFastLookups) {
            Instruction::SetLookup store;
            store.base = propertyBase.stackSlot();
            store.index = codegen->registerSetterLookup(propertyNameIndex);
            codegen->bytecodeGenerator->addInstruction(store);
        } else {
            Instruction::StoreProperty store;
            store.base = propertyBase.stackSlot();
            store.name = propertyNameIndex;
            codegen->bytecodeGenerator->addInstruction(store);
        }
        return;
    case Subscript: {
        Instruction::StoreElement store;
        store.base = elementBase;
        store.index = elementSubscript.stackSlot();
        codegen->bytecodeGenerator->addInstruction(store);
    } return;
    case QmlScopeObject: {
        Instruction::StoreScopeObjectProperty store;
        store.base = qmlBase;
        store.propertyIndex = qmlCoreIndex;
        codegen->bytecodeGenerator->addInstruction(store);
    } return;
    case QmlContextObject: {
        Instruction::StoreContextObjectProperty store;
        store.base = qmlBase;
        store.propertyIndex = qmlCoreIndex;
        codegen->bytecodeGenerator->addInstruction(store);
    } return;
    case Invalid:
    case Accumulator:
    case Const:
        break;
    }

    Q_ASSERT(false);
    Q_UNREACHABLE();
}

void Codegen::Reference::loadInAccumulator() const
{
    switch (type) {
    case Accumulator:
        return;
    case Const: {
QT_WARNING_PUSH
QT_WARNING_DISABLE_GCC("-Wmaybe-uninitialized") // the loads below are empty structs.
        if (constant == Encode::null()) {
            Instruction::LoadNull load;
            codegen->bytecodeGenerator->addInstruction(load);
        } else if (constant == Encode(true)) {
            Instruction::LoadTrue load;
            codegen->bytecodeGenerator->addInstruction(load);
        } else if (constant == Encode(false)) {
            Instruction::LoadFalse load;
            codegen->bytecodeGenerator->addInstruction(load);
        } else if (constant == Encode::undefined()) {
            Instruction::LoadUndefined load;
            codegen->bytecodeGenerator->addInstruction(load);
        } else {
            Value p = Primitive::fromReturnedValue(constant);
            if (p.isNumber()) {
                double d = p.asDouble();
                int i = static_cast<int>(d);
                if (d == i && (d != 0 || !std::signbit(d))) {
                    if (!i) {
                        Instruction::LoadZero load;
                        codegen->bytecodeGenerator->addInstruction(load);
                        return;
                    }
                    Instruction::LoadInt load;
                    load.value = Primitive::fromReturnedValue(constant).toInt32();
                    codegen->bytecodeGenerator->addInstruction(load);
                    return;
                }
            }
            Instruction::LoadConst load;
            load.index = codegen->registerConstant(constant);
            codegen->bytecodeGenerator->addInstruction(load);
        }
QT_WARNING_POP
    } return;
    case StackSlot: {
        Instruction::LoadReg load;
        load.reg = stackSlot();
        codegen->bytecodeGenerator->addInstruction(load);
    } return;
    case ScopedLocal: {
        if (!scope) {
            Instruction::LoadLocal load;
            load.index = index;
            codegen->bytecodeGenerator->addInstruction(load);
        } else {
            Instruction::LoadScopedLocal load;
            load.index = index;
            load.scope = scope;
            codegen->bytecodeGenerator->addInstruction(load);
        }
        return;
    }
    case Name:
        if (global) {
            // these value properties of the global object are immutable, we we can directly convert them
            // to their numeric value here
            if (name == QStringLiteral("undefined")) {
                Reference::fromConst(codegen, Encode::undefined()).loadInAccumulator();
                return;
            } else if (name == QStringLiteral("Infinity")) {
                Reference::fromConst(codegen, Encode(qInf())).loadInAccumulator();
                return;
            } else if (name == QStringLiteral("Nan")) {
                Reference::fromConst(codegen, Encode(qQNaN())).loadInAccumulator();
                return;
            }
        }
        if (!disable_lookups && codegen->useFastLookups && global) {
            Instruction::LoadGlobalLookup load;
            load.index = codegen->registerGlobalGetterLookup(nameAsIndex());
            codegen->bytecodeGenerator->addInstruction(load);
        } else {
            Instruction::LoadName load;
            load.name = nameAsIndex();
            codegen->bytecodeGenerator->addInstruction(load);
        }
        return;
    case Member:
        if (!disable_lookups && codegen->useFastLookups) {
            if (propertyBase.isAccumulator()) {
                Instruction::GetLookupA load;
                load.index = codegen->registerGetterLookup(propertyNameIndex);
                codegen->bytecodeGenerator->addInstruction(load);
            } else {
                Instruction::GetLookup load;
                load.base = propertyBase.storeOnStack().stackSlot();
                load.index = codegen->registerGetterLookup(propertyNameIndex);
                codegen->bytecodeGenerator->addInstruction(load);
            }
        } else {
            if (propertyBase.isAccumulator()) {
                Instruction::LoadPropertyA load;
                load.name = propertyNameIndex;
                codegen->bytecodeGenerator->addInstruction(load);
            } else {
                Instruction::LoadProperty load;
                load.base = propertyBase.storeOnStack().stackSlot();
                load.name = propertyNameIndex;
                codegen->bytecodeGenerator->addInstruction(load);
            }
        }
        return;
    case Subscript: {
        if (elementSubscript.isAccumulator()) {
            Instruction::LoadElementA load;
            load.base = elementBase;
            codegen->bytecodeGenerator->addInstruction(load);
        } else if (elementSubscript.isConst()) {
            Reference::fromConst(codegen, elementSubscript.constantValue()).loadInAccumulator();
            Instruction::LoadElementA load;
            load.base = elementBase;
            codegen->bytecodeGenerator->addInstruction(load);
        } else {
            Instruction::LoadElement load;
            load.base = elementBase;
            load.index = elementSubscript.storeOnStack().stackSlot();
            codegen->bytecodeGenerator->addInstruction(load);
        }
    } return;
    case QmlScopeObject: {
        Instruction::LoadScopeObjectProperty load;
        load.base = qmlBase;
        load.propertyIndex = qmlCoreIndex;
        load.captureRequired = capturePolicy == CaptureAtRuntime;
        codegen->bytecodeGenerator->addInstruction(load);
        if (capturePolicy == CaptureAheadOfTime)
            codegen->_context->scopeObjectPropertyDependencies.insert(qmlCoreIndex, qmlNotifyIndex);
    } return;
    case QmlContextObject: {
        Instruction::LoadContextObjectProperty load;
        load.base = qmlBase;
        load.propertyIndex = qmlCoreIndex;
        load.captureRequired = capturePolicy == CaptureAtRuntime;
        codegen->bytecodeGenerator->addInstruction(load);
        if (capturePolicy == CaptureAheadOfTime)
            codegen->_context->contextObjectPropertyDependencies.insert(qmlCoreIndex, qmlNotifyIndex);
    } return;
    case Invalid:
        break;
    }
    Q_ASSERT(false);
    Q_UNREACHABLE();
}
