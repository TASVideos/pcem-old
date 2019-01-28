static inline int LOAD_SP_WITH_OFFSET(ir_data_t *ir, int offset)
{
        if (stack32)
        {
                if (offset)
                {
                        uop_ADD_IMM(ir, IREG_eaaddr, IREG_ESP, offset);
                        return IREG_eaaddr;
                }
                else
                        return IREG_ESP;
        }
        else
        {
                if (offset)
                {
                        uop_ADD_IMM(ir, IREG_eaaddr_W, IREG_SP, offset);
                        uop_MOVZX(ir, IREG_eaaddr, IREG_eaaddr_W);
                        return IREG_eaaddr;
                }
                else
                {
                        uop_MOVZX(ir, IREG_eaaddr, IREG_SP);
                        return IREG_eaaddr;
                }
        }
}

static inline int LOAD_SP(ir_data_t *ir)
{
        return LOAD_SP_WITH_OFFSET(ir, 0);
}

static inline void ADD_SP(ir_data_t *ir, int offset)
{
        if (stack32)
                uop_ADD_IMM(ir, IREG_ESP, IREG_ESP, offset);
        else
                uop_ADD_IMM(ir, IREG_SP, IREG_SP, offset);
}
static inline void SUB_SP(ir_data_t *ir, int offset)
{
        if (stack32)
                uop_SUB_IMM(ir, IREG_ESP, IREG_ESP, offset);
        else
                uop_SUB_IMM(ir, IREG_SP, IREG_SP, offset);
}

static inline void fpu_POP(codeblock_t *block, ir_data_t *ir)
{
        if (block->flags & CODEBLOCK_STATIC_TOP)
                uop_MOV_IMM(ir, IREG_FPU_TOP, cpu_state.TOP + 1);
        else
                uop_ADD_IMM(ir, IREG_FPU_TOP, IREG_FPU_TOP, 1);
}
static inline void fpu_POP2(codeblock_t *block, ir_data_t *ir)
{
        if (block->flags & CODEBLOCK_STATIC_TOP)
                uop_MOV_IMM(ir, IREG_FPU_TOP, cpu_state.TOP + 2);
        else
                uop_ADD_IMM(ir, IREG_FPU_TOP, IREG_FPU_TOP, 2);
}
static inline void fpu_PUSH(codeblock_t *block, ir_data_t *ir)
{
        if (block->flags & CODEBLOCK_STATIC_TOP)
                uop_MOV_IMM(ir, IREG_FPU_TOP, cpu_state.TOP - 1);
        else
                uop_SUB_IMM(ir, IREG_FPU_TOP, IREG_FPU_TOP, 1);
}
