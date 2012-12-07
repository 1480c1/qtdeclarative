/****************************************************************************
**
** Copyright (C) 2012 Digia Plc and/or its subsidiary(-ies).
** Contact: http://www.qt-project.org/legal
**
** This file is part of Qt Creator.
**
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
****************************************************************************/

#ifndef DEBUGGING_H
#define DEBUGGING_H

#include "qmljs_engine.h"
#include "qmljs_environment.h"

#include <QHash>

namespace QQmlJS {

namespace IR {
struct BasicBlock;
struct Function;
} // namespace IR

namespace Debugging {

class Debugger;

struct FunctionDebugInfo { // TODO: use opaque d-pointers here
    IR::Function *function;
    QHash<ptrdiff_t, IR::BasicBlock *> blockOffsets;
    unsigned startLine, startColumn;

    FunctionDebugInfo(IR::Function *function): function(function), startLine(0), startColumn(0) {}

    void addBasicBlockOffset(IR::BasicBlock *block, ptrdiff_t offset) {
        blockOffsets.insert(offset, block);
    }

    void setSourceLocation(unsigned line, unsigned column)
    { startLine = line; startColumn = column; }
};

class FunctionState
{
public:
    FunctionState(VM::ExecutionContext *context);
    virtual ~FunctionState();

    virtual VM::Value *argument(unsigned idx);
    virtual VM::Value *local(unsigned idx);
    virtual VM::Value *temp(unsigned idx) = 0;

    VM::ExecutionContext *context() const
    { return _context; }

    Debugger *debugger() const
    { return _context->engine->debugger; }

private:
    VM::ExecutionContext *_context;
};

struct CallInfo
{
    VM::ExecutionContext *context;
    VM::FunctionObject *function;
    FunctionState *state;

    CallInfo(VM::ExecutionContext *context = 0, VM::FunctionObject *function = 0, FunctionState *state = 0)
        : context(context)
        , function(function)
        , state(state)
    {}
};

class Debugger
{
public:
    Debugger(VM::ExecutionEngine *_engine);
    ~Debugger();

public: // compile-time interface
    void addFunction(IR::Function *function);
    void addaddBasicBlockOffset(IR::Function *function, IR::BasicBlock *block, ptrdiff_t blockOffset);
    void setSourceLocation(IR::Function *function, unsigned line, unsigned column);

public: // run-time querying interface
    FunctionDebugInfo *debugInfo(VM::FunctionObject *function) const;
    QString name(VM::FunctionObject *function) const;

public: // execution hooks
    void aboutToCall(VM::FunctionObject *function, VM::ExecutionContext *context);
    void justLeft(VM::ExecutionContext *context);
    void enterFunction(FunctionState *state);
    void leaveFunction(FunctionState *state);
    void aboutToThrow(VM::Value *value);

public: // debugging hooks
    FunctionState *currentState() const;
    const char *currentArg(unsigned idx) const;
    const char *currentLocal(unsigned idx) const;
    const char *currentTemp(unsigned idx) const;
    void printStackTrace() const;

private:
    int callIndex(VM::ExecutionContext *context);

private: // TODO: use opaque d-pointers here
    VM::ExecutionEngine *_engine;
    QHash<IR::Function *, FunctionDebugInfo *> _functionInfo;
    QVector<CallInfo> _callStack;
};

} // namespace Debugging
} // namespace QQmlJS

#endif // DEBUGGING_H
