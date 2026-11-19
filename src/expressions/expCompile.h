// expCompile.h
//
// The code in this file is part of Pyxplot
// <http://www.pyxplot.org.uk>
//
// Copyright (C) 2006-2026 Dominic Ford <coders@pyxplot.org.uk>
//               2008-2013 Ross Church
//
// $Id$
//
// Pyxplot is free software; you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software
// Foundation; either version 2 of the License, or (at your option) any later
// version.
//
// You should have received a copy of the GNU General Public License along with
// Pyxplot; if not, write to the Free Software Foundation, Inc., 51 Franklin
// Street, Fifth Floor, Boston, MA  02110-1301, USA

// ----------------------------------------------------------------------------

// Data structures used to compile algebraic expressions into bytecode, and to evaluate that bytecode.
//
// Compilation happens in two stages (see expCompile.c):
//  1. ppl_expTokenise() labels each character of the expression with the type of token it belongs to (pplTokenCode).
//  2. ppl_expCompile() converts the tokens into a sequence of reverse-Polish instructions (pplExprInstruction) using
//     Dijkstra's shunting-yard algorithm, with an operator stack of pplExprStackItem entries.
// The instructions are executed by ppl_expEval() (see expEval.c), using a stack of pplObj values.

#ifndef _EXPCOMPILE_H
#define _EXPCOMPILE_H 1

// ----------------------------------------------------------------------------
// Operator codes
// ----------------------------------------------------------------------------

// The tokeniser labels each operator with one of the codes below, which is carried through to the compiled bytecode.
// The bits of each code have the following meanings:
//   bit 7 (0x80)        -- set if the operator is right-associative, e.g. a**b**c means a**(b**c)
//   bits 5-6 (0x60)     -- the number of operands taken by the operator: 1 (unary), 2 (binary) or 3 (ternary)
//   bits 0-4 (0x1F)     -- distinguish operators which share the above properties
// Assignment operators (= += etc.) use a separate set of codes, which overlap with these, because they are always
// distinguished by the type of token or instruction they appear in.

#define PPL_OP_RIGHT_ASSOC 0x80

// Unary operators which modify a variable
#define PPL_OP_POST_DECREMENT 0x21 // x--
#define PPL_OP_POST_INCREMENT 0x22 // x++
#define PPL_OP_PRE_DECREMENT  0x23 // --x
#define PPL_OP_PRE_INCREMENT  0x24 // ++x

// Unary operators which act on a value
#define PPL_OP_NEGATE      0x25 // -x
#define PPL_OP_UNARY_PLUS  0x26 // +x
#define PPL_OP_BITWISE_NOT 0xA7 // ~x
#define PPL_OP_LOGICAL_NOT 0xA8 // !x, or "not x"

// Binary operators
#define PPL_OP_STRING_SUBST  0x40 // "format" % (values)
#define PPL_OP_MULTIPLY      0x4A // *
#define PPL_OP_DIVIDE        0x4B // /
#define PPL_OP_MODULO        0x4C // %
#define PPL_OP_ADD           0x4D // +
#define PPL_OP_SUBTRACT      0x4E // -
#define PPL_OP_SHIFT_LEFT    0x4F // <<
#define PPL_OP_SHIFT_RIGHT   0x50 // >>
#define PPL_OP_LESS          0x51 // <
#define PPL_OP_LESS_EQUAL    0x52 // <=
#define PPL_OP_GREATER_EQUAL 0x53 // >=
#define PPL_OP_GREATER       0x54 // >
#define PPL_OP_EQUAL         0x55 // ==
#define PPL_OP_NOT_EQUAL     0x56 // != or <>
#define PPL_OP_BITWISE_AND   0x57 // &
#define PPL_OP_BITWISE_XOR   0x58 // ^
#define PPL_OP_BITWISE_OR    0x59 // |
#define PPL_OP_LOGICAL_AND   0x5A // && or "and" -- compiled into a conditional branch
#define PPL_OP_LOGICAL_OR    0x5B // || or "or" -- compiled into a conditional branch
#define PPL_OP_COMMA         0x5C // a, b -- evaluates both operands and keeps the second
#define PPL_OP_COLLECT       0x5F // a comma separating the items of a list, function arguments, etc. -- no operation
#define PPL_OP_POWER         0xC9 // **

