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
#include <config.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>

#include <qdebug.h>

namespace WTF {

void* fastMalloc(size_t size)
{
    return malloc(size);
}

void* fastRealloc(void* ptr, size_t size)
{
    return realloc(ptr, size);
}

void fastFree(void* ptr)
{
    free(ptr);
}

uint32_t cryptographicallyRandomNumber()
{
    return 0;
}

static FILE* s_dataFile = stderr;

void setDataFile(FILE* f)
{
    s_dataFile = f;
}

FILE* dataFile()
{
    return s_dataFile;
}

void dataLogV(const char* format, va_list args)
{
    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), format, args);
    qDebug("%s", buffer);
}

void dataLog(const char* format, ...)
{
    char buffer[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    qDebug("%s", buffer);
}

void dataLogString(const char* str)
{
    qDebug("%s", str);
}

}

extern "C" {

void WTFReportAssertionFailure(const char* /*file*/, int /*line*/, const char* /*function*/, const char* /*assertion*/)
{
}

void WTFReportBacktrace()
{
}

void WTFInvokeCrashHook()
{
}

}


#if ENABLE(ASSEMBLER) && CPU(X86) && !OS(MAC_OS_X)
#include <MacroAssemblerX86Common.h>

JSC::MacroAssemblerX86Common::SSE2CheckState JSC::MacroAssemblerX86Common::s_sse2CheckState = JSC::MacroAssemblerX86Common::NotCheckedSSE2;
#endif

