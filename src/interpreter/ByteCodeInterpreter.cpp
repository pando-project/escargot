/*
 * Copyright (c) 2016-present Samsung Electronics Co., Ltd
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301
 *  USA
 */

#include "Escargot.h"
#include "ByteCode.h"
#include "ByteCodeInterpreter.h"
#include "runtime/Environment.h"
#include "runtime/EnvironmentRecord.h"
#include "runtime/FunctionObject.h"
#include "runtime/Context.h"
#include "runtime/SandBox.h"
#include "runtime/GlobalObject.h"
#include "runtime/StringObject.h"
#include "runtime/NumberObject.h"
#include "runtime/EnumerateObject.h"
#include "runtime/ErrorObject.h"
#include "runtime/ArrayObject.h"
#include "runtime/VMInstance.h"
#include "runtime/IteratorObject.h"
#include "runtime/GeneratorObject.h"
#include "runtime/PromiseObject.h"
#include "runtime/ScriptFunctionObject.h"
#include "runtime/ScriptArrowFunctionObject.h"
#include "runtime/ScriptClassConstructorFunctionObject.h"
#include "runtime/ScriptClassMethodFunctionObject.h"
#include "runtime/ScriptGeneratorFunctionObject.h"
#include "runtime/ScriptAsyncFunctionObject.h"
#include "runtime/ScriptAsyncGeneratorFunctionObject.h"
#include "parser/ScriptParser.h"
#include "util/Util.h"
#include "../third_party/checked_arithmetic/CheckedArithmetic.h"
#include "runtime/ProxyObject.h"

namespace Escargot {

#define ADD_PROGRAM_COUNTER(CodeType) programCounter += sizeof(CodeType);

ALWAYS_INLINE size_t jumpTo(char* codeBuffer, const size_t jumpPosition)
{
    return (size_t)&codeBuffer[jumpPosition];
}

ALWAYS_INLINE size_t resolveProgramCounter(char* codeBuffer, const size_t programCounter)
{
    return programCounter - (size_t)codeBuffer;
}

class ExecutionStateProgramCounterBinder {
public:
    ExecutionStateProgramCounterBinder(ExecutionState& state, size_t* newAddress)
        : m_state(state)
    {
        m_oldAddress = state.m_programCounter;
        state.m_programCounter = newAddress;
    }

