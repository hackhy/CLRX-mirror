/*
 *  CLRadeonExtender - Unofficial OpenCL Radeon Extensions Library
 *  Copyright (C) 2014-2015 Mateusz Szpakowski
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <CLRX/Config.h>
#include <string>
#include <vector>
#include <stack>
#include <algorithm>
#include <unordered_set>
#include <CLRX/utils/Utilities.h>
#include <CLRX/amdasm/Assembler.h>

using namespace CLRX;

static inline const char* skipSpacesToEnd(const char* string, const char* end)
{
    while (string!=end && *string == ' ') string++;
    return string;
}

/*
 * expressions
 */

AsmExpression::AsmExpression() : symOccursNum(0), relativeSymOccurs(false),
            baseExpr(false)
{ }

void AsmExpression::setParams(size_t _symOccursNum,
          bool _relativeSymOccurs, size_t _opsNum, const AsmExprOp* _ops, size_t _opPosNum,
          const LineCol* _opPos, size_t _argsNum, const AsmExprArg* _args, bool _baseExpr)
{
    symOccursNum = _symOccursNum;
    relativeSymOccurs = _relativeSymOccurs;
    baseExpr = _baseExpr;
    args.reset(new AsmExprArg[_argsNum]);
    ops.assign(_ops, _ops+_opsNum);
    messagePositions.reset(new LineCol[_opPosNum]);
    std::copy(_args, _args+_argsNum, args.get());
    std::copy(_opPos, _opPos+_opPosNum, messagePositions.get());
}

AsmExpression::AsmExpression(const AsmSourcePos& _pos, size_t _symOccursNum,
          bool _relSymOccurs, size_t _opsNum, const AsmExprOp* _ops, size_t _opPosNum,
          const LineCol* _opPos, size_t _argsNum, const AsmExprArg* _args,
          bool _baseExpr)
        : sourcePos(_pos), symOccursNum(_symOccursNum), relativeSymOccurs(_relSymOccurs),
          baseExpr(_baseExpr), ops(_ops, _ops+_opsNum)
{
    args.reset(new AsmExprArg[_argsNum]);
    messagePositions.reset(new LineCol[_opPosNum]);
    std::copy(_args, _args+_argsNum, args.get());
    std::copy(_opPos, _opPos+_opPosNum, messagePositions.get());
}

AsmExpression::AsmExpression(const AsmSourcePos& _pos, size_t _symOccursNum,
            bool _relSymOccurs, size_t _opsNum, size_t _opPosNum, size_t _argsNum,
            bool _baseExpr)
        : sourcePos(_pos), symOccursNum(_symOccursNum), relativeSymOccurs(_relSymOccurs),
          baseExpr(_baseExpr), ops(_opsNum)
{
    args.reset(new AsmExprArg[_argsNum]);
    messagePositions.reset(new LineCol[_opPosNum]);
}

AsmExpression::~AsmExpression()
{
    if (!baseExpr)
    {
        for (size_t i = 0, j = 0; i < ops.size(); i++)
            if (ops[i] == AsmExprOp::ARG_SYMBOL)
            {
                args[j].symbol->second.removeOccurrenceInExpr(this, j, i);
                j++;
            }
            else if (ops[i]==AsmExprOp::ARG_VALUE || ops[i]==AsmExprOp::ARG_RELSYMBOL)
                j++;
    }
}

