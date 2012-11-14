#ifndef QV4INSTR_MOTH_P_H
#define QV4INSTR_MOTH_P_H

#include <QtCore/qglobal.h>
#include "qmljs_objects.h"

#define FOR_EACH_MOTH_INSTR(F) \
    F(Ret, ret) \
    F(LoadUndefined, loadUndefined) \
    F(LoadNull, loadNull) \
    F(LoadFalse, loadFalse) \
    F(LoadTrue, loadTrue) \
    F(LoadNumber, loadNumber) \
    F(LoadString, loadString) \
    F(LoadClosure, loadClosure) \
    F(MoveTemp, moveTemp) \
    F(LoadName, loadName) \
    F(StoreName, storeName) \
    F(LoadElement, loadElement) \
    F(StoreElement, storeElement) \
    F(LoadProperty, loadProperty) \
    F(StoreProperty, storeProperty) \
    F(Push, push) \
    F(CallValue, callValue) \
    F(CallProperty, callProperty) \
    F(CallBuiltin, callBuiltin) \
    F(CallBuiltinDeleteMember, callBuiltinDeleteMember) \
    F(CallBuiltinDeleteSubscript, callBuiltinDeleteSubscript) \
    F(CallBuiltinDeleteName, callBuiltinDeleteName) \
    F(CallBuiltinDeleteValue, callBuiltinDeleteValue) \
    F(CreateValue, createValue) \
    F(CreateProperty, createProperty) \
    F(CreateActivationProperty, createActivationProperty) \
    F(Jump, jump) \
    F(CJump, jump) \
    F(Unop, unop) \
    F(Binop, binop) \
    F(LoadThis, loadThis) \
    F(InplaceElementOp, inplaceElementOp) \
    F(InplaceMemberOp, inplaceMemberOp) \
    F(InplaceNameOp, inplaceNameOp)

#if defined(Q_CC_GNU) && (!defined(Q_CC_INTEL) || __INTEL_COMPILER >= 1200)
#  define MOTH_THREADED_INTERPRETER
#endif

#define MOTH_INSTR_ALIGN_MASK (Q_ALIGNOF(QQmlJS::Moth::Instr) - 1)

#ifdef MOTH_THREADED_INTERPRETER
#  define MOTH_INSTR_HEADER void *code;
#else
#  define MOTH_INSTR_HEADER quint8 instructionType;
#endif

#define MOTH_INSTR_ENUM(I, FMT)  I,
#define MOTH_INSTR_SIZE(I, FMT) ((sizeof(QQmlJS::Moth::Instr::instr_##FMT) + MOTH_INSTR_ALIGN_MASK) & ~MOTH_INSTR_ALIGN_MASK)


namespace QQmlJS {
namespace Moth {

union Instr
{
    union ValueOrTemp {
        VM::Value value;
        int tempIndex;
    };

    enum Type {
        FOR_EACH_MOTH_INSTR(MOTH_INSTR_ENUM)
    };

