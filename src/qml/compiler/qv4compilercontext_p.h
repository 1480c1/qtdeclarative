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
#ifndef QV4COMPILERCONTEXT_P_H
#define QV4COMPILERCONTEXT_P_H

//
//  W A R N I N G
//  -------------
//
// This file is not part of the Qt API.  It exists purely as an
// implementation detail.  This header file may change from version to
// version without notice, or even be removed.
//
// We mean it.
//

#include "private/qv4global_p.h"
#include <private/qqmljsast_p.h>
#include <private/qv4compileddata_p.h>
#include <QtCore/QStringList>
#include <QtCore/QDateTime>
#include <QtCore/QStack>
#include <QtCore/QHash>

QT_BEGIN_NAMESPACE

namespace QV4 {

namespace Compiler {

struct ControlFlow;

enum class ContextType {
    Global,
    Function,
    Eval,
    Binding, // This is almost the same as Eval, except:
               //  * function declarations are moved to the return address when encountered
               //  * return statements are allowed everywhere (like in FunctionCode)
               //  * variable declarations are treated as true locals (like in FunctionCode)
    Block
};

struct Context;

struct Module {
    Module(bool debugMode)
        : debugMode(debugMode)
    {}
    ~Module() {
        qDeleteAll(contextMap);
    }

    Context *newContext(QQmlJS::AST::Node *node, Context *parent, ContextType compilationMode);

    QHash<QQmlJS::AST::Node *, Context *> contextMap;
    QList<Context *> functions;
    QList<Context *> blocks;
    Context *rootContext;
    QString fileName;
    QString finalUrl;
    QDateTime sourceTimeStamp;
    uint unitFlags = 0; // flags merged into CompiledData::Unit::flags
    bool debugMode = false;
};


struct Context {
    Context *parent;
    QString name;
    int line = 0;
    int column = 0;
    int registerCountInFunction = 0;
    int functionIndex = -1;
    int blockIndex = -1;

    enum MemberType {
        UndefinedMember,
        ThisFunctionName,
        VariableDefinition,
        VariableDeclaration,
        FunctionDefinition
    };

    struct Member {
        MemberType type = UndefinedMember;
        int index = -1;
        QQmlJS::AST::VariableScope scope = QQmlJS::AST::VariableScope::Var;
        mutable bool canEscape = false;
        QQmlJS::AST::FunctionExpression *function = nullptr;

        bool isLexicallyScoped() const { return this->scope != QQmlJS::AST::VariableScope::Var; }
    };
    typedef QMap<QString, Member> MemberMap;

    MemberMap members;
    QSet<QString> usedVariables;
    QQmlJS::AST::FormalParameterList *formals = nullptr;
    QStringList arguments;
    QStringList locals;
    QVector<Context *> nestedContexts;

    ControlFlow *controlFlow = nullptr;
    QByteArray code;
    QVector<CompiledData::CodeOffsetToLine> lineNumberMapping;

    int maxNumberOfArguments = 0;
    int nRegisters = 0;
    int registerOffset = -1;
    bool hasDirectEval = false;
    bool allVarsEscape = false;
    bool hasNestedFunctions = false;
    bool isStrict = false;
    bool isArrowFunction = false;
    bool isGenerator = false;
    bool usesThis = false;
    bool hasTry = false;
    bool returnsClosure = false;
    mutable bool argumentsCanEscape = false;
    bool requiresExecutionContext = false;
    bool isWithBlock = false;
    bool isCatchBlock = false;
    QString caughtVariable;

    enum UsesArgumentsObject {
        ArgumentsObjectUnknown,
        ArgumentsObjectNotUsed,
        ArgumentsObjectUsed
    };

    UsesArgumentsObject usesArgumentsObject = ArgumentsObjectUnknown;

    ContextType contextType;

    template <typename T>
    class SmallSet: public QVarLengthArray<T, 8>
    {
    public:
        void insert(int value)
        {
            for (auto it : *this) {
                if (it == value)
                    return;
            }
            this->append(value);
        }
    };

    // Map from meta property index (existence implies dependency) to notify signal index
    struct KeyValuePair
    {
        quint32 _key = 0;
        quint32 _value = 0;

        KeyValuePair() {}
        KeyValuePair(quint32 key, quint32 value): _key(key), _value(value) {}

        quint32 key() const { return _key; }
        quint32 value() const { return _value; }
    };

    class PropertyDependencyMap: public QVarLengthArray<KeyValuePair, 8>
    {
    public:
        void insert(quint32 key, quint32 value)
        {
            for (auto it = begin(), eit = end(); it != eit; ++it) {
                if (it->_key == key) {
                    it->_value = value;
                    return;
                }
            }
            append(KeyValuePair(key, value));
        }
    };

    // Qml extension:
    SmallSet<int> idObjectDependencies;
    PropertyDependencyMap contextObjectPropertyDependencies;
    PropertyDependencyMap scopeObjectPropertyDependencies;

    Context(Context *parent, ContextType type)
        : parent(parent)
        , contextType(type)
    {
        if (parent && parent->isStrict)
            isStrict = true;
    }

    int findArgument(const QString &name)
    {
        // search backwards to handle duplicate argument names correctly
        for (int i = arguments.size() - 1; i >= 0; --i) {
            if (arguments.at(i) == name)
                return i;
        }
        return -1;
    }

    Member findMember(const QString &name) const
    {
        MemberMap::const_iterator it = members.find(name);
        if (it == members.end())
            return Member();
        Q_ASSERT(it->index != -1 || !parent);
        return (*it);
    }

    bool memberInfo(const QString &name, const Member **m) const
    {
        Q_ASSERT(m);
        MemberMap::const_iterator it = members.find(name);
        if (it == members.end()) {
            *m = nullptr;
            return false;
        }
        *m = &(*it);
        return true;
    }

    bool requiresImplicitReturnValue() const {
        return contextType == ContextType::Binding ||
               contextType == ContextType::Eval ||
               contextType == ContextType::Global;
    }

    void addUsedVariable(const QString &name) {
        usedVariables.insert(name);
    }

    bool addLocalVar(const QString &name, MemberType contextType, QQmlJS::AST::VariableScope scope, QQmlJS::AST::FunctionExpression *function = nullptr);

    struct ResolvedName {
        enum Type {
            Unresolved,
            Global,
            Local,
            Stack
        };
        Type type = Unresolved;
        bool isArgOrEval = false;
        int scope = -1;
        int index = -1;
        bool isValid() const { return type != Unresolved; }
    };
    ResolvedName resolveName(const QString &name);
    void emitBlockHeader(Compiler::Codegen *codegen);
    void emitBlockFooter(Compiler::Codegen *codegen);

    void setupFunctionIndices(Moth::BytecodeGenerator *bytecodeGenerator);
};


} } // namespace QV4::Compiler

QT_END_NAMESPACE

#endif // QV4CODEGEN_P_H
