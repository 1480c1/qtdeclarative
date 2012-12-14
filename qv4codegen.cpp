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

#include "qv4codegen_p.h"
#include "debugging.h"

#include <QtCore/QStringList>
#include <QtCore/QSet>
#include <QtCore/QBuffer>
#include <QtCore/QBitArray>
#include <QtCore/QStack>
#include <private/qqmljsast_p.h>
#include <qmljs_runtime.h>
#include <qmljs_environment.h>
#include <cmath>
#include <iostream>
#include <cassert>

using namespace QQmlJS;
using namespace AST;

namespace {
QTextStream qout(stdout, QIODevice::WriteOnly);

void dfs(IR::BasicBlock *block,
         QSet<IR::BasicBlock *> *V,
         QVector<IR::BasicBlock *> *blocks)
{
    if (! V->contains(block)) {
        V->insert(block);

        foreach (IR::BasicBlock *succ, block->out)
            dfs(succ, V, blocks);

        blocks->append(block);
    }
}

struct ComputeUseDef: IR::StmtVisitor, IR::ExprVisitor
{
    IR::Function *_function;
    IR::Stmt *_stmt;

    ComputeUseDef(IR::Function *function)
        : _function(function)
        , _stmt(0) {}

    void operator()(IR::Stmt *s) {
        assert(! s->d);
        s->d = new IR::Stmt::Data;
        qSwap(_stmt, s);
        _stmt->accept(this);
        qSwap(_stmt, s);
    }

    virtual void visitConst(IR::Const *) {}
    virtual void visitString(IR::String *) {}
    virtual void visitRegExp(IR::RegExp *) {}
    virtual void visitName(IR::Name *) {}
    virtual void visitClosure(IR::Closure *) {}
    virtual void visitUnop(IR::Unop *e) { e->expr->accept(this); }
    virtual void visitBinop(IR::Binop *e) { e->left->accept(this); e->right->accept(this); }
    virtual void visitSubscript(IR::Subscript *e) { e->base->accept(this); e->index->accept(this); }
    virtual void visitMember(IR::Member *e) { e->base->accept(this); }
    virtual void visitExp(IR::Exp *s) { s->expr->accept(this); }
    virtual void visitEnter(IR::Enter *) {}
    virtual void visitLeave(IR::Leave *) {}
    virtual void visitJump(IR::Jump *) {}
    virtual void visitCJump(IR::CJump *s) { s->cond->accept(this); }
    virtual void visitRet(IR::Ret *s) { s->expr->accept(this); }

    virtual void visitTemp(IR::Temp *e) {
        if (e->index < 0)
            return;

        if (! _stmt->d->uses.contains(e->index))
            _stmt->d->uses.append(e->index);
    }

    virtual void visitCall(IR::Call *e) {
        e->base->accept(this);
        for (IR::ExprList *it = e->args; it; it = it->next)
            it->expr->accept(this);
    }

    virtual void visitNew(IR::New *e) {
        e->base->accept(this);
        for (IR::ExprList *it = e->args; it; it = it->next)
            it->expr->accept(this);
    }

    virtual void visitMove(IR::Move *s) {
        if (IR::Temp *t = s->target->asTemp()) {
            if (t->index >= 0) {
                if (! _stmt->d->defs.contains(t->index))
                    _stmt->d->defs.append(t->index);
            }
        } else {
            s->target->accept(this);
        }
        s->source->accept(this);
    }
};

void liveness(IR::Function *function)
{
    QSet<IR::BasicBlock *> V;
    QVector<IR::BasicBlock *> blocks;

    ComputeUseDef computeUseDef(function);
    foreach (IR::BasicBlock *block, function->basicBlocks) {
        foreach (IR::Stmt *s, block->statements)
            computeUseDef(s);
    }

    dfs(function->basicBlocks.at(0), &V, &blocks);

    bool changed;
    do {
        changed = false;

        foreach (IR::BasicBlock *block, blocks) {
            const QBitArray previousLiveIn = block->liveIn;
            const QBitArray previousLiveOut = block->liveOut;
            QBitArray live(function->tempCount);
            foreach (IR::BasicBlock *succ, block->out)
                live |= succ->liveIn;
            block->liveOut = live;
            for (int i = block->statements.size() - 1; i != -1; --i) {
                IR::Stmt *s = block->statements.at(i);
                s->d->liveOut = live;
                foreach (unsigned d, s->d->defs)
                    live.clearBit(d);
                foreach (unsigned u, s->d->uses)
                    live.setBit(u);
                s->d->liveIn = live;
            }
            block->liveIn = live;
            if (! changed) {
                if (previousLiveIn != block->liveIn || previousLiveOut != block->liveOut)
                    changed = true;
            }
        }
    } while (changed);
}

} // end of anonymous namespace

class Codegen::ScanFunctions: Visitor
{
public:
    ScanFunctions(Codegen *cg)
        : _cg(cg)
        , _env(0)
    {
    }

    void operator()(Node *node)
    {
        _env = 0;
        if (node)
            node->accept(this);
    }

protected:
    using Visitor::visit;
    using Visitor::endVisit;

    inline void enterEnvironment(Node *node)
    {
        Environment *e = _cg->newEnvironment(node, _env);
        if (!e->isStrict)
            e->isStrict = _cg->_context->strictMode;
        _envStack.append(e);
        _env = e;
    }

    inline void leaveEnvironment()
    {
        _envStack.pop();
        _env = _envStack.isEmpty() ? 0 : _envStack.top();
    }

    void checkDirectivePrologue(SourceElements *ast)
    {
        for (SourceElements *it = ast; it; it = it->next) {
            if (StatementSourceElement *stmt = cast<StatementSourceElement *>(it->element)) {
                if (ExpressionStatement *expr = cast<ExpressionStatement *>(stmt->statement)) {
                    if (StringLiteral *strLit = cast<StringLiteral *>(expr->expression)) {
                        if (strLit->value == QLatin1String("use strict")) {
                            _env->isStrict = true;
                        } else {
                            // TODO: give a warning.
                        }
                        continue;
                    }
                }
            }

            break;
        }
    }

    void checkName(const QStringRef &name, const SourceLocation &loc)
    {
        if (_env->isStrict) {
            if (name == QLatin1String("implements")
                    || name == QLatin1String("interface")
                    || name == QLatin1String("let")
                    || name == QLatin1String("package")
                    || name == QLatin1String("private")
                    || name == QLatin1String("protected")
                    || name == QLatin1String("public")
                    || name == QLatin1String("static")
                    || name == QLatin1String("yield")) {
                _cg->throwSyntaxError(loc, "Unexpected strict mode reserved word");
            }
        }
    }
    void checkForArguments(AST::FormalParameterList *parameters)
    {
        while (parameters) {
            if (parameters->name == QStringLiteral("arguments"))
                _env->usesArgumentsObject = Environment::ArgumentsObjectNotUsed;
            parameters = parameters->next;
        }
    }

    virtual bool visit(Program *ast)
    {
        enterEnvironment(ast);
        checkDirectivePrologue(ast->elements);
        return true;
    }

    virtual void endVisit(Program *)
    {
        leaveEnvironment();
    }

    virtual bool visit(CallExpression *ast)
    {
        if (! _env->hasDirectEval) {
            if (IdentifierExpression *id = cast<IdentifierExpression *>(ast->base)) {
                if (id->name == QStringLiteral("eval")) {
                    if (_env->usesArgumentsObject == Environment::ArgumentsObjectUnknown)
                        _env->usesArgumentsObject = Environment::ArgumentsObjectUsed;
                    _env->hasDirectEval = true;
                }
            }
        }
        int argc = 0;
        for (ArgumentList *it = ast->arguments; it; it = it->next)
            ++argc;
        _env->maxNumberOfArguments = qMax(_env->maxNumberOfArguments, argc);
        return true;
    }

    virtual bool visit(NewMemberExpression *ast)
    {
        int argc = 0;
        for (ArgumentList *it = ast->arguments; it; it = it->next)
            ++argc;
        _env->maxNumberOfArguments = qMax(_env->maxNumberOfArguments, argc);
        return true;
    }

    virtual bool visit(VariableDeclaration *ast)
    {
        checkName(ast->name, ast->identifierToken);
        if (ast->name == QLatin1String("arguments"))
            _env->usesArgumentsObject = Environment::ArgumentsObjectNotUsed;
        _env->enter(ast->name.toString());
        return true;
    }

    virtual bool visit(IdentifierExpression *ast)
    {
        checkName(ast->name, ast->identifierToken);
        if (_env->usesArgumentsObject == Environment::ArgumentsObjectUnknown && ast->name == QLatin1String("arguments"))
            _env->usesArgumentsObject = Environment::ArgumentsObjectUsed;
        return true;
    }

    virtual bool visit(FunctionExpression *ast)
    {
        if (_env) {
            _env->hasNestedFunctions = true;
            _env->enter(ast->name.toString());
        }
        enterEnvironment(ast);
        checkForArguments(ast->formals);
        if (ast->body)
            checkDirectivePrologue(ast->body->elements);
        return true;
    }