bool AsmExpression::evaluate(Assembler& assembler, uint64_t& value, cxuint& sectionId) const
{
    if (symOccursNum != 0)
        throw Exception("Expression can't be evaluated if symbols still are unresolved!");
    
    bool failed = false;
    if (!relativeSymOccurs)
    {   // all value is absolute
        std::stack<uint64_t> stack;
        
        size_t argPos = 0;
        size_t opPos = 0;
        size_t messagePosIndex = 0;
        
        while (opPos < ops.size())
        {
            const AsmExprOp op = ops[opPos++];
            if (op == AsmExprOp::ARG_VALUE)
            {
                stack.push(args[argPos++].value);
                continue;
            }
            value = stack.top();
            stack.pop();
            if (isUnaryOp(op))
            {
                switch (op)
                {
                    case AsmExprOp::NEGATE:
                        value = -value;
                        break;
                    case AsmExprOp::BIT_NOT:
                        value = ~value;
                        break;
                    case AsmExprOp::LOGICAL_NOT:
                        value = !value;
                        break;
                    default:
                        break;
                }
            }
            else if (isBinaryOp(op))
            {
                uint64_t value2 = stack.top();
                stack.pop();
                switch (op)
                {
                    case AsmExprOp::ADDITION:
                        value = value2 + value;
                        break;
                    case AsmExprOp::SUBTRACT:
                        value = value2 - value;
                        break;
                    case AsmExprOp::MULTIPLY:
                        value = value2 * value;
                        break;
                    case AsmExprOp::DIVISION:
                        if (value != 0)
                            value = value2 / value;
                        else // error
                        {
                            assembler.printError(getSourcePos(messagePosIndex),
                                   "Division by zero");
                            failed = true;
                            value = 0;
                        }
                        messagePosIndex++;
                        break;
                    case AsmExprOp::SIGNED_DIVISION:
                        if (value != 0)
                            value = int64_t(value2) / int64_t(value);
                        else // error
                        {
                            assembler.printError(getSourcePos(messagePosIndex),
                                   "Division by zero");
                            failed = true;
                            value = 0;
                        }
                        messagePosIndex++;
                        break;
                    case AsmExprOp::MODULO:
                        if (value != 0)
                            value = value2 % value;
                        else // error
                        {
                            assembler.printError(getSourcePos(messagePosIndex),
                                   "Division by zero");
                            failed = true;
                            value = 0;
                        }
                        messagePosIndex++;
                        break;
                    case AsmExprOp::SIGNED_MODULO:
                        if (value != 0)
                            value = int64_t(value2) % int64_t(value);
                        else // error
                        {
                            assembler.printError(getSourcePos(messagePosIndex),
                                   "Division by zero");
                            failed = true;
                            value = 0;
                        }
                        messagePosIndex++;
                        break;
                    case AsmExprOp::BIT_AND:
                        value = value2 & value;
                        break;
                    case AsmExprOp::BIT_OR:
                        value = value2 | value;
                        break;
                    case AsmExprOp::BIT_XOR:
                        value = value2 ^ value;
                        break;
                    case AsmExprOp::BIT_ORNOT:
                        value = value2 | ~value;
                        break;
                    case AsmExprOp::SHIFT_LEFT:
                        if (value < 64)
                            value = value2 << value;
                        else
                        {
                            assembler.printWarning(getSourcePos(messagePosIndex),
                                   "Shift count out of range (between 0 and 63)");
                            value = 0;
                        }
                        messagePosIndex++;
                        break;
                    case AsmExprOp::SHIFT_RIGHT:
                        if (value < 64)
                            value = value2 >> value;
                        else
                        {
                            assembler.printWarning(getSourcePos(messagePosIndex),
                                   "Shift count out of range (between 0 and 63)");
                            value = 0;
                        }
                        messagePosIndex++;
                        break;
                    case AsmExprOp::SIGNED_SHIFT_RIGHT:
                        if (value < 64)
                            value = int64_t(value2) >> value;
                        else
                        {
                            assembler.printWarning(getSourcePos(messagePosIndex),
                                   "Shift count out of range (between 0 and 63)");
                            value = (value2>=(1ULL<<63)) ? UINT64_MAX : 0;
                        }
                        messagePosIndex++;
                        break;
                    case AsmExprOp::LOGICAL_AND:
                        value = value2 && value;
                        break;
                    case AsmExprOp::LOGICAL_OR:
                        value = value2 || value;
                        break;
                    case AsmExprOp::EQUAL:
                        value = (value2 == value) ? UINT64_MAX : 0;
                        break;
                    case AsmExprOp::NOT_EQUAL:
                        value = (value2 != value) ? UINT64_MAX : 0;
                        break;
                    case AsmExprOp::LESS:
                        value = (int64_t(value2) < int64_t(value))? UINT64_MAX: 0;
                        break;
                    case AsmExprOp::LESS_EQ:
                        value = (int64_t(value2) <= int64_t(value)) ? UINT64_MAX : 0;
                        break;
                    case AsmExprOp::GREATER:
                        value = (int64_t(value2) > int64_t(value)) ? UINT64_MAX : 0;
                        break;
                    case AsmExprOp::GREATER_EQ:
                        value = (int64_t(value2) >= int64_t(value)) ? UINT64_MAX : 0;
                        break;
                    case AsmExprOp::BELOW:
                        value = (value2 < value)? UINT64_MAX: 0;
                        break;
                    case AsmExprOp::BELOW_EQ:
                        value = (value2 <= value) ? UINT64_MAX : 0;
                        break;
                    case AsmExprOp::ABOVE:
                        value = (value2 > value) ? UINT64_MAX : 0;
                        break;
                    case AsmExprOp::ABOVE_EQ:
                        value = (value2 >= value) ? UINT64_MAX : 0;
                        break;
                    default:
                        break;
                }
                
            }
            else if (op == AsmExprOp::CHOICE)
            {
                const uint64_t value2 = stack.top();
                stack.pop();
                const uint64_t value3 = stack.top();
                stack.pop();
                value = value3 ? value2 : value;
            }
            stack.push(value);
        }
        
        if (!stack.empty())
            value = stack.top();
        sectionId = ASMSECT_ABS;
    }
    else
    {   // relative symbols
        struct RelMultiply
        {
            uint64_t multiply;
            cxuint sectionId;
        };
        
        struct ValueAndMultiplies
        {
            uint64_t value;
            Array<RelMultiply> relatives;
            
            ValueAndMultiplies(uint64_t _value = 0) : value(_value)
            { }
            ValueAndMultiplies(uint64_t _value, cxuint _sectionId) : value(_value),
                relatives({{1,_sectionId}})
            { }
        };
        
        std::stack<ValueAndMultiplies> stack;
        size_t argPos = 0;
        size_t opPos = 0;
        size_t messagePosIndex = 0;
        std::vector<RelMultiply> relatives;
        
        while (opPos < ops.size())
        {
            const AsmExprOp op = ops[opPos++];
            if (op == AsmExprOp::ARG_VALUE)
            {
                stack.push(args[argPos++].value);
                continue;
            }
            else if (op == AsmExprOp::ARG_RELSYMBOL)
            {
                const AsmSymbol& sym = args[argPos++].symbol->second;
                stack.push(ValueAndMultiplies(sym.value, sym.sectionId));
                continue;
            }
            value = stack.top().value;
            relatives.assign(stack.top().relatives.begin(), stack.top().relatives.end());
            stack.pop();
            if (isUnaryOp(op))
            {
                switch (op)
                {
                    case AsmExprOp::NEGATE:
                        for (RelMultiply& r: relatives)
                            r.multiply = -r.multiply;
                        value = -value;
                        break;
                    case AsmExprOp::BIT_NOT:
                        for (RelMultiply& r: relatives)
                            r.multiply = -r.multiply;
                        value = ~value;
                        break;
                    case AsmExprOp::LOGICAL_NOT:
                        if (!relatives.empty())
                            assembler.printError(sourcePos,
                                 "Logical negation is not allowed to relative values");
                        value = !value;
                        break;
                    default:
                        break;
                }
            }
            else if (isBinaryOp(op))
            {
                uint64_t value2 = stack.top().value;
                const Array<RelMultiply> relatives2 = stack.top().relatives;
                stack.pop();
                switch (op)
                {
                    case AsmExprOp::ADDITION:
                    {
                        for (const RelMultiply& r2: relatives2)
                        {
                            bool rfound = false;
                            for (RelMultiply& r: relatives)
                                if (r.sectionId == r2.sectionId)
                                {
                                    r.multiply += r2.multiply;
                                    rfound = true;
                                }
                           if (!rfound)
                               relatives.push_back(r2);
                        }
                        // remove zeroes from relatives
                        relatives.resize(std::remove_if(relatives.begin(), relatives.end(),
                           [](const RelMultiply& r) { return r.multiply==0; }) -
                           relatives.begin());
                        value = value2 + value;
                        break;
                    }
                    case AsmExprOp::SUBTRACT:
                        for (const RelMultiply& r2: relatives2)
                        {
                            bool rfound = false;
                            for (RelMultiply& r: relatives)
                                if (r.sectionId == r2.sectionId)
                                {
                                    r.multiply -= r2.multiply;
                                    rfound = true;
                                }
                           if (!rfound)
                               relatives.push_back({-r2.multiply, r2.sectionId});
                        }
                        // remove zeroes from relatives
                        relatives.resize(std::remove_if(relatives.begin(), relatives.end(),
                           [](const RelMultiply& r) { return r.multiply==0; }) -
                           relatives.begin());
                        value = value2 - value;
                        break;
                    case AsmExprOp::MULTIPLY:
                        if (!relatives.empty() && !relatives2.empty())
                        {
                            assembler.printError(sourcePos,
                                 "Multiplication is not allowed for two relative values");
                            failed = true;
                        }
                        if (relatives2.empty())
                        {   // multiply relatives
                            if (value2 != 0)
                                for (RelMultiply& r: relatives)
                                    r.multiply *= value2;
                            else
                                relatives.clear();
                        }
                        else
                        {   // multiply relatives2
                            if (value != 0)
                            {
                                relatives.assign(relatives2.begin(), relatives2.end());
                                for (RelMultiply& r: relatives)
                                    r.multiply *= value;
                            }
                        }
                        value = value2 * value;
                        break;
                    case AsmExprOp::DIVISION:
                        if (!relatives.empty() || !relatives2.empty())
                        {
                            assembler.printError(sourcePos,
                                 "Division is not allowed for any relative value");
                            failed = true;
                        }
                        if (value != 0)
                            value = value2 / value;
                        else // error
                        {
                            assembler.printError(getSourcePos(messagePosIndex),
                                   "Division by zero");
                            failed = true;
                            value = 0;
                        }
                        messagePosIndex++;
                        break;
                    case AsmExprOp::SIGNED_DIVISION:
                        if (!relatives.empty() || !relatives2.empty())
                        {
                            assembler.printError(sourcePos,
                                 "Signed division is not allowed for any relative value");
                            failed = true;
                        }
                        if (value != 0)
                            value = int64_t(value2) / int64_t(value);
                        else // error
                        {
                            assembler.printError(getSourcePos(messagePosIndex),
                                   "Division by zero");
                            failed = true;
                            value = 0;
                        }
                        messagePosIndex++;
                        break;
                    case AsmExprOp::MODULO:
                        if (!relatives.empty() || !relatives2.empty())
                        {
                            assembler.printError(sourcePos,
                                 "Modulo is not allowed for any relative value");
                            failed = true;
                        }
                        if (value != 0)
                            value = value2 % value;
                        else // error
                        {
                            assembler.printError(getSourcePos(messagePosIndex),
                                   "Division by zero");
                            failed = true;
                            value = 0;
                        }
                        messagePosIndex++;
                        break;
                    case AsmExprOp::SIGNED_MODULO:
                        if (!relatives.empty() || !relatives2.empty())
                        {
                            assembler.printError(sourcePos,
                                 "Signed Modulo is not allowed for any relative value");
                            failed = true;
                        }
                        if (value != 0)
                            value = int64_t(value2) % int64_t(value);
                        else // error
                        {
                            assembler.printError(getSourcePos(messagePosIndex),
                                   "Division by zero");
                            failed = true;
                            value = 0;
                        }
                        messagePosIndex++;
                        break;
                    case AsmExprOp::BIT_AND:
                        if (!relatives.empty() || !relatives2.empty())
                        {
                            assembler.printError(sourcePos,
                                 "Binary AND is not allowed for any relative value");
                            failed = true;
                        }
                        value = value2 & value;
                        break;
                    case AsmExprOp::BIT_OR:
                        if (!relatives.empty() || !relatives2.empty())
                        {
                            assembler.printError(sourcePos,
                                 "Binary OR is not allowed for any relative value");
                            failed = true;
                        }
                        value = value2 | value;
                        break;
                    case AsmExprOp::BIT_XOR:
                        if (!relatives.empty() || !relatives2.empty())
                        {
                            assembler.printError(sourcePos,
                                 "Binary XOR is not allowed for any relative value");
                            failed = true;
                        }
                        value = value2 ^ value;
                        break;
                    case AsmExprOp::BIT_ORNOT:
                        if (!relatives.empty() || !relatives2.empty())
                        {
                            assembler.printError(sourcePos,
                                 "Binary ORNOT is not allowed for any relative value");
                            failed = true;
                        }
                        value = value2 | ~value;
                        break;
                    case AsmExprOp::SHIFT_LEFT:
                        if (!relatives.empty())
                        {
                            assembler.printError(sourcePos, "Shift left is not allowed "
                                    "for any for relative second value");
                            failed = true;
                        }
                        else if (value < 64)
                        {
                            relatives.assign(relatives2.begin(), relatives2.end());
                            for (RelMultiply& r: relatives)
                                r.multiply <<= value;
                            value = value2 << value;
                        }
                        else
                        {
                            assembler.printWarning(getSourcePos(messagePosIndex),
                                   "Shift count out of range (between 0 and 63)");
                            value = 0;
                        }
                        messagePosIndex++;
                        break;
                    case AsmExprOp::SHIFT_RIGHT:
                        if (!relatives.empty() || !relatives2.empty())
                        {
                            assembler.printError(sourcePos,
                                 "Shift right is not allowed for any relative value");
                            failed = true;
                        }
                        if (value < 64)
                            value = value2 >> value;
                        else
                        {
                            assembler.printWarning(getSourcePos(messagePosIndex),
                                   "Shift count out of range (between 0 and 63)");
                            value = 0;
                        }
                        messagePosIndex++;
                        break;
                    case AsmExprOp::SIGNED_SHIFT_RIGHT:
                        if (!relatives.empty() || !relatives2.empty())
                        {
                            assembler.printError(sourcePos, "Signed shift right is not "
                                    "allowed for any relative value");
                            failed = true;
                        }
                        if (value < 64)
                            value = int64_t(value2) >> value;
                        else
                        {
                            assembler.printWarning(getSourcePos(messagePosIndex),
                                   "Shift count out of range (between 0 and 63)");
                            value = (value2>=(1ULL<<63)) ? UINT64_MAX : 0;
                        }
                        messagePosIndex++;
                        break;
                    case AsmExprOp::LOGICAL_AND:
                        if (!relatives.empty() || !relatives2.empty())
                        {
                            assembler.printError(sourcePos,
                                 "Logical AND is not allowed for any relative value");
                            failed = true;
                        }
                        value = value2 && value;
                        break;
                    case AsmExprOp::LOGICAL_OR:
                        if (!relatives.empty() || !relatives2.empty())
                        {
                            assembler.printError(sourcePos,
                                 "Logical OR is not allowed for any relative value");
                            failed = true;
                        }
                        value = value2 || value;
                        break;
                    case AsmExprOp::EQUAL:
                    case AsmExprOp::NOT_EQUAL:
                    case AsmExprOp::LESS:
                    case AsmExprOp::LESS_EQ:
                    case AsmExprOp::GREATER:
                    case AsmExprOp::GREATER_EQ:
                    case AsmExprOp::BELOW:
                    case AsmExprOp::BELOW_EQ:
                    case AsmExprOp::ABOVE:
                    case AsmExprOp::ABOVE_EQ:
                    {
                        size_t requals = 0;
                        if (relatives2.size() != relatives.size())
                        {
                            assembler.printError(sourcePos,
                                 "For comparisons two values must have same relatives!");
                            failed = true;
                        }
                        else
                        {
                            for (const RelMultiply& r: relatives2)
                                for (RelMultiply& r2: relatives)
                                    if (r.multiply == r2.multiply &&
                                            r.sectionId == r2.sectionId)
                                    {
                                        r2.sectionId = ASMSECT_ABS; // ignore in next iter
                                        requals++;
                                        break;
                                    }
                            if (requals != relatives.size())
                            {
                                assembler.printError(sourcePos, "For comparisons "
                                        "two values must have same relatives!");
                                failed = true;
                            }
                        }
                        relatives.clear();
                        switch(op)
                        {
                            case AsmExprOp::EQUAL:
                                value = (value2 == value) ? UINT64_MAX : 0;
                                break;
                            case AsmExprOp::NOT_EQUAL:
                                value = (value2 != value) ? UINT64_MAX : 0;
                                break;
                            case AsmExprOp::LESS:
                                value = (int64_t(value2) < int64_t(value))? UINT64_MAX: 0;
                                break;
                            case AsmExprOp::LESS_EQ:
                                value = (int64_t(value2) <= int64_t(value)) ?
                                        UINT64_MAX : 0;
                                break;
                            case AsmExprOp::GREATER:
                                value = (int64_t(value2) > int64_t(value)) ? UINT64_MAX : 0;
                                break;
                            case AsmExprOp::GREATER_EQ:
                                value = (int64_t(value2) >= int64_t(value)) ?
                                        UINT64_MAX : 0;
                                break;
                            case AsmExprOp::BELOW:
                                value = (value2 < value)? UINT64_MAX: 0;
                                break;
                            case AsmExprOp::BELOW_EQ:
                                value = (value2 <= value)? UINT64_MAX: 0;
                                break;
                            case AsmExprOp::ABOVE:
                                value = (value2 > value)? UINT64_MAX: 0;
                                break;
                            case AsmExprOp::ABOVE_EQ:
                                value = (value2 >= value)? UINT64_MAX: 0;
                                break;
                            default:
                                break;
                        }
                        break;
                    }
                    default:
                        break;
                }
                
            }
            else if (op == AsmExprOp::CHOICE)
            {
                const uint64_t value2 = stack.top().value;
                const Array<RelMultiply> relatives2 = stack.top().relatives;
                stack.pop();
                const uint64_t value3 = stack.top().value;
                const Array<RelMultiply> relatives3 = stack.top().relatives;
                stack.pop();
                if (!relatives3.empty())
                {
                    assembler.printError(sourcePos,
                         "Choice is not allowed for first relative value");
                    failed = true;
                }
                if (value3)
                    relatives.assign(relatives2.begin(), relatives2.end());
                value = value3 ? value2 : value;
            }
            
            ValueAndMultiplies relOut(value);
            relOut.relatives.assign(relatives.begin(), relatives.end());
            stack.push(relOut);
        }
        
        if (!stack.empty())
        {
            value = stack.top().value;
            relatives.assign(stack.top().relatives.begin(), stack.top().relatives.end());
        }
        if (relatives.empty())
            sectionId = ASMSECT_ABS;
        else if (relatives.size() == 1 && relatives.front().multiply == 1)
            sectionId = relatives.front().sectionId;
        else
        {
            assembler.printError(sourcePos,
                     "Only one relative=1 (section) can be result of expression");
            failed = true;
        }
    }
    return !failed;
}

