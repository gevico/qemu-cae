/*
 * CAE RISC-V instruction classifier — table-driven decoder.
 *
 * Replaces the previous hand-written switch-case classifier with a
 * data-driven pipeline: decode_opcode() → table lookup → extract_operands().
 * Adding a new instruction only requires a new table row.
 *
 * References disas/riscv.c concepts (rv_op, rv_codec) but is fully
 * self-contained — does not link or include disassembler code.
 */
#include "qemu/osdep.h"
#include "qom/object.h"
#include "cae/uop.h"

#if defined(TARGET_RISCV32)
#define CAE_RISCV_XLEN 32
#else
#define CAE_RISCV_XLEN 64
#endif

/* ------------------------------------------------------------------ */
/*  Field extractors                                                   */
/* ------------------------------------------------------------------ */

#define RV_OPCODE(insn)  ((insn) & 0x7f)
#define RV_FUNCT3(insn)  (((insn) >> 12) & 0x7)
#define RV_FUNCT7(insn)  (((insn) >> 25) & 0x7f)
#define RV_RD(insn)      (((insn) >> 7) & 0x1f)
#define RV_RS1(insn)     (((insn) >> 15) & 0x1f)
#define RV_RS2(insn)     (((insn) >> 20) & 0x1f)

/* Compressed register fields */
#define RVC_RDP(insn)    ((uint8_t)(((insn) >> 2) & 0x7) + 8)
#define RVC_RS1P(insn)   ((uint8_t)(((insn) >> 7) & 0x7) + 8)
#define RVC_RS2P(insn)   ((uint8_t)(((insn) >> 2) & 0x7) + 8)
#define RVC_RD(insn)     ((uint8_t)(((insn) >> 7) & 0x1f))
#define RVC_RS2(insn)    ((uint8_t)(((insn) >> 2) & 0x1f))

/* ------------------------------------------------------------------ */
/*  Codec: how to extract operands from an instruction                 */
/* ------------------------------------------------------------------ */

typedef enum {
    CODEC_NONE,
    CODEC_R,        /* rd, rs1, rs2 */
    CODEC_I,        /* rd, rs1 */
    CODEC_S,        /* rs1, rs2 */
    CODEC_B,        /* rs1, rs2 */
    CODEC_U,        /* rd */
    CODEC_J,        /* rd */
    CODEC_R4,       /* rd, rs1, rs2, rs3 (FMA) */
    CODEC_CR,       /* rd/rs1, rs2 (compressed register) */
    CODEC_CI,       /* rd/rs1 (compressed immediate) */
    CODEC_CSS,      /* rs2 (compressed stack store, base=sp) */
    CODEC_CIW,      /* rd' (compressed wide imm, base=sp) */
    CODEC_CL,       /* rd', rs1' (compressed load) */
    CODEC_CS,       /* rs1', rs2' (compressed store) */
    CODEC_CB,       /* rs1' (compressed branch) */
    CODEC_CJ,       /* (compressed jump, no GPR src) */
    CODEC_C_MV,     /* rd, rs2 (c.mv: dst=rd, src=rs2) */
    CODEC_C_JR,     /* rs1 (c.jr: src=rs1, no dst) */
    CODEC_C_JALR,   /* rs1 (c.jalr: dst=x1, src=rs1) */
    CODEC_CI_SP,    /* rd (compressed load from sp) */
    CODEC_C_ALU2,   /* rd', rs2' (compressed 2-reg ALU: sub/xor/or/and) */
} CaeRvCodec;

/* ------------------------------------------------------------------ */
/*  Semantic flags                                                     */
/* ------------------------------------------------------------------ */

#define F_LOAD     (1u << 0)
#define F_STORE    (1u << 1)
#define F_BRANCH   (1u << 2)
#define F_CALL     (1u << 3)
#define F_RETURN   (1u << 4)
#define F_INDIRECT (1u << 5)
#define F_COND     (1u << 6)

/* ------------------------------------------------------------------ */
/*  CaeRvOp: CAE-owned opcode enumeration                             */
/* ------------------------------------------------------------------ */