    ~ExecutionStateProgramCounterBinder()
    {
        m_state.m_programCounter = m_oldAddress;
    }

private:
    ExecutionState& m_state;
    size_t* m_oldAddress;
};

Value ByteCodeInterpreter::interpret(ExecutionState* state, ByteCodeBlock* byteCodeBlock, size_t programCounter, Value* registerFile)
{
#if defined(COMPILER_GCC) || defined(COMPILER_CLANG)
    if (UNLIKELY(byteCodeBlock == nullptr)) {
        goto FillOpcodeTableLbl;
    }
#endif

    ASSERT(byteCodeBlock != nullptr);
    ASSERT(registerFile != nullptr);

    {
        ExecutionStateProgramCounterBinder binder(*state, &programCounter);
        char* codeBuffer = byteCodeBlock->m_code.data();
        programCounter = (size_t)(codeBuffer + programCounter);

#if defined(COMPILER_GCC) || defined(COMPILER_CLANG)
#define DEFINE_OPCODE(codeName) codeName##OpcodeLbl
#define DEFINE_DEFAULT
#define NEXT_INSTRUCTION() \
    goto*(((ByteCode*)programCounter)->m_opcodeInAddress);
#define JUMP_INSTRUCTION(opcode) \
    goto opcode##OpcodeLbl;

        /* Execute first instruction. */
        NEXT_INSTRUCTION();
#else

#define DEFINE_OPCODE(codeName) case codeName##Opcode
#define DEFINE_DEFAULT                \
    default:                          \
        RELEASE_ASSERT_NOT_REACHED(); \
        }
#define NEXT_INSTRUCTION() \
    goto NextInstruction;
#define JUMP_INSTRUCTION(opcode)    \
    currentOpcode = opcode##Opcode; \
    goto NextInstructionWithoutFetchOpcode;

    NextInstruction:
        Opcode currentOpcode = ((ByteCode*)programCounter)->m_opcode;

    NextInstructionWithoutFetchOpcode:
        switch (currentOpcode) {
#endif

        DEFINE_OPCODE(LoadLiteral)
            :
        {
            LoadLiteral* code = (LoadLiteral*)programCounter;
            registerFile[code->m_registerIndex] = code->m_value;
            ADD_PROGRAM_COUNTER(LoadLiteral);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(Move)
            :
        {
            Move* code = (Move*)programCounter;
            ASSERT(code->m_registerIndex1 < (byteCodeBlock->m_requiredRegisterFileSizeInValueSize + byteCodeBlock->m_codeBlock->asInterpretedCodeBlock()->totalStackAllocatedVariableSize() + 1));
            ASSERT(!registerFile[code->m_registerIndex0].isEmpty());
            registerFile[code->m_registerIndex1] = registerFile[code->m_registerIndex0];
            ADD_PROGRAM_COUNTER(Move);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(GetGlobalVariable)
            :
        {
            GetGlobalVariable* code = (GetGlobalVariable*)programCounter;
            ASSERT(byteCodeBlock->m_codeBlock->context() == state->context());
            Context* ctx = state->context();
            GlobalObject* globalObject = state->context()->globalObject();
            auto slot = code->m_slot;
            auto idx = slot->m_lexicalIndexCache;
            bool isCacheWork = false;

            if (LIKELY(idx != std::numeric_limits<size_t>::max())) {
                if (LIKELY(ctx->globalDeclarativeStorage()->size() == slot->m_lexicalIndexCache && globalObject->structure() == slot->m_cachedStructure)) {
                    ASSERT(globalObject->m_values.data() <= slot->m_cachedAddress);
                    ASSERT(slot->m_cachedAddress < (globalObject->m_values.data() + globalObject->structure()->propertyCount()));
                    registerFile[code->m_registerIndex] = *((SmallValue*)slot->m_cachedAddress);
                    isCacheWork = true;
                } else if (slot->m_cachedStructure == nullptr) {
                    const SmallValue& val = ctx->globalDeclarativeStorage()->at(idx);
                    isCacheWork = true;
                    if (UNLIKELY(val.isEmpty())) {
                        ErrorObject::throwBuiltinError(*state, ErrorObject::ReferenceError, ctx->globalDeclarativeRecord()->at(idx).m_name.string(), false, String::emptyString, errorMessage_IsNotInitialized);
                    }
                    registerFile[code->m_registerIndex] = val;
                }
            }
            if (UNLIKELY(!isCacheWork)) {
                registerFile[code->m_registerIndex] = getGlobalVariableSlowCase(*state, globalObject, slot, byteCodeBlock);
            }
            ADD_PROGRAM_COUNTER(GetGlobalVariable);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(SetGlobalVariable)
            :
        {
            SetGlobalVariable* code = (SetGlobalVariable*)programCounter;
            ASSERT(byteCodeBlock->m_codeBlock->context() == state->context());
            Context* ctx = state->context();
            GlobalObject* globalObject = state->context()->globalObject();
            auto slot = code->m_slot;
            auto idx = slot->m_lexicalIndexCache;

            bool isCacheWork = false;
            if (LIKELY(idx != std::numeric_limits<size_t>::max())) {
                if (LIKELY(ctx->globalDeclarativeStorage()->size() == slot->m_lexicalIndexCache && globalObject->structure() == slot->m_cachedStructure)) {
                    ASSERT(globalObject->m_values.data() <= slot->m_cachedAddress);
                    ASSERT(slot->m_cachedAddress < (globalObject->m_values.data() + globalObject->structure()->propertyCount()));
                    *((SmallValue*)slot->m_cachedAddress) = registerFile[code->m_registerIndex];
                    isCacheWork = true;
                } else if (slot->m_cachedStructure == nullptr) {
                    isCacheWork = true;
                    if (UNLIKELY(ctx->globalDeclarativeStorage()->at(idx).isEmpty())) {
                        ErrorObject::throwBuiltinError(*state, ErrorObject::ReferenceError, ctx->globalDeclarativeRecord()->at(idx).m_name.string(), false, String::emptyString, errorMessage_IsNotInitialized);
                    }
                    ctx->globalDeclarativeStorage()->at(idx) = registerFile[code->m_registerIndex];
                }
            }

            if (UNLIKELY(!isCacheWork)) {
                setGlobalVariableSlowCase(*state, globalObject, slot, registerFile[code->m_registerIndex], byteCodeBlock);
            }

            ADD_PROGRAM_COUNTER(SetGlobalVariable);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(BinaryPlus)
            :
        {
            BinaryPlus* code = (BinaryPlus*)programCounter;
            const Value& v0 = registerFile[code->m_srcIndex0];
            const Value& v1 = registerFile[code->m_srcIndex1];
            Value ret(Value::ForceUninitialized);
            if (v0.isInt32() && v1.isInt32()) {
                int32_t a = v0.asInt32();
                int32_t b = v1.asInt32();
                int32_t c;
                bool result = ArithmeticOperations<int32_t, int32_t, int32_t>::add(a, b, c);
                if (LIKELY(result)) {
                    ret = Value(c);
                } else {
                    ret = Value(Value::EncodeAsDouble, (double)a + (double)b);
                }
            } else if (v0.isNumber() && v1.isNumber()) {
                ret = Value(v0.asNumber() + v1.asNumber());
            } else {
                ret = plusSlowCase(*state, v0, v1);
            }
            registerFile[code->m_dstIndex] = ret;
            ADD_PROGRAM_COUNTER(BinaryPlus);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(BinaryMinus)
            :
        {
            BinaryMinus* code = (BinaryMinus*)programCounter;
            const Value& left = registerFile[code->m_srcIndex0];
            const Value& right = registerFile[code->m_srcIndex1];
            Value ret(Value::ForceUninitialized);
            if (left.isInt32() && right.isInt32()) {
                int32_t a = left.asInt32();
                int32_t b = right.asInt32();
                int32_t c;
                bool result = ArithmeticOperations<int32_t, int32_t, int32_t>::sub(a, b, c);
                if (LIKELY(result)) {
                    ret = Value(c);
                } else {
                    ret = Value(Value::EncodeAsDouble, (double)a - (double)b);
                }
            } else {
                ret = Value(left.toNumber(*state) - right.toNumber(*state));
            }
            registerFile[code->m_dstIndex] = ret;
            ADD_PROGRAM_COUNTER(BinaryMinus);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(BinaryMultiply)
            :
        {
            BinaryMultiply* code = (BinaryMultiply*)programCounter;
            const Value& left = registerFile[code->m_srcIndex0];
            const Value& right = registerFile[code->m_srcIndex1];
            Value ret(Value::ForceUninitialized);
            if (left.isInt32() && right.isInt32()) {
                int32_t a = left.asInt32();
                int32_t b = right.asInt32();
                if (UNLIKELY((!a || !b) && (a >> 31 || b >> 31))) { // -1 * 0 should be treated as -0, not +0
                    ret = Value(left.asNumber() * right.asNumber());
                } else {
                    int32_t c = right.asInt32();
                    bool result = ArithmeticOperations<int32_t, int32_t, int32_t>::multiply(a, b, c);
                    if (LIKELY(result)) {
                        ret = Value(c);
                    } else {
                        ret = Value(Value::EncodeAsDouble, a * (double)b);
                    }
                }
            } else {
                auto first = left.toNumber(*state);
                auto second = right.toNumber(*state);
                ret = Value(Value::EncodeAsDouble, first * second);
            }
            registerFile[code->m_dstIndex] = ret;
            ADD_PROGRAM_COUNTER(BinaryMultiply);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(BinaryDivision)
            :
        {
            BinaryDivision* code = (BinaryDivision*)programCounter;
            const Value& left = registerFile[code->m_srcIndex0];
            const Value& right = registerFile[code->m_srcIndex1];
            registerFile[code->m_dstIndex] = Value(left.toNumber(*state) / right.toNumber(*state));
            ADD_PROGRAM_COUNTER(BinaryDivision);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(BinaryEqual)
            :
        {
            BinaryEqual* code = (BinaryEqual*)programCounter;
            const Value& left = registerFile[code->m_srcIndex0];
            const Value& right = registerFile[code->m_srcIndex1];
            registerFile[code->m_dstIndex] = Value(left.abstractEqualsTo(*state, right));
            ADD_PROGRAM_COUNTER(BinaryEqual);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(BinaryNotEqual)
            :
        {
            BinaryNotEqual* code = (BinaryNotEqual*)programCounter;
            const Value& left = registerFile[code->m_srcIndex0];
            const Value& right = registerFile[code->m_srcIndex1];
            registerFile[code->m_dstIndex] = Value(!left.abstractEqualsTo(*state, right));
            ADD_PROGRAM_COUNTER(BinaryNotEqual);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(BinaryStrictEqual)
            :
        {
            BinaryStrictEqual* code = (BinaryStrictEqual*)programCounter;
            const Value& left = registerFile[code->m_srcIndex0];
            const Value& right = registerFile[code->m_srcIndex1];
            registerFile[code->m_dstIndex] = Value(left.equalsTo(*state, right));
            ADD_PROGRAM_COUNTER(BinaryStrictEqual);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(BinaryNotStrictEqual)
            :
        {
            BinaryNotStrictEqual* code = (BinaryNotStrictEqual*)programCounter;
            const Value& left = registerFile[code->m_srcIndex0];
            const Value& right = registerFile[code->m_srcIndex1];
            registerFile[code->m_dstIndex] = Value(!left.equalsTo(*state, right));
            ADD_PROGRAM_COUNTER(BinaryNotStrictEqual);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(BinaryLessThan)
            :
        {
            BinaryLessThan* code = (BinaryLessThan*)programCounter;
            const Value& left = registerFile[code->m_srcIndex0];
            const Value& right = registerFile[code->m_srcIndex1];
            registerFile[code->m_dstIndex] = Value(abstractRelationalComparison(*state, left, right, true));
            ADD_PROGRAM_COUNTER(BinaryLessThan);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(BinaryLessThanOrEqual)
            :
        {
            BinaryLessThanOrEqual* code = (BinaryLessThanOrEqual*)programCounter;
            const Value& left = registerFile[code->m_srcIndex0];
            const Value& right = registerFile[code->m_srcIndex1];
            registerFile[code->m_dstIndex] = Value(abstractRelationalComparisonOrEqual(*state, left, right, true));
            ADD_PROGRAM_COUNTER(BinaryLessThanOrEqual);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(BinaryGreaterThan)
            :
        {
            BinaryGreaterThan* code = (BinaryGreaterThan*)programCounter;
            const Value& left = registerFile[code->m_srcIndex0];
            const Value& right = registerFile[code->m_srcIndex1];
            registerFile[code->m_dstIndex] = Value(abstractRelationalComparison(*state, right, left, false));
            ADD_PROGRAM_COUNTER(BinaryGreaterThan);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(BinaryGreaterThanOrEqual)
            :
        {
            BinaryGreaterThanOrEqual* code = (BinaryGreaterThanOrEqual*)programCounter;
            const Value& left = registerFile[code->m_srcIndex0];
            const Value& right = registerFile[code->m_srcIndex1];
            registerFile[code->m_dstIndex] = Value(abstractRelationalComparisonOrEqual(*state, right, left, false));
            ADD_PROGRAM_COUNTER(BinaryGreaterThanOrEqual);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(ToNumberIncrement)
            :
        {
            ToNumberIncrement* code = (ToNumberIncrement*)programCounter;
            Value toNumberValue = Value(registerFile[code->m_srcIndex].toNumber(*state));
            registerFile[code->m_dstIndex] = toNumberValue;
            registerFile[code->m_storeIndex] = incrementOperation(*state, toNumberValue);
            ADD_PROGRAM_COUNTER(ToNumberIncrement);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(Increment)
            :
        {
            Increment* code = (Increment*)programCounter;
            registerFile[code->m_dstIndex] = incrementOperation(*state, registerFile[code->m_srcIndex]);
            ADD_PROGRAM_COUNTER(Increment);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(ToNumberDecrement)
            :
        {
            ToNumberDecrement* code = (ToNumberDecrement*)programCounter;
            Value toNumberValue = Value(registerFile[code->m_srcIndex].toNumber(*state));
            registerFile[code->m_dstIndex] = toNumberValue;
            registerFile[code->m_storeIndex] = decrementOperation(*state, toNumberValue);
            ADD_PROGRAM_COUNTER(ToNumberDecrement);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(Decrement)
            :
        {
            Decrement* code = (Decrement*)programCounter;
            registerFile[code->m_dstIndex] = decrementOperation(*state, registerFile[code->m_srcIndex]);
            ADD_PROGRAM_COUNTER(Decrement);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(UnaryMinus)
            :
        {
            UnaryMinus* code = (UnaryMinus*)programCounter;
            const Value& val = registerFile[code->m_srcIndex];
            registerFile[code->m_dstIndex] = Value(-val.toNumber(*state));
            ADD_PROGRAM_COUNTER(UnaryMinus);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(UnaryNot)
            :
        {
            UnaryNot* code = (UnaryNot*)programCounter;
            const Value& val = registerFile[code->m_srcIndex];
            registerFile[code->m_dstIndex] = Value(!val.toBoolean(*state));
            ADD_PROGRAM_COUNTER(UnaryNot);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(GetObject)
            :
        {
            GetObject* code = (GetObject*)programCounter;
            const Value& willBeObject = registerFile[code->m_objectRegisterIndex];
            const Value& property = registerFile[code->m_propertyRegisterIndex];
            PointerValue* v;
            if (LIKELY(willBeObject.isObject() && (v = willBeObject.asPointerValue())->hasTag(g_arrayObjectTag))) {
                ArrayObject* arr = (ArrayObject*)v;
                if (LIKELY(arr->isFastModeArray())) {
                    uint32_t idx = property.tryToUseAsArrayIndex(*state);
                    if (LIKELY(idx != Value::InvalidArrayIndexValue) && LIKELY(idx < arr->getArrayLength(*state))) {
                        const Value& v = arr->m_fastModeData[idx];
                        if (LIKELY(!v.isEmpty())) {
                            registerFile[code->m_storeRegisterIndex] = v;
                            ADD_PROGRAM_COUNTER(GetObject);
                            NEXT_INSTRUCTION();
                        }
                    }
                }
            }
            JUMP_INSTRUCTION(GetObjectOpcodeSlowCase);
        }

        DEFINE_OPCODE(SetObjectOperation)
            :
        {
            SetObjectOperation* code = (SetObjectOperation*)programCounter;
            const Value& willBeObject = registerFile[code->m_objectRegisterIndex];
            const Value& property = registerFile[code->m_propertyRegisterIndex];
            if (LIKELY(willBeObject.isObject() && (willBeObject.asPointerValue())->hasTag(g_arrayObjectTag))) {
                ArrayObject* arr = willBeObject.asObject()->asArrayObject();
                uint32_t idx = property.tryToUseAsArrayIndex(*state);
                if (LIKELY(arr->isFastModeArray())) {
                    if (LIKELY(idx != Value::InvalidArrayIndexValue)) {
                        uint32_t len = arr->getArrayLength(*state);
                        if (UNLIKELY(len <= idx)) {
                            if (UNLIKELY(!arr->isExtensible(*state))) {
                                JUMP_INSTRUCTION(SetObjectOpcodeSlowCase);
                            }
                            if (UNLIKELY(!arr->setArrayLength(*state, idx + 1)) || UNLIKELY(!arr->isFastModeArray())) {
                                JUMP_INSTRUCTION(SetObjectOpcodeSlowCase);
                            }
                        }
                        arr->m_fastModeData[idx] = registerFile[code->m_loadRegisterIndex];
                        ADD_PROGRAM_COUNTER(SetObjectOperation);
                        NEXT_INSTRUCTION();
                    }
                }
            }
            JUMP_INSTRUCTION(SetObjectOpcodeSlowCase);
        }

        DEFINE_OPCODE(GetObjectPreComputedCase)
            :
        {
            GetObjectPreComputedCase* code = (GetObjectPreComputedCase*)programCounter;
            const Value& willBeObject = registerFile[code->m_objectRegisterIndex];
            Object* obj;
            if (LIKELY(willBeObject.isObject())) {
                obj = willBeObject.asObject();
            } else {
                obj = fastToObject(*state, willBeObject);
            }
            registerFile[code->m_storeRegisterIndex] = getObjectPrecomputedCaseOperation(*state, obj, willBeObject, code, byteCodeBlock);
            ADD_PROGRAM_COUNTER(GetObjectPreComputedCase);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(SetObjectPreComputedCase)
            :
        {
            SetObjectPreComputedCase* code = (SetObjectPreComputedCase*)programCounter;
            setObjectPreComputedCaseOperation(*state, registerFile[code->m_objectRegisterIndex], registerFile[code->m_loadRegisterIndex], code, byteCodeBlock);
            ADD_PROGRAM_COUNTER(SetObjectPreComputedCase);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(Jump)
            :
        {
            Jump* code = (Jump*)programCounter;
            ASSERT(code->m_jumpPosition != SIZE_MAX);
            programCounter = code->m_jumpPosition;
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(JumpIfRelation)
            :
        {
            JumpIfRelation* code = (JumpIfRelation*)programCounter;
            ASSERT(code->m_jumpPosition != SIZE_MAX);
            const Value& left = registerFile[code->m_registerIndex0];
            const Value& right = registerFile[code->m_registerIndex1];
            bool relation;
            if (code->m_isEqual) {
                relation = abstractRelationalComparisonOrEqual(*state, left, right, code->m_isLeftFirst);
            } else {
                relation = abstractRelationalComparison(*state, left, right, code->m_isLeftFirst);
            }

            if (relation) {
                ADD_PROGRAM_COUNTER(JumpIfRelation);
            } else {
                programCounter = code->m_jumpPosition;
            }
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(JumpIfEqual)
            :
        {
            JumpIfEqual* code = (JumpIfEqual*)programCounter;
            ASSERT(code->m_jumpPosition != SIZE_MAX);
            const Value& left = registerFile[code->m_registerIndex0];
            const Value& right = registerFile[code->m_registerIndex1];
            bool equality;
            if (code->m_isStrict) {
                equality = left.equalsTo(*state, right);
            } else {
                equality = left.abstractEqualsTo(*state, right);
            }

            if (equality ^ code->m_shouldNegate) {
                ADD_PROGRAM_COUNTER(JumpIfEqual);
            } else {
                programCounter = code->m_jumpPosition;
            }
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(JumpIfTrue)
            :
        {
            JumpIfTrue* code = (JumpIfTrue*)programCounter;
            ASSERT(code->m_jumpPosition != SIZE_MAX);
            if (registerFile[code->m_registerIndex].toBoolean(*state)) {
                programCounter = code->m_jumpPosition;
            } else {
                ADD_PROGRAM_COUNTER(JumpIfTrue);
            }
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(JumpIfFalse)
            :
        {
            JumpIfFalse* code = (JumpIfFalse*)programCounter;
            ASSERT(code->m_jumpPosition != SIZE_MAX);
            if (!registerFile[code->m_registerIndex].toBoolean(*state)) {
                programCounter = code->m_jumpPosition;
            } else {
                ADD_PROGRAM_COUNTER(JumpIfFalse);
            }
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(CallFunction)
            :
        {
            CallFunction* code = (CallFunction*)programCounter;
            const Value& callee = registerFile[code->m_calleeIndex];

            // if PointerValue is not callable, PointerValue::call function throws builtin error
            // https://www.ecma-international.org/ecma-262/6.0/#sec-call
            // If IsCallable(F) is false, throw a TypeError exception.
            if (UNLIKELY(!callee.isPointerValue())) {
                ErrorObject::throwBuiltinError(*state, ErrorObject::TypeError, errorMessage_NOT_Callable);
            }
            // Return F.[[Call]](V, argumentsList).
            registerFile[code->m_resultIndex] = callee.asPointerValue()->call(*state, Value(), code->m_argumentCount, &registerFile[code->m_argumentsStartIndex]);

            ADD_PROGRAM_COUNTER(CallFunction);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(CallFunctionWithReceiver)
            :
        {
            CallFunctionWithReceiver* code = (CallFunctionWithReceiver*)programCounter;
            const Value& callee = registerFile[code->m_calleeIndex];
            const Value& receiver = registerFile[code->m_receiverIndex];

            // if PointerValue is not callable, PointerValue::call function throws builtin error
            // https://www.ecma-international.org/ecma-262/6.0/#sec-call
            // If IsCallable(F) is false, throw a TypeError exception.
            if (UNLIKELY(!callee.isPointerValue())) {
                ErrorObject::throwBuiltinError(*state, ErrorObject::TypeError, errorMessage_NOT_Callable);
            }
            // Return F.[[Call]](V, argumentsList).
            registerFile[code->m_resultIndex] = callee.asPointerValue()->call(*state, receiver, code->m_argumentCount, &registerFile[code->m_argumentsStartIndex]);

            ADD_PROGRAM_COUNTER(CallFunctionWithReceiver);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(LoadByHeapIndex)
            :
        {
            LoadByHeapIndex* code = (LoadByHeapIndex*)programCounter;
            LexicalEnvironment* upperEnv = state->lexicalEnvironment();
            for (size_t i = 0; i < code->m_upperIndex; i++) {
                upperEnv = upperEnv->outerEnvironment();
            }
            registerFile[code->m_registerIndex] = upperEnv->record()->asDeclarativeEnvironmentRecord()->getHeapValueByIndex(*state, code->m_index);
            ADD_PROGRAM_COUNTER(LoadByHeapIndex);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(StoreByHeapIndex)
            :
        {
            StoreByHeapIndex* code = (StoreByHeapIndex*)programCounter;
            LexicalEnvironment* upperEnv = state->lexicalEnvironment();
            for (size_t i = 0; i < code->m_upperIndex; i++) {
                upperEnv = upperEnv->outerEnvironment();
            }
            upperEnv->record()->setMutableBindingByIndex(*state, code->m_index, registerFile[code->m_registerIndex]);
            ADD_PROGRAM_COUNTER(StoreByHeapIndex);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(BinaryMod)
            :
        {
            BinaryMod* code = (BinaryMod*)programCounter;
            const Value& left = registerFile[code->m_srcIndex0];
            const Value& right = registerFile[code->m_srcIndex1];
            registerFile[code->m_dstIndex] = modOperation(*state, left, right);
            ADD_PROGRAM_COUNTER(BinaryMod);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(BinaryBitwiseAnd)
            :
        {
            BinaryBitwiseAnd* code = (BinaryBitwiseAnd*)programCounter;
            const Value& left = registerFile[code->m_srcIndex0];
            const Value& right = registerFile[code->m_srcIndex1];
            registerFile[code->m_dstIndex] = Value(left.toInt32(*state) & right.toInt32(*state));
            ADD_PROGRAM_COUNTER(BinaryBitwiseAnd);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(BinaryBitwiseOr)
            :
        {
            BinaryBitwiseOr* code = (BinaryBitwiseOr*)programCounter;
            const Value& left = registerFile[code->m_srcIndex0];
            const Value& right = registerFile[code->m_srcIndex1];
            registerFile[code->m_dstIndex] = Value(left.toInt32(*state) | right.toInt32(*state));
            ADD_PROGRAM_COUNTER(BinaryBitwiseOr);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(BinaryBitwiseXor)
            :
        {
            BinaryBitwiseXor* code = (BinaryBitwiseXor*)programCounter;
            const Value& left = registerFile[code->m_srcIndex0];
            const Value& right = registerFile[code->m_srcIndex1];
            registerFile[code->m_dstIndex] = Value(left.toInt32(*state) ^ right.toInt32(*state));
            ADD_PROGRAM_COUNTER(BinaryBitwiseXor);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(BinaryLeftShift)
            :
        {
            BinaryLeftShift* code = (BinaryLeftShift*)programCounter;
            const Value& left = registerFile[code->m_srcIndex0];
            const Value& right = registerFile[code->m_srcIndex1];
            int32_t lnum = left.toInt32(*state);
            int32_t rnum = right.toInt32(*state);
            lnum <<= ((unsigned int)rnum) & 0x1F;
            registerFile[code->m_dstIndex] = Value(lnum);
            ADD_PROGRAM_COUNTER(BinaryLeftShift);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(BinarySignedRightShift)
            :
        {
            BinarySignedRightShift* code = (BinarySignedRightShift*)programCounter;
            const Value& left = registerFile[code->m_srcIndex0];
            const Value& right = registerFile[code->m_srcIndex1];
            int32_t lnum = left.toInt32(*state);
            int32_t rnum = right.toInt32(*state);
            lnum >>= ((unsigned int)rnum) & 0x1F;
            registerFile[code->m_dstIndex] = Value(lnum);
            ADD_PROGRAM_COUNTER(BinarySignedRightShift);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(BinaryUnsignedRightShift)
            :
        {
            BinaryUnsignedRightShift* code = (BinaryUnsignedRightShift*)programCounter;
            const Value& left = registerFile[code->m_srcIndex0];
            const Value& right = registerFile[code->m_srcIndex1];
            uint32_t lnum = left.toUint32(*state);
            uint32_t rnum = right.toUint32(*state);
            lnum = (lnum) >> ((rnum)&0x1F);
            registerFile[code->m_dstIndex] = Value(lnum);
            ADD_PROGRAM_COUNTER(BinaryUnsignedRightShift);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(BinaryExponentiation)
            :
        {
            BinaryExponentiation* code = (BinaryExponentiation*)programCounter;
            const Value& left = registerFile[code->m_srcIndex0];
            const Value& right = registerFile[code->m_srcIndex1];
            registerFile[code->m_dstIndex] = exponentialOperation(*state, left, right);
            ADD_PROGRAM_COUNTER(BinaryExponentiation);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(UnaryBitwiseNot)
            :
        {
            UnaryBitwiseNot* code = (UnaryBitwiseNot*)programCounter;
            const Value& val = registerFile[code->m_srcIndex];
            registerFile[code->m_dstIndex] = Value(~val.toInt32(*state));
            ADD_PROGRAM_COUNTER(UnaryBitwiseNot);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(GetParameter)
            :
        {
            GetParameter* code = (GetParameter*)programCounter;
            if (code->m_paramIndex < state->argc()) {
                registerFile[code->m_registerIndex] = state->argv()[code->m_paramIndex];
            } else {
                registerFile[code->m_registerIndex] = Value();
            }
            ADD_PROGRAM_COUNTER(GetParameter);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(End)
            :
        {
            End* code = (End*)programCounter;
            return registerFile[code->m_registerIndex];
        }

        DEFINE_OPCODE(ToNumber)
            :
        {
            ToNumber* code = (ToNumber*)programCounter;
            const Value& val = registerFile[code->m_srcIndex];
            registerFile[code->m_dstIndex] = Value(val.toNumber(*state));
            ADD_PROGRAM_COUNTER(ToNumber);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(InitializeGlobalVariable)
            :
        {
            InitializeGlobalVariable* code = (InitializeGlobalVariable*)programCounter;
            ASSERT(byteCodeBlock->m_codeBlock->context() == state->context());
            initializeGlobalVariable(*state, code, registerFile[code->m_registerIndex]);
            ADD_PROGRAM_COUNTER(InitializeGlobalVariable);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(InitializeByHeapIndex)
            :
        {
            InitializeByHeapIndex* code = (InitializeByHeapIndex*)programCounter;
            state->lexicalEnvironment()->record()->initializeBindingByIndex(*state, code->m_index, registerFile[code->m_registerIndex]);
            ADD_PROGRAM_COUNTER(InitializeByHeapIndex);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(ObjectDefineOwnPropertyOperation)
            :
        {
            ObjectDefineOwnPropertyOperation* code = (ObjectDefineOwnPropertyOperation*)programCounter;
            objectDefineOwnPropertyOperation(*state, code, registerFile);
            ADD_PROGRAM_COUNTER(ObjectDefineOwnPropertyOperation);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(ObjectDefineOwnPropertyWithNameOperation)
            :
        {
            ObjectDefineOwnPropertyWithNameOperation* code = (ObjectDefineOwnPropertyWithNameOperation*)programCounter;
            objectDefineOwnPropertyWithNameOperation(*state, code, registerFile);
            ADD_PROGRAM_COUNTER(ObjectDefineOwnPropertyWithNameOperation);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(ArrayDefineOwnPropertyOperation)
            :
        {
            ArrayDefineOwnPropertyOperation* code = (ArrayDefineOwnPropertyOperation*)programCounter;
            arrayDefineOwnPropertyOperation(*state, code, registerFile);
            ADD_PROGRAM_COUNTER(ArrayDefineOwnPropertyOperation);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(ArrayDefineOwnPropertyBySpreadElementOperation)
            :
        {
            ArrayDefineOwnPropertyBySpreadElementOperation* code = (ArrayDefineOwnPropertyBySpreadElementOperation*)programCounter;
            arrayDefineOwnPropertyBySpreadElementOperation(*state, code, registerFile);
            ADD_PROGRAM_COUNTER(ArrayDefineOwnPropertyBySpreadElementOperation);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(CreateSpreadArrayObject)
            :
        {
            CreateSpreadArrayObject* code = (CreateSpreadArrayObject*)programCounter;
            createSpreadArrayObject(*state, code, registerFile);
            ADD_PROGRAM_COUNTER(CreateSpreadArrayObject);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(NewOperation)
            :
        {
            NewOperation* code = (NewOperation*)programCounter;
            registerFile[code->m_resultIndex] = Object::construct(*state, registerFile[code->m_calleeIndex], code->m_argumentCount, &registerFile[code->m_argumentsStartIndex]);
            ADD_PROGRAM_COUNTER(NewOperation);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(UnaryTypeof)
            :
        {
            UnaryTypeof* code = (UnaryTypeof*)programCounter;
            unaryTypeof(*state, code, registerFile);
            ADD_PROGRAM_COUNTER(UnaryTypeof);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(GetObjectOpcodeSlowCase)
            :
        {
            GetObject* code = (GetObject*)programCounter;
            getObjectOpcodeSlowCase(*state, code, registerFile);
            ADD_PROGRAM_COUNTER(GetObject);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(SetObjectOpcodeSlowCase)
            :
        {
            SetObjectOperation* code = (SetObjectOperation*)programCounter;
            setObjectOpcodeSlowCase(*state, code, registerFile);
            ADD_PROGRAM_COUNTER(SetObjectOperation);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(LoadByName)
            :
        {
            LoadByName* code = (LoadByName*)programCounter;
            registerFile[code->m_registerIndex] = loadByName(*state, state->lexicalEnvironment(), code->m_name);
            ADD_PROGRAM_COUNTER(LoadByName);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(StoreByName)
            :
        {
            StoreByName* code = (StoreByName*)programCounter;
            storeByName(*state, state->lexicalEnvironment(), code->m_name, registerFile[code->m_registerIndex]);
            ADD_PROGRAM_COUNTER(StoreByName);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(InitializeByName)
            :
        {
            InitializeByName* code = (InitializeByName*)programCounter;
            initializeByName(*state, state->lexicalEnvironment(), code->m_name, code->m_isLexicallyDeclaredName, registerFile[code->m_registerIndex]);
            ADD_PROGRAM_COUNTER(InitializeByName);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(CreateObject)
            :
        {
            CreateObject* code = (CreateObject*)programCounter;
            registerFile[code->m_registerIndex] = new Object(*state);
            ADD_PROGRAM_COUNTER(CreateObject);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(CreateArray)
            :
        {
            CreateArray* code = (CreateArray*)programCounter;
            registerFile[code->m_registerIndex] = new ArrayObject(*state, (uint64_t)code->m_length);
            ADD_PROGRAM_COUNTER(CreateArray);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(CreateFunction)
            :
        {
            CreateFunction* code = (CreateFunction*)programCounter;
            createFunctionOperation(*state, code, byteCodeBlock, registerFile);
            ADD_PROGRAM_COUNTER(CreateFunction);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(CreateClass)
            :
        {
            CreateClass* code = (CreateClass*)programCounter;
            classOperation(*state, code, registerFile);
            ADD_PROGRAM_COUNTER(CreateClass);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(SuperReference)
            :
        {
            SuperReference* code = (SuperReference*)programCounter;
            superOperation(*state, code, registerFile);
            ADD_PROGRAM_COUNTER(SuperReference);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(SuperSetObjectOperation)
            :
        {
            SuperSetObjectOperation* code = (SuperSetObjectOperation*)programCounter;
            superSetObjectOperation(*state, code, registerFile, byteCodeBlock);
            ADD_PROGRAM_COUNTER(SuperSetObjectOperation);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(SuperGetObjectOperation)
            :
        {
            SuperGetObjectOperation* code = (SuperGetObjectOperation*)programCounter;
            registerFile[code->m_storeRegisterIndex] = superGetObjectOperation(*state, code, registerFile, byteCodeBlock);
            ADD_PROGRAM_COUNTER(SuperGetObjectOperation);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(CreateRestElement)
            :
        {
            CreateRestElement* code = (CreateRestElement*)programCounter;
            registerFile[code->m_registerIndex] = createRestElementOperation(*state, byteCodeBlock);
            ADD_PROGRAM_COUNTER(CreateRestElement);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(LoadThisBinding)
            :
        {
            LoadThisBinding* code = (LoadThisBinding*)programCounter;
            EnvironmentRecord* envRec = state->getThisEnvironment();
            ASSERT(envRec->isDeclarativeEnvironmentRecord() && envRec->asDeclarativeEnvironmentRecord()->isFunctionEnvironmentRecord());
            registerFile[code->m_dstIndex] = envRec->asDeclarativeEnvironmentRecord()->asFunctionEnvironmentRecord()->getThisBinding(*state);
            ADD_PROGRAM_COUNTER(LoadThisBinding);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(TryOperation)
            :
        {
            Value v = tryOperation(state, programCounter, byteCodeBlock, registerFile);
            if (!v.isEmpty()) {
                return v;
            }
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(TryCatchFinallyWithBlockBodyEnd)
            :
        {
            (*(state->rareData()->m_controlFlowRecord))[state->rareData()->m_controlFlowRecord->size() - 1] = nullptr;
            return Value();
        }

        DEFINE_OPCODE(ThrowOperation)
            :
        {
            ThrowOperation* code = (ThrowOperation*)programCounter;
            state->context()->throwException(*state, registerFile[code->m_registerIndex]);
        }

        DEFINE_OPCODE(WithOperation)
            :
        {
            Value v = withOperation(state, programCounter, byteCodeBlock, registerFile);
            if (!v.isEmpty()) {
                return v;
            }
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(JumpComplexCase)
            :
        {
            JumpComplexCase* code = (JumpComplexCase*)programCounter;
            state->ensureRareData()->m_controlFlowRecord->back() = code->m_controlFlowRecord->clone();
            return Value();
        }

        DEFINE_OPCODE(CreateEnumerateObject)
            :
        {
            CreateEnumerateObject* code = (CreateEnumerateObject*)programCounter;
            auto data = createEnumerateObject(*state, registerFile[code->m_objectRegisterIndex].toObject(*state), code->m_isDestruction);
            registerFile[code->m_dataRegisterIndex] = Value((PointerValue*)data);
            ADD_PROGRAM_COUNTER(CreateEnumerateObject);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(CheckLastEnumerateKey)
            :
        {
            CheckLastEnumerateKey* code = (CheckLastEnumerateKey*)programCounter;
            checkLastEnumerateKey(*state, code, codeBuffer, programCounter, registerFile);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(GetEnumerateKey)
            :
        {
            GetEnumerateKey* code = (GetEnumerateKey*)programCounter;
            EnumerateObject* data = (EnumerateObject*)registerFile[code->m_dataRegisterIndex].asPointerValue();
            registerFile[code->m_registerIndex] = Value(data->m_keys[data->m_index++]);
            ADD_PROGRAM_COUNTER(GetEnumerateKey);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(MarkEnumerateKey)
            :
        {
            MarkEnumerateKey* code = (MarkEnumerateKey*)programCounter;
            markEnumerateKey(*state, code, registerFile);

            ADD_PROGRAM_COUNTER(MarkEnumerateKey);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(GetIterator)
            :
        {
            GetIterator* code = (GetIterator*)programCounter;
            getIteratorOperation(*state, code, registerFile);
            ADD_PROGRAM_COUNTER(GetIterator);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(IteratorStep)
            :
        {
            IteratorStep* code = (IteratorStep*)programCounter;
            iteratorStepOperation(*state, programCounter, registerFile, codeBuffer);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(IteratorClose)
            :
        {
            IteratorClose* code = (IteratorClose*)programCounter;
            iteratorCloseOperation(*state, code, registerFile);
            ADD_PROGRAM_COUNTER(IteratorClose);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(IteratorBind)
            :
        {
            IteratorBind* code = (IteratorBind*)programCounter;
            iteratorBindOperation(*state, programCounter, registerFile);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(LoadRegexp)
            :
        {
            LoadRegexp* code = (LoadRegexp*)programCounter;
            registerFile[code->m_registerIndex] = new RegExpObject(*state, code->m_body, code->m_option);
            ADD_PROGRAM_COUNTER(LoadRegexp);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(UnaryDelete)
            :
        {
            UnaryDelete* code = (UnaryDelete*)programCounter;
            deleteOperation(*state, state->lexicalEnvironment(), code, registerFile, byteCodeBlock);
            ADD_PROGRAM_COUNTER(UnaryDelete);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(TemplateOperation)
            :
        {
            TemplateOperation* code = (TemplateOperation*)programCounter;
            templateOperation(*state, state->lexicalEnvironment(), code, registerFile);
            ADD_PROGRAM_COUNTER(TemplateOperation);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(BinaryInOperation)
            :
        {
            BinaryInOperation* code = (BinaryInOperation*)programCounter;
            const Value& left = registerFile[code->m_srcIndex0];
            const Value& right = registerFile[code->m_srcIndex1];
            bool result = binaryInOperation(*state, left, right);
            registerFile[code->m_dstIndex] = Value(result);
            ADD_PROGRAM_COUNTER(BinaryInOperation);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(BinaryInstanceOfOperation)
            :
        {
            BinaryInstanceOfOperation* code = (BinaryInstanceOfOperation*)programCounter;
            instanceOfOperation(*state, code, registerFile);
            ADD_PROGRAM_COUNTER(BinaryInstanceOfOperation);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(NewTargetOperation)
            :
        {
            NewTargetOperation* code = (NewTargetOperation*)programCounter;
            newTargetOperation(*state, code, registerFile);
            ADD_PROGRAM_COUNTER(NewTargetOperation);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(ObjectDefineGetterSetter)
            :
        {
            ObjectDefineGetterSetter* code = (ObjectDefineGetterSetter*)programCounter;
            defineObjectGetterSetter(*state, code, registerFile);
            ADD_PROGRAM_COUNTER(ObjectDefineGetterSetter);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(ReturnFunctionSlowCase)
            :
        {
            ReturnFunctionSlowCase* code = (ReturnFunctionSlowCase*)programCounter;
            if (UNLIKELY(state->rareData() != nullptr) && state->rareData()->m_controlFlowRecord && state->rareData()->m_controlFlowRecord->size()) {
                state->rareData()->m_controlFlowRecord->back() = new ControlFlowRecord(ControlFlowRecord::NeedsReturn, registerFile[code->m_registerIndex], state->rareData()->m_controlFlowRecord->size());
            }
            return Value();
        }

        DEFINE_OPCODE(CallFunctionComplexCase)
            :
        {
            CallFunctionComplexCase* code = (CallFunctionComplexCase*)programCounter;
            callFunctionComplexCase(*state, code, registerFile, byteCodeBlock);
            ADD_PROGRAM_COUNTER(CallFunctionComplexCase);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(ThrowStaticErrorOperation)
            :
        {
            ThrowStaticErrorOperation* code = (ThrowStaticErrorOperation*)programCounter;
            ErrorObject::throwBuiltinError(*state, (ErrorObject::Code)code->m_errorKind, code->m_errorMessage, code->m_templateDataString);
        }

        DEFINE_OPCODE(NewOperationWithSpreadElement)
            :
        {
            NewOperationWithSpreadElement* code = (NewOperationWithSpreadElement*)programCounter;
            const Value& callee = registerFile[code->m_calleeIndex];

            {
                ValueVector spreadArgs;
                spreadFunctionArguments(*state, &registerFile[code->m_argumentsStartIndex], code->m_argumentCount, spreadArgs);
                registerFile[code->m_resultIndex] = Object::construct(*state, registerFile[code->m_calleeIndex], spreadArgs.size(), spreadArgs.data());
            }

            ADD_PROGRAM_COUNTER(NewOperationWithSpreadElement);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(BindingRestElement)
            :
        {
            BindingRestElement* code = (BindingRestElement*)programCounter;

            Object* result;
            Value& iterOrEnum = registerFile[code->m_iterOrEnumIndex];

            if (UNLIKELY(iterOrEnum.asPointerValue()->isEnumerateObject())) {
                ASSERT(iterOrEnum.asPointerValue()->isEnumerateObject());
                EnumerateObject* enumObj = (EnumerateObject*)iterOrEnum.asPointerValue();
                result = new Object(*state);
                enumObj->fillRestElement(*state, result);
            } else {
                result = restBindOperation(*state, iterOrEnum);
            }

            registerFile[code->m_dstIndex] = result;
            ADD_PROGRAM_COUNTER(BindingRestElement);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(ExecutionResume)
            :
        {
            Value v = executionResumeOperation(state, programCounter, byteCodeBlock);
            if (!v.isEmpty()) {
                return v;
            }
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(ExecutionPause)
            :
        {
            ExecutionPause* code = (ExecutionPause*)programCounter;
            if (code->m_reason == ExecutionPause::Reason::Yield || code->m_reason == ExecutionPause::Reason::Await) {
                executionPauseOperation(*state, registerFile, programCounter, codeBuffer);
            } else if (code->m_reason == ExecutionPause::Reason::YieldDelegate) {
                Value result = executionPauseOperation(*state, registerFile, programCounter, codeBuffer);
                if (result.isEmpty()) {
                    NEXT_INSTRUCTION();
                }
                return result;
            }
            ASSERT_NOT_REACHED();
        }

        DEFINE_OPCODE(BlockOperation)
            :
        {
            BlockOperation* code = (BlockOperation*)programCounter;
            Value v = blockOperation(state, code, programCounter, byteCodeBlock, registerFile);
            if (!v.isEmpty()) {
                return v;
            }
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(ReplaceBlockLexicalEnvironmentOperation)
            :
        {
            replaceBlockLexicalEnvironmentOperation(*state, programCounter, byteCodeBlock);
            ADD_PROGRAM_COUNTER(ReplaceBlockLexicalEnvironmentOperation);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(EnsureArgumentsObject)
            :
        {
            ensureArgumentsObjectOperation(*state, byteCodeBlock, registerFile);
            ADD_PROGRAM_COUNTER(EnsureArgumentsObject);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(ResolveNameAddress)
            :
        {
            ResolveNameAddress* code = (ResolveNameAddress*)programCounter;
            resolveNameAddress(*state, code, registerFile);
            ADD_PROGRAM_COUNTER(ResolveNameAddress);
            NEXT_INSTRUCTION();
        }

        DEFINE_OPCODE(StoreByNameWithAddress)
            :
        {
            StoreByNameWithAddress* code = (StoreByNameWithAddress*)programCounter;
            storeByNameWithAddress(*state, code, registerFile);
            ADD_PROGRAM_COUNTER(StoreByNameWithAddress);
            NEXT_INSTRUCTION();
        }

        DEFINE_DEFAULT
    }

    ASSERT_NOT_REACHED();

#if defined(COMPILER_GCC) || defined(COMPILER_CLANG)
FillOpcodeTableLbl:
#define REGISTER_TABLE(opcode, pushCount, popCount) g_opcodeTable.m_table[opcode##Opcode] = &&opcode##OpcodeLbl;
    FOR_EACH_BYTECODE_OP(REGISTER_TABLE);
#undef REGISTER_TABLE
#endif

    return Value();
}

NEVER_INLINE EnvironmentRecord* ByteCodeInterpreter::getBindedEnvironmentRecordByName(ExecutionState& state, LexicalEnvironment* env, const AtomicString& name, Value& bindedValue, bool throwException)
{
    while (env) {
        EnvironmentRecord::GetBindingValueResult result = env->record()->getBindingValue(state, name);
        if (result.m_hasBindingValue) {
            bindedValue = result.m_value;
            return env->record();
        }
        env = env->outerEnvironment();
    }

    if (throwException)
        ErrorObject::throwBuiltinError(state, ErrorObject::ReferenceError, name.string(), false, String::emptyString, errorMessage_IsNotDefined);

    return NULL;
}

NEVER_INLINE Value ByteCodeInterpreter::loadByName(ExecutionState& state, LexicalEnvironment* env, const AtomicString& name, bool throwException)
{
    while (env) {
        EnvironmentRecord::GetBindingValueResult result = env->record()->getBindingValue(state, name);
        if (result.m_hasBindingValue) {
            return result.m_value;
        }
        env = env->outerEnvironment();
    }

    if (UNLIKELY((bool)state.context()->virtualIdentifierCallback())) {
        Value virtialIdResult = state.context()->virtualIdentifierCallback()(state, name.string());
        if (!virtialIdResult.isEmpty())
            return virtialIdResult;
    }

    if (throwException)
        ErrorObject::throwBuiltinError(state, ErrorObject::ReferenceError, name.string(), false, String::emptyString, errorMessage_IsNotDefined);

    return Value();
}

NEVER_INLINE void ByteCodeInterpreter::storeByName(ExecutionState& state, LexicalEnvironment* env, const AtomicString& name, const Value& value)
{
    while (env) {
        auto result = env->record()->hasBinding(state, name);
        if (result.m_index != SIZE_MAX) {
            env->record()->setMutableBindingByBindingSlot(state, result, name, value);
            return;
        }
        env = env->outerEnvironment();
    }
    if (state.inStrictMode()) {
        ErrorObject::throwBuiltinError(state, ErrorObject::Code::ReferenceError, name.string(), false, String::emptyString, errorMessage_IsNotDefined);
    }
    GlobalObject* o = state.context()->globalObject();
    o->setThrowsExceptionWhenStrictMode(state, name, value, o);
}

NEVER_INLINE void ByteCodeInterpreter::initializeByName(ExecutionState& state, LexicalEnvironment* env, const AtomicString& name, bool isLexicallyDeclaredName, const Value& value)
{
    if (isLexicallyDeclaredName) {
        state.lexicalEnvironment()->record()->initializeBinding(state, name, value);
    } else {
        while (env) {
            if (env->record()->isVarDeclarationTarget()) {
                auto result = env->record()->hasBinding(state, name);
                if (result.m_index != SIZE_MAX) {
                    env->record()->initializeBinding(state, name, value);
                    return;
                }
            }
            env = env->outerEnvironment();
        }
        ErrorObject::throwBuiltinError(state, ErrorObject::Code::ReferenceError, name.string(), false, String::emptyString, errorMessage_IsNotDefined);
    }
}

NEVER_INLINE void ByteCodeInterpreter::resolveNameAddress(ExecutionState& state, ResolveNameAddress* code, Value* registerFile)
{
    LexicalEnvironment* env = state.lexicalEnvironment();
    int64_t count = 0;
    while (env) {
        auto result = env->record()->hasBinding(state, code->m_name);
        if (result.m_index != SIZE_MAX) {
            registerFile[code->m_registerIndex] = Value(count);
            return;
        }
        count++;
        env = env->outerEnvironment();
    }
    registerFile[code->m_registerIndex] = Value(-1);
}

NEVER_INLINE void ByteCodeInterpreter::storeByNameWithAddress(ExecutionState& state, StoreByNameWithAddress* code, Value* registerFile)
{
    LexicalEnvironment* env = state.lexicalEnvironment();
    const Value& value = registerFile[code->m_valueRegisterIndex];
    int64_t count = registerFile[code->m_addressRegisterIndex].toNumber(state);
    if (count != -1) {
        int64_t idx = 0;
        while (env) {
            if (idx == count) {
                env->record()->setMutableBinding(state, code->m_name, value);
                return;
            }
            idx++;
            env = env->outerEnvironment();
        }
    }
    if (state.inStrictMode()) {
        ErrorObject::throwBuiltinError(state, ErrorObject::Code::ReferenceError, code->m_name.string(), false, String::emptyString, errorMessage_IsNotDefined);
    }
    GlobalObject* o = state.context()->globalObject();
    o->setThrowsExceptionWhenStrictMode(state, code->m_name, value, o);
}

NEVER_INLINE Value ByteCodeInterpreter::plusSlowCase(ExecutionState& state, const Value& left, const Value& right)
{
    Value ret(Value::ForceUninitialized);
    Value lval(Value::ForceUninitialized);
    Value rval(Value::ForceUninitialized);

    // http://www.ecma-international.org/ecma-262/5.1/#sec-8.12.8
    // No hint is provided in the calls to ToPrimitive in steps 5 and 6.
    // All native ECMAScript objects except Date objects handle the absence of a hint as if the hint Number were given;
    // Date objects handle the absence of a hint as if the hint String were given.
    // Host objects may handle the absence of a hint in some other manner.
    lval = left.toPrimitive(state);
    rval = right.toPrimitive(state);
    if (lval.isString() || rval.isString()) {
        ret = RopeString::createRopeString(lval.toString(state), rval.toString(state), &state);
    } else {
        ret = Value(lval.toNumber(state) + rval.toNumber(state));
    }

    return ret;
}

NEVER_INLINE Value ByteCodeInterpreter::modOperation(ExecutionState& state, const Value& left, const Value& right)
{
    Value ret(Value::ForceUninitialized);

    int32_t intLeft;
    int32_t intRight;
    if (left.isInt32() && ((intLeft = left.asInt32()) > 0) && right.isInt32() && (intRight = right.asInt32())) {
        ret = Value(intLeft % intRight);
    } else {
        double lvalue = left.toNumber(state);
        double rvalue = right.toNumber(state);
        // http://www.ecma-international.org/ecma-262/5.1/#sec-11.5.3
        if (std::isnan(lvalue) || std::isnan(rvalue)) {
            ret = Value(std::numeric_limits<double>::quiet_NaN());
        } else if (std::isinf(lvalue) || rvalue == 0 || rvalue == -0.0) {
            ret = Value(std::numeric_limits<double>::quiet_NaN());
        } else if (std::isinf(rvalue)) {
            ret = Value(lvalue);
        } else if (lvalue == 0.0) {
            if (std::signbit(lvalue))
                ret = Value(Value::EncodeAsDouble, -0.0);
            else
                ret = Value(0);
        } else {
            bool isLNeg = lvalue < 0.0;
            lvalue = std::abs(lvalue);
            rvalue = std::abs(rvalue);
            double r = fmod(lvalue, rvalue);
            if (isLNeg)
                r = -r;
            ret = Value(r);
        }
    }

    return ret;
}

NEVER_INLINE Value ByteCodeInterpreter::exponentialOperation(ExecutionState& state, const Value& left, const Value& right)
{
    Value ret(Value::ForceUninitialized);

    double base = left.toNumber(state);
    double exp = right.toNumber(state);

    // The result of base ** exponent when base is 1 or -1 and exponent is +Infinity or -Infinity differs from IEEE 754-2008. The first edition of ECMAScript specified a result of NaN for this operation, whereas later versions of IEEE 754-2008 specified 1. The historical ECMAScript behaviour is preserved for compatibility reasons.
    if ((base == -1 || base == 1) && (exp == std::numeric_limits<double>::infinity() || exp == -std::numeric_limits<double>::infinity())) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    return Value(pow(base, exp));
}

NEVER_INLINE void ByteCodeInterpreter::instanceOfOperation(ExecutionState& state, BinaryInstanceOfOperation* code, Value* registerFile)
{
    registerFile[code->m_dstIndex] = Value(registerFile[code->m_srcIndex0].instanceOf(state, registerFile[code->m_srcIndex1]));
}

NEVER_INLINE void ByteCodeInterpreter::templateOperation(ExecutionState& state, LexicalEnvironment* env, TemplateOperation* code, Value* registerFile)
{
    const Value& s1 = registerFile[code->m_src0Index];
    const Value& s2 = registerFile[code->m_src1Index];

    StringBuilder builder;
    builder.appendString(s1.toString(state));
    builder.appendString(s2.toString(state));
    registerFile[code->m_dstIndex] = Value(builder.finalize(&state));
}

NEVER_INLINE void ByteCodeInterpreter::deleteOperation(ExecutionState& state, LexicalEnvironment* env, UnaryDelete* code, Value* registerFile, ByteCodeBlock* byteCodeBlock)
{
    if (code->m_id.string()->length()) {
        bool result;
        AtomicString arguments = state.context()->staticStrings().arguments;
        if (UNLIKELY(code->m_id == arguments && !env->record()->isGlobalEnvironmentRecord())) {
            if (UNLIKELY(env->record()->isObjectEnvironmentRecord() && env->record()->hasBinding(state, arguments).m_index != SIZE_MAX)) {
                result = env->deleteBinding(state, code->m_id);
            } else {
                result = false;
            }
        } else {
            result = env->deleteBinding(state, code->m_id);
        }
        registerFile[code->m_dstIndex] = Value(result);
    } else if (code->m_hasSuperExpression) {
        if (byteCodeBlock->m_codeBlock->needsToLoadThisBindingFromEnvironment()) {
            EnvironmentRecord* envRec = state.getThisEnvironment();
            ASSERT(envRec->isDeclarativeEnvironmentRecord() && envRec->asDeclarativeEnvironmentRecord()->isFunctionEnvironmentRecord());
            envRec->asDeclarativeEnvironmentRecord()->asFunctionEnvironmentRecord()->getThisBinding(state); // check thisbinding
        }
        const Value& p = registerFile[code->m_srcIndex1];
        auto name = ObjectPropertyName(state, p);

        const Value& o = state.makeSuperPropertyReference();
        Object* obj = o.toObject(state);

        ErrorObject::throwBuiltinError(state, ErrorObject::ReferenceError, name.toObjectStructurePropertyName(state).toExceptionString(), false, String::emptyString, "ReferenceError: Unsupported reference to 'super'");
    } else {
        const Value& o = registerFile[code->m_srcIndex0];
        const Value& p = registerFile[code->m_srcIndex1];

        auto name = ObjectPropertyName(state, p);
        Object* obj = o.toObject(state);

        bool result = obj->deleteOwnProperty(state, name);
        if (!result && state.inStrictMode())
            Object::throwCannotDeleteError(state, name.toObjectStructurePropertyName(state));
        registerFile[code->m_dstIndex] = Value(result);
    }
}

ALWAYS_INLINE bool ByteCodeInterpreter::abstractRelationalComparison(ExecutionState& state, const Value& left, const Value& right, bool leftFirst)
{
    // consume very fast case
    if (LIKELY(left.isInt32() && right.isInt32())) {
        return left.asInt32() < right.asInt32();
    }

    if (LIKELY(left.isNumber() && right.isNumber())) {
        return left.asNumber() < right.asNumber();
    }

    return abstractRelationalComparisonSlowCase(state, left, right, leftFirst);
}

ALWAYS_INLINE bool ByteCodeInterpreter::abstractRelationalComparisonOrEqual(ExecutionState& state, const Value& left, const Value& right, bool leftFirst)
{
    // consume very fast case
    if (LIKELY(left.isInt32() && right.isInt32())) {
        return left.asInt32() <= right.asInt32();
    }

    if (LIKELY(left.isNumber() && right.isNumber())) {
        return left.asNumber() <= right.asNumber();
    }

    return abstractRelationalComparisonOrEqualSlowCase(state, left, right, leftFirst);
}

NEVER_INLINE bool ByteCodeInterpreter::abstractRelationalComparisonSlowCase(ExecutionState& state, const Value& left, const Value& right, bool leftFirst)
{
    Value lval(Value::ForceUninitialized);
    Value rval(Value::ForceUninitialized);
    if (leftFirst) {
        lval = left.toPrimitive(state, Value::PreferNumber);
        rval = right.toPrimitive(state, Value::PreferNumber);
    } else {
        rval = right.toPrimitive(state, Value::PreferNumber);
        lval = left.toPrimitive(state, Value::PreferNumber);
    }

    if (lval.isInt32() && rval.isInt32()) {
        return lval.asInt32() < rval.asInt32();
    } else if (lval.isString() && rval.isString()) {
        return *lval.asString() < *rval.asString();
    } else {
        double n1 = lval.toNumber(state);
        double n2 = rval.toNumber(state);
        return n1 < n2;
    }
}

NEVER_INLINE bool ByteCodeInterpreter::abstractRelationalComparisonOrEqualSlowCase(ExecutionState& state, const Value& left, const Value& right, bool leftFirst)
{
    Value lval(Value::ForceUninitialized);
    Value rval(Value::ForceUninitialized);
    if (leftFirst) {
        lval = left.toPrimitive(state, Value::PreferNumber);
        rval = right.toPrimitive(state, Value::PreferNumber);
    } else {
        rval = right.toPrimitive(state, Value::PreferNumber);
        lval = left.toPrimitive(state, Value::PreferNumber);
    }

    if (lval.isInt32() && rval.isInt32()) {
        return lval.asInt32() <= rval.asInt32();
    } else if (lval.isString() && rval.isString()) {
        return *lval.asString() <= *rval.asString();
    } else {
        double n1 = lval.toNumber(state);
        double n2 = rval.toNumber(state);
        return n1 <= n2;
    }
}

ALWAYS_INLINE Value ByteCodeInterpreter::getObjectPrecomputedCaseOperation(ExecutionState& state, Object* obj, const Value& receiver, GetObjectPreComputedCase* code, ByteCodeBlock* block)
{
    Object* orgObj = obj;
    if (LIKELY(code->m_inlineCache != nullptr)) {
        auto inlineCache = code->m_inlineCache;
        const size_t cacheFillCount = inlineCache->m_cache.size();
        GetObjectInlineCacheData* cacheData = inlineCache->m_cache.data();
        unsigned currentCacheIndex = 0;
    TestCache:
        for (; currentCacheIndex < cacheFillCount; currentCacheIndex++) {
            const GetObjectInlineCacheData& data = cacheData[currentCacheIndex];
            const size_t cSiz = data.m_cachedhiddenClassChainLength - 1;
            if (cSiz) {
                obj = orgObj;
                ObjectStructure** cachedHiddenClassChain = data.m_cachedhiddenClassChain;
                size_t cachedIndex = data.m_cachedIndex;
                for (size_t i = 0; i < cSiz; i++) {
                    if (cachedHiddenClassChain[i] != obj->structure()) {
                        currentCacheIndex++;
                        goto TestCache;
                    }
                    Object* protoObject = obj->Object::getPrototypeObject(state);
                    if (protoObject != nullptr) {
                        obj = protoObject;
                    } else {
                        currentCacheIndex++;
                        goto TestCache;
                    }
                }

                if (LIKELY(cachedHiddenClassChain[cSiz] == obj->structure())) {
                    if (LIKELY(data.m_cachedIndex != SIZE_MAX)) {
                        return obj->getOwnPropertyUtilForObject(state, data.m_cachedIndex, receiver);
                    } else {
                        return Value();
                    }
                }
            } else {
                if (LIKELY(data.m_cachedhiddenClass == orgObj->structure())) {
                    if (LIKELY(data.m_cachedIndex != SIZE_MAX)) {
                        return orgObj->getOwnPropertyUtilForObject(state, data.m_cachedIndex, receiver);
                    } else {
                        return Value();
                    }
                }
            }
        }
    }

    return getObjectPrecomputedCaseOperationCacheMiss(state, orgObj, receiver, code, block);
}

NEVER_INLINE Value ByteCodeInterpreter::getObjectPrecomputedCaseOperationCacheMiss(ExecutionState& state, Object* obj, const Value& receiver, GetObjectPreComputedCase* code, ByteCodeBlock* block)
{
    if (code->m_isLength && obj->hasTag(g_arrayObjectTag)) {
        return Value(obj->asArrayObject()->getArrayLength(state));
    }

    const int maxCacheMissCount = 16;
    const int minCacheFillCount = 3;
    const size_t maxCacheCount = 6;

    // cache miss.
    if (code->m_cacheMissCount > maxCacheMissCount) {
        return obj->get(state, ObjectPropertyName(state, code->m_propertyName)).value(state, receiver);
    }

    code->m_cacheMissCount++;
    if (code->m_cacheMissCount <= minCacheFillCount) {
        return obj->get(state, ObjectPropertyName(state, code->m_propertyName)).value(state, receiver);
    }

    if (UNLIKELY(!obj->isInlineCacheable())) {
        code->m_cacheMissCount = maxCacheMissCount + 1;
        return obj->get(state, ObjectPropertyName(state, code->m_propertyName)).value(state, receiver);
    }

    if (UNLIKELY(code->m_cacheMissCount == maxCacheMissCount)) {
        if (code->m_inlineCache) {
            code->m_inlineCache->m_cache.clear();
        }
        return obj->get(state, ObjectPropertyName(state, code->m_propertyName)).value(state, receiver);
    }

    auto& currentCodeSizeTotal = state.context()->vmInstance()->compiledByteCodeSize();

    if (!code->m_inlineCache) {
        code->m_inlineCache = new GetObjectInlineCache();
        block->m_inlineCacheDataSize += sizeof(GetObjectInlineCache);
        currentCodeSizeTotal += sizeof(GetObjectInlineCache);
        block->m_literalData.push_back(code->m_inlineCache);
    }

    auto inlineCache = code->m_inlineCache;

    if (inlineCache->m_cache.size() > maxCacheCount) {
        return obj->get(state, ObjectPropertyName(state, code->m_propertyName)).value(state, receiver);
    }

    Object* orgObj = obj;
    inlineCache->m_cache.insert(0, GetObjectInlineCacheData());
    block->m_inlineCacheDataSize += sizeof(GetObjectInlineCacheData);
    currentCodeSizeTotal += sizeof(GetObjectInlineCacheData);

    auto& newItem = inlineCache->m_cache[0];
    VectorWithInlineStorage<24, ObjectStructure*, std::allocator<ObjectStructure*>> cachedhiddenClassChain;

    while (true) {
        auto s = obj->structure();
        cachedhiddenClassChain.push_back(s);
        auto result = s->findProperty(code->m_propertyName);

        if (result.first != SIZE_MAX) {
            newItem.m_cachedIndex = result.first;
            break;
        }

        obj = obj->Object::getPrototypeObject(state);

        if (!obj) {
            newItem.m_cachedIndex = SIZE_MAX;
            break;
        }

        if (UNLIKELY(!obj->isInlineCacheable())) {
            inlineCache->m_cache.clear();
            code->m_cacheMissCount = maxCacheMissCount + 1;
            return obj->get(state, ObjectPropertyName(state, code->m_propertyName)).value(state, receiver);
        }
    }

    newItem.m_cachedhiddenClassChainLength = cachedhiddenClassChain.size();
    if (newItem.m_cachedhiddenClassChainLength == 1) {
        newItem.m_cachedhiddenClass = cachedhiddenClassChain[0];
    } else {
        block->m_inlineCacheDataSize += sizeof(size_t) * cachedhiddenClassChain.size();
        currentCodeSizeTotal += sizeof(size_t) * cachedhiddenClassChain.size();
        newItem.m_cachedhiddenClassChain = (ObjectStructure**)GC_MALLOC(sizeof(ObjectStructure*) * cachedhiddenClassChain.size());
        memcpy(newItem.m_cachedhiddenClassChain, cachedhiddenClassChain.data(), sizeof(ObjectStructure*) * cachedhiddenClassChain.size());
    }

    if (newItem.m_cachedIndex != SIZE_MAX) {
        return obj->getOwnPropertyUtilForObject(state, newItem.m_cachedIndex, receiver);
    } else {
        return Value();
    }
}

ALWAYS_INLINE void ByteCodeInterpreter::setObjectPreComputedCaseOperation(ExecutionState& state, const Value& willBeObject, const Value& value, SetObjectPreComputedCase* code, ByteCodeBlock* block)
{
    Object* obj;
    if (UNLIKELY(!willBeObject.isObject())) {
        obj = willBeObject.toObject(state);
        if (willBeObject.isPrimitive()) {
            obj->preventExtensions(state);
        }
    } else {
        obj = willBeObject.asObject();
    }
    Object* originalObject = obj;
    ASSERT(originalObject != nullptr);

    ObjectStructure* testItem = obj->structure();

    auto inlineCache = code->m_inlineCache;

    if (inlineCache) {
        if (inlineCache->m_cachedhiddenClassChainLength == 1 && inlineCache->m_cachedHiddenClass == testItem) {
            // cache hit!
            obj->m_values[inlineCache->m_cachedIndex] = value;
            return;
        } else if (inlineCache->m_hiddenClassWillBe) {
            const auto& cSiz = inlineCache->m_cachedhiddenClassChainLength;
            bool miss = false;
            for (size_t i = 0; i < cSiz - 1; i++) {
                if (UNLIKELY(inlineCache->m_cachedHiddenClassChainData[i] != obj->structure())) {
                    miss = true;
                    break;
                } else {
                    Object* o = obj->Object::getPrototypeObject(state);
                    if (UNLIKELY(!o)) {
                        miss = true;
                        break;
                    }
                    obj = o;
                }
            }
            if (LIKELY(!miss) && inlineCache->m_cachedHiddenClassChainData[cSiz - 1] == obj->structure()) {
                // cache hit!
                obj = originalObject;
                ASSERT(obj->structure()->inTransitionMode());
                obj->m_values.push_back(value, inlineCache->m_hiddenClassWillBe->propertyCount());
                obj->m_structure = inlineCache->m_hiddenClassWillBe;
                return;
            }
        }
    }

    setObjectPreComputedCaseOperationCacheMiss(state, originalObject, willBeObject, value, code, block);
}

NEVER_INLINE void ByteCodeInterpreter::setObjectPreComputedCaseOperationCacheMiss(ExecutionState& state, Object* originalObject, const Value& willBeObject, const Value& value, SetObjectPreComputedCase* code, ByteCodeBlock* block)
{
    if (code->m_isLength && originalObject->hasTag(g_arrayObjectTag) && originalObject->asArrayObject()->isFastModeArray()) {
        if (!originalObject->asArrayObject()->setArrayLength(state, value) && state.inStrictMode()) {
            ErrorObject::throwBuiltinError(state, ErrorObject::Code::TypeError, code->m_propertyName.toExceptionString(), false, String::emptyString, errorMessage_DefineProperty_NotWritable);
        }
        return;
    }

    // cache miss
    if (code->m_missCount > 16) {
        originalObject->setThrowsExceptionWhenStrictMode(state, ObjectPropertyName(state, code->m_propertyName), value, willBeObject);
        return;
    }

    if (code->m_missCount < 3) {
        code->m_missCount++;
        originalObject->setThrowsExceptionWhenStrictMode(state, ObjectPropertyName(state, code->m_propertyName), value, willBeObject);
        return;
    }

    if (UNLIKELY(!originalObject->isInlineCacheable())) {
        code->m_missCount++;
        originalObject->setThrowsExceptionWhenStrictMode(state, ObjectPropertyName(state, code->m_propertyName), value, willBeObject);
        return;
    }

    auto& currentCodeSizeTotal = state.context()->vmInstance()->compiledByteCodeSize();

    if (code->m_inlineCache == nullptr) {
        code->m_inlineCache = new SetObjectInlineCache();
        block->m_inlineCacheDataSize += sizeof(SetObjectInlineCache);
        currentCodeSizeTotal += sizeof(SetObjectInlineCache);
        block->m_literalData.push_back(code->m_inlineCache);
    }

    auto inlineCache = code->m_inlineCache;

    inlineCache->invalidateCache();
    code->m_missCount++;

    Object* obj = originalObject;

    auto findResult = obj->structure()->findProperty(code->m_propertyName);
    if (findResult.first != SIZE_MAX) {
        // own property
        obj->setOwnPropertyThrowsExceptionWhenStrictMode(state, findResult.first, value, willBeObject);
        // Don't update the inline cache if the property is removed by a setter function.
        /* example code
        var o = { set foo (a) { var a = delete o.foo } };
        o.foo = 0;
        */
        if (UNLIKELY(findResult.first >= obj->structure()->propertyCount())) {
            return;
        }

        const auto& propertyData = obj->structure()->readProperty(findResult.first);
        const auto& desc = propertyData.m_descriptor;
        if (propertyData.m_propertyName == code->m_propertyName && desc.isPlainDataProperty() && desc.isWritable()) {
            inlineCache->m_cachedIndex = findResult.first;
            inlineCache->m_cachedhiddenClassChainLength = 1;
            inlineCache->m_cachedHiddenClass = obj->structure();
        }
    } else {
        Object* orgObject = obj;
        if (UNLIKELY(!obj->structure()->inTransitionMode())) {
            inlineCache->invalidateCache();
            orgObject->setThrowsExceptionWhenStrictMode(state, ObjectPropertyName(state, code->m_propertyName), value, willBeObject);
            return;
        }

        VectorWithInlineStorage<24, ObjectStructure*, std::allocator<ObjectStructure*>> cachedhiddenClassChain;
        cachedhiddenClassChain.push_back(obj->structure());
        Value proto = obj->getPrototype(state);
        while (proto.isObject()) {
            obj = proto.asObject();

            if (!UNLIKELY(obj->isInlineCacheable())) {
                code->m_missCount++;
                originalObject->setThrowsExceptionWhenStrictMode(state, ObjectPropertyName(state, code->m_propertyName), value, willBeObject);
                return;
            }

            cachedhiddenClassChain.push_back(obj->structure());
            proto = obj->getPrototype(state);
        }

        inlineCache->m_hiddenClassWillBe = nullptr;
        inlineCache->m_cachedhiddenClassChainLength = cachedhiddenClassChain.size();
        inlineCache->m_cachedHiddenClassChainData = (ObjectStructure**)GC_MALLOC(sizeof(ObjectStructure*) * inlineCache->m_cachedhiddenClassChainLength);
        memcpy(inlineCache->m_cachedHiddenClassChainData, cachedhiddenClassChain.data(), sizeof(ObjectStructure*) * inlineCache->m_cachedhiddenClassChainLength);

        block->m_inlineCacheDataSize += sizeof(size_t) * inlineCache->m_cachedhiddenClassChainLength;
        currentCodeSizeTotal += sizeof(size_t) * inlineCache->m_cachedhiddenClassChainLength;
        bool s = orgObject->set(state, ObjectPropertyName(state, code->m_propertyName), value, willBeObject);
        if (UNLIKELY(!s)) {
            if (state.inStrictMode()) {
                orgObject->throwCannotWriteError(state, code->m_propertyName);
            }

            inlineCache->invalidateCache();
            return;
        }
        if (!orgObject->structure()->inTransitionMode()) {
            inlineCache->invalidateCache();
            return;
        }

        auto result = orgObject->get(state, ObjectPropertyName(state, code->m_propertyName));
        if (!result.hasValue() || !result.isDataProperty()) {
            inlineCache->invalidateCache();
            return;
        }

        inlineCache->m_hiddenClassWillBe = orgObject->structure();
    }
}

ALWAYS_INLINE Object* ByteCodeInterpreter::fastToObject(ExecutionState& state, const Value& obj)
{
    if (LIKELY(obj.isString())) {
        StringObject* o = state.context()->globalObject()->stringProxyObject();
        o->setPrimitiveValue(state, obj.asString());
        return o;
    } else if (obj.isNumber()) {
        NumberObject* o = state.context()->globalObject()->numberProxyObject();
        o->setPrimitiveValue(state, obj.asNumber());
        return o;
    }
    return obj.toObject(state);
}

NEVER_INLINE Value ByteCodeInterpreter::getGlobalVariableSlowCase(ExecutionState& state, Object* go, GlobalVariableAccessCacheItem* slot, ByteCodeBlock* block)
{
    Context* ctx = state.context();
    auto& records = *ctx->globalDeclarativeRecord();
    AtomicString name = slot->m_propertyName;
    auto siz = records.size();

    for (size_t i = 0; i < siz; i++) {
        if (records[i].m_name == name) {
            slot->m_lexicalIndexCache = i;
            slot->m_cachedAddress = nullptr;
            slot->m_cachedStructure = nullptr;
            auto v = (*state.context()->globalDeclarativeStorage())[i];
            if (UNLIKELY(v.isEmpty())) {
                ErrorObject::throwBuiltinError(state, ErrorObject::ReferenceError, name.string(), false, String::emptyString, errorMessage_IsNotInitialized);
            }
            return v;
        }
    }

    auto findResult = go->structure()->findProperty(slot->m_propertyName);
    if (UNLIKELY(findResult.first == SIZE_MAX)) {
        ObjectGetResult res = go->get(state, ObjectPropertyName(state, slot->m_propertyName));
        if (res.hasValue()) {
            return res.value(state, go);
        } else {
            if (UNLIKELY((bool)state.context()->virtualIdentifierCallback())) {
                Value virtialIdResult = state.context()->virtualIdentifierCallback()(state, name.string());
                if (!virtialIdResult.isEmpty())
                    return virtialIdResult;
            }
            ErrorObject::throwBuiltinError(state, ErrorObject::ReferenceError, name.string(), false, String::emptyString, errorMessage_IsNotDefined);
            ASSERT_NOT_REACHED();
            return Value(Value::EmptyValue);
        }
    } else {
        const ObjectStructureItem* item = findResult.second.value();
        if (!item->m_descriptor.isPlainDataProperty() || !item->m_descriptor.isWritable()) {
            slot->m_cachedStructure = nullptr;
            slot->m_cachedAddress = nullptr;
            slot->m_lexicalIndexCache = std::numeric_limits<size_t>::max();
            return go->getOwnPropertyUtilForObject(state, findResult.first, go);
        }

        slot->m_cachedAddress = &go->m_values.data()[findResult.first];
        slot->m_cachedStructure = go->structure();
        slot->m_lexicalIndexCache = siz;
        return *((SmallValue*)slot->m_cachedAddress);
    }
}

class VirtualIdDisabler {
public:
    explicit VirtualIdDisabler(Context* c)
        : fn(c->virtualIdentifierCallback())
        , ctx(c)
    {
        c->setVirtualIdentifierCallback(nullptr);
    }
    ~VirtualIdDisabler()
    {
        ctx->setVirtualIdentifierCallback(fn);
    }

    VirtualIdentifierCallback fn;
    Context* ctx;
};

NEVER_INLINE void ByteCodeInterpreter::setGlobalVariableSlowCase(ExecutionState& state, Object* go, GlobalVariableAccessCacheItem* slot, const Value& value, ByteCodeBlock* block)
{
    Context* ctx = state.context();
    auto& records = *ctx->globalDeclarativeRecord();
    AtomicString name = slot->m_propertyName;
    auto siz = records.size();
    for (size_t i = 0; i < siz; i++) {
        if (records[i].m_name == name) {
            slot->m_lexicalIndexCache = i;
            slot->m_cachedAddress = nullptr;
            slot->m_cachedStructure = nullptr;
            auto& place = (*ctx->globalDeclarativeStorage())[i];
            if (UNLIKELY(place.isEmpty())) {
                ErrorObject::throwBuiltinError(state, ErrorObject::ReferenceError, name.string(), false, String::emptyString, errorMessage_IsNotInitialized);
            }
            place = value;
            return;
        }
    }


    auto findResult = go->structure()->findProperty(slot->m_propertyName);
    if (UNLIKELY(findResult.first == SIZE_MAX)) {
        if (UNLIKELY(state.inStrictMode())) {
            ErrorObject::throwBuiltinError(state, ErrorObject::ReferenceError, slot->m_propertyName.string(), false, String::emptyString, errorMessage_IsNotDefined);
        }
        VirtualIdDisabler d(state.context());
        go->setThrowsExceptionWhenStrictMode(state, ObjectPropertyName(state, slot->m_propertyName), value, go);
    } else {
        const ObjectStructureItem* item = findResult.second.value();
        if (!item->m_descriptor.isPlainDataProperty() || !item->m_descriptor.isWritable()) {
            slot->m_cachedStructure = nullptr;
            slot->m_cachedAddress = nullptr;
            slot->m_lexicalIndexCache = std::numeric_limits<size_t>::max();
            go->setThrowsExceptionWhenStrictMode(state, ObjectPropertyName(state, slot->m_propertyName), value, go);
            return;
        }

        slot->m_cachedAddress = &go->m_values.data()[findResult.first];
        slot->m_cachedStructure = go->structure();
        slot->m_lexicalIndexCache = siz;

        go->setOwnPropertyThrowsExceptionWhenStrictMode(state, findResult.first, value, go);
    }
}

NEVER_INLINE void ByteCodeInterpreter::initializeGlobalVariable(ExecutionState& state, InitializeGlobalVariable* code, const Value& value)
{
    Context* ctx = state.context();
    auto& records = *ctx->globalDeclarativeRecord();
    for (size_t i = 0; i < records.size(); i++) {
        if (records[i].m_name == code->m_variableName) {
            state.context()->globalDeclarativeStorage()->at(i) = value;
            return;
        }
    }
    ASSERT_NOT_REACHED();
}

NEVER_INLINE void ByteCodeInterpreter::createFunctionOperation(ExecutionState& state, CreateFunction* code, ByteCodeBlock* byteCodeBlock, Value* registerFile)
{
    CodeBlock* cb = code->m_codeBlock;

    LexicalEnvironment* outerLexicalEnvironment = state.mostNearestHeapAllocatedLexicalEnvironment();

    bool isGenerator = cb->isGenerator();
    bool isAsync = cb->isAsync();

    if (UNLIKELY(isGenerator && isAsync)) {
        Value thisValue = cb->isArrowFunctionExpression() ? registerFile[byteCodeBlock->m_requiredRegisterFileSizeInValueSize] : Value(Value::EmptyValue);
        Object* homeObject = (cb->isClassMethod() || cb->isClassStaticMethod()) ? registerFile[code->m_homeObjectRegisterIndex].asObject() : nullptr;
        registerFile[code->m_registerIndex] = new ScriptAsyncGeneratorFunctionObject(state, code->m_codeBlock, outerLexicalEnvironment, thisValue, homeObject);
    } else if (UNLIKELY(isGenerator)) {
        Value thisValue = cb->isArrowFunctionExpression() ? registerFile[byteCodeBlock->m_requiredRegisterFileSizeInValueSize] : Value(Value::EmptyValue);
        Object* homeObject = (cb->isClassMethod() || cb->isClassStaticMethod()) ? registerFile[code->m_homeObjectRegisterIndex].asObject() : nullptr;
        registerFile[code->m_registerIndex] = new ScriptGeneratorFunctionObject(state, code->m_codeBlock, outerLexicalEnvironment, thisValue, homeObject);
    } else if (UNLIKELY(isAsync)) {
        Value thisValue = cb->isArrowFunctionExpression() ? registerFile[byteCodeBlock->m_requiredRegisterFileSizeInValueSize] : Value(Value::EmptyValue);
        Object* homeObject = (cb->isClassMethod() || cb->isClassStaticMethod()) ? registerFile[code->m_homeObjectRegisterIndex].asObject() : nullptr;
        registerFile[code->m_registerIndex] = new ScriptAsyncFunctionObject(state, code->m_codeBlock, outerLexicalEnvironment, thisValue, homeObject);
    } else if (cb->isArrowFunctionExpression()) {
        registerFile[code->m_registerIndex] = new ScriptArrowFunctionObject(state, code->m_codeBlock, outerLexicalEnvironment, registerFile[byteCodeBlock->m_requiredRegisterFileSizeInValueSize]);
    } else if (cb->isClassMethod() || cb->isClassStaticMethod()) {
        registerFile[code->m_registerIndex] = new ScriptClassMethodFunctionObject(state, code->m_codeBlock, outerLexicalEnvironment, registerFile[code->m_homeObjectRegisterIndex].asObject());
    } else {
        registerFile[code->m_registerIndex] = new ScriptFunctionObject(state, code->m_codeBlock, outerLexicalEnvironment, true, false, false);
    }
}

NEVER_INLINE ArrayObject* ByteCodeInterpreter::createRestElementOperation(ExecutionState& state, ByteCodeBlock* byteCodeBlock)
{
    ASSERT(state.resolveCallee());

    ArrayObject* newArray;
    size_t parameterLen = (size_t)byteCodeBlock->m_codeBlock->parameterCount() - 1; // parameter length except the rest element
    size_t argc = state.argc();
    Value* argv = state.argv();

    if (argc > parameterLen) {
        size_t arrLen = argc - parameterLen;
        newArray = new ArrayObject(state, (double)arrLen);
        for (size_t i = 0; i < arrLen; i++) {
            newArray->setIndexedProperty(state, Value(i), argv[parameterLen + i]);
        }
    } else {
        newArray = new ArrayObject(state);
    }

    return newArray;
}

NEVER_INLINE Value ByteCodeInterpreter::tryOperation(ExecutionState*& state, size_t& programCounter, ByteCodeBlock* byteCodeBlock, Value* registerFile)
{
    char* codeBuffer = byteCodeBlock->m_code.data();
    TryOperation* code = (TryOperation*)programCounter;
    bool oldInTryStatement = state->m_inTryStatement;

    bool inPauserScope = state->inPauserScope();
    bool inPauserResumeProcess = code->m_isTryResumeProcess || code->m_isCatchResumeProcess || code->m_isFinallyResumeProcess;
    bool shouldUseHeapAllocatedState = inPauserScope && !inPauserResumeProcess;
    ExecutionState* newState;

    if (UNLIKELY(shouldUseHeapAllocatedState)) {
        newState = new ExecutionState(state, state->lexicalEnvironment(), state->inStrictMode());
        newState->ensureRareData();
    } else {
        newState = new (alloca(sizeof(ExecutionState))) ExecutionState(state, state->lexicalEnvironment(), state->inStrictMode());
    }

    if (!LIKELY(inPauserResumeProcess)) {
        if (!state->ensureRareData()->m_controlFlowRecord) {
            state->ensureRareData()->m_controlFlowRecord = new ControlFlowRecordVector();
        }
        state->ensureRareData()->m_controlFlowRecord->pushBack(nullptr);
        newState->ensureRareData()->m_controlFlowRecord = state->rareData()->m_controlFlowRecord;
    }

    if (LIKELY(!code->m_isCatchResumeProcess && !code->m_isFinallyResumeProcess)) {
        try {
            newState->m_inTryStatement = true;
            size_t newPc = programCounter + sizeof(TryOperation);
            interpret(newState, byteCodeBlock, resolveProgramCounter(codeBuffer, newPc), registerFile);
            clearStack<512>();
            if (UNLIKELY(code->m_isTryResumeProcess)) {
                state = newState->parent();
                code = (TryOperation*)(byteCodeBlock->m_code.data() + newState->rareData()->m_programCounterWhenItStoppedByYield);
                newState = new ExecutionState(state, state->lexicalEnvironment(), state->inStrictMode());
                newState->ensureRareData()->m_controlFlowRecord = state->rareData()->m_controlFlowRecord;
            }
            newState->m_inTryStatement = oldInTryStatement;
        } catch (const Value& val) {
            if (UNLIKELY(code->m_isTryResumeProcess)) {
                state = newState->parent();
                code = (TryOperation*)(byteCodeBlock->m_code.data() + newState->rareData()->m_programCounterWhenItStoppedByYield);
                newState = new ExecutionState(state, state->lexicalEnvironment(), state->inStrictMode());
                newState->ensureRareData()->m_controlFlowRecord = state->rareData()->m_controlFlowRecord;
            }
            newState->m_inTryStatement = oldInTryStatement;

            newState->context()->vmInstance()->currentSandBox()->fillStackDataIntoErrorObject(val);

#ifndef NDEBUG
            if (getenv("DUMP_ERROR_IN_TRY_CATCH") && strlen(getenv("DUMP_ERROR_IN_TRY_CATCH"))) {
                ErrorObject::StackTraceData* data = ErrorObject::StackTraceData::create(newState->context()->vmInstance()->currentSandBox());
                StringBuilder builder;
                builder.appendString("Caught error in try-catch block\n");
                data->buildStackTrace(newState->context(), builder);
                ESCARGOT_LOG_ERROR("%s\n", builder.finalize()->toUTF8StringData().data());
            }
#endif

            newState->context()->vmInstance()->currentSandBox()->m_stackTraceData.clear();
            if (!code->m_hasCatch) {
                newState->rareData()->m_controlFlowRecord->back() = new ControlFlowRecord(ControlFlowRecord::NeedsThrow, val);
            } else {
                registerFile[code->m_catchedValueRegisterIndex] = val;
                try {
                    interpret(newState, byteCodeBlock, code->m_catchPosition, registerFile);
                } catch (const Value& val) {
                    newState->rareData()->m_controlFlowRecord->back() = new ControlFlowRecord(ControlFlowRecord::NeedsThrow, val);
                }
            }
        }
    } else if (code->m_isCatchResumeProcess) {
        try {
            interpret(newState, byteCodeBlock, resolveProgramCounter(codeBuffer, programCounter + sizeof(TryOperation)), registerFile);
            state = newState->parent();
            code = (TryOperation*)(byteCodeBlock->m_code.data() + newState->rareData()->m_programCounterWhenItStoppedByYield);
        } catch (const Value& val) {
            state = newState->parent();
            code = (TryOperation*)(byteCodeBlock->m_code.data() + newState->rareData()->m_programCounterWhenItStoppedByYield);
            state->rareData()->m_controlFlowRecord->back() = new ControlFlowRecord(ControlFlowRecord::NeedsThrow, val);
        }
    }

    if (code->m_isFinallyResumeProcess) {
        interpret(newState, byteCodeBlock, resolveProgramCounter(codeBuffer, programCounter + sizeof(TryOperation)), registerFile);
        state = newState->parent();
        code = (TryOperation*)(byteCodeBlock->m_code.data() + newState->rareData()->m_programCounterWhenItStoppedByYield);
    } else if (code->m_hasFinalizer) {
        if (UNLIKELY(code->m_isTryResumeProcess || code->m_isCatchResumeProcess)) {
            state = newState->parent();
            code = (TryOperation*)(codeBuffer + newState->rareData()->m_programCounterWhenItStoppedByYield);
            newState = new ExecutionState(state, state->lexicalEnvironment(), state->inStrictMode());
            newState->ensureRareData()->m_controlFlowRecord = state->rareData()->m_controlFlowRecord;
        }
        interpret(newState, byteCodeBlock, code->m_tryCatchEndPosition, registerFile);
    }

    clearStack<512>();

    ControlFlowRecord* record = state->rareData()->m_controlFlowRecord->back();
    state->rareData()->m_controlFlowRecord->erase(state->rareData()->m_controlFlowRecord->size() - 1);

    if (record != nullptr) {
        if (record->reason() == ControlFlowRecord::NeedsJump) {
            size_t pos = record->wordValue();
            record->m_count--;
            if (record->count() && (record->outerLimitCount() < record->count())) {
                state->rareData()->m_controlFlowRecord->back() = record;
                return Value();
            } else {
                programCounter = jumpTo(codeBuffer, pos);
                return Value(Value::EmptyValue);
            }
        } else if (record->reason() == ControlFlowRecord::NeedsThrow) {
            state->context()->throwException(*state, record->value());
            ASSERT_NOT_REACHED();
            // never get here. but I add return statement for removing compile warning
            return Value(Value::EmptyValue);
        } else {
            ASSERT(record->reason() == ControlFlowRecord::NeedsReturn);
            record->m_count--;
            if (record->count()) {
                state->rareData()->m_controlFlowRecord->back() = record;
            }
            return record->value();
        }
    } else {
        programCounter = jumpTo(codeBuffer, code->m_finallyEndPosition);
        return Value(Value::EmptyValue);
    }
}

NEVER_INLINE void ByteCodeInterpreter::classOperation(ExecutionState& state, CreateClass* code, Value* registerFile)
{
    Value protoParent;
    Value constructorParent;

    // http://www.ecma-international.org/ecma-262/6.0/#sec-runtime-semantics-classdefinitionevaluation

    bool heritagePresent = code->m_superClassRegisterIndex != REGISTER_LIMIT;

    // 5.
    if (!heritagePresent) {
        // 5.a
        protoParent = state.context()->globalObject()->objectPrototype();
        // 5.b
        constructorParent = state.context()->globalObject()->functionPrototype();

    } else {
        // 5.a-c
        const Value& superClass = registerFile[code->m_superClassRegisterIndex];

        if (superClass.isNull()) {
            protoParent = Value(Value::Null);
            constructorParent = state.context()->globalObject()->functionPrototype();
        } else if (!superClass.isConstructor()) {
            ErrorObject::throwBuiltinError(state, ErrorObject::TypeError, errorMessage_Class_Extends_Value_Is_Not_Object_Nor_Null);
        } else {
            if (superClass.isObject() && superClass.asObject()->isGeneratorObject()) {
                ErrorObject::throwBuiltinError(state, ErrorObject::TypeError, errorMessage_Class_Prototype_Is_Not_Object_Nor_Null);
            }

            protoParent = superClass.asObject()->get(state, ObjectPropertyName(state.context()->staticStrings().prototype)).value(state, Value());

            if (!protoParent.isObject() && !protoParent.isNull()) {
                ErrorObject::throwBuiltinError(state, ErrorObject::TypeError, errorMessage_Class_Prototype_Is_Not_Object_Nor_Null);
            }

            constructorParent = superClass;
        }
    }

    ScriptClassConstructorPrototypeObject* proto = new ScriptClassConstructorPrototypeObject(state);
    proto->setPrototype(state, protoParent);

    ScriptClassConstructorFunctionObject* constructor;

    if (code->m_codeBlock) {
        constructor = new ScriptClassConstructorFunctionObject(state, code->m_codeBlock, state.mostNearestHeapAllocatedLexicalEnvironment(), proto, code->m_classSrc);
    } else {
        if (!heritagePresent) {
            Value argv[] = { String::emptyString, String::emptyString };
            auto functionSource = FunctionObject::createFunctionSourceFromScriptSource(state, state.context()->staticStrings().constructor, 1, &argv[0], argv[1], true, false, false, false);
            functionSource.codeBlock->setAsClassConstructor();
            constructor = new ScriptClassConstructorFunctionObject(state, functionSource.codeBlock, functionSource.outerEnvironment, proto, code->m_classSrc);
        } else {
            Value argv[] = { new ASCIIString("...args"), new ASCIIString("super(...args)") };
            auto functionSource = FunctionObject::createFunctionSourceFromScriptSource(state, state.context()->staticStrings().constructor, 1, &argv[0], argv[1], true, false, false, true);
            functionSource.codeBlock->setAsClassConstructor();
            functionSource.codeBlock->setAsDerivedClassConstructor();
            constructor = new ScriptClassConstructorFunctionObject(state, functionSource.codeBlock, functionSource.outerEnvironment, proto, code->m_classSrc);
        }
    }

    constructor->setPrototype(state, constructorParent);
    constructor->asFunctionObject()->setFunctionPrototype(state, proto);

    // Perform CreateMethodProperty(proto, "constructor", F).
    // --> CreateMethodProperty: Let newDesc be the PropertyDescriptor{[[Value]]: V, [[Writable]]: true, [[Enumerable]]: false, [[Configurable]]: true}.
    proto->defineOwnProperty(state, state.context()->staticStrings().constructor, ObjectPropertyDescriptor(constructor, (ObjectPropertyDescriptor::PresentAttribute)(ObjectPropertyDescriptor::WritablePresent | ObjectPropertyDescriptor::ConfigurablePresent | ObjectPropertyDescriptor::NonEnumerablePresent | ObjectPropertyDescriptor::ValuePresent)));

    registerFile[code->m_classConstructorRegisterIndex] = constructor;
    registerFile[code->m_classPrototypeRegisterIndex] = proto;
}

NEVER_INLINE void ByteCodeInterpreter::superOperation(ExecutionState& state, SuperReference* code, Value* registerFile)
{
    if (code->m_isCall) {
        // Let newTarget be GetNewTarget().
        Object* newTarget = state.getNewTarget();
        // If newTarget is undefined, throw a ReferenceError exception.
        if (!newTarget) {
            ErrorObject::throwBuiltinError(state, ErrorObject::TypeError, errorMessage_New_Target_Is_Undefined);
        }
        registerFile[code->m_dstIndex] = state.getSuperConstructor();
    } else {
        registerFile[code->m_dstIndex] = state.makeSuperPropertyReference();
    }
}

NEVER_INLINE void ByteCodeInterpreter::superSetObjectOperation(ExecutionState& state, SuperSetObjectOperation* code, Value* registerFile, ByteCodeBlock* byteCodeBlock)
{
    // find `this` value for receiver
    Value thisValue(Value::ForceUninitialized);
    if (byteCodeBlock->m_codeBlock->needsToLoadThisBindingFromEnvironment()) {
        EnvironmentRecord* envRec = state.getThisEnvironment();
        ASSERT(envRec->isDeclarativeEnvironmentRecord() && envRec->asDeclarativeEnvironmentRecord()->isFunctionEnvironmentRecord());
        thisValue = envRec->asDeclarativeEnvironmentRecord()->asFunctionEnvironmentRecord()->getThisBinding(state);
    } else {
        thisValue = registerFile[byteCodeBlock->m_requiredRegisterFileSizeInValueSize];
    }

    Value object = registerFile[code->m_objectRegisterIndex];
    object.toObject(state)->set(state, ObjectPropertyName(state, registerFile[code->m_propertyNameIndex]), registerFile[code->m_loadRegisterIndex], thisValue);
}

NEVER_INLINE Value ByteCodeInterpreter::superGetObjectOperation(ExecutionState& state, SuperGetObjectOperation* code, Value* registerFile, ByteCodeBlock* byteCodeBlock)
{
    // find `this` value for receiver
    Value thisValue(Value::ForceUninitialized);
    if (byteCodeBlock->m_codeBlock->needsToLoadThisBindingFromEnvironment()) {
        EnvironmentRecord* envRec = state.getThisEnvironment();
        ASSERT(envRec->isDeclarativeEnvironmentRecord() && envRec->asDeclarativeEnvironmentRecord()->isFunctionEnvironmentRecord());
        thisValue = envRec->asDeclarativeEnvironmentRecord()->asFunctionEnvironmentRecord()->getThisBinding(state);
    } else {
        thisValue = registerFile[byteCodeBlock->m_requiredRegisterFileSizeInValueSize];
    }

    Value object = registerFile[code->m_objectRegisterIndex];
    return object.toObject(state)->get(state, ObjectPropertyName(state, registerFile[code->m_propertyNameIndex])).value(state, thisValue);
}

NEVER_INLINE Value ByteCodeInterpreter::withOperation(ExecutionState*& state, size_t& programCounter, ByteCodeBlock* byteCodeBlock, Value* registerFile)
{
    WithOperation* code = (WithOperation*)programCounter;

    bool inPauserScope = state->inPauserScope();
    bool inPauserResumeProcess = code->m_registerIndex == REGISTER_LIMIT;

    LexicalEnvironment* newEnv;
    if (!LIKELY(inPauserResumeProcess)) {
        LexicalEnvironment* env = state->lexicalEnvironment();
        if (!state->ensureRareData()->m_controlFlowRecord) {
            state->ensureRareData()->m_controlFlowRecord = new ControlFlowRecordVector();
        }
        state->ensureRareData()->m_controlFlowRecord->pushBack(nullptr);
        size_t newPc = programCounter + sizeof(WithOperation);
        char* codeBuffer = byteCodeBlock->m_code.data();

        // setup new env
        EnvironmentRecord* newRecord = new ObjectEnvironmentRecord(registerFile[code->m_registerIndex].toObject(*state));
        newEnv = new LexicalEnvironment(newRecord, env);
    } else {
        newEnv = nullptr;
    }

    bool shouldUseHeapAllocatedState = inPauserScope && !inPauserResumeProcess;
    ExecutionState* newState;

    if (UNLIKELY(shouldUseHeapAllocatedState)) {
        newState = new ExecutionState(state, newEnv, state->inStrictMode());
        newState->ensureRareData();
    } else {
        newState = new (alloca(sizeof(ExecutionState))) ExecutionState(state, newEnv, state->inStrictMode());
    }

    if (!LIKELY(inPauserResumeProcess)) {
        newState->ensureRareData()->m_controlFlowRecord = state->rareData()->m_controlFlowRecord;
    }

    size_t newPc = programCounter + sizeof(WithOperation);
    char* codeBuffer = byteCodeBlock->m_code.data();
    interpret(newState, byteCodeBlock, resolveProgramCounter(codeBuffer, newPc), registerFile);

    if (UNLIKELY(inPauserResumeProcess)) {
        state = newState->parent();
        code = (WithOperation*)(byteCodeBlock->m_code.data() + newState->rareData()->m_programCounterWhenItStoppedByYield);
    }

    ControlFlowRecord* record = state->rareData()->m_controlFlowRecord->back();
    state->rareData()->m_controlFlowRecord->erase(state->rareData()->m_controlFlowRecord->size() - 1);

    if (record) {
        if (record->reason() == ControlFlowRecord::NeedsJump) {
            size_t pos = record->wordValue();
            record->m_count--;
            if (record->count() && (record->outerLimitCount() < record->count())) {
                state->rareData()->m_controlFlowRecord->back() = record;
                return Value();
            } else {
                programCounter = jumpTo(codeBuffer, pos);
                return Value(Value::EmptyValue);
            }
        } else {
            ASSERT(record->reason() == ControlFlowRecord::NeedsReturn);
            record->m_count--;
            if (record->count()) {
                state->rareData()->m_controlFlowRecord->back() = record;
            }
            return record->value();
        }
    } else {
        programCounter = jumpTo(codeBuffer, code->m_withEndPostion);
        return Value(Value::EmptyValue);
    }
}

NEVER_INLINE void ByteCodeInterpreter::replaceBlockLexicalEnvironmentOperation(ExecutionState& state, size_t programCounter, ByteCodeBlock* byteCodeBlock)
{
    ReplaceBlockLexicalEnvironmentOperation* code = (ReplaceBlockLexicalEnvironmentOperation*)programCounter;
    // setup new env
    EnvironmentRecord* newRecord;
    LexicalEnvironment* newEnv;

    bool shouldUseIndexedStorage = byteCodeBlock->m_codeBlock->canUseIndexedVariableStorage();
    ASSERT(code->m_blockInfo->m_shouldAllocateEnvironment);
    if (LIKELY(shouldUseIndexedStorage)) {
        newRecord = new DeclarativeEnvironmentRecordIndexed(state, code->m_blockInfo);
    } else {
        newRecord = new DeclarativeEnvironmentRecordNotIndexed(state);

        auto& iv = code->m_blockInfo->m_identifiers;
        auto siz = iv.size();
        for (size_t i = 0; i < siz; i++) {
            newRecord->createBinding(state, iv[i].m_name, false, iv[i].m_isMutable, false);
        }
    }
    newEnv = new LexicalEnvironment(newRecord, state.lexicalEnvironment()->outerEnvironment());
    ASSERT(newEnv->isAllocatedOnHeap());

    state.setLexicalEnvironment(newEnv, state.inStrictMode());
}

NEVER_INLINE Value ByteCodeInterpreter::blockOperation(ExecutionState*& state, BlockOperation* code, size_t& programCounter, ByteCodeBlock* byteCodeBlock, Value* registerFile)
{
    if (!state->ensureRareData()->m_controlFlowRecord) {
        state->ensureRareData()->m_controlFlowRecord = new ControlFlowRecordVector();
    }
    state->ensureRareData()->m_controlFlowRecord->pushBack(nullptr);
    size_t newPc = programCounter + sizeof(BlockOperation);
    char* codeBuffer = byteCodeBlock->m_code.data();

    // setup new env
    EnvironmentRecord* newRecord;
    LexicalEnvironment* newEnv;
    bool inPauserResumeProcess = code->m_blockInfo == nullptr;
    bool shouldUseIndexedStorage = byteCodeBlock->m_codeBlock->canUseIndexedVariableStorage();
    bool inPauserScope = state->inPauserScope();

    if (LIKELY(!inPauserResumeProcess)) {
        ASSERT(code->m_blockInfo->m_shouldAllocateEnvironment);
        if (LIKELY(shouldUseIndexedStorage)) {
            newRecord = new DeclarativeEnvironmentRecordIndexed(*state, code->m_blockInfo);
        } else {
            newRecord = new DeclarativeEnvironmentRecordNotIndexed(*state);

            auto& iv = code->m_blockInfo->m_identifiers;
            auto siz = iv.size();
            for (size_t i = 0; i < siz; i++) {
                newRecord->createBinding(*state, iv[i].m_name, false, iv[i].m_isMutable, false);
            }
        }
        newEnv = new LexicalEnvironment(newRecord, state->lexicalEnvironment());
        ASSERT(newEnv->isAllocatedOnHeap());
    } else {
        newRecord = nullptr;
        newEnv = nullptr;
    }

    bool shouldUseHeapAllocatedState = inPauserScope && !inPauserResumeProcess;
    ExecutionState* newState;
    if (UNLIKELY(shouldUseHeapAllocatedState)) {
        newState = new ExecutionState(state, newEnv, state->inStrictMode());
        newState->ensureRareData();
    } else {
        newState = new (alloca(sizeof(ExecutionState))) ExecutionState(state, newEnv, state->inStrictMode());
    }

    if (!LIKELY(inPauserResumeProcess)) {
        newState->ensureRareData()->m_controlFlowRecord = state->rareData()->m_controlFlowRecord;
    }

    interpret(newState, byteCodeBlock, resolveProgramCounter(codeBuffer, newPc), registerFile);

    if (UNLIKELY(inPauserResumeProcess)) {
        state = newState->parent();
        code = (BlockOperation*)(byteCodeBlock->m_code.data() + newState->rareData()->m_programCounterWhenItStoppedByYield);
    }

    ControlFlowRecord* record = state->rareData()->m_controlFlowRecord->back();
    state->rareData()->m_controlFlowRecord->erase(state->rareData()->m_controlFlowRecord->size() - 1);

    if (record != nullptr) {
        if (record->reason() == ControlFlowRecord::NeedsJump) {
            size_t pos = record->wordValue();
            record->m_count--;
            if (record->count() && (record->outerLimitCount() < record->count())) {
                state->rareData()->m_controlFlowRecord->back() = record;
                return Value();
            } else {
                programCounter = jumpTo(codeBuffer, pos);
                return Value(Value::EmptyValue);
            }
        } else {
            ASSERT(record->reason() == ControlFlowRecord::NeedsReturn);
            record->m_count--;
            if (record->count()) {
                state->rareData()->m_controlFlowRecord->back() = record;
            }
            return record->value();
        }
    } else {
        programCounter = jumpTo(codeBuffer, code->m_blockEndPosition);
        return Value(Value::EmptyValue);
    }
}

NEVER_INLINE bool ByteCodeInterpreter::binaryInOperation(ExecutionState& state, const Value& left, const Value& right)
{
    if (!right.isObject()) {
        ErrorObject::throwBuiltinError(state, ErrorObject::TypeError, "type of rvalue is not Object");
        return false;
    }

    // https://www.ecma-international.org/ecma-262/5.1/#sec-11.8.7
    // Return the result of calling the [[HasProperty]] internal method of rval with argument ToString(lval).
    return right.toObject(state)->hasProperty(state, ObjectPropertyName(state, left));
}

NEVER_INLINE void ByteCodeInterpreter::callFunctionComplexCase(ExecutionState& state, CallFunctionComplexCase* code, Value* registerFile, ByteCodeBlock* byteCodeBlock)
{
    switch (code->m_kind) {
    case CallFunctionComplexCase::InWithScope: {
        const AtomicString& calleeName = code->m_calleeName;
        // NOTE: record for with scope
        Object* receiverObj = NULL;
        Value callee;
        EnvironmentRecord* bindedRecord = getBindedEnvironmentRecordByName(state, state.lexicalEnvironment(), calleeName, callee);
        if (!bindedRecord) {
            callee = Value();
        }

        if (bindedRecord && bindedRecord->isObjectEnvironmentRecord()) {
            receiverObj = bindedRecord->asObjectEnvironmentRecord()->bindingObject();
        } else {
            receiverObj = state.context()->globalObject();
        }

        if (code->m_hasSpreadElement) {
            ValueVector spreadArgs;
            spreadFunctionArguments(state, &registerFile[code->m_argumentsStartIndex], code->m_argumentCount, spreadArgs);
            registerFile[code->m_resultIndex] = Object::call(state, callee, receiverObj, spreadArgs.size(), spreadArgs.data());
        } else {
            registerFile[code->m_resultIndex] = Object::call(state, callee, receiverObj, code->m_argumentCount, &registerFile[code->m_argumentsStartIndex]);
        }
        break;
    }
    case CallFunctionComplexCase::MayBuiltinApply: {
        const Value& callee = registerFile[code->m_calleeIndex];
        const Value& receiver = registerFile[code->m_receiverIndex];
        if (UNLIKELY(!callee.isPointerValue() || !receiver.isPointerValue())) {
            ErrorObject::throwBuiltinError(state, ErrorObject::TypeError, errorMessage_NOT_Callable);
        }

        auto functionRecord = state.mostNearestFunctionLexicalEnvironment()->record()->asDeclarativeEnvironmentRecord()->asFunctionEnvironmentRecord();

        if (callee.asPointerValue() == state.context()->globalObject()->functionApply()) {
            if (!functionRecord->argumentsObject()) {
                Value* v = ALLOCA(sizeof(Value) * state.argc(), Value, state);
                memcpy(v, state.argv(), sizeof(Value) * state.argc());
                registerFile[code->m_resultIndex] = receiver.asPointerValue()->call(state, registerFile[code->m_argumentsStartIndex], state.argc(), v);
                return;
            }
        }

        ensureArgumentsObjectOperation(state, byteCodeBlock, registerFile);

        const auto& idInfo = byteCodeBlock->m_codeBlock->identifierInfos();
        AtomicString argumentsAtomicString = state.context()->staticStrings().arguments;
        Value argumentsValue;
        for (size_t i = 0; i < idInfo.size(); i++) {
            if (idInfo[i].m_name == argumentsAtomicString) {
                if (idInfo[i].m_needToAllocateOnStack) {
                    argumentsValue = registerFile[idInfo[i].m_indexForIndexedStorage + byteCodeBlock->m_requiredRegisterFileSizeInValueSize];
                } else {
                    argumentsValue = functionRecord->getBindingValue(state, argumentsAtomicString).m_value;
                }
            }
        }
        Value argv[2] = { registerFile[code->m_argumentsStartIndex], argumentsValue };
        registerFile[code->m_resultIndex] = callee.asPointerValue()->call(state, registerFile[code->m_receiverIndex], 2, argv);
        break;
    }
    case CallFunctionComplexCase::MayBuiltinEval: {
        size_t argc;
        Value* argv;
        ValueVector spreadArgs;

        if (code->m_hasSpreadElement) {
            spreadFunctionArguments(state, &registerFile[code->m_argumentsStartIndex], code->m_argumentCount, spreadArgs);
            argv = spreadArgs.data();
            argc = spreadArgs.size();
        } else {
            argc = code->m_argumentCount;
            argv = &registerFile[code->m_argumentsStartIndex];
        }

        Value eval = registerFile[code->m_calleeIndex];
        if (eval.equalsTo(state, state.context()->globalObject()->eval())) {
            // do eval
            Value arg;
            if (argc) {
                arg = argv[0];
            }
            Value* stackStorage = registerFile + byteCodeBlock->m_requiredRegisterFileSizeInValueSize;
            registerFile[code->m_resultIndex] = state.context()->globalObject()->evalLocal(state, arg, stackStorage[0], byteCodeBlock->m_codeBlock->asInterpretedCodeBlock(), code->m_inWithScope);
        } else {
            Value thisValue;
            if (code->m_inWithScope) {
                LexicalEnvironment* env = state.lexicalEnvironment();
                while (env) {
                    EnvironmentRecord::GetBindingValueResult result = env->record()->getBindingValue(state, state.context()->staticStrings().eval);
                    if (result.m_hasBindingValue) {
                        break;
                    }
                    env = env->outerEnvironment();
                }
                if (env->record()->isObjectEnvironmentRecord()) {
                    thisValue = env->record()->asObjectEnvironmentRecord()->bindingObject();
                }
            }
            registerFile[code->m_resultIndex] = Object::call(state, eval, thisValue, argc, argv);
        }
        break;
    }
    case CallFunctionComplexCase::WithSpreadElement: {
        const Value& callee = registerFile[code->m_calleeIndex];
        const Value& receiver = code->m_receiverIndex == REGISTER_LIMIT ? Value() : registerFile[code->m_receiverIndex];

        // if PointerValue is not callable, PointerValue::call function throws builtin error
        // https://www.ecma-international.org/ecma-262/6.0/#sec-call
        // If IsCallable(F) is false, throw a TypeError exception.
        if (UNLIKELY(!callee.isPointerValue())) {
            ErrorObject::throwBuiltinError(state, ErrorObject::TypeError, errorMessage_NOT_Callable);
        }

        {
            ValueVector spreadArgs;
            spreadFunctionArguments(state, &registerFile[code->m_argumentsStartIndex], code->m_argumentCount, spreadArgs);
            // Return F.[[Call]](V, argumentsList).
            registerFile[code->m_resultIndex] = callee.asPointerValue()->call(state, receiver, spreadArgs.size(), spreadArgs.data());
        }
        break;
    }
    case CallFunctionComplexCase::Super: {
        // Let newTarget be GetNewTarget().
        Object* newTarget = state.getNewTarget();
        // If newTarget is undefined, throw a ReferenceError exception. <-- we checked this at superOperation

        // Let func be GetSuperConstructor(). <-- superOperation did this to calleeIndex

        // Let argList be ArgumentListEvaluation of Arguments.
        // ReturnIfAbrupt(argList).
        size_t argc;
        Value* argv;
        ValueVector spreadArgs;

        if (code->m_hasSpreadElement) {
            spreadFunctionArguments(state, &registerFile[code->m_argumentsStartIndex], code->m_argumentCount, spreadArgs);
            argv = spreadArgs.data();
            argc = spreadArgs.size();
        } else {
            argc = code->m_argumentCount;
            argv = &registerFile[code->m_argumentsStartIndex];
        }

        // Let result be Construct(func, argList, newTarget).
        Value result = Object::construct(state, registerFile[code->m_calleeIndex], argc, argv, newTarget);

        // Let thisER be GetThisEnvironment( ).
        // Return thisER.BindThisValue(result).
        EnvironmentRecord* thisER = state.getThisEnvironment();
        thisER->asDeclarativeEnvironmentRecord()->asFunctionEnvironmentRecord()->bindThisValue(state, result);
        registerFile[code->m_resultIndex] = result;
        break;
    }
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

NEVER_INLINE void ByteCodeInterpreter::spreadFunctionArguments(ExecutionState& state, const Value* argv, const size_t argc, ValueVector& argVector)
{
    for (size_t i = 0; i < argc; i++) {
        Value arg = argv[i];
        if (arg.isObject() && arg.asObject()->isSpreadArray()) {
            ArrayObject* spreadArray = arg.asObject()->asArrayObject();
            ASSERT(spreadArray->isFastModeArray());
            for (size_t i = 0; i < spreadArray->getArrayLength(state); i++) {
                argVector.push_back(spreadArray->m_fastModeData[i]);
            }
        } else {
            argVector.push_back(arg);
        }
    }
}

NEVER_INLINE EnumerateObject* ByteCodeInterpreter::createEnumerateObject(ExecutionState& state, Object* obj, bool isDestruction)
{
    EnumerateObject* enumObj;
    if (isDestruction) {
        enumObj = new EnumerateObjectWithDestruction(state, obj);
    } else {
        enumObj = new EnumerateObjectWithIteration(state, obj);
    }

    return enumObj;
}

NEVER_INLINE void ByteCodeInterpreter::checkLastEnumerateKey(ExecutionState& state, CheckLastEnumerateKey* code, char* codeBuffer, size_t& programCounter, Value* registerFile)
{
    EnumerateObject* data = (EnumerateObject*)registerFile[code->m_registerIndex].asPointerValue();
    if (data->checkLastEnumerateKey(state)) {
        programCounter = jumpTo(codeBuffer, code->m_exitPosition);
    } else {
        ADD_PROGRAM_COUNTER(CheckLastEnumerateKey);
    }
}

NEVER_INLINE void ByteCodeInterpreter::markEnumerateKey(ExecutionState& state, MarkEnumerateKey* code, Value* registerFile)
{
    EnumerateObject* data = (EnumerateObject*)registerFile[code->m_dataRegisterIndex].asPointerValue();
    Value key = registerFile[code->m_keyRegisterIndex];
    bool mark = false;
    for (size_t i = 0; i < data->m_keys.size(); i++) {
        Value cachedKey = data->m_keys[i];
        if (!cachedKey.isEmpty()) {
            if (key.isString() && cachedKey.isString()) {
                mark = key.asString()->equals(cachedKey.asString());
            } else if (key.isSymbol() && cachedKey.isSymbol()) {
                mark = (key.asSymbol() == cachedKey.asSymbol());
            }
        }
        if (mark) {
            data->m_keys[i] = Value(Value::EmptyValue);
            break;
        }
    }
}

NEVER_INLINE Value ByteCodeInterpreter::executionPauseOperation(ExecutionState& state, Value* registerFile, size_t& programCounter, char* codeBuffer)
{
    ExecutionPause* code = (ExecutionPause*)programCounter;
    if (code->m_reason == ExecutionPause::Yield) {
        // http://www.ecma-international.org/ecma-262/6.0/#sec-generator-function-definitions-runtime-semantics-evaluation
        auto yieldIndex = code->m_yieldData.m_yieldIndex;
        Value ret = yieldIndex == REGISTER_LIMIT ? Value() : registerFile[yieldIndex];
        auto dstIdx = code->m_yieldData.m_dstIndex;

        size_t nextProgramCounter = programCounter - (size_t)codeBuffer + sizeof(ExecutionPause) + code->m_yieldData.m_tailDataLength;

        ExecutionPauser::pause(state, ret, programCounter + sizeof(ExecutionPause), code->m_yieldData.m_tailDataLength, nextProgramCounter, dstIdx, ExecutionPauser::PauseReason::Yield);
    } else if (code->m_reason == ExecutionPause::YieldDelegate) {
        // http://www.ecma-international.org/ecma-262/6.0/#sec-generator-function-definitions-runtime-semantics-evaluation
        const Value iteratorRecord = registerFile[code->m_yieldDelegateData.m_iterIntex];
        Object* iterator = iteratorRecord.asObject()->getOwnProperty(state, state.context()->staticStrings().iterator).value(state, iteratorRecord).asObject();
        GeneratorObject::GeneratorState resultState = GeneratorObject::GeneratorState::SuspendedYield;
        Value nextResult;
        bool done = true;
        Value nextValue;
        try {
            nextResult = IteratorObject::iteratorNext(state, iteratorRecord);
            if (iterator->isGeneratorObject()) {
                resultState = iterator->asGeneratorObject()->m_generatorState;
            }
        } catch (const Value& v) {
            resultState = GeneratorObject::GeneratorState::CompletedThrow;
            nextValue = v;
        }

        switch (resultState) {
        case GeneratorObject::GeneratorState::CompletedReturn: {
            Value ret = iterator->get(state, ObjectPropertyName(state.context()->staticStrings().stringReturn)).value(state, iterator);

            nextValue = IteratorObject::iteratorValue(state, nextResult);

            if (ret.isUndefined()) {
                return nextValue;
            }

            Value innerResult = Object::call(state, ret, iterator, 1, &nextValue);

            if (!innerResult.isObject()) {
                ErrorObject::throwBuiltinError(state, ErrorObject::TypeError, "IteratorResult is not an object");
            }

            nextValue = IteratorObject::iteratorValue(state, innerResult);
            done = IteratorObject::iteratorComplete(state, innerResult);
            break;
        }
        case GeneratorObject::GeneratorState::SuspendedYield: {
            done = IteratorObject::iteratorComplete(state, nextResult);
            nextValue = IteratorObject::iteratorValue(state, nextResult);
            break;
        }
        default: {
            ASSERT(resultState == GeneratorObject::GeneratorState::CompletedThrow);
            Value throwMethod = iterator->get(state, ObjectPropertyName(state.context()->staticStrings().stringThrow)).value(state, iterator);

            if (!throwMethod.isUndefined()) {
                Value innerResult;
                innerResult = Object::call(state, throwMethod, iterator, 1, &nextValue);
                if (!innerResult.isObject()) {
                    ErrorObject::throwBuiltinError(state, ErrorObject::TypeError, "IteratorResult is not an object");
                }
                nextValue = IteratorObject::iteratorValue(state, innerResult);
                done = IteratorObject::iteratorComplete(state, innerResult);
            } else {
                IteratorObject::iteratorClose(state, iteratorRecord, Value(), false);
                ErrorObject::throwBuiltinError(state, ErrorObject::TypeError, "yield* violation");
            }
        }
        }

        // yield
        registerFile[code->m_yieldDelegateData.m_dstIndex] = nextValue;
        if (done) {
            programCounter = jumpTo(codeBuffer, code->m_yieldDelegateData.m_endPosition);
            return Value(Value::EmptyValue);
        }

        registerFile[code->m_yieldDelegateData.m_valueIndex] = nextValue;

        size_t nextProgramCounter = programCounter - (size_t)codeBuffer + sizeof(ExecutionPause) + code->m_yieldDelegateData.m_tailDataLength;
        ExecutionPauser::pause(state, nextResult, programCounter + sizeof(ExecutionPause), code->m_yieldDelegateData.m_tailDataLength, nextProgramCounter, REGISTER_LIMIT, ExecutionPauser::PauseReason::YieldDelegate);
    } else if (code->m_reason == ExecutionPause::Await) {
        ExecutionState* p = &state;
        ExecutionPauser* executionPauser;
        while (true) {
            executionPauser = p->pauseSource();
            if (executionPauser) {
                break;
            }
            p = p->parent();
        }

        const Value& awaitValue = registerFile[code->m_awaitData.m_awaitIndex];
        size_t nextProgramCounter = programCounter - (size_t)codeBuffer + sizeof(ExecutionPause) + code->m_awaitData.m_tailDataLength;
        size_t tailDataPosition = programCounter + sizeof(ExecutionPause);
        size_t tailDataLength = code->m_awaitData.m_tailDataLength;
        ByteCodeRegisterIndex dstRegisterIndex = code->m_awaitData.m_dstIndex;

        await(state, executionPauser, awaitValue, executionPauser->sourceObject());
        ExecutionPauser::pause(state, awaitValue, tailDataPosition, tailDataLength, nextProgramCounter, dstRegisterIndex, ExecutionPauser::PauseReason::Await);
    }

    ASSERT_NOT_REACHED();
    // never get here. but I add return statement for removing compile warning
    return Value(Value::EmptyValue);
}

NEVER_INLINE Value ByteCodeInterpreter::executionResumeOperation(ExecutionState*& state, size_t& programCounter, ByteCodeBlock* byteCodeBlock)
{
    ExecutionResume* code = (ExecutionResume*)programCounter;

    bool needsReturn = code->m_needsReturn;
    bool needsThrow = code->m_needsThrow;

    // update old parent
    auto data = code->m_pauser;
    ExecutionState* orgTreePointer = data->m_executionState;
    ExecutionState* tmpTreePointer = state;

    size_t pos = programCounter + sizeof(ExecutionResume);
    size_t recursiveStatementCodeStackSize = *((size_t*)pos);
    pos += sizeof(size_t) + sizeof(size_t) * recursiveStatementCodeStackSize;

    for (size_t i = 0; i < recursiveStatementCodeStackSize; i++) {
        pos -= sizeof(size_t);
        size_t codePos = *((size_t*)pos);

#ifndef NDEBUG
        if (orgTreePointer->hasRareData() && orgTreePointer->pauseSource()) {
            // this ExecutuionState must be allocated by generatorResume function;
            ASSERT(tmpTreePointer->lexicalEnvironment()->record()->asDeclarativeEnvironmentRecord()->asFunctionEnvironmentRecord()->functionObject()->codeBlock()->isGenerator());
        }
#endif
        auto tmpTreePointerSave = tmpTreePointer;
        auto orgTreePointerSave = orgTreePointer;
        orgTreePointer = orgTreePointer->parent();
        tmpTreePointer = tmpTreePointer->parent();

        tmpTreePointerSave->setParent(orgTreePointerSave->parent());
        tmpTreePointerSave->ensureRareData()->m_programCounterWhenItStoppedByYield = codePos;
    }

    state = data->m_executionState;

    // remove extra code
    byteCodeBlock->m_code.resize(data->m_extraDataByteCodePosition);

    // update program counter
    programCounter = data->m_byteCodePosition + (size_t)byteCodeBlock->m_code.data();

    if (needsReturn) {
        if (state->rareData() && state->rareData()->m_controlFlowRecord && state->rareData()->m_controlFlowRecord->size()) {
            state->rareData()->m_controlFlowRecord->back() = new ControlFlowRecord(ControlFlowRecord::NeedsReturn, data->m_resumeValue, state->rareData()->m_controlFlowRecord->size());
        }
        return data->m_resumeValue;
    } else if (needsThrow) {
        state->throwException(data->m_resumeValue);
    }

    return Value(Value::EmptyValue);
}

NEVER_INLINE void ByteCodeInterpreter::newTargetOperation(ExecutionState& state, NewTargetOperation* code, Value* registerFile)
{
    auto newTarget = state.getNewTarget();
    if (newTarget) {
        registerFile[code->m_registerIndex] = state.getNewTarget();
    } else {
        registerFile[code->m_registerIndex] = Value();
    }
}

static Value createObjectPropertyFunctionName(ExecutionState& state, const Value& name, const char* prefix)
{
    StringBuilder builder;
    if (name.isSymbol()) {
        builder.appendString(prefix);
        builder.appendString("[");
        builder.appendString(name.asSymbol()->description());
        builder.appendString("]");
    } else {
        builder.appendString(prefix);
        builder.appendString(name.toString(state));
    }
    return builder.finalize(&state);
}

NEVER_INLINE void ByteCodeInterpreter::objectDefineOwnPropertyOperation(ExecutionState& state, ObjectDefineOwnPropertyOperation* code, Value* registerFile)
{
    const Value& willBeObject = registerFile[code->m_objectRegisterIndex];
    const Value& property = registerFile[code->m_propertyRegisterIndex];
    const Value& value = registerFile[code->m_loadRegisterIndex];

    Value propertyStringOrSymbol = property.isSymbol() ? property : property.toString(state);

    if (code->m_needsToRedefineFunctionNameOnValue) {
        Value fnName = createObjectPropertyFunctionName(state, propertyStringOrSymbol, "");
        value.asFunction()->defineOwnProperty(state, state.context()->staticStrings().name, ObjectPropertyDescriptor(fnName));
    }

    ObjectPropertyName objPropName = ObjectPropertyName(state, propertyStringOrSymbol);
    // http://www.ecma-international.org/ecma-262/6.0/#sec-__proto__-property-names-in-object-initializers
    if (!willBeObject.asObject()->isScriptClassConstructorPrototypeObject() && (propertyStringOrSymbol.isString() && propertyStringOrSymbol.asString()->equals("__proto__"))) {
        willBeObject.asObject()->setPrototype(state, value);
    } else {
        willBeObject.asObject()->defineOwnProperty(state, objPropName, ObjectPropertyDescriptor(value, code->m_presentAttribute));
    }
}

NEVER_INLINE void ByteCodeInterpreter::objectDefineOwnPropertyWithNameOperation(ExecutionState& state, ObjectDefineOwnPropertyWithNameOperation* code, Value* registerFile)
{
    const Value& willBeObject = registerFile[code->m_objectRegisterIndex];
    // http://www.ecma-international.org/ecma-262/6.0/#sec-__proto__-property-names-in-object-initializers
    if (!willBeObject.asObject()->isScriptClassConstructorPrototypeObject() && (code->m_propertyName == state.context()->staticStrings().__proto__)) {
        willBeObject.asObject()->setPrototype(state, registerFile[code->m_loadRegisterIndex]);
    } else {
        willBeObject.asObject()->defineOwnProperty(state, ObjectPropertyName(code->m_propertyName), ObjectPropertyDescriptor(registerFile[code->m_loadRegisterIndex], code->m_presentAttribute));
    }
}

NEVER_INLINE void ByteCodeInterpreter::arrayDefineOwnPropertyOperation(ExecutionState& state, ArrayDefineOwnPropertyOperation* code, Value* registerFile)
{
    ArrayObject* arr = registerFile[code->m_objectRegisterIndex].asObject()->asArrayObject();
    if (LIKELY(arr->isFastModeArray())) {
        for (size_t i = 0; i < code->m_count; i++) {
            if (LIKELY(code->m_loadRegisterIndexs[i] != REGISTER_LIMIT)) {
                arr->m_fastModeData[i + code->m_baseIndex] = registerFile[code->m_loadRegisterIndexs[i]];
            }
        }
    } else {
        for (size_t i = 0; i < code->m_count; i++) {
            if (LIKELY(code->m_loadRegisterIndexs[i] != REGISTER_LIMIT)) {
                const Value& element = registerFile[code->m_loadRegisterIndexs[i]];
                arr->defineOwnProperty(state, ObjectPropertyName(state, i + code->m_baseIndex), ObjectPropertyDescriptor(element, ObjectPropertyDescriptor::AllPresent));
            }
        }
    }
}

NEVER_INLINE void ByteCodeInterpreter::arrayDefineOwnPropertyBySpreadElementOperation(ExecutionState& state, ArrayDefineOwnPropertyBySpreadElementOperation* code, Value* registerFile)
{
    ArrayObject* arr = registerFile[code->m_objectRegisterIndex].asObject()->asArrayObject();

    if (LIKELY(arr->isFastModeArray())) {
        size_t baseIndex = arr->getArrayLength(state);
        size_t elementLength = code->m_count;
        for (size_t i = 0; i < code->m_count; i++) {
            if (code->m_loadRegisterIndexs[i] != REGISTER_LIMIT) {
                Value element = registerFile[code->m_loadRegisterIndexs[i]];
                if (element.isObject() && element.asObject()->isSpreadArray()) {
                    elementLength = elementLength + element.asObject()->asArrayObject()->getArrayLength(state) - 1;
                }
            }
        }

        size_t newLength = baseIndex + elementLength;
        arr->setArrayLength(state, newLength);
        ASSERT(arr->isFastModeArray());

        size_t elementIndex = 0;
        for (size_t i = 0; i < code->m_count; i++) {
            if (LIKELY(code->m_loadRegisterIndexs[i] != REGISTER_LIMIT)) {
                Value element = registerFile[code->m_loadRegisterIndexs[i]];
                if (element.isObject() && element.asObject()->isSpreadArray()) {
                    ArrayObject* spreadArray = element.asObject()->asArrayObject();
                    ASSERT(spreadArray->isFastModeArray());
                    for (size_t spreadIndex = 0; spreadIndex < spreadArray->getArrayLength(state); spreadIndex++) {
                        arr->m_fastModeData[baseIndex + elementIndex] = spreadArray->m_fastModeData[spreadIndex];
                        elementIndex++;
                    }
                } else {
                    arr->m_fastModeData[baseIndex + elementIndex] = element;
                    elementIndex++;
                }
            } else {
                elementIndex++;
            }
        }

        ASSERT(elementIndex == elementLength);

    } else {
        ByteCodeRegisterIndex objectRegisterIndex = code->m_objectRegisterIndex;
        size_t count = code->m_count;
        ByteCodeRegisterIndex* loadRegisterIndexs = code->m_loadRegisterIndexs;
        ArrayObject* arr = registerFile[objectRegisterIndex].asObject()->asArrayObject();
        ASSERT(!arr->isFastModeArray());

        size_t baseIndex = arr->getArrayLength(state);
        size_t elementIndex = 0;

        for (size_t i = 0; i < count; i++) {
            if (LIKELY(loadRegisterIndexs[i] != REGISTER_LIMIT)) {
                Value element = registerFile[loadRegisterIndexs[i]];
                if (element.isObject() && element.asObject()->isSpreadArray()) {
                    ArrayObject* spreadArray = element.asObject()->asArrayObject();
                    ASSERT(spreadArray->isFastModeArray());
                    Value spreadElement;
                    for (size_t spreadIndex = 0; spreadIndex < spreadArray->getArrayLength(state); spreadIndex++) {
                        spreadElement = spreadArray->m_fastModeData[spreadIndex];
                        arr->defineOwnProperty(state, ObjectPropertyName(state, baseIndex + elementIndex), ObjectPropertyDescriptor(spreadElement, ObjectPropertyDescriptor::AllPresent));
                        elementIndex++;
                    }
                } else {
                    arr->defineOwnProperty(state, ObjectPropertyName(state, baseIndex + elementIndex), ObjectPropertyDescriptor(element, ObjectPropertyDescriptor::AllPresent));
                    elementIndex++;
                }
            } else {
                elementIndex++;
            }
        }
    }
}

NEVER_INLINE void ByteCodeInterpreter::createSpreadArrayObject(ExecutionState& state, CreateSpreadArrayObject* code, Value* registerFile)
{
    ArrayObject* spreadArray = ArrayObject::createSpreadArray(state);
    ASSERT(spreadArray->isFastModeArray());

    Value iteratorRecord = IteratorObject::getIterator(state, registerFile[code->m_argumentIndex]);
    size_t i = 0;
    while (true) {
        Value next = IteratorObject::iteratorStep(state, iteratorRecord);
        if (next.isFalse()) {
            break;
        }
        Value value = IteratorObject::iteratorValue(state, next);
        spreadArray->setIndexedProperty(state, Value(i++), value);
    }
    registerFile[code->m_registerIndex] = spreadArray;
}

NEVER_INLINE void ByteCodeInterpreter::defineObjectGetterSetter(ExecutionState& state, ObjectDefineGetterSetter* code, Value* registerFile)
{
    FunctionObject* fn = registerFile[code->m_objectPropertyValueRegisterIndex].asFunction();
    Value pName = registerFile[code->m_objectPropertyNameRegisterIndex];
    Value fnName;
    if (code->m_isGetter) {
        fnName = createObjectPropertyFunctionName(state, pName, "get ");
    } else {
        fnName = createObjectPropertyFunctionName(state, pName, "set ");
    }
    fn->defineOwnProperty(state, state.context()->staticStrings().name, ObjectPropertyDescriptor(fnName));
    JSGetterSetter* gs;
    if (code->m_isGetter) {
        gs = new (alloca(sizeof(JSGetterSetter))) JSGetterSetter(registerFile[code->m_objectPropertyValueRegisterIndex].asFunction(), Value(Value::EmptyValue));
    } else {
        gs = new (alloca(sizeof(JSGetterSetter))) JSGetterSetter(Value(Value::EmptyValue), registerFile[code->m_objectPropertyValueRegisterIndex].asFunction());
    }
    ObjectPropertyDescriptor desc(*gs, code->m_presentAttribute);
    Object* object = registerFile[code->m_objectRegisterIndex].toObject(state);
    object->defineOwnPropertyThrowsExceptionWhenStrictMode(state, ObjectPropertyName(state, pName), desc);
}

ALWAYS_INLINE Value ByteCodeInterpreter::incrementOperation(ExecutionState& state, const Value& value)
{
    if (LIKELY(value.isInt32())) {
        int32_t a = value.asInt32();
        int32_t b = 1;
        int32_t c;
        bool result = ArithmeticOperations<int32_t, int32_t, int32_t>::add(a, b, c);
        if (LIKELY(result)) {
            return Value(c);
        } else {
            return Value(Value::EncodeAsDouble, (double)a + (double)b);
        }
    } else {
        return plusSlowCase(state, Value(value.toNumber(state)), Value(1));
    }
}

ALWAYS_INLINE Value ByteCodeInterpreter::decrementOperation(ExecutionState& state, const Value& value)
{
    if (LIKELY(value.isInt32())) {
        int32_t a = value.asInt32();
        int32_t b = -1;
        int32_t c;
        bool result = ArithmeticOperations<int32_t, int32_t, int32_t>::add(a, b, c);
        if (LIKELY(result)) {
            return Value(c);
        } else {
            return Value(Value::EncodeAsDouble, (double)a + (double)b);
        }
    } else {
        return Value(value.toNumber(state) - 1);
    }
}

NEVER_INLINE void ByteCodeInterpreter::unaryTypeof(ExecutionState& state, UnaryTypeof* code, Value* registerFile)
{
    Value val;
    if (code->m_id.string()->length()) {
        val = loadByName(state, state.lexicalEnvironment(), code->m_id, false);
    } else {
        val = registerFile[code->m_srcIndex];
    }
    if (val.isUndefined()) {
        val = state.context()->staticStrings().undefined.string();
    } else if (val.isNull()) {
        val = state.context()->staticStrings().object.string();
    } else if (val.isBoolean()) {
        val = state.context()->staticStrings().boolean.string();
    } else if (val.isNumber()) {
        val = state.context()->staticStrings().number.string();
    } else if (val.isString()) {
        val = state.context()->staticStrings().string.string();
    } else {
        ASSERT(val.isPointerValue());
        PointerValue* p = val.asPointerValue();
        if (p->isCallable()) {
            val = state.context()->staticStrings().function.string();
        } else if (p->isSymbol()) {
            val = state.context()->staticStrings().symbol.string();
        } else {
            val = state.context()->staticStrings().object.string();
        }
    }

    registerFile[code->m_dstIndex] = val;
}

NEVER_INLINE void ByteCodeInterpreter::getIteratorOperation(ExecutionState& state, GetIterator* code, Value* registerFile)
{
    const Value& obj = registerFile[code->m_objectRegisterIndex];
    registerFile[code->m_registerIndex] = IteratorObject::getIterator(state, obj);
}

NEVER_INLINE void ByteCodeInterpreter::iteratorStepOperation(ExecutionState& state, size_t& programCounter, Value* registerFile, char* codeBuffer)
{
    IteratorStep* code = (IteratorStep*)programCounter;
    Value nextResult = IteratorObject::iteratorStep(state, registerFile[code->m_iterRegisterIndex]);

    if (nextResult.isFalse()) {
        programCounter = jumpTo(codeBuffer, code->m_forOfEndPosition);
    } else {
        registerFile[code->m_registerIndex] = IteratorObject::iteratorValue(state, nextResult);
        ADD_PROGRAM_COUNTER(IteratorStep);
    }
}

NEVER_INLINE void ByteCodeInterpreter::iteratorCloseOperation(ExecutionState& state, IteratorClose* code, Value* registerFile)
{
    bool exceptionWasThrown = state.hasRareData() && state.rareData()->m_controlFlowRecord && state.rareData()->m_controlFlowRecord->back() && state.rareData()->m_controlFlowRecord->back()->reason() == ControlFlowRecord::NeedsThrow;
    const Value& iteratorRecord = registerFile[code->m_iterRegisterIndex];
    Object* iterator = iteratorRecord.asObject()->getOwnProperty(state, state.context()->staticStrings().iterator).value(state, iteratorRecord).asObject();
    Value returnFunction = iterator->get(state, ObjectPropertyName(state.context()->staticStrings().stringReturn)).value(state, iterator);
    if (returnFunction.isUndefined()) {
        return;
    }

    Value innerResult;
    bool innerResultHasException = false;
    try {
        innerResult = Object::call(state, returnFunction, iterator, 0, nullptr);
    } catch (const Value& e) {
        innerResult = e;
        innerResultHasException = true;
    }

    if (exceptionWasThrown) {
        return;
    }

    if (innerResultHasException) {
        state.throwException(innerResult);
    }

    if (!innerResult.isObject()) {
        ErrorObject::throwBuiltinError(state, ErrorObject::TypeError, "Iterator close result is not an object");
    }
}

NEVER_INLINE void ByteCodeInterpreter::iteratorBindOperation(ExecutionState& state, size_t& programCounter, Value* registerFile)
{
    auto strings = &state.context()->staticStrings();

    IteratorBind* code = (IteratorBind*)programCounter;
    Value nextResult, value;
    Value iteratorRecord = registerFile[code->m_iterRegisterIndex];

    if (iteratorRecord.asObject()->getOwnProperty(state, strings->done).value(state, iteratorRecord).isFalse()) {
        try {
            nextResult = IteratorObject::iteratorStep(state, iteratorRecord);
        } catch (const Value& e) {
            Value exceptionValue = e;
            iteratorRecord.asObject()->setThrowsException(state, ObjectPropertyName(strings->done), Value(true), iteratorRecord);
            state.throwException(exceptionValue);
        }

        if (nextResult.isFalse()) {
            iteratorRecord.asObject()->setThrowsException(state, ObjectPropertyName(strings->done), Value(true), iteratorRecord);
        } else {
            try {
                value = IteratorObject::iteratorValue(state, nextResult);
            } catch (const Value& e) {
                Value exceptionValue = e;
                iteratorRecord.asObject()->setThrowsException(state, ObjectPropertyName(strings->done), Value(true), iteratorRecord);
                state.throwException(exceptionValue);
            }
        }
    }

    registerFile[code->m_registerIndex] = value;
    ADD_PROGRAM_COUNTER(IteratorBind);
}

NEVER_INLINE Object* ByteCodeInterpreter::restBindOperation(ExecutionState& state, Value& iteratorRecord)
{
    auto strings = &state.context()->staticStrings();

    Object* result = new ArrayObject(state);
    Value nextResult, value;
    size_t index = 0;

    while (true) {
        if (iteratorRecord.asObject()->getOwnProperty(state, strings->done).value(state, iteratorRecord).isFalse()) {
            try {
                nextResult = IteratorObject::iteratorStep(state, iteratorRecord);
            } catch (const Value& e) {
                Value exceptionValue = e;
                iteratorRecord.asObject()->setThrowsException(state, ObjectPropertyName(strings->done), Value(true), iteratorRecord);
                state.throwException(exceptionValue);
            }

            if (nextResult.isFalse()) {
                iteratorRecord.asObject()->setThrowsException(state, ObjectPropertyName(strings->done), Value(true), iteratorRecord);
            }
        }

        if (iteratorRecord.asObject()->getOwnProperty(state, strings->done).value(state, iteratorRecord).isTrue()) {
            break;
        }

        try {
            value = IteratorObject::iteratorValue(state, nextResult);
        } catch (const Value& e) {
            Value exceptionValue = e;
            iteratorRecord.asObject()->setThrowsException(state, ObjectPropertyName(strings->done), Value(true), iteratorRecord);
            state.throwException(exceptionValue);
        }

        result->setIndexedProperty(state, Value(index++), value);
    }

    return result;
}

NEVER_INLINE void ByteCodeInterpreter::getObjectOpcodeSlowCase(ExecutionState& state, GetObject* code, Value* registerFile)
{
    const Value& willBeObject = registerFile[code->m_objectRegisterIndex];
    const Value& property = registerFile[code->m_propertyRegisterIndex];
    Object* obj;
    if (LIKELY(willBeObject.isObject())) {
        obj = willBeObject.asObject();
    } else {
        obj = fastToObject(state, willBeObject);
    }
    registerFile[code->m_storeRegisterIndex] = obj->getIndexedProperty(state, property).value(state, willBeObject);
}

NEVER_INLINE void ByteCodeInterpreter::setObjectOpcodeSlowCase(ExecutionState& state, SetObjectOperation* code, Value* registerFile)
{
    const Value& willBeObject = registerFile[code->m_objectRegisterIndex];
    const Value& property = registerFile[code->m_propertyRegisterIndex];
    Object* obj = willBeObject.toObject(state);
    if (willBeObject.isPrimitive()) {
        obj->preventExtensions(state);
    }

    bool result = obj->setIndexedProperty(state, property, registerFile[code->m_loadRegisterIndex]);
    if (UNLIKELY(!result) && state.inStrictMode()) {
        Object::throwCannotWriteError(state, ObjectStructurePropertyName(state, property.toString(state)));
    }
}

NEVER_INLINE void ByteCodeInterpreter::ensureArgumentsObjectOperation(ExecutionState& state, ByteCodeBlock* byteCodeBlock, Value* registerFile)
{
    auto functionRecord = state.mostNearestFunctionLexicalEnvironment()->record()->asDeclarativeEnvironmentRecord()->asFunctionEnvironmentRecord();
    auto functionObject = functionRecord->functionObject()->asScriptFunctionObject();
    bool isMapped = functionObject->codeBlock()->shouldHaveMappedArguments();
    functionObject->generateArgumentsObject(state, state.argc(), state.argv(), functionRecord, registerFile + byteCodeBlock->m_requiredRegisterFileSizeInValueSize, isMapped);
}
}
