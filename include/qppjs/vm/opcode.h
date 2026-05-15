#pragma once

#include <cstdint>

namespace qppjs {

// Opcode definitions via X-Macro.
// Format: X(Name, operand_bytes)
// operand_bytes: 0=no operand, 1=u8, 2=u16, 4=i32
#define QPPJS_OPCODES(X)                \
    /* Value loading */                 \
    X(LoadUndefined, 0)                 \
    X(LoadNull, 0)                      \
    X(LoadTrue, 0)                      \
    X(LoadFalse, 0)                     \
    X(LoadNumber, 2)                    \
    X(LoadString, 2)                    \
    X(LoadThis, 0)                      \
    /* Variables */                     \
    X(GetVar, 2)                        \
    X(SetVar, 2)                        \
    X(DefVar, 2)                        \
    X(DefLet, 2)                        \
    X(DefConst, 2)                      \
    X(InitVar, 2)                       \
    /* Scope */                         \
    X(PushScope, 0)                     \
    X(PopScope, 0)                      \
    /* Object properties */             \
    X(NewObject, 0)                     \
    X(NewArray, 0)                      \
    X(GetProp, 2)                       \
    X(SetProp, 2)                       \
    X(GetElem, 0)                       \
    X(SetElem, 0)                       \
    /* Functions and calls */           \
    X(MakeFunction, 2)                  \
    X(Call, 1)                          \
    X(CallMethod, 1)                    \
    X(NewCall, 1)                       \
    X(Return, 0)                        \
    X(ReturnUndefined, 0)               \
    /* Arithmetic */                    \
    X(Add, 0)                           \
    X(Sub, 0)                           \
    X(Mul, 0)                           \
    X(Div, 0)                           \
    X(Mod, 0)                           \
    /* Unary */                         \
    X(Neg, 0)                           \
    X(Pos, 0)                           \
    X(BitNot, 0)                        \
    X(Not, 0)                           \
    /* Comparison */                    \
    X(Lt, 0)                            \
    X(LtEq, 0)                          \
    X(Gt, 0)                            \
    X(GtEq, 0)                          \
    X(Eq, 0)                            \
    X(NEq, 0)                           \
    X(StrictEq, 0)                      \
    X(StrictNEq, 0)                     \
    /* Type */                          \
    X(Typeof, 0)                        \
    X(TypeofVar, 2)                     \
    X(Instanceof, 0)                    \
    /* Control flow */                  \
    X(Jump, 4)                          \
    X(JumpIfFalse, 4)                   \
    X(JumpIfTrue, 4)                    \
    /* Stack */                         \
    X(Pop, 0)                           \
    X(Dup, 0)                           \
    /* Exception control flow */        \
    X(Throw, 0)                         \
    X(EnterTry, 4)                      \
    X(LeaveTry, 0)                      \
    X(GetException, 0)                  \
    X(Gosub, 4)                         \
    X(Ret, 0)                           \
    /* Module */                        \
    X(SetExportDefault, 0)              \
    /* Async/Await */                   \
    X(Await, 0)                         \
    /* Array hole (elision) */          \
    X(ArrayHole, 0)                     \
    /* Dynamic import() */              \
    X(ImportCall, 0)                    \
    /* Update (++/--) variables */      \
    X(VarPreInc, 2)                     \
    X(VarPreDec, 2)                     \
    X(VarPostInc, 2)                    \
    X(VarPostDec, 2)                    \
    /* Update (++/--) properties */     \
    X(PropPreInc, 2)                    \
    X(PropPreDec, 2)                    \
    X(PropPostInc, 2)                   \
    X(PropPostDec, 2)                   \
    /* Update (++/--) elements */       \
    X(ElemPreInc, 0)                    \
    X(ElemPreDec, 0)                    \
    X(ElemPostInc, 0)                   \
    X(ElemPostDec, 0)                   \
    /* import.meta */                   \
    X(MetaProperty, 0)

enum class Opcode : uint8_t {
#define X(name, _operand_bytes) k##name,
    QPPJS_OPCODES(X)
#undef X
};

}  // namespace qppjs