// Ternary operator a ? b : c -- compiled into conditional branches
#define PPL_OP_TERNARY_QUESTION 0xFD // ?
#define PPL_OP_TERNARY_COLON    0xFE // :

// Assignment operators
#define PPL_ASSIGN             0x40 // =
#define PPL_ASSIGN_ADD         0x41 // +=
#define PPL_ASSIGN_SUBTRACT    0x42 // -=
#define PPL_ASSIGN_MULTIPLY    0x43 // *=
#define PPL_ASSIGN_DIVIDE      0x44 // /=
#define PPL_ASSIGN_MODULO      0x45 // %=
#define PPL_ASSIGN_BITWISE_AND 0x46 // &=
#define PPL_ASSIGN_BITWISE_XOR 0x47 // ^=
#define PPL_ASSIGN_BITWISE_OR  0x48 // |=
#define PPL_ASSIGN_SHIFT_LEFT  0x49 // <<=
#define PPL_ASSIGN_SHIFT_RIGHT 0x4A // >>=
#define PPL_ASSIGN_POWER       0x4B // **=

// Flags describing the form of a subscript a[...], stored in the operand of a PPL_BC_SLICE instruction
#define PPL_SLICE_SINGLE_ITEM 1 // a[x] rather than a range a[x:y]
#define PPL_SLICE_MAX_SET     2 // a range with an upper limit, e.g. a[:y] or a[x:y]
#define PPL_SLICE_MIN_SET     4 // a range with a lower limit, e.g. a[x:] or a[x:y]

// ----------------------------------------------------------------------------
// Tokens
// ----------------------------------------------------------------------------

//! pplTokenCode - The label which the tokeniser attaches to each character of an expression
typedef struct pplTokenCode {
    unsigned char
            state; // The type of token the character belongs to, e.g. 'J' for a binary operator (see expCompile.c)
    unsigned char opcode; // For operators, the operator code (PPL_OP_* or PPL_ASSIGN_*); for subscripts, PPL_SLICE_*
    unsigned char precedence; // For operators, the precedence; lower numbers bind more tightly
    unsigned int depth; // For closing brackets, the number of comma-separated items between the brackets
} pplTokenCode;

// ----------------------------------------------------------------------------
// Bytecode
// ----------------------------------------------------------------------------

//! pplExprOpcode - The instructions which make up compiled bytecode. Each instruction pushes values onto, or pops
//! values from, the evaluation stack.
typedef enum pplExprOpcode {
    PPL_BC_RETURN = 0, // End of the expression; its value is on top of the stack
    PPL_BC_PUSH_NUMBER = 1, // Push the number <operand.number>
    PPL_BC_PUSH_STRING = 2, // Push the string <operand.string>
    PPL_BC_LOOKUP = 3, // Push the value of the variable named <operand.string>
    PPL_BC_LOOKUP_POINTER = 4, // Push a reference to the variable named <operand.string>, for assignment, creating
    // it if it does not exist
    PPL_BC_DEREFERENCE = 5, // Replace the object on top of the stack with its member named <operand.string>
    PPL_BC_DEREFERENCE_POINTER = 6, // As PPL_BC_DEREFERENCE, but push a reference to the member, for assignment
    PPL_BC_SLICE = 7, // Subscript the object below the top of the stack, as described by <operand.sliceFlags>
    PPL_BC_MAKE_DICT = 8, // Pop <operand.count> key-value pairs from the stack, and push a dictionary
    PPL_BC_MAKE_LIST = 9, // Pop <operand.count> values from the stack, and push a list
    PPL_BC_CALL = 10, // Call the function below <operand.count> arguments on the stack
    PPL_BC_OPERATOR = 11, // Apply the unary or binary operator <operand.operatorCode> to the top of the stack
    PPL_BC_ASSIGN = 12, // Apply the assignment operator <operand.operatorCode> (PPL_ASSIGN_*)
    PPL_BC_INCREMENT = 13, // Apply the increment or decrement operator <operand.operatorCode>
    PPL_BC_STRING_SUBST = 14, // Apply the % operator to a format string and <operand.count> values
    PPL_BC_DOLLAR = 15, // Replace the column number or name on top of the stack with that column of data ($)
    PPL_BC_SLICE_POINTER = 16, // As PPL_BC_SLICE, but push a reference to the item, for assignment
    PPL_BC_BRANCH_IF_FALSE = 17, // If the value on top of the stack is false, jump to <operand.target>
    PPL_BC_BRANCH_IF_TRUE = 18, // If the value on top of the stack is true, jump to <operand.target>
    PPL_BC_GOTO = 19, // Jump to <operand.target>
    PPL_BC_MAKE_BOOLEAN = 20 // Convert the value on top of the stack to a boolean
} pplExprOpcode;