static const cxbyte asmOpPrioritiesTbl[] =
{   /* higher value, higher priority */
    7, // ARG_VALUE
    7, // ARG_SYMBOL
    7, // ARG_RELSYMBOL
    6, // NEGATE
    6, // BIT_NOT
    6, // LOGICAL_NOT
    6, // PLUS
    3, // ADDITION
    3, // SUBTRACT
    5, // MULTIPLY
    5, // DIVISION
    5, // SIGNED_DIVISION
    5, // MODULO
    5, // SIGNED_MODULO
    4, // BIT_AND
    4, // BIT_OR
    4, // BIT_XOR
    4, // BIT_ORNOT
    5, // SHIFT_LEFT
    5, // SHIFT_RIGHT
    5, // SIGNED_SHIFT_RIGHT
    1, // LOGICAL_AND
    1, // LOGICAL_OR
    2, // EQUAL
    2, // NOT_EQUAL
    2, // LESS
    2, // LESS_EQ
    2, // GREATER
    2, // GREATER_EQ
    2, // BELOW
    2, // BELOW_EQ
    2, // ABOVE
    2, // ABOVE_EQ
    0, // CHOICE
    0 // CHOICE_END
};

AsmExpression* AsmExpression::parse(Assembler& assembler, size_t linePos,
                size_t& outLinePos, bool makeBase)
{
    const char* outend;
    AsmExpression* expr = parse(assembler, assembler.line+linePos, outend, makeBase);
    outLinePos = outend-(assembler.line+linePos);
    return expr;
}   