typedef enum {
    CAE_RV_OP_UNKNOWN = 0,
    /* Base: U-type */
    CAE_RV_OP_LUI,
    CAE_RV_OP_AUIPC,
    /* Base: jumps */
    CAE_RV_OP_JAL,
    CAE_RV_OP_JALR,
    /* Base: branches */
    CAE_RV_OP_BEQ,
    CAE_RV_OP_BNE,
    CAE_RV_OP_BLT,
    CAE_RV_OP_BGE,
    CAE_RV_OP_BLTU,
    CAE_RV_OP_BGEU,
    /* Base: loads */
    CAE_RV_OP_LB,
    CAE_RV_OP_LH,
    CAE_RV_OP_LW,
    CAE_RV_OP_LBU,
    CAE_RV_OP_LHU,
    CAE_RV_OP_LWU,
    CAE_RV_OP_LD,
    /* Base: stores */
    CAE_RV_OP_SB,
    CAE_RV_OP_SH,
    CAE_RV_OP_SW,
    CAE_RV_OP_SD,
    /* Base: ALU immediate */
    CAE_RV_OP_ADDI,
    CAE_RV_OP_SLTI,
    CAE_RV_OP_SLTIU,
    CAE_RV_OP_XORI,
    CAE_RV_OP_ORI,
    CAE_RV_OP_ANDI,
    CAE_RV_OP_SLLI,
    CAE_RV_OP_SRLI,
    CAE_RV_OP_SRAI,
    /* Base: ALU register */
    CAE_RV_OP_ADD,
    CAE_RV_OP_SUB,
    CAE_RV_OP_SLL,
    CAE_RV_OP_SLT,
    CAE_RV_OP_SLTU,
    CAE_RV_OP_XOR,
    CAE_RV_OP_SRL,
    CAE_RV_OP_SRA,
    CAE_RV_OP_OR,
    CAE_RV_OP_AND,
    /* M extension (MUL..MULHU and DIV..REMU must stay contiguous) */
    CAE_RV_OP_MUL,
    CAE_RV_OP_MULH,
    CAE_RV_OP_MULHSU,
    CAE_RV_OP_MULHU,
    CAE_RV_OP_DIV,
    CAE_RV_OP_DIVU,
    CAE_RV_OP_REM,
    CAE_RV_OP_REMU,
    /* RV64 ALU */
    CAE_RV_OP_ADDIW,
    CAE_RV_OP_SLLIW,
    CAE_RV_OP_SRLIW,
    CAE_RV_OP_SRAIW,
    CAE_RV_OP_ADDW,
    CAE_RV_OP_SUBW,
    CAE_RV_OP_SLLW,
    CAE_RV_OP_SRLW,
    CAE_RV_OP_SRAW,
    CAE_RV_OP_MULW,
    CAE_RV_OP_DIVW,
    CAE_RV_OP_DIVUW,
    CAE_RV_OP_REMW,
    CAE_RV_OP_REMUW,
    /* System */
    CAE_RV_OP_FENCE,
    CAE_RV_OP_ECALL,
    CAE_RV_OP_EBREAK,
    CAE_RV_OP_CSR,
    /* Atomic */
    CAE_RV_OP_AMO,
    /* FP load/store */
    CAE_RV_OP_FLW,
    CAE_RV_OP_FLD,
    CAE_RV_OP_FSW,
    CAE_RV_OP_FSD,
    /* FP compute */
    CAE_RV_OP_FP_OP,
    CAE_RV_OP_FMADD,
    CAE_RV_OP_FMSUB,
    CAE_RV_OP_FNMSUB,
    CAE_RV_OP_FNMADD,
    /* Compressed Q0 */
    CAE_RV_OP_C_ADDI4SPN,
    CAE_RV_OP_C_FLD,
    CAE_RV_OP_C_LW,
    CAE_RV_OP_C_LD,
    CAE_RV_OP_C_FSD,
    CAE_RV_OP_C_SW,
    CAE_RV_OP_C_SD,
    CAE_RV_OP_C_FLW_RV32,
    CAE_RV_OP_C_FSW_RV32,
    /* Compressed Q1 */
    CAE_RV_OP_C_NOP,
    CAE_RV_OP_C_ADDI,
    CAE_RV_OP_C_ADDIW,
    CAE_RV_OP_C_JAL_RV32,
    CAE_RV_OP_C_LI,
    CAE_RV_OP_C_ADDI16SP,
    CAE_RV_OP_C_LUI,
    CAE_RV_OP_C_SRLI,
    CAE_RV_OP_C_SRAI,
    CAE_RV_OP_C_ANDI,
    CAE_RV_OP_C_SUB,
    CAE_RV_OP_C_XOR,
    CAE_RV_OP_C_OR,
    CAE_RV_OP_C_AND,
    CAE_RV_OP_C_SUBW,
    CAE_RV_OP_C_ADDW,
    CAE_RV_OP_C_J,
    CAE_RV_OP_C_BEQZ,
    CAE_RV_OP_C_BNEZ,
    /* Compressed Q2 */
    CAE_RV_OP_C_SLLI,
    CAE_RV_OP_C_FLDSP,
    CAE_RV_OP_C_LWSP,
    CAE_RV_OP_C_LDSP,
    CAE_RV_OP_C_FLWSP_RV32,
    CAE_RV_OP_C_JR,
    CAE_RV_OP_C_MV,
    CAE_RV_OP_C_EBREAK,
    CAE_RV_OP_C_JALR,
    CAE_RV_OP_C_ADD,
    CAE_RV_OP_C_FSDSP,
    CAE_RV_OP_C_SWSP,
    CAE_RV_OP_C_SDSP,
    CAE_RV_OP_C_FSWSP_RV32,
    /* Sentinel */
    CAE_RV_OP_COUNT,
} CaeRvOp;

/* ------------------------------------------------------------------ */
/*  Per-opcode semantic table entry                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    CaeUopType  uop_type;
    CaeFuType   fu_type;
    CaeRvCodec  codec;
    uint8_t     flags;
} CaeRvOpInfo;

/* Shorthand for common patterns */
#define ALU_R   { CAE_UOP_ALU,    CAE_FU_ALU,    CODEC_R,    0 }
#define ALU_I   { CAE_UOP_ALU,    CAE_FU_ALU,    CODEC_I,    0 }
#define ALU_U   { CAE_UOP_ALU,    CAE_FU_ALU,    CODEC_U,    0 }
#define LOAD_I  { CAE_UOP_LOAD,   CAE_FU_LOAD,   CODEC_I,    F_LOAD }
#define STORE_S { CAE_UOP_STORE,  CAE_FU_STORE,  CODEC_S,    F_STORE }
#define BR_B    { CAE_UOP_BRANCH, CAE_FU_BRANCH, CODEC_B,    F_BRANCH | F_COND }
#define MUL_R   { CAE_UOP_MUL,   CAE_FU_MUL,    CODEC_R,    0 }
#define DIV_R   { CAE_UOP_DIV,   CAE_FU_DIV,    CODEC_R,    0 }
#define FP_R    { CAE_UOP_FPU,   CAE_FU_FPU,    CODEC_R,    0 }
#define FP_I    { CAE_UOP_FPU,   CAE_FU_FPU,    CODEC_I,    0 }
#define FP_R4   { CAE_UOP_FPU,   CAE_FU_FPU,    CODEC_R4,   0 }
#define FP_LD   { CAE_UOP_LOAD,  CAE_FU_LOAD,   CODEC_I,    F_LOAD }
#define FP_ST   { CAE_UOP_STORE, CAE_FU_STORE,  CODEC_S,    F_STORE }
#define SYS_N   { CAE_UOP_SYSTEM,CAE_FU_SYSTEM, CODEC_NONE, 0 }
#define ATOM_R  { CAE_UOP_ATOMIC,CAE_FU_LOAD,   CODEC_R,    F_LOAD | F_STORE }
#define UNK     { CAE_UOP_UNKNOWN, CAE_FU_NONE, CODEC_NONE, 0 }