    virtual void endVisit(FunctionExpression *)
    {
        leaveEnvironment();
    }

    virtual bool visit(FunctionDeclaration *ast)
    {
        _env->functions.append(ast);
        _env->hasNestedFunctions = true;
        _env->enter(ast->name.toString());
        if (ast->name == QLatin1String("arguments"))
            _env->usesArgumentsObject = Environment::ArgumentsObjectNotUsed;
        enterEnvironment(ast);
        checkForArguments(ast->formals);
        if (ast->body)
            checkDirectivePrologue(ast->body->elements);
        return true;
    }

    virtual void endVisit(FunctionDeclaration *)
    {
        leaveEnvironment();
    }

    virtual bool visit(WithStatement *ast)
    {
        if (_env->isStrict) {
            // TODO: give a proper error message
            qDebug("TODO: give proper error message @%u:%u", ast->withToken.startLine, ast->withToken.startColumn);
            return false;
        }

        return true;
    }

    Codegen *_cg;
    Environment *_env;
    QStack<Environment *> _envStack;
};

Codegen::Codegen(VM::ExecutionContext *context)
    : _module(0)
    , _function(0)
    , _block(0)
    , _exitBlock(0)
    , _throwBlock(0)
    , _returnAddress(0)
    , _mode(GlobalCode)
    , _env(0)
    , _loop(0)
    , _labelledStatement(0)
    , _tryCleanup(0)
    , _context(context)
    , _debugger(context->engine->debugger)
{
}

IR::Function *Codegen::operator()(const QString &fileName, Program *node, IR::Module *module, Mode mode)
{
    assert(node);

    _fileName = fileName;
    _module = module;
    _env = 0;

    ScanFunctions scan(this);
    scan(node);

    IR::Function *globalCode = defineFunction(QStringLiteral("%entry"), node, 0, node->elements, mode);
    if (_debugger) {
        if (node->elements->element) {
            SourceLocation loc = node->elements->element->firstSourceLocation();
            _debugger->setSourceLocation(globalCode, loc.startLine, loc.startColumn);
        }
    }

    foreach (IR::Function *function, _module->functions) {
        linearize(function);
    }

    qDeleteAll(_envMap);
    _envMap.clear();

    return globalCode;
}

IR::Function *Codegen::operator()(const QString &fileName, AST::FunctionExpression *ast, IR::Module *module)
{
    _fileName = fileName;
    _module = module;
    _env = 0;

    ScanFunctions scan(this);
    scan(ast);

    IR::Function *function = defineFunction(ast->name.toString(), ast, ast->formals, ast->body ? ast->body->elements : 0);
    if (_debugger)
        _debugger->setSourceLocation(function, ast->functionToken.startLine, ast->functionToken.startColumn);

    foreach (IR::Function *function, _module->functions) {
        linearize(function);
    }

    qDeleteAll(_envMap);
    _envMap.clear();

    return function;
}


void Codegen::enterEnvironment(Node *node)
{
    _env = _envMap.value(node);
    assert(_env);
}

void Codegen::leaveEnvironment()
{
    assert(_env);
    _env = _env->parent;
}

void Codegen::enterLoop(Statement *node, IR::BasicBlock *breakBlock, IR::BasicBlock *continueBlock)
{
    _loop = new Loop(node, breakBlock, continueBlock, _loop);
    _loop->labelledStatement = _labelledStatement; // consume the enclosing labelled statement
    _loop->tryCleanup = _tryCleanup;
    _labelledStatement = 0;
}

void Codegen::leaveLoop()
{
    Loop *current = _loop;
    _loop = _loop->parent;
    delete current;
}

IR::Expr *Codegen::member(IR::Expr *base, const QString *name)
{
    if (base->asTemp() /*|| base->asName()*/)
        return _block->MEMBER(base->asTemp(), name);
    else {
        const unsigned t = _block->newTemp();
        move(_block->TEMP(t), base);
        return _block->MEMBER(_block->TEMP(t), name);
    }
}

IR::Expr *Codegen::subscript(IR::Expr *base, IR::Expr *index)
{
    if (! base->asTemp()) {
        const unsigned t = _block->newTemp();
        move(_block->TEMP(t), base);
        base = _block->TEMP(t);
    }

    if (! index->asTemp()) {
        const unsigned t = _block->newTemp();
        move(_block->TEMP(t), index);
        index = _block->TEMP(t);
    }

    assert(base->asTemp() && index->asTemp());
    return _block->SUBSCRIPT(base->asTemp(), index->asTemp());
}

IR::Expr *Codegen::argument(IR::Expr *expr)
{
    if (expr && ! expr->asTemp()) {
        const unsigned t = _block->newTemp();
        move(_block->TEMP(t), expr);
        expr = _block->TEMP(t);
    }
    return expr;
}

// keeps references alive, converts other expressions to temps
IR::Expr *Codegen::reference(IR::Expr *expr)
{
    if (expr && !expr->asTemp() && !expr->asName() && !expr->asMember() && !expr->asSubscript()) {
        const unsigned t = _block->newTemp();
        move(_block->TEMP(t), expr);
        expr = _block->TEMP(t);
    }
    return expr;
}

IR::Expr *Codegen::unop(IR::AluOp op, IR::Expr *expr)
{
    if (IR::Const *c = expr->asConst()) {
        if (c->type == IR::NumberType) {
            switch (op) {
            case IR::OpNot:
                return _block->CONST(IR::BoolType, !c->value);
            case IR::OpUMinus:
                return _block->CONST(IR::NumberType, -c->value);
            case IR::OpUPlus:
                return expr;
            case IR::OpCompl:
                return _block->CONST(IR::NumberType, ~VM::Value::toInt32(c->value));
            case IR::OpIncrement:
                return _block->CONST(IR::NumberType, c->value + 1);
            case IR::OpDecrement:
                return _block->CONST(IR::NumberType, c->value - 1);
            default:
                break;
            }
        }
    }
    if (! expr->asTemp()) {
        const unsigned t = _block->newTemp();
        move(_block->TEMP(t), expr);
        expr = _block->TEMP(t);
    }
    assert(expr->asTemp());
    return _block->UNOP(op, expr->asTemp());
}

IR::Expr *Codegen::binop(IR::AluOp op, IR::Expr *left, IR::Expr *right)
{
    if (IR::Const *c1 = left->asConst()) {
        if (IR::Const *c2 = right->asConst()) {
            if (c1->type == IR::NumberType && c2->type == IR::NumberType) {
                switch (op) {
                case IR::OpAdd: return _block->CONST(IR::NumberType, c1->value + c2->value);
                case IR::OpAnd: return _block->CONST(IR::BoolType, c1->value ? c2->value : 0);
                case IR::OpBitAnd: return _block->CONST(IR::NumberType, int(c1->value) & int(c2->value));
                case IR::OpBitOr: return _block->CONST(IR::NumberType, int(c1->value) | int(c2->value));
                case IR::OpBitXor: return _block->CONST(IR::NumberType, int(c1->value) ^ int(c2->value));
                case IR::OpDiv: return _block->CONST(IR::NumberType, c1->value / c2->value);
                case IR::OpEqual: return _block->CONST(IR::BoolType, c1->value == c2->value);
                case IR::OpNotEqual: return _block->CONST(IR::BoolType, c1->value != c2->value);
                case IR::OpStrictEqual: return _block->CONST(IR::BoolType, c1->value == c2->value);
                case IR::OpStrictNotEqual: return _block->CONST(IR::BoolType, c1->value != c2->value);
                case IR::OpGe: return _block->CONST(IR::BoolType, c1->value >= c2->value);
                case IR::OpGt: return _block->CONST(IR::BoolType, c1->value > c2->value);
                case IR::OpLe: return _block->CONST(IR::BoolType, c1->value <= c2->value);
                case IR::OpLt: return _block->CONST(IR::BoolType, c1->value < c2->value);
                case IR::OpLShift: return _block->CONST(IR::NumberType, VM::Value::toInt32(c1->value) << (VM::Value::toUInt32(c2->value) & 0x1f));
                case IR::OpMod: return _block->CONST(IR::NumberType, ::fmod(c1->value, c2->value));
                case IR::OpMul: return _block->CONST(IR::NumberType, c1->value * c2->value);
                case IR::OpOr: return _block->CONST(IR::NumberType, c1->value ? c1->value : c2->value);
                case IR::OpRShift: return _block->CONST(IR::NumberType, VM::Value::toInt32(c1->value) << (VM::Value::toUInt32(c2->value) & 0x1f));
                case IR::OpSub: return _block->CONST(IR::NumberType, c1->value - c2->value);
                case IR::OpURShift: return _block->CONST(IR::NumberType,VM::Value::toUInt32(c1->value) << (VM::Value::toUInt32(c2->value) & 0x1f));

                case IR::OpInstanceof:
                case IR::OpIn:
                    assert(!"unreachabe");
                    break;

                case IR::OpIfTrue: // unary ops
                case IR::OpNot:
                case IR::OpUMinus:
                case IR::OpUPlus:
                case IR::OpCompl:
                case IR::OpIncrement:
                case IR::OpDecrement:
                case IR::OpInvalid:
                    break;
                }
            }
        }
    } else if (op == IR::OpAdd) {
        if (IR::String *s1 = left->asString()) {
            if (IR::String *s2 = right->asString()) {
                return _block->STRING(_function->newString(*s1->value + *s2->value));
            }
        }
    }

    if (!left->asTemp() && !left->asConst()) {
        const unsigned t = _block->newTemp();
        move(_block->TEMP(t), left);
        left = _block->TEMP(t);
    }

    if (!right->asTemp() && !right->asConst()) {
        const unsigned t = _block->newTemp();
        move(_block->TEMP(t), right);
        right = _block->TEMP(t);
    }

    assert(left->asTemp() || left->asConst());
    assert(right->asTemp() || right->asConst());

    return _block->BINOP(op, left, right);
}