struct CLRX_INTERNAL SymbolSnapshotHash: std::hash<std::string>
{
    size_t operator()(const AsmSymbolEntry* e1) const
    { return static_cast<const std::hash<std::string>&>(*this)(e1->first); }
};

struct CLRX_INTERNAL SymbolSnapshotEqual
{
    bool operator()(const AsmSymbolEntry* e1, const AsmSymbolEntry* e2) const
    { return e1->first == e2->first; }
};

class CLRX_INTERNAL CLRX::AsmExpression::TempSymbolSnapshotMap: 
        public std::unordered_set<AsmSymbolEntry*, SymbolSnapshotHash, SymbolSnapshotEqual>
{ };

static const uint64_t operatorWithMessage = 
        (1ULL<<int(AsmExprOp::DIVISION)) | (1ULL<<int(AsmExprOp::SIGNED_DIVISION)) |
        (1ULL<<int(AsmExprOp::MODULO)) | (1ULL<<int(AsmExprOp::SIGNED_MODULO)) |
        (1ULL<<int(AsmExprOp::SHIFT_LEFT)) | (1ULL<<int(AsmExprOp::SHIFT_RIGHT)) |
        (1ULL<<int(AsmExprOp::SIGNED_SHIFT_RIGHT));

AsmExpression* AsmExpression::createForSnapshot(const AsmSourcePos* exprSourcePos) const
{
    std::unique_ptr<AsmExpression> expr(new AsmExpression);
    size_t argsNum = 0;
    size_t msgPosNum = 0;
    for (AsmExprOp op: ops)
        if (AsmExpression::isArg(op))
            argsNum++;
        else if (operatorWithMessage & (1ULL<<int(op)))
            msgPosNum++;
    expr->sourcePos = sourcePos;
    expr->sourcePos.exprSourcePos = exprSourcePos;
    expr->ops = ops;
    expr->args.reset(new AsmExprArg[argsNum]);
    std::copy(args.get(), args.get()+argsNum, expr->args.get());
    expr->messagePositions.reset(new LineCol[msgPosNum]);
    std::copy(messagePositions.get(), messagePositions.get()+msgPosNum,
              expr->messagePositions.get());
    return expr.release();
}