static const CaeRvOpInfo op_table[CAE_RV_OP_COUNT] = {
    [CAE_RV_OP_UNKNOWN]      = UNK,
    /* Base: U-type */
    [CAE_RV_OP_LUI]          = ALU_U,
    [CAE_RV_OP_AUIPC]        = ALU_U,
    /* Base: JAL/JALR — flags set by special handling */
    [CAE_RV_OP_JAL]          = { CAE_UOP_BRANCH, CAE_FU_BRANCH, CODEC_J, F_BRANCH },
    [CAE_RV_OP_JALR]         = { CAE_UOP_BRANCH, CAE_FU_BRANCH, CODEC_I, F_BRANCH | F_INDIRECT },
    /* Base: branches */
    [CAE_RV_OP_BEQ]   = BR_B, [CAE_RV_OP_BNE]  = BR_B, [CAE_RV_OP_BLT]  = BR_B,
    [CAE_RV_OP_BGE]   = BR_B, [CAE_RV_OP_BLTU] = BR_B, [CAE_RV_OP_BGEU] = BR_B,
    /* Base: loads */
    [CAE_RV_OP_LB]  = LOAD_I, [CAE_RV_OP_LH]  = LOAD_I, [CAE_RV_OP_LW]  = LOAD_I,
    [CAE_RV_OP_LBU] = LOAD_I, [CAE_RV_OP_LHU] = LOAD_I,
    [CAE_RV_OP_LWU] = LOAD_I, [CAE_RV_OP_LD]  = LOAD_I,
    /* Base: stores */
    [CAE_RV_OP_SB] = STORE_S, [CAE_RV_OP_SH] = STORE_S,
    [CAE_RV_OP_SW] = STORE_S, [CAE_RV_OP_SD] = STORE_S,
    /* Base: ALU immediate */
    [CAE_RV_OP_ADDI]  = ALU_I, [CAE_RV_OP_SLTI]  = ALU_I, [CAE_RV_OP_SLTIU] = ALU_I,
    [CAE_RV_OP_XORI]  = ALU_I, [CAE_RV_OP_ORI]   = ALU_I, [CAE_RV_OP_ANDI]  = ALU_I,
    [CAE_RV_OP_SLLI]  = ALU_I, [CAE_RV_OP_SRLI]  = ALU_I, [CAE_RV_OP_SRAI]  = ALU_I,
    /* Base: ALU register */
    [CAE_RV_OP_ADD] = ALU_R, [CAE_RV_OP_SUB] = ALU_R,
    [CAE_RV_OP_SLL] = ALU_R, [CAE_RV_OP_SLT] = ALU_R, [CAE_RV_OP_SLTU] = ALU_R,
    [CAE_RV_OP_XOR] = ALU_R, [CAE_RV_OP_SRL] = ALU_R, [CAE_RV_OP_SRA]  = ALU_R,
    [CAE_RV_OP_OR]  = ALU_R, [CAE_RV_OP_AND] = ALU_R,
    /* M extension */
    [CAE_RV_OP_MUL]    = MUL_R, [CAE_RV_OP_MULH]   = MUL_R,
    [CAE_RV_OP_MULHSU] = MUL_R, [CAE_RV_OP_MULHU]  = MUL_R,
    [CAE_RV_OP_DIV]    = DIV_R, [CAE_RV_OP_DIVU]   = DIV_R,
    [CAE_RV_OP_REM]    = DIV_R, [CAE_RV_OP_REMU]   = DIV_R,
    /* RV64 ALU */
    [CAE_RV_OP_ADDIW] = ALU_I, [CAE_RV_OP_SLLIW] = ALU_I,
    [CAE_RV_OP_SRLIW] = ALU_I, [CAE_RV_OP_SRAIW] = ALU_I,
    [CAE_RV_OP_ADDW]  = ALU_R, [CAE_RV_OP_SUBW]  = ALU_R,
    [CAE_RV_OP_SLLW]  = ALU_R, [CAE_RV_OP_SRLW]  = ALU_R, [CAE_RV_OP_SRAW] = ALU_R,
    [CAE_RV_OP_MULW]  = MUL_R, [CAE_RV_OP_DIVW]  = DIV_R,
    [CAE_RV_OP_DIVUW] = DIV_R, [CAE_RV_OP_REMW]  = DIV_R, [CAE_RV_OP_REMUW] = DIV_R,
    /* System */
    [CAE_RV_OP_FENCE]  = { CAE_UOP_FENCE, CAE_FU_SYSTEM, CODEC_NONE, 0 },
    [CAE_RV_OP_ECALL]  = SYS_N, [CAE_RV_OP_EBREAK] = SYS_N,
    [CAE_RV_OP_CSR]    = { CAE_UOP_SYSTEM, CAE_FU_SYSTEM, CODEC_I, 0 },
    /* Atomic */
    [CAE_RV_OP_AMO]    = ATOM_R,
    /* FP load/store */
    [CAE_RV_OP_FLW] = FP_LD, [CAE_RV_OP_FLD] = FP_LD,
    [CAE_RV_OP_FSW] = FP_ST, [CAE_RV_OP_FSD] = FP_ST,
    /* FP compute */
    [CAE_RV_OP_FP_OP]  = FP_R,
    [CAE_RV_OP_FMADD]  = FP_R4, [CAE_RV_OP_FMSUB]  = FP_R4,
    [CAE_RV_OP_FNMSUB] = FP_R4, [CAE_RV_OP_FNMADD] = FP_R4,

    /* Compressed Q0 */
    [CAE_RV_OP_C_ADDI4SPN] = { CAE_UOP_ALU,   CAE_FU_ALU,   CODEC_CIW, 0 },
    [CAE_RV_OP_C_FLD]      = { CAE_UOP_LOAD,  CAE_FU_LOAD,  CODEC_CL,  F_LOAD },
    [CAE_RV_OP_C_LW]       = { CAE_UOP_LOAD,  CAE_FU_LOAD,  CODEC_CL,  F_LOAD },
    [CAE_RV_OP_C_LD]       = { CAE_UOP_LOAD,  CAE_FU_LOAD,  CODEC_CL,  F_LOAD },
    [CAE_RV_OP_C_FSD]      = { CAE_UOP_STORE, CAE_FU_STORE, CODEC_CS,  F_STORE },
    [CAE_RV_OP_C_SW]       = { CAE_UOP_STORE, CAE_FU_STORE, CODEC_CS,  F_STORE },
    [CAE_RV_OP_C_SD]       = { CAE_UOP_STORE, CAE_FU_STORE, CODEC_CS,  F_STORE },
    [CAE_RV_OP_C_FLW_RV32] = { CAE_UOP_LOAD,  CAE_FU_LOAD,  CODEC_CL,  F_LOAD },
    [CAE_RV_OP_C_FSW_RV32] = { CAE_UOP_STORE, CAE_FU_STORE, CODEC_CS,  F_STORE },
    /* Compressed Q1 */
    [CAE_RV_OP_C_NOP]      = { CAE_UOP_ALU,   CAE_FU_ALU,   CODEC_NONE, 0 },
    [CAE_RV_OP_C_ADDI]     = { CAE_UOP_ALU,   CAE_FU_ALU,   CODEC_CI,   0 },
    [CAE_RV_OP_C_ADDIW]    = { CAE_UOP_ALU,   CAE_FU_ALU,   CODEC_CI,   0 },
    [CAE_RV_OP_C_JAL_RV32] = { CAE_UOP_BRANCH,CAE_FU_BRANCH,CODEC_CJ,   F_BRANCH | F_CALL },
    [CAE_RV_OP_C_LI]       = { CAE_UOP_ALU,   CAE_FU_ALU,   CODEC_CI,   0 },
    [CAE_RV_OP_C_ADDI16SP] = { CAE_UOP_ALU,   CAE_FU_ALU,   CODEC_CI,   0 },
    [CAE_RV_OP_C_LUI]      = { CAE_UOP_ALU,   CAE_FU_ALU,   CODEC_CI,   0 },
    [CAE_RV_OP_C_SRLI]     = { CAE_UOP_ALU,   CAE_FU_ALU,   CODEC_CI,   0 },
    [CAE_RV_OP_C_SRAI]     = { CAE_UOP_ALU,   CAE_FU_ALU,   CODEC_CI,   0 },
    [CAE_RV_OP_C_ANDI]     = { CAE_UOP_ALU,   CAE_FU_ALU,   CODEC_CI,   0 },
    [CAE_RV_OP_C_SUB]      = { CAE_UOP_ALU,   CAE_FU_ALU,   CODEC_C_ALU2, 0 },
    [CAE_RV_OP_C_XOR]      = { CAE_UOP_ALU,   CAE_FU_ALU,   CODEC_C_ALU2, 0 },
    [CAE_RV_OP_C_OR]       = { CAE_UOP_ALU,   CAE_FU_ALU,   CODEC_C_ALU2, 0 },
    [CAE_RV_OP_C_AND]      = { CAE_UOP_ALU,   CAE_FU_ALU,   CODEC_C_ALU2, 0 },
    [CAE_RV_OP_C_SUBW]     = { CAE_UOP_ALU,   CAE_FU_ALU,   CODEC_C_ALU2, 0 },
    [CAE_RV_OP_C_ADDW]     = { CAE_UOP_ALU,   CAE_FU_ALU,   CODEC_C_ALU2, 0 },
    [CAE_RV_OP_C_J]        = { CAE_UOP_BRANCH,CAE_FU_BRANCH,CODEC_CJ,   F_BRANCH },
    [CAE_RV_OP_C_BEQZ]     = { CAE_UOP_BRANCH,CAE_FU_BRANCH,CODEC_CB,   F_BRANCH | F_COND },
    [CAE_RV_OP_C_BNEZ]     = { CAE_UOP_BRANCH,CAE_FU_BRANCH,CODEC_CB,   F_BRANCH | F_COND },
    /* Compressed Q2 */
    [CAE_RV_OP_C_SLLI]      = { CAE_UOP_ALU,   CAE_FU_ALU,   CODEC_CI,   0 },
    [CAE_RV_OP_C_FLDSP]     = { CAE_UOP_LOAD,  CAE_FU_LOAD,  CODEC_CI_SP,F_LOAD },
    [CAE_RV_OP_C_LWSP]      = { CAE_UOP_LOAD,  CAE_FU_LOAD,  CODEC_CI_SP,F_LOAD },
    [CAE_RV_OP_C_LDSP]      = { CAE_UOP_LOAD,  CAE_FU_LOAD,  CODEC_CI_SP,F_LOAD },
    [CAE_RV_OP_C_FLWSP_RV32]= { CAE_UOP_LOAD,  CAE_FU_LOAD,  CODEC_CI_SP,F_LOAD },
    [CAE_RV_OP_C_JR]        = { CAE_UOP_BRANCH,CAE_FU_BRANCH,CODEC_C_JR, F_BRANCH | F_INDIRECT },
    [CAE_RV_OP_C_MV]        = { CAE_UOP_ALU,   CAE_FU_ALU,   CODEC_C_MV, 0 },
    [CAE_RV_OP_C_EBREAK]    = SYS_N,
    [CAE_RV_OP_C_JALR]      = { CAE_UOP_BRANCH,CAE_FU_BRANCH,CODEC_C_JALR,F_BRANCH | F_INDIRECT | F_CALL },
    [CAE_RV_OP_C_ADD]       = { CAE_UOP_ALU,   CAE_FU_ALU,   CODEC_CR,   0 },
    [CAE_RV_OP_C_FSDSP]     = { CAE_UOP_STORE, CAE_FU_STORE, CODEC_CSS,  F_STORE },
    [CAE_RV_OP_C_SWSP]      = { CAE_UOP_STORE, CAE_FU_STORE, CODEC_CSS,  F_STORE },
    [CAE_RV_OP_C_SDSP]      = { CAE_UOP_STORE, CAE_FU_STORE, CODEC_CSS,  F_STORE },
    [CAE_RV_OP_C_FSWSP_RV32]= { CAE_UOP_STORE, CAE_FU_STORE, CODEC_CSS,  F_STORE },
};

