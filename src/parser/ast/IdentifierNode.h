/*
 * Copyright (c) 2016 Samsung Electronics Co., Ltd
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

#ifndef IdentifierNode_h
#define IdentifierNode_h

#include "Node.h"
#include "ExpressionNode.h"
#include "PatternNode.h"

namespace Escargot {

// interface Identifier <: Node, Expression, Pattern {
class IdentifierNode : public Node {
public:
    friend class ScriptParser;
    IdentifierNode(const AtomicString& name)
        : Node()
    {
        m_name = name;
    }

    virtual ASTNodeType type() { return ASTNodeType::Identifier; }

    AtomicString name()
    {
        return m_name;
    }

    virtual void generateStoreByteCode(ByteCodeBlock* codeBlock, ByteCodeGenerateContext* context)
    {
        codeBlock->pushCode(StoreByName(ByteCodeLOC(m_loc.line, m_loc.column, m_loc.index), context->getLastRegisterIndex(), m_name), context, this);
    }

    virtual void generateExpressionByteCode(ByteCodeBlock* codeBlock, ByteCodeGenerateContext* context)
    {
        codeBlock->pushCode(LoadByName(ByteCodeLOC(m_loc.line, m_loc.column, m_loc.index), context->getRegister(), m_name), context, this);
    }
protected:
    AtomicString m_name;
};

}

#endif