IR::Expr *Codegen::call(IR::Expr *base, IR::ExprList *args)
{
    if (base->asMember() || base->asName() || base->asTemp())
        return _block->CALL(base, args);
    const unsigned t = _block->newTemp();
    move(_block->TEMP(t), base);
    return _block->CALL(_block->TEMP(t), args);
}

void Codegen::move(IR::Expr *target, IR::Expr *source, IR::AluOp op)
{
    assert(target->isLValue());

    if (!source->asTemp() && !source->asConst() && (op != IR::OpInvalid || ! target->asTemp())) {
        unsigned t = _block->newTemp();
        _block->MOVE(_block->TEMP(t), source);
        source = _block->TEMP(t);
    }

    _block->MOVE(target, source, op);
}

void Codegen::cjump(IR::Expr *cond, IR::BasicBlock *iftrue, IR::BasicBlock *iffalse)
{
    if (! (cond->asTemp() || cond->asBinop())) {
        const unsigned t = _block->newTemp();
        move(_block->TEMP(t), cond);
        cond = _block->TEMP(t);
    }
    _block->CJUMP(cond, iftrue, iffalse);
}

void Codegen::accept(Node *node)
{
    if (node)
        node->accept(this);
}

void Codegen::statement(Statement *ast)
{
    accept(ast);
}

void Codegen::statement(ExpressionNode *ast)
{
    if (! ast) {
        return;
    } else if (_mode == EvalCode) {
        Result e = expression(ast);
        if (*e)
            move(_block->TEMP(_returnAddress), *e);
    } else {
        Result r(nx);
        qSwap(_expr, r);
        accept(ast);
        qSwap(_expr, r);
        if (r.format == ex) {
            if (r->asCall()) {
                _block->EXP(*r); // the nest nx representation for calls is EXP(CALL(c..))
            } else if (r->asTemp()) {
                // there is nothing to do
            } else {
                unsigned t = _block->newTemp();
                move(_block->TEMP(t), *r);
            }
        }
    }
}

void Codegen::condition(ExpressionNode *ast, IR::BasicBlock *iftrue, IR::BasicBlock *iffalse)
{
    if (ast) {
        Result r(iftrue, iffalse);
        qSwap(_expr, r);
        accept(ast);
        qSwap(_expr, r);
        if (r.format == ex) {
            cjump(*r, r.iftrue, r.iffalse);
        }
    }
}

Codegen::Result Codegen::expression(ExpressionNode *ast)
{
    Result r;
    if (ast) {
        qSwap(_expr, r);
        accept(ast);
        qSwap(_expr, r);
    }
    return r;
}

QString Codegen::propertyName(PropertyName *ast)
{
    QString p;
    if (ast) {
        qSwap(_property, p);
        accept(ast);
        qSwap(_property, p);
    }
    return p;
}

Codegen::Result Codegen::sourceElement(SourceElement *ast)
{
    Result r(nx);
    if (ast) {
        qSwap(_expr, r);
        accept(ast);
        qSwap(_expr, r);
    }
    return r;
}

Codegen::UiMember Codegen::uiObjectMember(UiObjectMember *ast)
{
    UiMember m;
    if (ast) {
        qSwap(_uiMember, m);
        accept(ast);
        qSwap(_uiMember, m);
    }
    return m;
}

void Codegen::functionBody(FunctionBody *ast)
{
    if (ast)
        sourceElements(ast->elements);
}

void Codegen::program(Program *ast)
{
    if (ast) {
        sourceElements(ast->elements);
    }
}

void Codegen::sourceElements(SourceElements *ast)
{
    for (SourceElements *it = ast; it; it = it->next) {
        sourceElement(it->element);
    }
}

void Codegen::variableDeclaration(VariableDeclaration *ast)
{
    IR::Expr *initializer = 0;
    if (ast->expression) {
        Result expr = expression(ast->expression);
        assert(expr.code);
        initializer = *expr;
    }

    if (! initializer)
        initializer = _block->CONST(IR::UndefinedType, 0);

    if (! _env->parent || _function->insideWith) {
        // it's global code.
        move(_block->NAME(ast->name.toString(), ast->identifierToken.startLine, ast->identifierToken.startColumn), initializer);
    } else {
        const int index = _env->findMember(ast->name.toString());
        assert(index != -1);
        move(_block->TEMP(index), initializer);
    }
}

void Codegen::variableDeclarationList(VariableDeclarationList *ast)
{
    for (VariableDeclarationList *it = ast; it; it = it->next) {
        variableDeclaration(it->declaration);
    }
}


bool Codegen::visit(ArgumentList *)
{
    assert(!"unreachable");
    return false;
}

bool Codegen::visit(CaseBlock *)
{
    assert(!"unreachable");
    return false;
}

bool Codegen::visit(CaseClause *)
{
    assert(!"unreachable");
    return false;
}

bool Codegen::visit(CaseClauses *)
{
    assert(!"unreachable");
    return false;
}

bool Codegen::visit(Catch *)
{
    assert(!"unreachable");
    return false;
}

bool Codegen::visit(DefaultClause *)
{
    assert(!"unreachable");
    return false;
}

bool Codegen::visit(ElementList *)
{
    assert(!"unreachable");
    return false;
}

bool Codegen::visit(Elision *)
{
    assert(!"unreachable");
    return false;
}

bool Codegen::visit(Finally *)
{
    assert(!"unreachable");
    return false;
}

bool Codegen::visit(FormalParameterList *)
{
    assert(!"unreachable");
    return false;
}

bool Codegen::visit(FunctionBody *)
{
    assert(!"unreachable");
    return false;
}

bool Codegen::visit(Program *)
{
    assert(!"unreachable");
    return false;
}

bool Codegen::visit(PropertyNameAndValueList *)
{
    assert(!"unreachable");
    return false;
}

bool Codegen::visit(SourceElements *)
{
    assert(!"unreachable");
    return false;
}

bool Codegen::visit(StatementList *)
{
    assert(!"unreachable");
    return false;
}

bool Codegen::visit(UiArrayMemberList *)
{
    assert(!"unreachable");
    return false;
}

bool Codegen::visit(UiImport *)
{
    assert(!"unreachable");
    return false;
}

bool Codegen::visit(UiImportList *)
{
    assert(!"unreachable");
    return false;
}

bool Codegen::visit(UiObjectInitializer *)
{
    assert(!"unreachable");
    return false;
}

bool Codegen::visit(UiObjectMemberList *)
{
    assert(!"unreachable");
    return false;
}

bool Codegen::visit(UiParameterList *)
{
    assert(!"unreachable");
    return false;
}

bool Codegen::visit(UiProgram *)
{
    assert(!"unreachable");
    return false;
}

bool Codegen::visit(UiQualifiedId *)
{
    assert(!"unreachable");
    return false;
}

bool Codegen::visit(VariableDeclaration *)
{
    assert(!"unreachable");
    return false;
}

bool Codegen::visit(VariableDeclarationList *)
{
    assert(!"unreachable");
    return false;
}

bool Codegen::visit(Expression *ast)
{
    statement(ast->left);
    accept(ast->right);
    return false;
}

bool Codegen::visit(ArrayLiteral *ast)
{
    const unsigned t = _block->newTemp();
    move(_block->TEMP(t), _block->NEW(_block->NAME(QStringLiteral("Array"), ast->firstSourceLocation().startLine, ast->firstSourceLocation().startColumn)));
    int index = 0;
    for (ElementList *it = ast->elements; it; it = it->next) {
        for (Elision *elision = it->elision; elision; elision = elision->next)
            ++index;
        Result expr = expression(it->expression);
        move(subscript(_block->TEMP(t), _block->CONST(IR::NumberType, index)), *expr);
        ++index;
    }
    for (Elision *elision = ast->elision; elision; elision = elision->next)
        ++index; // ### set the size of the array
    _expr.code = _block->TEMP(t);
    return false;
}