/* ------------------------------------------------------------------ */
/*  decode: instruction bits → CaeRvOp                                 */
/* ------------------------------------------------------------------ */

static bool is_mul_div(uint32_t insn, uint32_t opcode)
{
    return (opcode == 0x33 || opcode == 0x3B) &&
           (RV_FUNCT7(insn) & 0x01);
}

static CaeRvOp decode_base(uint32_t insn)
{
    uint32_t op = RV_OPCODE(insn);
    uint32_t f3 = RV_FUNCT3(insn);
    uint32_t f7 = RV_FUNCT7(insn);

    switch (op) {
    case 0x37: return CAE_RV_OP_LUI;
    case 0x17: return CAE_RV_OP_AUIPC;
    case 0x6F: return CAE_RV_OP_JAL;
    case 0x67: return CAE_RV_OP_JALR;
    case 0x63:
        switch (f3) {
        case 0: return CAE_RV_OP_BEQ;  case 1: return CAE_RV_OP_BNE;
        case 4: return CAE_RV_OP_BLT;  case 5: return CAE_RV_OP_BGE;
        case 6: return CAE_RV_OP_BLTU; case 7: return CAE_RV_OP_BGEU;
        default: return CAE_RV_OP_UNKNOWN;
        }
    case 0x03:
        switch (f3) {
        case 0: return CAE_RV_OP_LB;  case 1: return CAE_RV_OP_LH;
        case 2: return CAE_RV_OP_LW;  case 3: return CAE_RV_OP_LD;
        case 4: return CAE_RV_OP_LBU; case 5: return CAE_RV_OP_LHU;
        case 6: return CAE_RV_OP_LWU;
        default: return CAE_RV_OP_UNKNOWN;
        }
    case 0x23:
        switch (f3) {
        case 0: return CAE_RV_OP_SB; case 1: return CAE_RV_OP_SH;
        case 2: return CAE_RV_OP_SW; case 3: return CAE_RV_OP_SD;
        default: return CAE_RV_OP_UNKNOWN;
        }
    case 0x13:
        switch (f3) {
        case 0: return CAE_RV_OP_ADDI; case 1: return CAE_RV_OP_SLLI;
        case 2: return CAE_RV_OP_SLTI; case 3: return CAE_RV_OP_SLTIU;
        case 4: return CAE_RV_OP_XORI;
        case 5: return (f7 & 0x20) ? CAE_RV_OP_SRAI : CAE_RV_OP_SRLI;
        case 6: return CAE_RV_OP_ORI;  case 7: return CAE_RV_OP_ANDI;
        default: return CAE_RV_OP_UNKNOWN;
        }
    case 0x33:
        if (is_mul_div(insn, op)) {
            return (f3 < 4) ? CAE_RV_OP_MUL + f3
                            : CAE_RV_OP_DIV + (f3 - 4);
        }
        switch (f3) {
        case 0: return (f7 & 0x20) ? CAE_RV_OP_SUB : CAE_RV_OP_ADD;
        case 1: return CAE_RV_OP_SLL; case 2: return CAE_RV_OP_SLT;
        case 3: return CAE_RV_OP_SLTU; case 4: return CAE_RV_OP_XOR;
        case 5: return (f7 & 0x20) ? CAE_RV_OP_SRA : CAE_RV_OP_SRL;
        case 6: return CAE_RV_OP_OR; case 7: return CAE_RV_OP_AND;
        default: return CAE_RV_OP_UNKNOWN;
        }
    case 0x1B:
        switch (f3) {
        case 0: return CAE_RV_OP_ADDIW; case 1: return CAE_RV_OP_SLLIW;
        case 5: return (f7 & 0x20) ? CAE_RV_OP_SRAIW : CAE_RV_OP_SRLIW;
        default: return CAE_RV_OP_UNKNOWN;
        }
    case 0x3B:
        if (is_mul_div(insn, op)) {
            switch (f3) {
            case 0: return CAE_RV_OP_MULW; case 4: return CAE_RV_OP_DIVW;
            case 5: return CAE_RV_OP_DIVUW; case 6: return CAE_RV_OP_REMW;
            case 7: return CAE_RV_OP_REMUW;
            default: return CAE_RV_OP_UNKNOWN;
            }
        }
        switch (f3) {
        case 0: return (f7 & 0x20) ? CAE_RV_OP_SUBW : CAE_RV_OP_ADDW;
        case 1: return CAE_RV_OP_SLLW;
        case 5: return (f7 & 0x20) ? CAE_RV_OP_SRAW : CAE_RV_OP_SRLW;
        default: return CAE_RV_OP_UNKNOWN;
        }
    case 0x0F: return CAE_RV_OP_FENCE;
    case 0x73:
        if (f3 == 0) {
            uint32_t imm12 = insn >> 20;
            return (imm12 == 0) ? CAE_RV_OP_ECALL : CAE_RV_OP_EBREAK;
        }
        return CAE_RV_OP_CSR;
    case 0x2F: return CAE_RV_OP_AMO;
    case 0x07:
        if (f3 == 2) return CAE_RV_OP_FLW;
        if (f3 == 3) return CAE_RV_OP_FLD;
        return CAE_RV_OP_UNKNOWN;
    case 0x27:
        if (f3 == 2) return CAE_RV_OP_FSW;
        if (f3 == 3) return CAE_RV_OP_FSD;
        return CAE_RV_OP_UNKNOWN;
    case 0x53: return CAE_RV_OP_FP_OP;
    case 0x43: return CAE_RV_OP_FMADD;
    case 0x47: return CAE_RV_OP_FMSUB;
    case 0x4B: return CAE_RV_OP_FNMSUB;
    case 0x4F: return CAE_RV_OP_FNMADD;
    default: return CAE_RV_OP_UNKNOWN;
    }
}