static inline AsmSymbolEntry* createSymbolEntryForSnapshot(const AsmSymbolEntry& symEntry,
            const AsmSourcePos* exprSourcePos)
{
    std::unique_ptr<AsmExpression> expr(
            symEntry.second.expression->createForSnapshot(exprSourcePos));
    
    std::unique_ptr<AsmSymbolEntry> newSymEntry(new AsmSymbolEntry{
            symEntry.first, AsmSymbol(expr.get(), false, false)});
    
    expr->setTarget(AsmExprTarget::symbolTarget(newSymEntry.get()));
    expr.release();
    newSymEntry->second.base = true;
    return newSymEntry.release();
}

bool AsmExpression::makeSymbolSnapshot(Assembler& assembler,
           TempSymbolSnapshotMap* snapshotMap, const AsmSymbolEntry& symEntry,
           AsmSymbolEntry*& outSymEntry, const AsmSourcePos* topParentSourcePos)
{
    struct StackEntry
    {
        AsmSymbolEntry* entry;
        size_t opIndex;
        size_t argIndex;
        
        explicit StackEntry(AsmSymbolEntry* _entry)
            : entry(_entry), opIndex(0), argIndex(0)
        { }
        
        AsmSymbolEntry* releaseEntry()
        {
            AsmSymbolEntry* out = entry;
            entry = nullptr;
            return out;
        }
    };
    std::stack<StackEntry> stack;
    
    outSymEntry = nullptr;
    {
        std::unique_ptr<AsmSymbolEntry> newSymEntry(
                    createSymbolEntryForSnapshot(symEntry, topParentSourcePos));
        auto res = snapshotMap->insert(newSymEntry.get());
        if (!res.second)
        {   // do nothing (symbol snapshot already made)
            outSymEntry = *res.first;
            outSymEntry->second.refCount++;
            return true;
        }
        stack.push(StackEntry(newSymEntry.release()));
    }
    
    bool good = true;
    
    while (!stack.empty())
    {
        StackEntry& se = stack.top();
        size_t opIndex = se.opIndex;
        size_t argIndex = se.argIndex;
        AsmExpression* expr = se.entry->second.expression;
        const size_t opsSize = expr->ops.size();
        
        AsmExprArg* args = expr->args.get();
        AsmExprOp* ops = expr->ops.data();
        if (opIndex < opsSize)
        {
            for (; opIndex < opsSize; opIndex++)
                if (ops[opIndex] == AsmExprOp::ARG_SYMBOL)
                {   // check this symbol
                    AsmSymbolEntry* nextSymEntry = args[argIndex].symbol;
                    if (nextSymEntry->second.base)
                    {   // new base expression (set by using .'eqv')
                        std::unique_ptr<AsmSymbolEntry> newSymEntry(
                                    createSymbolEntryForSnapshot(*nextSymEntry,
                                     &(expr->sourcePos)));
                        auto res = snapshotMap->insert(newSymEntry.get());
                        if (!res.second)
                        {    // replace this symEntry by symbol from tempSymbolMap
                            nextSymEntry = *res.first;
                            args[argIndex].symbol = nextSymEntry;
                            nextSymEntry->second.refCount++;
                        }
                        else
                        {   // new symEntry to stack
                            stack.push(StackEntry(newSymEntry.release()));
                            se.argIndex = argIndex;
                            se.opIndex = opIndex;
                            break;
                        }
                    }
                    
                    if (nextSymEntry->second.isDefined)
                    {   // put value to argument
                        if (nextSymEntry->second.sectionId == ASMSECT_ABS)
                        {
                            ops[opIndex] = AsmExprOp::ARG_VALUE;
                            args[argIndex].value = nextSymEntry->second.value;
                        }
                        else
                        {
                            ops[opIndex] = AsmExprOp::ARG_RELSYMBOL;
                            args[argIndex].symbol = nextSymEntry;
                            expr->relativeSymOccurs = true;
                        }
                    }
                    else // if not defined
                    {
                        args[argIndex].symbol->second.addOccurrenceInExpr(
                                        expr, argIndex, opIndex);
                        expr->symOccursNum++;
                    }
                    
                    argIndex++;
                }
                else if (ops[opIndex]==AsmExprOp::ARG_VALUE ||
                    ops[opIndex]==AsmExprOp::ARG_RELSYMBOL)
                    argIndex++;
        }
        if (opIndex == opsSize)
        {   // check if expression is evaluatable
            AsmSymbolEntry* thisSymEntry = se.releaseEntry();
            if (expr->symOccursNum == 0) // no symbols, we try to evaluate
            {   // evaluate and remove obsolete expression
                if (!expr->evaluate(assembler, thisSymEntry->second.value,
                            thisSymEntry->second.sectionId))
                    good = false;
                thisSymEntry->second.isDefined = true;
                delete thisSymEntry->second.expression;
                thisSymEntry->second.expression = nullptr;
            }
            thisSymEntry->second.base = false;
            thisSymEntry->second.snapshot = true;
            stack.pop();
            if (!stack.empty())
            {   // put to place in parent expression
                StackEntry& parentStackEntry = stack.top();
                AsmExpression* parentExpr = parentStackEntry.entry->second.expression;
                parentExpr->args[parentStackEntry.argIndex].symbol = thisSymEntry;
                parentExpr->ops[parentStackEntry.opIndex] = AsmExprOp::ARG_SYMBOL;
            }
            else
            {   // last we return it
                outSymEntry = thisSymEntry;
                break;
            }
        }
    }
    return good;
}