bool Codegen::visit(ArrayMemberExpression *ast)
{
    Result base = expression(ast->base);
    Result index = expression(ast->expression);
    _expr.code = subscript(*base, *index);
    return false;
}

static IR::AluOp baseOp(int op)
{
    switch ((QSOperator::Op) op) {
    case QSOperator::InplaceAnd: return IR::OpBitAnd;
    case QSOperator::InplaceSub: return IR::OpSub;
    case QSOperator::InplaceDiv: return IR::OpDiv;
    case QSOperator::InplaceAdd: return IR::OpAdd;
    case QSOperator::InplaceLeftShift: return IR::OpLShift;
    case QSOperator::InplaceMod: return IR::OpMod;
    case QSOperator::InplaceMul: return IR::OpMul;
    case QSOperator::InplaceOr: return IR::OpBitOr;
    case QSOperator::InplaceRightShift: return IR::OpRShift;
    case QSOperator::InplaceURightShift: return IR::OpURShift;
    case QSOperator::InplaceXor: return IR::OpBitXor;
    default: return IR::OpInvalid;
    }
}

bool Codegen::visit(BinaryExpression *ast)
{
    if (ast->op == QSOperator::And) {
        if (_expr.accept(cx)) {
            IR::BasicBlock *iftrue = _function->newBasicBlock();
            condition(ast->left, iftrue, _expr.iffalse);
            _block = iftrue;
            condition(ast->right, _expr.iftrue, _expr.iffalse);
        } else {
            IR::BasicBlock *iftrue = _function->newBasicBlock();
            IR::BasicBlock *iffalse = _function->newBasicBlock();
            IR::BasicBlock *endif = _function->newBasicBlock();

            const unsigned r = _block->newTemp();

            condition(ast->left, iftrue, iffalse);
            _block = iffalse;

            move(_block->TEMP(r), _block->CONST(IR::BoolType, 0));
            _block->JUMP(endif);

            _block = iftrue;
            move(_block->TEMP(r), *expression(ast->right));
            _block->JUMP(endif);

            _expr.code = _block->TEMP(r);
            _block = endif;
        }
        return false;
    } else if (ast->op == QSOperator::Or) {
        if (_expr.accept(cx)) {
            IR::BasicBlock *iffalse = _function->newBasicBlock();
            condition(ast->left, _expr.iftrue, iffalse);
            _block = iffalse;
            condition(ast->right, _expr.iftrue, _expr.iffalse);
        } else {
            IR::BasicBlock *iffalse = _function->newBasicBlock();
            IR::BasicBlock *endif = _function->newBasicBlock();

            const unsigned r = _block->newTemp();
            move(_block->TEMP(r), *expression(ast->left));
            cjump(_block->TEMP(r), endif, iffalse);
            _block = iffalse;
            move(_block->TEMP(r), *expression(ast->right));
            _block->JUMP(endif);

            _block = endif;
            _expr.code = _block->TEMP(r);
        }
        return false;
    }

    Result left = expression(ast->left);
    Result right = expression(ast->right);

    switch (ast->op) {
    case QSOperator::Or:
    case QSOperator::And:
        break;

    case QSOperator::Assign:
        if (! (left->asTemp() || left->asName() || left->asSubscript() || left->asMember())) {
            throwSyntaxError(ast->lastSourceLocation(), "syntax error");
        }

        if (_expr.accept(nx)) {
            move(*left, *right);
        } else {
            const unsigned t = _block->newTemp();
            move(_block->TEMP(t), *right);
            move(*left, _block->TEMP(t));
            _expr.code = *left;
        }
        break;

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
    case QSOperator::InplaceXor: {
        if (! (left->asTemp() || left->asName() || left->asSubscript() || left->asMember())) {
            throwSyntaxError(ast->lastSourceLocation(), "syntax error");
        }

        if (_expr.accept(nx)) {
            move(*left, *right, baseOp(ast->op));
        } else {
            const unsigned t = _block->newTemp();
            move(_block->TEMP(t), *right);
            move(*left, _block->TEMP(t), baseOp(ast->op));
            _expr.code = *left;
        }
        break;
    }

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
        if (_expr.accept(cx)) {
            cjump(binop(IR::binaryOperator(ast->op), *left, *right), _expr.iftrue, _expr.iffalse);
        } else {
            IR::Expr *e = binop(IR::binaryOperator(ast->op), *left, *right);
            if (e->asConst() || e->asString())
                _expr.code = e;
            else {
                const unsigned t = _block->newTemp();
                move(_block->TEMP(t), e);
                _expr.code = _block->TEMP(t);
            }
        }
        break;

    case QSOperator::Add:
    case QSOperator::BitAnd:
    case QSOperator::BitOr:
    case QSOperator::BitXor:
    case QSOperator::Div:
    case QSOperator::LShift:
    case QSOperator::Mod:
    case QSOperator::Mul:
    case QSOperator::RShift:
    case QSOperator::Sub:
    case QSOperator::URShift: {
        IR::Expr *e = binop(IR::binaryOperator(ast->op), *left, *right);
        if (e->asConst() || e->asString())
            _expr.code = e;
        else {
            const unsigned t = _block->newTemp();
            move(_block->TEMP(t), e);
            _expr.code = _block->TEMP(t);
        }
        break;
    }

    } // switch

    return false;
}

bool Codegen::visit(CallExpression *ast)
{
    Result base = expression(ast->base);
    IR::ExprList *args = 0, **args_it = &args;
    for (ArgumentList *it = ast->arguments; it; it = it->next) {
        Result arg = expression(it->expression);
        IR::Expr *actual = argument(*arg);
        *args_it = _function->New<IR::ExprList>();
        (*args_it)->init(actual);
        args_it = &(*args_it)->next;
    }
    _expr.code = call(*base, args);
    return false;
}

bool Codegen::visit(ConditionalExpression *ast)
{
    IR::BasicBlock *iftrue = _function->newBasicBlock();
    IR::BasicBlock *iffalse = _function->newBasicBlock();
    IR::BasicBlock *endif = _function->newBasicBlock();

    const unsigned t = _block->newTemp();

    condition(ast->expression, iftrue, iffalse);

    _block = iftrue;
    move(_block->TEMP(t), *expression(ast->ok));
    _block->JUMP(endif);

    _block = iffalse;
    move(_block->TEMP(t), *expression(ast->ko));
    _block->JUMP(endif);

    _block = endif;

    _expr.code = _block->TEMP(t);

    return false;
}

bool Codegen::visit(DeleteExpression *ast)
{
    Result expr = expression(ast->expression);
    if ((*expr)->asTemp() && (*expr)->asTemp()->index < 0) {
        // expr points to a function argument
        if (_function->isStrict)
            throwSyntaxError(ast->deleteToken, "Delete of an unqualified identifier in strict mode.");
        // can't delete an argument, just evaluate expr for side effects
        _expr.accept(nx);
        return false;
    }
    if (_function->isStrict && (*expr)->asName())
        throwSyntaxError(ast->deleteToken, "Delete of an unqualified identifier in strict mode.");
    IR::ExprList *args = _function->New<IR::ExprList>();
    args->init(reference(*expr));
    _expr.code = call(_block->NAME(IR::Name::builtin_delete, ast->deleteToken.startLine, ast->deleteToken.startColumn), args);
    return false;
}

bool Codegen::visit(FalseLiteral *)
{
    if (_expr.accept(cx)) {
        _block->JUMP(_expr.iffalse);
    } else {
        _expr.code = _block->CONST(IR::BoolType, 0);
    }
    return false;
}

bool Codegen::visit(FieldMemberExpression *ast)
{
    Result base = expression(ast->base);
    _expr.code = member(*base, _function->newString(ast->name.toString()));
    return false;
}

bool Codegen::visit(FunctionExpression *ast)
{
    IR::Function *function = defineFunction(ast->name.toString(), ast, ast->formals, ast->body ? ast->body->elements : 0);
    if (_debugger)
        _debugger->setSourceLocation(function, ast->functionToken.startLine, ast->functionToken.startColumn);
    _expr.code = _block->CLOSURE(function);
    return false;
}

IR::Expr *Codegen::identifier(const QString &name, int line, int col)
{
    int index = _env->findMember(name);

    if (! _function->hasDirectEval && !_function->insideWith && _env->parent) {
        if (index != -1) {
            return _block->TEMP(index);
        }
        index = indexOfArgument(&name);
        if (index != -1) {
            return _block->TEMP(-(index + 1));
        }
    }

    if (index >= _env->vars.size()) {
        // named local variable, e.g. in a catch statement
        return _block->TEMP(index);
    }

    return _block->NAME(name, line, col);

}

bool Codegen::visit(IdentifierExpression *ast)
{
    _expr.code = identifier(ast->name.toString(), ast->identifierToken.startLine, ast->identifierToken.startColumn);
    return false;
}