static CaeRvOp decode_compressed(uint16_t insn)
{
    uint8_t q = insn & 0x3;
    uint8_t f3 = (insn >> 13) & 0x7;

    switch (q) {
    case 0:
        switch (f3) {
        case 0:
            if ((insn & 0x1FE0) == 0) return CAE_RV_OP_UNKNOWN;
            return CAE_RV_OP_C_ADDI4SPN;
        case 1: return CAE_RV_OP_C_FLD;
        case 2: return CAE_RV_OP_C_LW;
        case 3: return (CAE_RISCV_XLEN == 32) ? CAE_RV_OP_C_FLW_RV32
                                               : CAE_RV_OP_C_LD;
        case 5: return CAE_RV_OP_C_FSD;
        case 6: return CAE_RV_OP_C_SW;
        case 7: return (CAE_RISCV_XLEN == 32) ? CAE_RV_OP_C_FSW_RV32
                                               : CAE_RV_OP_C_SD;
        default: return CAE_RV_OP_UNKNOWN;
        }
    case 1:
        switch (f3) {
        case 0: {
            uint8_t rd = RVC_RD(insn);
            return (rd == 0) ? CAE_RV_OP_C_NOP : CAE_RV_OP_C_ADDI;
        }
        case 1:
            return (CAE_RISCV_XLEN == 32) ? CAE_RV_OP_C_JAL_RV32
                                          : CAE_RV_OP_C_ADDIW;
        case 2: return CAE_RV_OP_C_LI;
        case 3: {
            uint8_t rd = RVC_RD(insn);
            return (rd == 2) ? CAE_RV_OP_C_ADDI16SP : CAE_RV_OP_C_LUI;
        }
        case 4: {
            uint8_t f2 = (insn >> 10) & 0x3;
            if (f2 == 0) return CAE_RV_OP_C_SRLI;
            if (f2 == 1) return CAE_RV_OP_C_SRAI;
            if (f2 == 2) return CAE_RV_OP_C_ANDI;
            /* f2 == 3: SUB/XOR/OR/AND/SUBW/ADDW */
            uint8_t sf = (insn >> 5) & 0x3;
            uint8_t bit12 = (insn >> 12) & 1;
            if (bit12 == 0) {
                switch (sf) {
                case 0: return CAE_RV_OP_C_SUB;
                case 1: return CAE_RV_OP_C_XOR;
                case 2: return CAE_RV_OP_C_OR;
                case 3: return CAE_RV_OP_C_AND;
                }
            } else {
                switch (sf) {
                case 0: return CAE_RV_OP_C_SUBW;
                case 1: return CAE_RV_OP_C_ADDW;
                default: return CAE_RV_OP_UNKNOWN;
                }
            }
            return CAE_RV_OP_UNKNOWN;
        }
        case 5: return CAE_RV_OP_C_J;
        case 6: return CAE_RV_OP_C_BEQZ;
        case 7: return CAE_RV_OP_C_BNEZ;
        default: return CAE_RV_OP_UNKNOWN;
        }
    case 2:
        switch (f3) {
        case 0: return CAE_RV_OP_C_SLLI;
        case 1: return CAE_RV_OP_C_FLDSP;
        case 2: {
            uint8_t rd = RVC_RD(insn);
            if (rd == 0) return CAE_RV_OP_UNKNOWN;
            return CAE_RV_OP_C_LWSP;
        }
        case 3:
            if (CAE_RISCV_XLEN == 32) return CAE_RV_OP_C_FLWSP_RV32;
            if (RVC_RD(insn) == 0) return CAE_RV_OP_UNKNOWN;
            return CAE_RV_OP_C_LDSP;
        case 4: {
            uint8_t bit12 = (insn >> 12) & 1;
            uint8_t rd_rs1 = RVC_RD(insn);
            uint8_t rs2 = RVC_RS2(insn);
            if (bit12 == 0) {
                if (rs2 == 0) {
                    if (rd_rs1 == 0) return CAE_RV_OP_UNKNOWN;
                    return CAE_RV_OP_C_JR;
                }
                return CAE_RV_OP_C_MV;
            }
            if (rs2 == 0) {
                return (rd_rs1 == 0) ? CAE_RV_OP_C_EBREAK
                                     : CAE_RV_OP_C_JALR;
            }
            return CAE_RV_OP_C_ADD;
        }
        case 5: return CAE_RV_OP_C_FSDSP;
        case 6: return CAE_RV_OP_C_SWSP;
        case 7: return (CAE_RISCV_XLEN == 32) ? CAE_RV_OP_C_FSWSP_RV32
                                               : CAE_RV_OP_C_SDSP;
        default: return CAE_RV_OP_UNKNOWN;
        }
    default:
        return CAE_RV_OP_UNKNOWN;
    }
}