bool AsmExpression::makeSymbolSnapshot(Assembler& assembler,
           const AsmSymbolEntry& symEntry, AsmSymbolEntry*& outSymEntry,
           const AsmSourcePos* parentExprSourcePos)
{
    TempSymbolSnapshotMap symbolSnapshots;
    bool good = true;
    try
    {
        good = makeSymbolSnapshot(assembler, &symbolSnapshots, symEntry, outSymEntry,
                    parentExprSourcePos);
        if (good)
            for (AsmSymbolEntry* symEntry: symbolSnapshots)
                // delete evaluated symbol entries (except output symbol entry)
                if (outSymEntry != symEntry && symEntry->second.isDefined)
                {
                    delete symEntry->second.expression;
                    symEntry->second.expression = nullptr;
                    delete symEntry;
                }
                else
                    assembler.symbolSnapshots.insert(symEntry);
        else // if failed
            for (AsmSymbolEntry* symEntry: symbolSnapshots)
            {
                assembler.symbolSnapshots.erase(symEntry);
                delete symEntry->second.expression;
                symEntry->second.expression = nullptr;
                delete symEntry;
            }
    }
    catch(...)
    {
        for (AsmSymbolEntry* symEntry: symbolSnapshots)
        {
            assembler.symbolSnapshots.erase(symEntry);
            delete symEntry->second.expression;
            symEntry->second.expression = nullptr;
            delete symEntry;
        }
        throw;
    }   
    return good;
}