bool Codegen::visit(NestedExpression *ast)
{
    accept(ast->expression);
    return false;
}

bool Codegen::visit(NewExpression *ast)
{
    Result base = expression(ast->expression);
    IR::Expr *expr = *base;
    if (expr && !expr->asTemp() && !expr->asName() && !expr->asMember()) {
        const unsigned t = _block->newTemp();
        move(_block->TEMP(t), expr);
        expr = _block->TEMP(t);
    }
    _expr.code = _block->NEW(expr, 0);
    return false;
}

bool Codegen::visit(NewMemberExpression *ast)
{
    Result base = expression(ast->base);
    IR::ExprList *args = 0, **args_it = &args;
    for (ArgumentList *it = ast->arguments; it; it = it->next) {
        Result arg = expression(it->expression);
        IR::Expr *actual = argument(*arg);
        *args_it = _function->New<IR::ExprList>();
        (*args_it)->init(actual);
        args_it = &(*args_it)->next;
    }
    const unsigned t = _block->newTemp();
    move(_block->TEMP(t), _block->NEW(*base, args));
    _expr.code = _block->TEMP(t);
    return false;
}

bool Codegen::visit(NotExpression *ast)
{
    Result expr = expression(ast->expression);
    const unsigned r = _block->newTemp();
    move(_block->TEMP(r), unop(IR::OpNot, *expr));
    _expr.code = _block->TEMP(r);
    return false;
}

bool Codegen::visit(NullExpression *)
{
    if (_expr.accept(cx)) _block->JUMP(_expr.iffalse);
    else _expr.code = _block->CONST(IR::NullType, 0);

    return false;
}

bool Codegen::visit(NumericLiteral *ast)
{
    if (_expr.accept(cx)) {
        if (ast->value) _block->JUMP(_expr.iftrue);
        else _block->JUMP(_expr.iffalse);
    } else {
        _expr.code = _block->CONST(IR::NumberType, ast->value);
    }
    return false;
}

bool Codegen::visit(ObjectLiteral *ast)
{
    const unsigned t = _block->newTemp();
    move(_block->TEMP(t), _block->NEW(_block->NAME(QStringLiteral("Object"), ast->firstSourceLocation().startLine, ast->firstSourceLocation().startColumn)));
    for (PropertyNameAndValueList *it = ast->properties; it; it = it->next) {
        QString name = propertyName(it->name);
        Result value = expression(it->value);
        move(member(_block->TEMP(t), _function->newString(name)), *value);
    }
    _expr.code = _block->TEMP(t);
    return false;
}

bool Codegen::visit(PostDecrementExpression *ast)
{
    // ###
    //    Throw a SyntaxError exception if the following conditions are all true:
    //    Type(lhs) is Reference is true
    //    IsStrictReference(lhs) is true
    //    Type(GetBase(lhs)) is Environment Record
    //    GetReferencedName(lhs) is either "eval" or "arguments"


    Result expr = expression(ast->base);
    if (_expr.accept(nx)) {
        move(*expr, unop(IR::OpDecrement, *expr));
    } else {
        const unsigned t = _block->newTemp();
        move(_block->TEMP(t), *expr);
        move(*expr, unop(IR::OpDecrement, _block->TEMP(t)));
        _expr.code = _block->TEMP(t);
    }
    return false;
}

bool Codegen::visit(PostIncrementExpression *ast)
{
    // ###
    //    Throw a SyntaxError exception if the following conditions are all true:
    //    Type(lhs) is Reference is true
    //    IsStrictReference(lhs) is true
    //    Type(GetBase(lhs)) is Environment Record
    //    GetReferencedName(lhs) is either "eval" or "arguments"

    Result expr = expression(ast->base);
    if (_expr.accept(nx)) {
        move(*expr, unop(IR::OpIncrement, *expr));
    } else {
        const unsigned t = _block->newTemp();
        move(_block->TEMP(t), *expr);
        move(*expr, unop(IR::OpIncrement, _block->TEMP(t)));
        _expr.code = _block->TEMP(t);
    }
    return false;
}

bool Codegen::visit(PreDecrementExpression *ast)
{
    // ###
    //    Throw a SyntaxError exception if the following conditions are all true:
    //    Type(lhs) is Reference is true
    //    IsStrictReference(lhs) is true
    //    Type(GetBase(lhs)) is Environment Record
    //    GetReferencedName(lhs) is either "eval" or "arguments"

    Result expr = expression(ast->expression);
    move(*expr, unop(IR::OpDecrement, *expr));
    if (_expr.accept(nx)) {
        // nothing to do
    } else {
        _expr.code = *expr;
    }
    return false;
}

bool Codegen::visit(PreIncrementExpression *ast)
{
    // ###
    //    Throw a SyntaxError exception if the following conditions are all true:
    //    Type(lhs) is Reference is true
    //    IsStrictReference(lhs) is true
    //    Type(GetBase(lhs)) is Environment Record
    //    GetReferencedName(lhs) is either "eval" or "arguments"

    Result expr = expression(ast->expression);
    move(*expr, unop(IR::OpIncrement, *expr));
    if (_expr.accept(nx)) {
        // nothing to do
    } else {
        _expr.code = *expr;
    }
    return false;
}

bool Codegen::visit(RegExpLiteral *ast)
{
    _expr.code = _block->REGEXP(_function->newString(ast->pattern.toString()), ast->flags);
    return false;
}

bool Codegen::visit(StringLiteral *ast)
{
    _expr.code = _block->STRING(_function->newString(ast->value.toString()));
    return false;
}

bool Codegen::visit(ThisExpression *ast)
{
    _expr.code = _block->NAME(QStringLiteral("this"), ast->thisToken.startLine, ast->thisToken.startColumn);
    return false;
}

bool Codegen::visit(TildeExpression *ast)
{
    Result expr = expression(ast->expression);
    const unsigned t = _block->newTemp();
    move(_block->TEMP(t), unop(IR::OpCompl, *expr));
    _expr.code = _block->TEMP(t);
    return false;
}

bool Codegen::visit(TrueLiteral *)
{
    if (_expr.accept(cx)) {
        _block->JUMP(_expr.iftrue);
    } else {
        _expr.code = _block->CONST(IR::BoolType, 1);
    }
    return false;
}

bool Codegen::visit(TypeOfExpression *ast)
{
    Result expr = expression(ast->expression);
    IR::ExprList *args = _function->New<IR::ExprList>();
    args->init(reference(*expr));
    _expr.code = call(_block->NAME(IR::Name::builtin_typeof, ast->typeofToken.startLine, ast->typeofToken.startColumn), args);
    return false;
}

bool Codegen::visit(UnaryMinusExpression *ast)
{
    Result expr = expression(ast->expression);
    const unsigned t = _block->newTemp();
    move(_block->TEMP(t), unop(IR::OpUMinus, *expr));
    _expr.code = _block->TEMP(t);
    return false;
}

bool Codegen::visit(UnaryPlusExpression *ast)
{
    Result expr = expression(ast->expression);
    const unsigned t = _block->newTemp();
    move(_block->TEMP(t), unop(IR::OpUPlus, *expr));
    _expr.code = _block->TEMP(t);
    return false;
}

bool Codegen::visit(VoidExpression *ast)
{
    statement(ast->expression);
    _expr.code = _block->CONST(IR::UndefinedType, 0);
    return false;
}

bool Codegen::visit(FunctionDeclaration * /*ast*/)
{
    _expr.accept(nx);
    return false;
}