/* ------------------------------------------------------------------ */
/*  extract_operands: codec → src_regs/dst_regs                        */
/* ------------------------------------------------------------------ */

static void extract_operands(CaeUop *uop, uint32_t insn, CaeRvCodec codec)
{
    switch (codec) {
    case CODEC_R:
        uop->dst_regs[0] = RV_RD(insn);
        uop->src_regs[0] = RV_RS1(insn);
        uop->src_regs[1] = RV_RS2(insn);
        if (uop->dst_regs[0]) uop->num_dst = 1;
        uop->num_src = 2;
        break;
    case CODEC_I:
        uop->dst_regs[0] = RV_RD(insn);
        uop->src_regs[0] = RV_RS1(insn);
        if (uop->dst_regs[0]) uop->num_dst = 1;
        uop->num_src = 1;
        break;
    case CODEC_S:
        uop->src_regs[0] = RV_RS1(insn);
        uop->src_regs[1] = RV_RS2(insn);
        uop->num_src = 2;
        break;
    case CODEC_B:
        uop->src_regs[0] = RV_RS1(insn);
        uop->src_regs[1] = RV_RS2(insn);
        uop->num_src = 2;
        break;
    case CODEC_U:
        uop->dst_regs[0] = RV_RD(insn);
        if (uop->dst_regs[0]) uop->num_dst = 1;
        break;
    case CODEC_J:
        uop->dst_regs[0] = RV_RD(insn);
        if (uop->dst_regs[0]) uop->num_dst = 1;
        break;
    case CODEC_R4:
        uop->dst_regs[0] = RV_RD(insn);
        uop->src_regs[0] = RV_RS1(insn);
        uop->src_regs[1] = RV_RS2(insn);
        if (uop->dst_regs[0]) uop->num_dst = 1;
        uop->num_src = 2;
        break;
    case CODEC_CR:  /* c.add: rd/rs1, rs2 */
        uop->dst_regs[0] = RVC_RD(insn);
        uop->src_regs[0] = RVC_RD(insn);
        uop->src_regs[1] = RVC_RS2(insn);
        if (uop->dst_regs[0]) uop->num_dst = 1;
        uop->num_src = 2;
        break;
    case CODEC_CI:  /* c.addi, c.li, c.lui, c.slli, etc: rd (=src for addi) */
    {
        uint8_t rd = RVC_RD(insn);
        uop->dst_regs[0] = rd;
        if (rd) {
            uop->num_dst = 1;
            uop->num_src = 1;
            uop->src_regs[0] = rd;
        }
        break;
    }
    case CODEC_CSS:  /* c.swsp/c.sdsp: base=x2, data=rs2 */
        uop->src_regs[0] = 2;
        uop->src_regs[1] = RVC_RS2(insn);
        uop->num_src = 2;
        break;
    case CODEC_CIW:  /* c.addi4spn: rd'=dst, src=x2(sp) */
        uop->dst_regs[0] = RVC_RDP(insn);
        if (uop->dst_regs[0]) uop->num_dst = 1;
        uop->src_regs[0] = 2;
        uop->num_src = 1;
        break;
    case CODEC_CL:   /* c.lw/c.ld: rd'=dst, rs1'=base */
        uop->dst_regs[0] = RVC_RDP(insn);
        uop->src_regs[0] = RVC_RS1P(insn);
        if (uop->dst_regs[0]) uop->num_dst = 1;
        uop->num_src = 1;
        break;
    case CODEC_CS:   /* c.sw/c.sd: rs1'=base, rs2'=data */
        uop->src_regs[0] = RVC_RS1P(insn);
        uop->src_regs[1] = RVC_RDP(insn);
        uop->num_src = 2;
        break;
    case CODEC_CB:   /* c.beqz/c.bnez: rs1' */
        uop->src_regs[0] = RVC_RS1P(insn);
        uop->num_src = 1;
        break;
    case CODEC_CJ:   /* c.j, c.jal: no GPR source */
        break;
    case CODEC_C_MV:  /* c.mv: dst=rd, src=rs2 */
        uop->dst_regs[0] = RVC_RD(insn);
        uop->src_regs[0] = RVC_RS2(insn);
        if (uop->dst_regs[0]) uop->num_dst = 1;
        uop->num_src = 1;
        break;
    case CODEC_C_JR:  /* c.jr: src=rs1, no dst */
        uop->src_regs[0] = RVC_RD(insn);
        uop->num_src = 1;
        break;
    case CODEC_C_JALR: /* c.jalr: dst=x1, src=rs1 */
        uop->dst_regs[0] = 1;
        uop->num_dst = 1;
        uop->src_regs[0] = RVC_RD(insn);
        uop->num_src = 1;
        break;
    case CODEC_CI_SP:  /* c.lwsp/c.ldsp: rd=dst, src=x2(sp) */
        uop->dst_regs[0] = RVC_RD(insn);
        if (uop->dst_regs[0]) uop->num_dst = 1;
        uop->src_regs[0] = 2;
        uop->num_src = 1;
        break;
    case CODEC_C_ALU2: /* c.sub/xor/or/and: rd'=dst+src1, rs2'=src2 */
    {
        uint8_t rdp = RVC_RS1P(insn);
        uop->dst_regs[0] = rdp;
        uop->src_regs[0] = rdp;
        uop->src_regs[1] = RVC_RDP(insn);
        if (rdp) uop->num_dst = 1;
        uop->num_src = 2;
        break;
    }
    case CODEC_NONE:
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/*  Special semantic fixups after table lookup                         */
/* ------------------------------------------------------------------ */

static void fixup_branch_hints(CaeUop *uop, CaeRvOp op, uint32_t insn)
{
    uop->is_branch = true;

    if (op == CAE_RV_OP_JAL) {
        uint8_t rd = RV_RD(insn);
        uop->is_call = (rd == 1 || rd == 5);
        uop->is_indirect = false;
        uop->is_conditional = false;
    } else if (op == CAE_RV_OP_JALR) {
        uint8_t rd = RV_RD(insn);
        uint8_t rs1 = RV_RS1(insn);
        uop->is_indirect = true;
        uop->is_conditional = false;
        if (rd == 0 && (rs1 == 1 || rs1 == 5)) {
            uop->is_return = true;
        } else if (rd == 1 || rd == 5) {
            uop->is_call = true;
        }
    } else if (op == CAE_RV_OP_C_JR) {
        uint8_t rs1 = RVC_RD(insn);
        uop->is_indirect = true;
        uop->is_return = (rs1 == 1 || rs1 == 5);
    } else if (op == CAE_RV_OP_C_JALR) {
        uop->is_indirect = true;
        uop->is_call = true;
    } else if (op == CAE_RV_OP_C_JAL_RV32) {
        uop->is_call = true;
        uop->dst_regs[0] = 1;
        uop->num_dst = 1;
    } else {
        const CaeRvOpInfo *info = &op_table[op];
        uop->is_conditional = !!(info->flags & F_COND);
    }
}

/* ------------------------------------------------------------------ */
/*  Public classifier entry point                                      */
/* ------------------------------------------------------------------ */

static bool riscv_cae_uop_classify(CaeUop *uop, uint64_t pc,
                                   const uint8_t *insn_buf,
                                   size_t insn_bytes)
{
    uint32_t insn = 0;
    if (insn_bytes >= 1) insn = insn_buf[0];
    if (insn_bytes >= 2) insn |= (uint32_t)insn_buf[1] << 8;
    if (insn_bytes >= 3) insn |= (uint32_t)insn_buf[2] << 16;
    if (insn_bytes >= 4) insn |= (uint32_t)insn_buf[3] << 24;

    uop->pc = pc;
    CaeRvOp op;

    if ((insn & 0x3) != 0x3) {
        if (insn_bytes < 2) return false;
        uop->insn = insn & 0xffffu;
        uop->insn_bytes = 2;
        op = decode_compressed((uint16_t)(insn & 0xffff));
    } else {
        if (insn_bytes < 4) return false;
        uop->insn = insn;
        uop->insn_bytes = 4;
        op = decode_base(insn);
    }

    if (op >= CAE_RV_OP_COUNT) op = CAE_RV_OP_UNKNOWN;
    const CaeRvOpInfo *info = &op_table[op];

    uop->type = info->uop_type;
    uop->fu_type = info->fu_type;
    uop->is_load  = !!(info->flags & F_LOAD);
    uop->is_store = !!(info->flags & F_STORE);

    extract_operands(uop, insn, info->codec);

    if (info->flags & F_BRANCH) {
        fixup_branch_hints(uop, op, insn);
    }

    /* C.LI: src is x0 (immediate load), not rd */
    if (op == CAE_RV_OP_C_LI) {
        uop->num_src = 0;
    }
    /* C.ADDI16SP: src=x2(sp) specifically */
    if (op == CAE_RV_OP_C_ADDI16SP) {
        uop->src_regs[0] = 2;
    }

    uop->latency = cae_uop_default_latency(uop->type);
    return true;
}

static const CaeUopClassifier riscv_cae_uop_classifier = {
    .max_insn_bytes = 4,
    .classify = riscv_cae_uop_classify,
};

static void riscv_cae_uop_register(void)
{
    cae_uop_register_classifier(&riscv_cae_uop_classifier);
}

type_init(riscv_cae_uop_register);