AsmExpression* AsmExpression::parse(Assembler& assembler, const char* string,
            const char*& outend, bool makeBase)
{
    struct ConExprOpEntry
    {
        AsmExprOp op;
        cxuint priority;
        size_t lineColPos;
    };

    std::stack<ConExprOpEntry> stack;
    std::vector<AsmExprOp> ops;
    std::vector<AsmExprArg> args;
    std::vector<LineCol> messagePositions;
    std::vector<LineCol> outMsgPositions;
    
    TempSymbolSnapshotMap symbolSnapshots;
    
    try
    {
    const char* startString = string;
    const char* end = assembler.line + assembler.lineSize;
    size_t parenthesisCount = 0;
    size_t symOccursNum = 0;
    bool good = true;
    bool relativeSymOccurs = false;
    
    enum ExpectedToken
    {
        XT_FIRST = 0,
        XT_OP = 1,
        XT_ARG = 2
    };
    ExpectedToken expectedToken = XT_FIRST;
    std::unique_ptr<AsmExpression> expr(new AsmExpression);
    expr->sourcePos = assembler.getSourcePos(startString);
    
    while (string != end)
    {
        string = skipSpacesToEnd(string, end);
        if (string == end) break;
        
        LineCol lineCol = { 0, 0 };
        AsmExprOp op = AsmExprOp::NONE;
        bool expectedPrimaryExpr = false;
        //const size_t oldParenthesisCount = parenthesisCount;
        bool doExit = false;
        
        const char* beforeToken = string;
        switch(*string)
        {
            case '(':
                if (expectedToken == XT_OP)
                {
                    assembler.printError(string, "Expected operator");
                    good = false;
                }
                else
                {
                    expectedToken = XT_FIRST;
                    parenthesisCount++;
                }
                string++;
                break;
            case ')':
                if (expectedToken != XT_OP)
                {
                    assembler.printError(string, "Expected operator or value or symbol");
                    good = false;
                }
                else
                {
                    if (parenthesisCount==0)
                    { doExit = true; break; }
                    parenthesisCount--;
                }
                string++;
                break;
            case '+':
                op = (expectedToken == XT_OP) ? AsmExprOp::ADDITION : AsmExprOp::PLUS;
                string++;
                break;
            case '-':
                op = (expectedToken == XT_OP) ? AsmExprOp::SUBTRACT : AsmExprOp::NEGATE;
                string++;
                break;
            case '*':
                op = AsmExprOp::MULTIPLY;
                string++;
                break;
            case '/':
                lineCol = assembler.translatePos(string);
                if (string+1 != end && string[1] == '/')
                {
                    op = AsmExprOp::DIVISION;
                    string++;
                }
                else // standard GNU as signed division
                    op = AsmExprOp::SIGNED_DIVISION;
                string++;
                break;
            case '%':
                lineCol = assembler.translatePos(string);
                if (string+1 != end && string[1] == '%')
                {
                    op = AsmExprOp::MODULO;
                    string++;
                }
                else // standard GNU as signed division
                    op = AsmExprOp::SIGNED_MODULO;
                string++;
                break;
            case '&':
                if (string+1 != end && string[1] == '&')
                {
                    op = AsmExprOp::LOGICAL_AND;
                    string++;
                }
                else // standard GNU as signed division
                    op = AsmExprOp::BIT_AND;
                string++;
                break;
            case '|':
                if (string+1 != end && string[1] == '|')
                {
                    op = AsmExprOp::LOGICAL_OR;
                    string++;
                }
                else // standard GNU as signed division
                    op = AsmExprOp::BIT_OR;
                string++;
                break;
            case '!':
                if (expectedToken == XT_OP) // ORNOT or logical not
                {
                    if (string+1 != end && string[1] == '=')
                    {
                        op = AsmExprOp::NOT_EQUAL;
                        string++;
                    }
                    else
                        op = AsmExprOp::BIT_ORNOT;
                }
                else
                    op = AsmExprOp::LOGICAL_NOT;
                string++;
                break;
            case '~':
                if (expectedToken != XT_OP)
                    op = AsmExprOp::BIT_NOT;
                else
                {
                    assembler.printError(string,
                        "Expected non-unary operator, '(', or end of expression");
                    good = false;
                }
                string++;
                break;
            case '^':
                op = AsmExprOp::BIT_XOR;
                string++;
                break;
            case '<':
                if (string+1 != end && string[1] == '<')
                {
                    lineCol = assembler.translatePos(string);
                    op = AsmExprOp::SHIFT_LEFT;
                    string++;
                }
                else if (string+1 != end && string[1] == '>')
                {
                    op = AsmExprOp::NOT_EQUAL;
                    string++;
                }
                else if (string+1 != end && string[1] == '=')
                {
                    if (string+2 != end && string[2] == '@')
                    {
                        op = AsmExprOp::BELOW_EQ;
                        string++;
                    }
                    else
                        op = AsmExprOp::LESS_EQ;
                    string++;
                }
                else if (string+1 != end && string[1] == '@')
                {
                    op = AsmExprOp::BELOW;
                    string++;
                }
                else
                    op = AsmExprOp::LESS;
                string++;
                break;
            case '>':
                if (string+1 != end && string[1] == '>')
                {
                    lineCol = assembler.translatePos(string);
                    if (string+2 != end && string[2] == '>')
                    {
                        op = AsmExprOp::SIGNED_SHIFT_RIGHT;
                        string++;
                    }
                    else
                        op = AsmExprOp::SHIFT_RIGHT;
                    string++;
                }
                else if (string+1 != end && string[1] == '=')
                {
                    if (string+2 != end && string[2] == '@')
                    {
                        op = AsmExprOp::ABOVE_EQ;
                        string++;
                    }
                    else
                        op = AsmExprOp::GREATER_EQ;
                    string++;
                }
                else if (string+1 != end && string[1] == '@')
                {
                    op = AsmExprOp::ABOVE;
                    string++;
                }
                else
                    op = AsmExprOp::GREATER;
                string++;
                break;
            case '=':
                if (string+1 != end && string[1] == '=')
                {
                    op = AsmExprOp::EQUAL;
                    string++;
                }
                else
                    expectedPrimaryExpr = true;
                string++;
                break;
            case '?':
                lineCol = assembler.translatePos(string);
                op = AsmExprOp::CHOICE_START;
                string++;
                break;
            case ':':
                op = AsmExprOp::CHOICE;
                string++;
                break;
            default: // parse symbol or value
                if (expectedToken != XT_OP)
                {
                    ExpectedToken oldExpectedToken = expectedToken;
                    expectedToken = XT_OP;
                    AsmSymbolEntry* symEntry;
                    bool symIsGood = assembler.parseSymbol(string, symEntry);
                    if (!symIsGood) good = false;
                    AsmExprArg arg;
                    if (symEntry != nullptr)
                    {
                        if (symEntry->second.base && !makeBase)
                            good = makeSymbolSnapshot(assembler, &symbolSnapshots,
                                      *symEntry, symEntry, &(expr->sourcePos));
                        
                        if (symEntry->second.isDefined && !makeBase)
                        {
                            if (!assembler.isAbsoluteSymbol(symEntry->second))
                            {
                                relativeSymOccurs = true;
                                arg.symbol = symEntry;
                                args.push_back(arg);
                                ops.push_back(AsmExprOp::ARG_RELSYMBOL);
                            }
                            else
                            {
                                arg.value = symEntry->second.value;
                                args.push_back(arg);
                                ops.push_back(AsmExprOp::ARG_VALUE);
                            }
                        }
                        else
                        {   /* add symbol */
                            symOccursNum++;
                            arg.symbol = symEntry;
                            args.push_back(arg);
                            ops.push_back(AsmExprOp::ARG_SYMBOL);
                        }
                        string += symEntry->first.size();
                    }
                    else if (parenthesisCount != 0 || (*string >= '0' && *string <= '9') ||
                             *string == '\'')
                    {   // other we try to parse number
                        const char* oldStr = string;
                        if (!assembler.parseLiteral(arg.value, string, string))
                        {
                            arg.value = 0;
                            if (string != end && oldStr == string)
                                // skip one character when end is in this same place
                                // (avoids infinity loops)
                                string++;
                            good = false;
                        }
                        args.push_back(arg);
                        ops.push_back(AsmExprOp::ARG_VALUE);
                    }
                    else // otherwise we finish parsing
                    {
                        expectedToken = oldExpectedToken;
                        doExit = true; break;
                    }
                }
                else
                {
                    if (parenthesisCount == 0)
                    { doExit = true; break; }
                    else
                    {
                        string++;
                        assembler.printError(string, "Garbages at end of expression");
                        good = false;
                    }
                }
        }
        
        if (op != AsmExprOp::NONE && !isUnaryOp(op) && expectedToken != XT_OP)
        {
            expectedPrimaryExpr = true;
            op = AsmExprOp::NONE;
        }
        if (expectedPrimaryExpr)
        {
            assembler.printError(beforeToken,
                     "Expected primary expression before operator");
            good = false;
            continue;
        }
        
        if (op != AsmExprOp::NONE && !isUnaryOp(op))
            expectedToken = (expectedToken == XT_OP) ? XT_ARG : XT_OP;
        
        //afterParenthesis = (oldParenthesisCount < parenthesisCount);
        const size_t lineColPos = (lineCol.lineNo!=0) ? messagePositions.size() : SIZE_MAX;
        if (lineCol.lineNo!=0)
            messagePositions.push_back(lineCol);
        
        if (op != AsmExprOp::NONE)
        {   // if operator
            const bool unaryOp = isUnaryOp(op);
            const cxuint priority = (parenthesisCount<<3) +
                        asmOpPrioritiesTbl[cxuint(op)];
            
            if (op == AsmExprOp::CHOICE)
            {   /* second part of choice */
                while (!stack.empty())
                {
                    const ConExprOpEntry& entry = stack.top();
                    if (priority > entry.priority || (priority == entry.priority &&
                        entry.op == AsmExprOp::CHOICE_START))
                        break;
                    if (entry.op != AsmExprOp::PLUS)
                        ops.push_back(entry.op);
                    if (entry.lineColPos != SIZE_MAX && entry.op != AsmExprOp::CHOICE_START)
                        outMsgPositions.push_back(messagePositions[entry.lineColPos]);
                    stack.pop();
                }
                if (stack.empty() || stack.top().op != AsmExprOp::CHOICE_START ||
                        stack.top().priority != priority)
                {   // not found
                    assembler.printError(beforeToken, "Missing '?' before ':'");
                    good = false;
                    continue; // do noy change stack and them entries
                }
                ConExprOpEntry& entry = stack.top();
                entry.op = AsmExprOp::CHOICE;
                entry.lineColPos = SIZE_MAX;
            }
            else
            {
                while (!stack.empty())
                {
                    const ConExprOpEntry& entry = stack.top();
                    if (priority +
                        /* because ?: is computed from right to left we adds 1 to priority
                         * to force put new node higher than left node */
                        (op == AsmExprOp::CHOICE_START) + unaryOp > entry.priority)
                        break;
                    if (entry.op == AsmExprOp::CHOICE_START)
                    {   // unfinished choice
                        assembler.printError(messagePositions[entry.lineColPos],
                                 "Missing ':' for '?'");
                        stack.pop();
                        good = false;
                        break;
                    }
                    if (entry.op != AsmExprOp::PLUS)
                        ops.push_back(entry.op);
                    if (entry.lineColPos != SIZE_MAX && entry.op != AsmExprOp::CHOICE_START)
                        outMsgPositions.push_back(messagePositions[entry.lineColPos]);
                    stack.pop();
                }
                stack.push({ op, priority, lineColPos });
            }
        }
        
        if (doExit) // exit from parsing
            break;
    }
    if (parenthesisCount != 0)
    {   // print error
        assembler.printError(string, "Missing ')'");
        good = false;
    }
    if (expectedToken != XT_OP)
    {
        if (ops.empty() && stack.empty())
        {
            ops.push_back(AsmExprOp::ARG_VALUE);
            args.push_back({0});
        }
        else
        {
            assembler.printError(string, "Missing primary expression");
            good = false;
        }
    }
    else
    {
        while (!stack.empty())
        {
            const ConExprOpEntry& entry = stack.top();
            if (entry.op == AsmExprOp::CHOICE_START)
            {   // unfinished choice
                assembler.printError(messagePositions[entry.lineColPos],
                         "Missing ':' for '?'");
                good = false;
                break;
            }
            if (entry.op != AsmExprOp::PLUS)
                ops.push_back(entry.op);
            if (entry.lineColPos != SIZE_MAX && entry.op != AsmExprOp::CHOICE_START)
                outMsgPositions.push_back(messagePositions[entry.lineColPos]);
            stack.pop();
        }
    }
    outend = string;
    
    if (good)
    {
        const size_t argsNum = args.size();
        expr->setParams(symOccursNum, relativeSymOccurs,
                  ops.size(), ops.data(), outMsgPositions.size(), outMsgPositions.data(),
                  argsNum, args.data(), makeBase);
        /*std::unique_ptr<AsmExpression> expr(new AsmExpression(
                  ));*/
        if (!makeBase)
        {   // add expression into symbol occurrences in expressions
            // only for non-base expressions
            for (size_t i = 0, j = 0; j < argsNum; i++)
                if (ops[i] == AsmExprOp::ARG_SYMBOL)
                {
                    args[j].symbol->second.addOccurrenceInExpr(expr.get(), j, i);
                    j++;
                }
                else if (ops[i]==AsmExprOp::ARG_VALUE || ops[i]==AsmExprOp::ARG_RELSYMBOL)
                    j++;
        }
        for (AsmSymbolEntry* symEntry: symbolSnapshots)
        {
            if (!symEntry->second.isDefined)
                assembler.symbolSnapshots.insert(symEntry);
            else
            {
                delete symEntry->second.expression;
                symEntry->second.expression = nullptr;
                delete symEntry;
            }
        }
        symbolSnapshots.clear();
        return expr.release();
    }
    else
    {
        for (AsmSymbolEntry* symEntry: symbolSnapshots)
        {
            delete symEntry->second.expression;
            symEntry->second.expression = nullptr;
            delete symEntry;
        }
        return nullptr;
    }
    }
    catch(...)
    {
        for (AsmSymbolEntry* symEntry: symbolSnapshots)
        {   // remove from assembler symbolSnapshots
            assembler.symbolSnapshots.erase(symEntry);
            // remove this snapshot
            delete symEntry->second.expression;
            symEntry->second.expression = nullptr;
            delete symEntry;
        }
        throw;
    }
}