void Codegen::linearize(IR::Function *function)
{
    IR::BasicBlock *exitBlock = function->basicBlocks.last();
    assert(exitBlock->isTerminated());
    assert(exitBlock->terminator()->asRet());

    QSet<IR::BasicBlock *> V;
    V.insert(exitBlock);

    QVector<IR::BasicBlock *> trace;

    for (int i = 0; i < function->basicBlocks.size(); ++i) {
        IR::BasicBlock *block = function->basicBlocks.at(i);
        if (!block->isTerminated() && (i + 1) < function->basicBlocks.size()) {
            IR::BasicBlock *next = function->basicBlocks.at(i + 1);
            block->JUMP(next);
        }
    }

    struct I { static void trace(IR::BasicBlock *block, QSet<IR::BasicBlock *> *V,
                                 QVector<IR::BasicBlock *> *output) {
            if (block == 0 || V->contains(block))
                return;

            V->insert(block);
            block->index = output->size();
            output->append(block);

            if (IR::Stmt *term = block->terminator()) {
                if (IR::Jump *j = term->asJump()) {
                    trace(j->target, V, output);
                } else if (IR::CJump *cj = term->asCJump()) {
                    if (! V->contains(cj->iffalse))
                        trace(cj->iffalse, V, output);
                    else
                        trace(cj->iftrue, V, output);
                }
            }

            // We could do this for each type above, but it is safer to have a
            // "catchall" here
            for (int ii = 0; ii < block->out.count(); ++ii)
                trace(block->out.at(ii), V, output);
        }
    };

    I::trace(function->basicBlocks.first(), &V, &trace);

    V.insert(exitBlock);
    exitBlock->index = trace.size();
    trace.append(exitBlock);

    QVarLengthArray<IR::BasicBlock*> blocksToDelete;
    foreach (IR::BasicBlock *b, function->basicBlocks)
        if (!V.contains(b)) {
                foreach (IR::BasicBlock *out, b->out) {
                    int idx = out->in.indexOf(b);
                    if (idx >= 0)
                        out->in.remove(idx);
                }
                blocksToDelete.append(b);
            }
    qDeleteAll(blocksToDelete);
    function->basicBlocks = trace;

#ifndef QV4_NO_LIVENESS
    liveness(function);
#endif

    static bool showCode = !qgetenv("SHOW_CODE").isNull();
    if (showCode) {
        QVector<IR::Stmt *> code;
        QHash<IR::Stmt *, IR::BasicBlock *> leader;

        foreach (IR::BasicBlock *block, function->basicBlocks) {
            leader.insert(block->statements.first(), block);
            foreach (IR::Stmt *s, block->statements) {
                code.append(s);
            }
        }

        QString name;
        if (function->name && !function->name->isEmpty())
            name = *function->name;
        else
            name.sprintf("%p", function);

        qout << "function " << name << "(";
        for (int i = 0; i < function->formals.size(); ++i) {
            if (i != 0)
                qout << ", ";
            qout << *function->formals.at(i);
        }
        qout << ")" << endl
             << "{" << endl;

        foreach (const QString *local, function->locals) {
            qout << "    var " << *local << ';' << endl;
        }

        for (int i = 0; i < code.size(); ++i) {
            IR::Stmt *s = code.at(i);

            if (IR::BasicBlock *bb = leader.value(s)) {
                qout << endl;
                QByteArray str;
                str.append('L');
                str.append(QByteArray::number(bb->index));
                str.append(':');
                for (int i = 66 - str.length(); i; --i)
                    str.append(' ');
                qout << str;
                qout << "// predecessor blocks:";
                foreach (IR::BasicBlock *in, bb->in)
                    qout << " L" << in->index;
                qout << endl;
            }
            IR::Stmt *n = (i + 1) < code.size() ? code.at(i + 1) : 0;
            if (n && s->asJump() && s->asJump()->target == leader.value(n)) {
                continue;
            }

            QByteArray str;
            QBuffer buf(&str);
            buf.open(QIODevice::WriteOnly);
            QTextStream out(&buf);
            s->dump(out, IR::Stmt::MIR);
            out.flush();

#ifndef QV4_NO_LIVENESS
            for (int i = 60 - str.size(); i >= 0; --i)
                str.append(' ');

            qout << "    " << str;

            //        if (! s->uses.isEmpty()) {
            //            qout << " // uses:";
            //            foreach (unsigned use, s->uses) {
            //                qout << " %" << use;
            //            }
            //        }

            //        if (! s->defs.isEmpty()) {
            //            qout << " // defs:";
            //            foreach (unsigned def, s->defs) {
            //                qout << " %" << def;
            //            }
            //        }

            if (! s->d->liveOut.isEmpty()) {
                qout << " // lives out:";
                for (int i = 0; i < s->d->liveOut.size(); ++i) {
                    if (s->d->liveOut.testBit(i))
                        qout << " %" << i;
                }
            }
#else
            qout << "    " << str;
#endif

            qout << endl;

            if (n && s->asCJump() && s->asCJump()->iffalse != leader.value(n)) {
                qout << "    goto L" << s->asCJump()->iffalse << ";" << endl;
            }
        }

        qout << "}" << endl
             << endl;
    }
}

IR::Function *Codegen::defineFunction(const QString &name, AST::Node *ast,
                                      AST::FormalParameterList *formals,
                                      AST::SourceElements *body, Mode mode)
{
    qSwap(_mode, mode); // enter function code.

    TryCleanup *tryCleanup = 0;

    enterEnvironment(ast);
    IR::Function *function = _module->newFunction(name, _function);

    if (_debugger)
        _debugger->addFunction(function);
    IR::BasicBlock *entryBlock = function->newBasicBlock();
    IR::BasicBlock *exitBlock = function->newBasicBlock(IR::Function::DontInsertBlock);
    IR::BasicBlock *throwBlock = function->newBasicBlock();
    function->hasDirectEval = _env->hasDirectEval;
    function->usesArgumentsObject = (_env->usesArgumentsObject == Environment::ArgumentsObjectUsed);
    function->maxNumberOfArguments = _env->maxNumberOfArguments;
    function->isStrict = _env->isStrict;

    // variables in global code are properties of the global context object, not locals as with other functions.
    if (_mode == FunctionCode) {
        for (int i = 0; i < _env->vars.size(); ++i) {
            const QString &local = _env->vars.at(i);
            function->LOCAL(local);
            unsigned t = entryBlock->newTemp();
            assert(t == unsigned(i));
        }
    } else {
        IR::ExprList *args = 0;
        for (int i = 0; i < _env->vars.size(); ++i) {
            const QString &local = _env->vars.at(i);
            IR::ExprList *next = function->New<IR::ExprList>();
            next->expr = entryBlock->NAME(local, 0, 0);
            next->next = args;
            args = next;
        }
        if (args) {
            IR::ExprList *next = function->New<IR::ExprList>();
            next->expr = entryBlock->CONST(IR::BoolType, mode == EvalCode);
            next->next = args;
            args = next;

            entryBlock->EXP(entryBlock->CALL(entryBlock->NAME(IR::Name::builtin_declare_vars, 0, 0), args));
        }
    }

    unsigned returnAddress = entryBlock->newTemp();

    entryBlock->MOVE(entryBlock->TEMP(returnAddress), entryBlock->CONST(IR::UndefinedType, 0));
    exitBlock->RET(exitBlock->TEMP(returnAddress));
    IR::ExprList *throwArgs = function->New<IR::ExprList>();
    throwArgs->expr = throwBlock->TEMP(returnAddress);
    throwBlock->EXP(throwBlock->CALL(throwBlock->NAME(IR::Name::builtin_throw, /*line*/0, /*column*/0), throwArgs));

    qSwap(_function, function);
    qSwap(_block, entryBlock);
    qSwap(_exitBlock, exitBlock);
    qSwap(_throwBlock, throwBlock);
    qSwap(_returnAddress, returnAddress);
    qSwap(_tryCleanup, tryCleanup);

    for (FormalParameterList *it = formals; it; it = it->next) {
        _function->RECEIVE(it->name.toString());
    }

    foreach (AST::FunctionDeclaration *f, _env->functions) {
        IR::Function *function = defineFunction(f->name.toString(), f, f->formals,
                                                f->body ? f->body->elements : 0);
        if (_debugger)
            _debugger->setSourceLocation(function, f->functionToken.startLine, f->functionToken.startColumn);
        if (! _env->parent)
            move(_block->NAME(f->name.toString(), f->identifierToken.startLine, f->identifierToken.startColumn),
                 _block->CLOSURE(function));
        else
            move(_block->TEMP(_env->findMember(f->name.toString())),
                 _block->CLOSURE(function));
    }

    sourceElements(body);

    _function->insertBasicBlock(_exitBlock);

    _block->JUMP(_exitBlock);

    qSwap(_function, function);
    qSwap(_block, entryBlock);
    qSwap(_exitBlock, exitBlock);
    qSwap(_throwBlock, throwBlock);
    qSwap(_returnAddress, returnAddress);
    qSwap(_tryCleanup, tryCleanup);

    leaveEnvironment();

    qSwap(_mode, mode);

    return function;
}

int Codegen::indexOfArgument(const QStringRef &string) const
{
    for (int i = 0; i < _function->formals.size(); ++i) {
        if (*_function->formals.at(i) == string)
            return i;
    }
    return -1;
}

bool Codegen::visit(IdentifierPropertyName *ast)
{
    _property = ast->id.toString();
    return false;
}

bool Codegen::visit(NumericLiteralPropertyName *ast)
{
    _property = QString::number(ast->id, 'g', 16);
    return false;
}

bool Codegen::visit(StringLiteralPropertyName *ast)
{
    _property = ast->id.toString();
    return false;
}

bool Codegen::visit(FunctionSourceElement *ast)
{
    statement(ast->declaration);
    return false;
}

bool Codegen::visit(StatementSourceElement *ast)
{
    statement(ast->statement);
    return false;
}

bool Codegen::visit(Block *ast)
{
    for (StatementList *it = ast->statements; it; it = it->next) {
        statement(it->statement);
    }
    return false;
}

