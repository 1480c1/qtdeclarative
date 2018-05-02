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
#include "qv4identifiertable_p.h"

QT_BEGIN_NAMESPACE

namespace QV4 {

static const uchar prime_deltas[] = {
    0,  0,  1,  3,  1,  5,  3,  3,  1,  9,  7,  5,  3,  9, 25,  3,
    1, 21,  3, 21,  7, 15,  9,  5,  3, 29, 15,  0,  0,  0,  0,  0
};

static inline int primeForNumBits(int numBits)
{
    return (1 << numBits) + prime_deltas[numBits];
}


IdentifierTable::IdentifierTable(ExecutionEngine *engine)
    : engine(engine)
    , size(0)
    , numBits(8)
{
    alloc = primeForNumBits(numBits);
    entriesByHash = (Heap::String **)malloc(alloc*sizeof(Heap::String *));
    entriesById = (Heap::String **)malloc(alloc*sizeof(Heap::String *));
    memset(entriesByHash, 0, alloc*sizeof(Heap::String *));
    memset(entriesById, 0, alloc*sizeof(Heap::String *));
}

IdentifierTable::~IdentifierTable()
{
    free(entriesByHash);
    free(entriesById);
    for (auto &h : idHashes)
        h->identifierTable = nullptr;
}

void IdentifierTable::addEntry(Heap::String *str)
{
    uint hash = str->hashValue();

    if (str->subtype == Heap::String::StringType_ArrayIndex)
        return;

    str->identifier = Identifier::fromHeapObject(str);

    bool grow = (alloc <= size*2);

    if (grow) {
        ++numBits;
        int newAlloc = primeForNumBits(numBits);
        Heap::String **newEntries = (Heap::String **)malloc(newAlloc*sizeof(Heap::String *));
        memset(newEntries, 0, newAlloc*sizeof(Heap::String *));
        for (int i = 0; i < alloc; ++i) {
            Heap::String *e = entriesByHash[i];
            if (!e)
                continue;
            uint idx = e->stringHash % newAlloc;
            while (newEntries[idx]) {
                ++idx;
                idx %= newAlloc;
            }
            newEntries[idx] = e;
        }
        free(entriesByHash);
        entriesByHash = newEntries;

        newEntries = (Heap::String **)malloc(newAlloc*sizeof(Heap::String *));
        memset(newEntries, 0, newAlloc*sizeof(Heap::String *));
        for (int i = 0; i < alloc; ++i) {
            Heap::String *e = entriesById[i];
            if (!e)
                continue;
            uint idx = e->identifier.id % newAlloc;
            while (newEntries[idx]) {
                ++idx;
                idx %= newAlloc;
            }
            newEntries[idx] = e;
        }
        free(entriesById);
        entriesById = newEntries;

        alloc = newAlloc;
    }

    uint idx = hash % alloc;
    while (entriesByHash[idx]) {
        ++idx;
        idx %= alloc;
    }
    entriesByHash[idx] = str;

    idx = str->identifier.id % alloc;
    while (entriesById[idx]) {
        ++idx;
        idx %= alloc;
    }
    entriesById[idx] = str;

    ++size;
}



Heap::String *IdentifierTable::insertString(const QString &s)
{
    uint subtype;
    uint hash = String::createHashValue(s.constData(), s.length(), &subtype);
    uint idx = hash % alloc;
    while (Heap::String *e = entriesByHash[idx]) {
        if (e->stringHash == hash && e->toQString() == s)
            return e;
        ++idx;
        idx %= alloc;
    }

    Heap::String *str = engine->newString(s);
    str->stringHash = hash;
    str->subtype = subtype;
    addEntry(str);
    return str;
}


Identifier IdentifierTable::identifierImpl(const Heap::String *str)
{
    if (str->identifier.isValid())
        return str->identifier;
    uint hash = str->hashValue();
    if (str->subtype == Heap::String::StringType_ArrayIndex) {
        str->identifier = Identifier::fromArrayIndex(hash);
        return str->identifier;
    }

    uint idx = hash % alloc;
    while (Heap::String *e = entriesByHash[idx]) {
        if (e->stringHash == hash && e->isEqualTo(str)) {
            str->identifier = e->identifier;
            return e->identifier;
        }
        ++idx;
        idx %= alloc;
    }

    addEntry(const_cast<QV4::Heap::String *>(str));
    return str->identifier;
}

Heap::String *IdentifierTable::stringForId(Identifier i) const
{
    uint arrayIdx = i.asArrayIndex();
    if (arrayIdx < UINT_MAX)
        return engine->newString(QString::number(arrayIdx));
    if (!i.isValid())
        return nullptr;

    uint idx = i.id % alloc;
    while (1) {
        Heap::String *e = entriesById[idx];
        Q_ASSERT(e);
        if (e->identifier == i)
            return e;
        ++idx;
        idx %= alloc;
    }
}

void IdentifierTable::markObjects(MarkStack *markStack)
{
    for (const auto &h : idHashes)
        h->markObjects(markStack);
}

template <typename Key>
int sweepTable(Heap::String **table, int alloc, std::function<Key(Heap::String *)> f) {
    int freed = 0;
    Key lastKey = 0;
    int lastEntry = -1;
    int start = 0;
    // start at an empty entry so we compress properly
    for (; start < alloc; ++start) {
        if (!table[start])
            break;
    }

    for (int i = 0; i < alloc; ++i) {
        int idx = (i + start) % alloc;
        Heap::String *entry = table[idx];
        if (!entry) {
            lastEntry = -1;
            continue;
        }
        if (entry->isMarked()) {
            if (lastEntry >= 0 && lastKey == f(entry)) {
                Q_ASSERT(table[lastEntry] == nullptr);
                table[lastEntry] = entry;
                table[idx] = nullptr;
                lastEntry = (lastEntry + 1) % alloc;
                Q_ASSERT(table[lastEntry] == nullptr);
            }
            continue;
        }
        if (lastEntry == -1) {
            lastEntry = idx;
            lastKey = f(entry);
        }
        table[idx] = nullptr;
        ++freed;
    }
    for (int i = 0; i < alloc; ++i) {
        Heap::String *entry = table[i];
        if (!entry)
            continue;
        Q_ASSERT(entry->isMarked());
    }
    return freed;
}

void IdentifierTable::sweep()
{
    int f = sweepTable<int>(entriesByHash, alloc, [](Heap::String *entry) {return entry->hashValue(); });
    int freed = sweepTable<quint64>(entriesById, alloc, [](Heap::String *entry) {return entry->identifier.id; });
    Q_UNUSED(f);
    Q_ASSERT(f == freed);
    size -= freed;
}

Identifier IdentifierTable::identifier(const QString &s)
{
    return insertString(s)->identifier;
}

Identifier IdentifierTable::identifier(const char *s, int len)
{
    uint subtype;
    uint hash = String::createHashValue(s, len, &subtype);
    if (hash == UINT_MAX)
        return identifier(QString::fromUtf8(s, len));

    QLatin1String latin(s, len);
    uint idx = hash % alloc;
    while (Heap::String *e = entriesByHash[idx]) {
        if (e->stringHash == hash && e->toQString() == latin)
            return e->identifier;
        ++idx;
        idx %= alloc;
    }

    Heap::String *str = engine->newString(QString::fromLatin1(s, len));
    str->stringHash = hash;
    str->subtype = subtype;
    addEntry(str);
    return str->identifier;
}

}

QT_END_NAMESPACE