    struct instr_common {
        MOTH_INSTR_HEADER
        int tempIndex;
    };
    struct instr_ret {
        MOTH_INSTR_HEADER
        int tempIndex;
    }; 
    struct instr_loadUndefined {
        MOTH_INSTR_HEADER
        int targetTempIndex;
    };
    struct instr_loadNull {
        MOTH_INSTR_HEADER
        int targetTempIndex;
    };
    struct instr_loadFalse {
        MOTH_INSTR_HEADER
        int targetTempIndex;
    };
    struct instr_loadTrue {
        MOTH_INSTR_HEADER
        int targetTempIndex;
    };
    struct instr_moveTemp {
        MOTH_INSTR_HEADER
        int fromTempIndex;
        int toTempIndex;
    };
    struct instr_loadNumber {
        MOTH_INSTR_HEADER
        double value;
        int targetTempIndex;
    };
    struct instr_loadString {
        MOTH_INSTR_HEADER
        VM::String *value;
        int targetTempIndex;
    };
    struct instr_loadClosure {
        MOTH_INSTR_HEADER
        IR::Function *value;
        int targetTempIndex;
    };
    struct instr_loadName {
        MOTH_INSTR_HEADER
        VM::String *name;
        int targetTempIndex;
    };
    struct instr_storeName {
        MOTH_INSTR_HEADER
        VM::String *name;
        int sourceTemp;
    };
    struct instr_loadProperty {
        MOTH_INSTR_HEADER
        int baseTemp;
        int targetTempIndex;
        VM::String *name;
    };
    struct instr_storeProperty {
        MOTH_INSTR_HEADER
        int sourceTemp;
        int baseTemp;
        VM::String *name;
    };
    struct instr_loadElement {
        MOTH_INSTR_HEADER
        int base;
        int index;
        int targetTempIndex;
    };
    struct instr_storeElement {
        MOTH_INSTR_HEADER
        int sourceTemp;
        int base;
        int index;
    };
    struct instr_push {
        MOTH_INSTR_HEADER
        quint32 value;
    };
    struct instr_callValue {
        MOTH_INSTR_HEADER
        quint32 argc;
        quint32 args;
        int destIndex;
        int targetTempIndex;
    };
    struct instr_callProperty {
        MOTH_INSTR_HEADER
        VM::String *name;
        int baseTemp;
        quint32 argc;
        quint32 args;
        int targetTempIndex;
    };
    struct instr_callBuiltin {
        MOTH_INSTR_HEADER
        enum {
            builtin_typeof,
            builtin_throw,
            builtin_create_exception_handler,
            builtin_delete_exception_handler,
            builtin_get_exception,
            builtin_foreach_iterator_object,
            builtin_foreach_next_property_name
        } builtin;
        quint32 argc;
        quint32 args;
        int targetTempIndex;
    };
    struct instr_callBuiltinDeleteMember {
        MOTH_INSTR_HEADER
        int base;
        VM::String *member;
        int targetTempIndex;
    };
    struct instr_callBuiltinDeleteSubscript {
        MOTH_INSTR_HEADER
        int base;
        int index;
        int targetTempIndex;
    };
    struct instr_callBuiltinDeleteName {
        MOTH_INSTR_HEADER
        VM::String *name;
        int targetTempIndex;
    };
    struct instr_callBuiltinDeleteValue {
        MOTH_INSTR_HEADER
        int tempIndex;
        int targetTempIndex;
    };
    struct instr_createValue {
        MOTH_INSTR_HEADER
        int func;
        quint32 argc;
        quint32 args;
        int targetTempIndex;
    };
    struct instr_createProperty {
        MOTH_INSTR_HEADER
        int base;
        VM::String *name;
        quint32 argc;
        quint32 args;
        int targetTempIndex;
    };
    struct instr_createActivationProperty {
        MOTH_INSTR_HEADER
        VM::String *name;
        quint32 argc;
        quint32 args;
        int targetTempIndex;
    };
    struct instr_jump {
        MOTH_INSTR_HEADER
        ptrdiff_t offset; 
        int tempIndex;
    };
    struct instr_unop {
        MOTH_INSTR_HEADER
        VM::Value (*alu)(const VM::Value value, VM::Context *ctx);
        int e;
        int targetTempIndex;
    };
    struct instr_binop {
        MOTH_INSTR_HEADER
        VM::Value (*alu)(const VM::Value , const VM::Value, VM::Context *);
        int targetTempIndex;
        ValueOrTemp lhs;
        ValueOrTemp rhs;
        unsigned lhsIsTemp:1;
        unsigned rhsIsTemp:1;
    };
    struct instr_loadThis {
        MOTH_INSTR_HEADER
        int targetTempIndex;
    };
    struct instr_inplaceElementOp {
        MOTH_INSTR_HEADER
        void (*alu)(VM::Value, VM::Value, VM::Value, VM::Context *);
        int targetBase;
        int targetIndex;
        int source;
    };
    struct instr_inplaceMemberOp {
        MOTH_INSTR_HEADER
        void (*alu)(VM::Value, VM::Value, VM::String *, VM::Context *);
        int targetBase;
        VM::String *targetMember;
        int source;
    };
    struct instr_inplaceNameOp {
        MOTH_INSTR_HEADER
        void (*alu)(VM::Value, VM::String *, VM::Context *);
        VM::String *targetName;
        int source;
    };

    instr_common common;
    instr_ret ret;
    instr_loadUndefined loadUndefined;
    instr_loadNull loadNull;
    instr_loadFalse loadFalse;
    instr_loadTrue loadTrue;
    instr_moveTemp moveTemp;
    instr_loadNumber loadNumber;
    instr_loadString loadString;
    instr_loadClosure loadClosure;
    instr_loadName loadName;
    instr_storeName storeName;
    instr_loadElement loadElement;
    instr_storeElement storeElement;
    instr_loadProperty loadProperty;
    instr_storeProperty storeProperty;
    instr_push push;
    instr_callValue callValue;
    instr_callProperty callProperty;
    instr_callBuiltin callBuiltin;
    instr_callBuiltinDeleteMember callBuiltinDeleteMember;
    instr_callBuiltinDeleteSubscript callBuiltinDeleteSubscript;
    instr_callBuiltinDeleteName callBuiltinDeleteName;
    instr_callBuiltinDeleteValue callBuiltinDeleteValue;
    instr_createValue createValue;
    instr_createProperty createProperty;
    instr_createActivationProperty createActivationProperty;
    instr_jump jump;
    instr_unop unop;
    instr_binop binop;
    instr_loadThis loadThis;
    instr_inplaceElementOp inplaceElementOp;
    instr_inplaceMemberOp inplaceMemberOp;
    instr_inplaceNameOp inplaceNameOp;

    static int size(Type type);
};

template<int N>
struct InstrMeta {
};

#define MOTH_INSTR_META_TEMPLATE(I, FMT) \
    template<> struct InstrMeta<(int)Instr::I> { \
        enum { Size = MOTH_INSTR_SIZE(I, FMT) }; \
        typedef Instr::instr_##FMT DataType; \
        static const DataType &data(const Instr &instr) { return instr.FMT; } \
        static void setData(Instr &instr, const DataType &v) { instr.FMT = v; } \
    }; 
FOR_EACH_MOTH_INSTR(MOTH_INSTR_META_TEMPLATE);
#undef MOTH_INSTR_META_TEMPLATE

template<int InstrType>
class InstrData : public InstrMeta<InstrType>::DataType
{
};

} // namespace Moth
} // namespace QQmlJS

#endif // QV4INSTR_MOTH_P_H
