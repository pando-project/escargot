/*
 * Copyright (c) 2016-present Samsung Electronics Co., Ltd
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#ifndef __EscargotByteCodeInterpreter__
#define __EscargotByteCodeInterpreter__

#include "parser/CodeBlock.h"
#include "runtime/ExecutionState.h"
#include "runtime/String.h"
#include "runtime/Value.h"

namespace Escargot {

class Context;
class ByteCodeBlock;
class LexicalEnvironment;
struct GetObjectInlineCache;
struct SetObjectInlineCache;
struct EnumerateObjectData;
class GetGlobalObject;
class SetGlobalObject;
class CallFunctionInWithScope;
class WithOperation;
class TryOperation;
class UnaryDelete;
class DeclareFunctionDeclarations;
class ObjectDefineGetter;
class ObjectDefineSetter;
class GlobalObject;

class ByteCodeInterpreter {
public:
    static Value interpret(ExecutionState& state, ByteCodeBlock* byteCodeBlock, register size_t programCounter, Value* registerFile, void* initAddressFiller);
    static Value loadByName(ExecutionState& state, LexicalEnvironment* env, const AtomicString& name, bool throwException = true);
    static EnvironmentRecord* getBindedEnvironmentRecordByName(ExecutionState& state, LexicalEnvironment* env, const AtomicString& name, Value& bindedValue, bool throwException = true);
    static void storeByName(ExecutionState& state, LexicalEnvironment* env, const AtomicString& name, const Value& value);
    static Value plusSlowCase(ExecutionState& state, const Value& a, const Value& b);
    static Value modOperation(ExecutionState& state, const Value& left, const Value& right);
    static Object* newOperation(ExecutionState& state, const Value& callee, size_t argc, Value* argv);
    static Value instanceOfOperation(ExecutionState& state, const Value& left, const Value& right);
    static void deleteOperation(ExecutionState& state, LexicalEnvironment* env, UnaryDelete* code, Value* registerFile);

    // http://www.ecma-international.org/ecma-262/5.1/#sec-11.8.5
    static bool abstractRelationalComparisonSlowCase(ExecutionState& state, const Value& left, const Value& right, bool leftFirst);
    static bool abstractRelationalComparison(ExecutionState& state, const Value& left, const Value& right, bool leftFirst);
    static bool abstractRelationalComparisonOrEqualSlowCase(ExecutionState& state, const Value& left, const Value& right, bool leftFirst);
    static bool abstractRelationalComparisonOrEqual(ExecutionState& state, const Value& left, const Value& right, bool leftFirst);

    static Value getObjectPrecomputedCaseOperation(ExecutionState& state, Object* obj, const Value& receiver, const PropertyName& name, GetObjectInlineCache& inlineCache, ByteCodeBlock* block);
    static Value getObjectPrecomputedCaseOperationCacheMiss(ExecutionState& state, Object* obj, const Value& receiver, const PropertyName& name, GetObjectInlineCache& inlineCache, ByteCodeBlock* block);
    static void setObjectPreComputedCaseOperation(ExecutionState& state, Object* obj, const PropertyName& name, const Value& value, SetObjectInlineCache& inlineCache, ByteCodeBlock* block);
    static void setObjectPreComputedCaseOperationCacheMiss(ExecutionState& state, Object* obj, const PropertyName& name, const Value& value, SetObjectInlineCache& inlineCache, ByteCodeBlock* block);

    static EnumerateObjectData* executeEnumerateObject(ExecutionState& state, Object* obj);
    static EnumerateObjectData* updateEnumerateObjectData(ExecutionState& state, EnumerateObjectData* data);

    static Object* fastToObject(ExecutionState& state, const Value& obj);

    static Value getGlobalObjectSlowCase(ExecutionState& state, Object* go, GetGlobalObject* code, ByteCodeBlock* block);
    static void setGlobalObjectSlowCase(ExecutionState& state, Object* go, SetGlobalObject* code, const Value& value, ByteCodeBlock* block);

    static size_t tryOperation(ExecutionState& state, TryOperation* code, ExecutionContext* ec, LexicalEnvironment* env, size_t programCounter, ByteCodeBlock* byteCodeBlock, Value* registerFile);

    static Value withOperation(ExecutionState& state, WithOperation* code, Object* obj, ExecutionContext* ec, LexicalEnvironment* env, size_t& programCounter, ByteCodeBlock* byteCodeBlock, Value* registerFile, Value* stackStorage);
    static Value callFunctionInWithScope(ExecutionState& state, CallFunctionInWithScope* code, ExecutionContext* ec, LexicalEnvironment* env, Value* argv);

    static void declareFunctionDeclarations(ExecutionState& state, DeclareFunctionDeclarations* code, LexicalEnvironment* lexicalEnvironment, Value* stackStorage);
    static void defineObjectGetter(ExecutionState& state, ObjectDefineGetter* code, Value* registerFile);
    static void defineObjectSetter(ExecutionState& state, ObjectDefineSetter* code, Value* registerFile);

    static void processException(ExecutionState& state, const Value& value, ExecutionContext* ec, size_t programCounter);
};
}

#endif