//! pplExprInstruction - A single bytecode instruction
typedef struct pplExprInstruction {
    pplExprOpcode opcode; // The instruction to execute
    int charpos; // The position in the expression's source text of the token which produced this instruction,
    // used to report errors
    unsigned char popOnBranch; // For conditional branches: if true, the value tested is popped from the stack whether
    // or not the branch is taken (used by ?:); if false, it is left on the stack as the result when the branch is
    // taken (used by && and ||, where it is the value of the whole expression)
    union {
        double number; // PPL_BC_PUSH_NUMBER: the number to push
        int string; // PPL_BC_PUSH_STRING, _LOOKUP*, _DEREFERENCE*: offset of a string in pplExpr.strings
        int count; // PPL_BC_MAKE_DICT, _MAKE_LIST, _CALL, _STRING_SUBST: the number of items / arguments
        int operatorCode; // PPL_BC_OPERATOR, _ASSIGN, _INCREMENT: a PPL_OP_* or PPL_ASSIGN_* code
        int sliceFlags; // PPL_BC_SLICE*: a combination of the PPL_SLICE_* flags
        int target; // PPL_BC_BRANCH_*, _GOTO: the index of the instruction to jump to
    } operand;
} pplExprInstruction;

//! pplExpr - A compiled expression
typedef struct pplExpr {
    int refCount; // Reference count; the expression is freed by pplExpr_free() when this reaches zero
    long srcId; // The ID of the source of the expression, used to report errors
    int srcLineN; // The line number of the source of the expression, used to report errors
    char *srcFname; // The filename of the source of the expression, used to report errors
    char *ascii; // The source text of the expression
    pplExprInstruction *bytecode; // The compiled instructions, ending with PPL_BC_RETURN
    int bcLen; // The number of instructions in <bytecode>
    char *strings; // The null-terminated strings referred to by instructions, stored one after another
    int stringsLen; // The number of bytes in <strings>
} pplExpr;

// ----------------------------------------------------------------------------
// The operator stack used by the shunting-yard algorithm
// ----------------------------------------------------------------------------

//! pplExprStackItemType - The types of item which can be placed on the operator stack
typedef enum pplExprStackItemType {
    PPL_STACK_OPERATOR, // A unary or binary operator, waiting for its right-hand operand
    PPL_STACK_ASSIGNMENT, // An assignment operator, waiting for the value to assign
    PPL_STACK_DOLLAR, // The $ operator, waiting for the column number or name it acts on
    PPL_STACK_TERNARY, // The ? of a ternary operator, waiting for its matching :
    PPL_STACK_PARENTHESIS, // An opening ( of a bracketed sub-expression or function call
    PPL_STACK_LIST_BRACKET, // An opening [ of a list literal
    PPL_STACK_DICT_BRACE, // An opening { of a dictionary literal
    PPL_STACK_SUBSCRIPT_BRACKET // An opening [ of a subscript, e.g. a[1]
} pplExprStackItemType;

//! pplExprStackItem - An item on the operator stack used by the shunting-yard algorithm
typedef struct pplExprStackItem {
    pplExprStackItemType type; // The type of item
    unsigned char operatorCode; // For operators, the operator code (PPL_OP_* or PPL_ASSIGN_*)
    unsigned char precedence; // For operators, the precedence; lower numbers bind more tightly
    int charpos; // The position of the token in the source text, used to report errors
    int branchInstruction; // For &&, || and ?:, the index of the branch instruction whose target is set when the
    // operator is popped from the stack
} pplExprStackItem;

#endif