bool Codegen::visit(BreakStatement *ast)
{
    unwindException(_loop->tryCleanup);
    if (ast->label.isEmpty())
        _block->JUMP(_loop->breakBlock);
    else {
        for (Loop *loop = _loop; loop; loop = loop->parent) {
            if (loop->labelledStatement && loop->labelledStatement->label == ast->label) {
                _block->JUMP(loop->breakBlock);
                return false;
            }
        }
        throwSyntaxError(ast->lastSourceLocation(), QString("Undefined label '") + ast->label.toString() + QString("'"));
    }

    return false;
}

bool Codegen::visit(ContinueStatement *ast)
{
    unwindException(_loop->tryCleanup);

    if (ast->label.isEmpty()) {
        _block->JUMP(_loop->continueBlock);
    } else {
        for (Loop *loop = _loop; loop; loop = loop->parent) {
            if (loop->labelledStatement && loop->labelledStatement->label == ast->label) {
                if (! loop->continueBlock)
                    break;

                _block->JUMP(loop->continueBlock);
                return false;
            }
        }
        throwSyntaxError(ast->lastSourceLocation(), QString("Undefined label '") + ast->label.toString() + QString("'"));
    }
    return false;
}

bool Codegen::visit(DebuggerStatement *)
{
    assert(!"not implemented");
    return false;
}

bool Codegen::visit(DoWhileStatement *ast)
{
    IR::BasicBlock *loopbody = _function->newBasicBlock();
    IR::BasicBlock *loopend = _function->newBasicBlock();

    enterLoop(ast, loopend, loopbody);

    _block->JUMP(loopbody);
    _block = loopbody;
    statement(ast->statement);
    condition(ast->expression, loopbody, loopend);
    _block = loopend;

    leaveLoop();

    return false;
}

bool Codegen::visit(EmptyStatement *)
{
    return false;
}

bool Codegen::visit(ExpressionStatement *ast)
{
    statement(ast->expression);
    return false;
}

bool Codegen::visit(ForEachStatement *ast)
{
    IR::BasicBlock *foreachin = _function->newBasicBlock();
    IR::BasicBlock *foreachbody = _function->newBasicBlock();
    IR::BasicBlock *foreachend = _function->newBasicBlock();

    enterLoop(ast, foreachend, foreachin);

    int iterator = _block->newTemp();
    move(_block->TEMP(iterator), *expression(ast->expression));
    IR::ExprList *args = _function->New<IR::ExprList>();
    args->init(_block->TEMP(iterator));
    move(_block->TEMP(iterator), _block->CALL(_block->NAME(IR::Name::builtin_foreach_iterator_object, 0, 0), args));

    _block->JUMP(foreachin);

    _block = foreachbody;
    int temp = _block->newTemp();
    move(*expression(ast->initialiser), _block->TEMP(temp));
    statement(ast->statement);
    _block->JUMP(foreachin);

    _block = foreachin;

    args = _function->New<IR::ExprList>();
    args->init(_block->TEMP(iterator));
    move(_block->TEMP(temp), _block->CALL(_block->NAME(IR::Name::builtin_foreach_next_property_name, 0, 0), args));
    int null = _block->newTemp();
    move(_block->TEMP(null), _block->CONST(IR::NullType, 0));
    cjump(_block->BINOP(IR::OpStrictNotEqual, _block->TEMP(temp), _block->TEMP(null)), foreachbody, foreachend);
    _block = foreachend;

    leaveLoop();
    return false;
}

bool Codegen::visit(ForStatement *ast)
{
    IR::BasicBlock *forcond = _function->newBasicBlock();
    IR::BasicBlock *forbody = _function->newBasicBlock();
    IR::BasicBlock *forstep = _function->newBasicBlock();
    IR::BasicBlock *forend = _function->newBasicBlock();

    enterLoop(ast, forend, forstep);

    statement(ast->initialiser);
    _block->JUMP(forcond);

    _block = forcond;
    condition(ast->condition, forbody, forend);

    _block = forbody;
    statement(ast->statement);
    _block->JUMP(forstep);

    _block = forstep;
    statement(ast->expression);
    _block->JUMP(forcond);
    _block = forend;

    leaveLoop();

    return false;
}

bool Codegen::visit(IfStatement *ast)
{
    IR::BasicBlock *iftrue = _function->newBasicBlock();
    IR::BasicBlock *iffalse = ast->ko ? _function->newBasicBlock() : 0;
    IR::BasicBlock *endif = _function->newBasicBlock();
    condition(ast->expression, iftrue, ast->ko ? iffalse : endif);

    _block = iftrue;
    statement(ast->ok);
    _block->JUMP(endif);

    if (ast->ko) {
        _block = iffalse;
        statement(ast->ko);
        _block->JUMP(endif);
    }

    _block = endif;

    return false;
}

bool Codegen::visit(LabelledStatement *ast)
{
    _labelledStatement = ast;

    if (AST::cast<AST::SwitchStatement *>(ast->statement) ||
            AST::cast<AST::WhileStatement *>(ast->statement) ||
            AST::cast<AST::DoWhileStatement *>(ast->statement) ||
            AST::cast<AST::ForStatement *>(ast->statement) ||
            AST::cast<AST::ForEachStatement *>(ast->statement) ||
            AST::cast<AST::LocalForStatement *>(ast->statement) ||
            AST::cast<AST::LocalForEachStatement *>(ast->statement)) {
        statement(ast->statement); // labelledStatement will be associated with the ast->statement's loop.
    } else {
        IR::BasicBlock *breakBlock = _function->newBasicBlock();
        enterLoop(ast->statement, breakBlock, /*continueBlock*/ 0);
        statement(ast->statement);
        _block->JUMP(breakBlock);
        _block = breakBlock;
    }

    return false;
}

bool Codegen::visit(LocalForEachStatement *ast)
{
    IR::BasicBlock *foreachin = _function->newBasicBlock();
    IR::BasicBlock *foreachbody = _function->newBasicBlock();
    IR::BasicBlock *foreachend = _function->newBasicBlock();

    enterLoop(ast, foreachend, foreachin);

    variableDeclaration(ast->declaration);

    int iterator = _block->newTemp();
    move(_block->TEMP(iterator), *expression(ast->expression));
    IR::ExprList *args = _function->New<IR::ExprList>();
    args->init(_block->TEMP(iterator));
    move(_block->TEMP(iterator), _block->CALL(_block->NAME(IR::Name::builtin_foreach_iterator_object, 0, 0), args));

    _block->JUMP(foreachin);

    _block = foreachbody;
    int temp = _block->newTemp();
    move(identifier(ast->declaration->name.toString()), _block->TEMP(temp));
    statement(ast->statement);
    _block->JUMP(foreachin);

    _block = foreachin;

    args = _function->New<IR::ExprList>();
    args->init(_block->TEMP(iterator));
    move(_block->TEMP(temp), _block->CALL(_block->NAME(IR::Name::builtin_foreach_next_property_name, 0, 0), args));
    int null = _block->newTemp();
    move(_block->TEMP(null), _block->CONST(IR::NullType, 0));
    cjump(_block->BINOP(IR::OpStrictNotEqual, _block->TEMP(temp), _block->TEMP(null)), foreachbody, foreachend);
    _block = foreachend;

    leaveLoop();
    return false;
}

bool Codegen::visit(LocalForStatement *ast)
{
    IR::BasicBlock *forcond = _function->newBasicBlock();
    IR::BasicBlock *forbody = _function->newBasicBlock();
    IR::BasicBlock *forstep = _function->newBasicBlock();
    IR::BasicBlock *forend = _function->newBasicBlock();

    enterLoop(ast, forend, forstep);

    variableDeclarationList(ast->declarations);
    _block->JUMP(forcond);

    _block = forcond;
    condition(ast->condition, forbody, forend);

    _block = forbody;
    statement(ast->statement);
    _block->JUMP(forstep);

    _block = forstep;
    statement(ast->expression);
    _block->JUMP(forcond);
    _block = forend;

    leaveLoop();

    return false;
}

bool Codegen::visit(ReturnStatement *ast)
{
    unwindException(0);
    if (ast->expression) {
        Result expr = expression(ast->expression);
        move(_block->TEMP(_returnAddress), *expr);
    }

    _block->JUMP(_exitBlock);
    return false;
}

bool Codegen::visit(SwitchStatement *ast)
{
    IR::BasicBlock *switchend = _function->newBasicBlock();

    if (ast->block) {
        Result lhs = expression(ast->expression);
        IR::BasicBlock *switchcond = _block;

        QHash<Node *, IR::BasicBlock *> blockMap;

        enterLoop(ast, switchend, 0);

        for (CaseClauses *it = ast->block->clauses; it; it = it->next) {
            CaseClause *clause = it->clause;

            _block = _function->newBasicBlock();
            blockMap[clause] = _block;

            for (StatementList *it2 = clause->statements; it2; it2 = it2->next)
                statement(it2->statement);
        }

        if (ast->block->defaultClause) {
            _block = _function->newBasicBlock();
            blockMap[ast->block->defaultClause] = _block;

            for (StatementList *it2 = ast->block->defaultClause->statements; it2; it2 = it2->next)
                statement(it2->statement);
        }

        for (CaseClauses *it = ast->block->moreClauses; it; it = it->next) {
            CaseClause *clause = it->clause;

            _block = _function->newBasicBlock();
            blockMap[clause] = _block;

            for (StatementList *it2 = clause->statements; it2; it2 = it2->next)
                statement(it2->statement);
        }

        leaveLoop();

        _block->JUMP(switchend);

        _block = switchcond;
        for (CaseClauses *it = ast->block->clauses; it; it = it->next) {
            CaseClause *clause = it->clause;
            Result rhs = expression(clause->expression);
            IR::BasicBlock *iftrue = blockMap[clause];
            IR::BasicBlock *iffalse = _function->newBasicBlock();
            cjump(binop(IR::OpEqual, *lhs, *rhs), iftrue, iffalse);
            _block = iffalse;
        }

        for (CaseClauses *it = ast->block->moreClauses; it; it = it->next) {
            CaseClause *clause = it->clause;
            Result rhs = expression(clause->expression);
            IR::BasicBlock *iftrue = blockMap[clause];
            IR::BasicBlock *iffalse = _function->newBasicBlock();
            cjump(binop(IR::OpEqual, *lhs, *rhs), iftrue, iffalse);
            _block = iffalse;
        }

        if (ast->block->defaultClause) {
            _block->JUMP(blockMap[ast->block->defaultClause]);
        }
    }

    _block->JUMP(switchend);

    _block = switchend;
    return false;
}

bool Codegen::visit(ThrowStatement *ast)
{
    Result expr = expression(ast->expression);
    move(_block->TEMP(_returnAddress), *expr);
    _block->JUMP(_throwBlock);
    return false;
}

bool Codegen::visit(TryStatement *ast)
{
    IR::BasicBlock *tryBody = _function->newBasicBlock();
    IR::BasicBlock *catchBody = ast->catchExpression ?  _function->newBasicBlock() : 0;
    // We always need a finally body to clean up the exception handler
    IR::BasicBlock *finallyBody = _function->newBasicBlock();

    TryCleanup tcf(_tryCleanup, ast->finallyExpression);
    _tryCleanup = &tcf;

    IR::BasicBlock *after = _function->newBasicBlock();
    assert(catchBody || finallyBody);

    int inCatch = 0;
    if (catchBody) {
        inCatch = _block->newTemp();
        move(_block->TEMP(inCatch), _block->CONST(IR::BoolType, false));
    }

    int hasException = _block->newTemp();
    move(_block->TEMP(hasException), _block->CALL(_block->NAME(IR::Name::builtin_create_exception_handler, 0, 0), 0));

    _block->CJUMP(_block->TEMP(hasException), catchBody ? catchBody : finallyBody, tryBody);

    _block = tryBody;
    statement(ast->statement);
    _block->JUMP(finallyBody ? finallyBody : after);

    // regular flow does not go into the catch statement
    if (catchBody) {
        _block = catchBody;

        if (inCatch != 0) {
            // check if an exception got thrown within catch. Go to finally
            // and then rethrow
            IR::BasicBlock *b = _function->newBasicBlock();
            _block->CJUMP(_block->TEMP(inCatch), finallyBody, b);
            _block = b;
        }
        // if we have finally we need to clear the exception here, so we don't rethrow
        move(_block->TEMP(inCatch), _block->CONST(IR::BoolType, true));
        move(_block->TEMP(hasException), _block->CONST(IR::BoolType, false));

        const int exception = _block->newTemp();
        move(_block->TEMP(exception), _block->CALL(_block->NAME(IR::Name::builtin_get_exception, 0, 0), 0));

        // the variable used in the catch statement is local and hides any global
        // variable with the same name.
        int hiddenIndex = _env->findMember(ast->catchExpression->name.toString());
        _env->members.insert(ast->catchExpression->name.toString(), exception);

        statement(ast->catchExpression->statement);

        // reset the variable name to the one from the outer scope
        _env->members.insert(ast->catchExpression->name.toString(), hiddenIndex);
        _block->JUMP(finallyBody ? finallyBody : after);
    }

    _tryCleanup = tcf.parent;

    if (finallyBody)
        generateFinallyBlock(finallyBody, !catchBody, ast->finallyExpression, hasException, after);

    _block = after;

    return false;
}

void Codegen::generateFinallyBlock(IR::BasicBlock *finallyBlock, bool exceptionNeedsSaving, Finally *ast, int hasException, IR::BasicBlock *after)
{
    _block = finallyBlock;

    int exception;
    if (exceptionNeedsSaving) {
        // save the exception so we can rethrow it later.
        IR::BasicBlock *saveExceptionBlock = _function->newBasicBlock();
        IR::BasicBlock *realFinallyBlock = _function->newBasicBlock();
        _block->CJUMP(_block->TEMP(hasException), saveExceptionBlock, realFinallyBlock);

        _block = saveExceptionBlock;
        exception = saveExceptionBlock->newTemp();
        move(_block->TEMP(exception), _block->CALL(_block->NAME(IR::Name::builtin_get_exception, 0, 0), 0));
        _block->JUMP(realFinallyBlock);

        _block = realFinallyBlock;
    }

    _block->EXP(_block->CALL(_block->NAME(IR::Name::builtin_delete_exception_handler, 0, 0), 0));
    if (ast && ast->statement)
        statement(ast->statement);

    if (exceptionNeedsSaving) {
        IR::BasicBlock *restoreExceptionBlock = _function->newBasicBlock();
        _block->CJUMP(_block->TEMP(hasException), restoreExceptionBlock, after);
        _block = restoreExceptionBlock;
        _block->MOVE(_block->TEMP(_returnAddress), _block->TEMP(exception));
        _block->JUMP(_throwBlock);
    } else {
        _block->CJUMP(_block->TEMP(hasException), _throwBlock, after);
    }
}

void Codegen::unwindException(Codegen::TryCleanup *outest)
{
    TryCleanup *tryCleanup = _tryCleanup;
    qSwap(_tryCleanup, tryCleanup);
    while (_tryCleanup != outest) {
        _block->EXP(_block->CALL(_block->NAME(IR::Name::builtin_delete_exception_handler, 0, 0), 0));
        TryCleanup *tc = _tryCleanup;
        _tryCleanup = tc->parent;
        if (tc->finally && tc->finally->statement)
            statement(tc->finally->statement);
    }
}

bool Codegen::visit(VariableStatement *ast)
{
    variableDeclarationList(ast->declarations);
    return false;
}

bool Codegen::visit(WhileStatement *ast)
{
    IR::BasicBlock *whilecond = _function->newBasicBlock();
    IR::BasicBlock *whilebody = _function->newBasicBlock();
    IR::BasicBlock *whileend = _function->newBasicBlock();

    enterLoop(ast, whileend, whilecond);

    _block->JUMP(whilecond);
    _block = whilecond;
    condition(ast->expression, whilebody, whileend);

    _block = whilebody;
    statement(ast->statement);
    _block->JUMP(whilecond);

    _block = whileend;
    leaveLoop();

    return false;
}

bool Codegen::visit(WithStatement *ast)
{
    IR::BasicBlock *withBlock = _function->newBasicBlock();

    _block->JUMP(withBlock);
    _block = withBlock;
    int withObject = _block->newTemp();
    _block->MOVE(_block->TEMP(withObject), *expression(ast->expression));
    IR::ExprList *args = _function->New<IR::ExprList>();
    args->init(_block->TEMP(withObject));
    _block->EXP(_block->CALL(_block->NAME(IR::Name::builtin_push_with, 0, 0), args));
    ++_function->insideWith;
    statement(ast->statement);
    --_function->insideWith;
    _block->EXP(_block->CALL(_block->NAME(IR::Name::builtin_pop_with, 0, 0), 0));

    IR::BasicBlock *next = _function->newBasicBlock();
    _block->JUMP(next);
    _block = next;

    return false;
}

bool Codegen::visit(UiArrayBinding *)
{
    assert(!"not implemented");
    return false;
}

bool Codegen::visit(UiObjectBinding *)
{
    assert(!"not implemented");
    return false;
}

bool Codegen::visit(UiObjectDefinition *)
{
    assert(!"not implemented");
    return false;
}

bool Codegen::visit(UiPublicMember *)
{
    assert(!"not implemented");
    return false;
}

bool Codegen::visit(UiScriptBinding *)
{
    assert(!"not implemented");
    return false;
}

bool Codegen::visit(UiSourceElement *)
{
    assert(!"not implemented");
    return false;
}

void Codegen::throwSyntaxError(const SourceLocation &loc, const QString &detail)
{
    VM::DiagnosticMessage *msg = new VM::DiagnosticMessage;
    msg->fileName = _fileName;
    msg->offset = loc.begin();
    msg->startLine = loc.startLine;
    msg->startColumn = loc.startColumn;
    msg->message = detail;
    _context->throwSyntaxError(msg);
}
