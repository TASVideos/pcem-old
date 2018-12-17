#include "ibm.h"
#include "codegen.h"
#include "codegen_backend.h"
#include "codegen_ir_defs.h"
#include "codegen_reg.h"

uint8_t reg_last_version[IREG_COUNT];
uint8_t reg_version_refcount[IREG_COUNT][256];

ir_reg_t invalid_ir_reg = {IREG_INVALID};

ir_reg_t _host_regs[CODEGEN_HOST_REGS];
static uint8_t _host_reg_dirty[CODEGEN_HOST_REGS];

ir_reg_t host_fp_regs[CODEGEN_HOST_FP_REGS];
static uint8_t host_fp_reg_dirty[CODEGEN_HOST_FP_REGS];

typedef struct host_reg_set_t
{
        ir_reg_t *regs;
        uint8_t *dirty;
        int *reg_list;
        uint16_t locked;
        int nr_regs;
} host_reg_set_t;

static host_reg_set_t host_reg_set, host_fp_reg_set;

enum
{
        REG_BYTE,
        REG_WORD,
        REG_DWORD,
        REG_QWORD,
        REG_POINTER,
        REG_DOUBLE,
        REG_FPU_ST_BYTE,
        REG_FPU_ST_DOUBLE,
        REG_FPU_ST_QWORD
};

enum
{
        REG_INTEGER,
        REG_FP
};

struct
{
        int native_size;
        void *p;
        int type;
} ireg_data[IREG_COUNT] =
{
        [IREG_EAX] = {REG_DWORD, &EAX, REG_INTEGER},
	[IREG_ECX] = {REG_DWORD, &ECX, REG_INTEGER},
	[IREG_EDX] = {REG_DWORD, &EDX, REG_INTEGER},
	[IREG_EBX] = {REG_DWORD, &EBX, REG_INTEGER},
	[IREG_ESP] = {REG_DWORD, &ESP, REG_INTEGER},
	[IREG_EBP] = {REG_DWORD, &EBP, REG_INTEGER},
	[IREG_ESI] = {REG_DWORD, &ESI, REG_INTEGER},
	[IREG_EDI] = {REG_DWORD, &EDI, REG_INTEGER},

	[IREG_flags_op]  = {REG_DWORD, &cpu_state.flags_op,  REG_INTEGER},
	[IREG_flags_res] = {REG_DWORD, &cpu_state.flags_res, REG_INTEGER},
	[IREG_flags_op1] = {REG_DWORD, &cpu_state.flags_op1, REG_INTEGER},
	[IREG_flags_op2] = {REG_DWORD, &cpu_state.flags_op2, REG_INTEGER},

	[IREG_pc]    = {REG_DWORD, &cpu_state.pc,    REG_INTEGER},
	[IREG_oldpc] = {REG_DWORD, &cpu_state.oldpc, REG_INTEGER},

	[IREG_eaaddr] = {REG_DWORD, &cpu_state.eaaddr,   REG_INTEGER},
	[IREG_ea_seg] = {REG_POINTER, &cpu_state.ea_seg, REG_INTEGER},

	[IREG_op32] = {REG_DWORD, &cpu_state.op32,  REG_INTEGER},
	[IREG_ssegsx] = {REG_BYTE, &cpu_state.ssegs, REG_INTEGER},
	
	[IREG_rm_mod_reg] = {REG_DWORD, &cpu_state.rm_data.rm_mod_reg_data, REG_INTEGER},

	[IREG_ins]    = {REG_DWORD, &cpu_state.cpu_recomp_ins, REG_INTEGER},
	[IREG_cycles] = {REG_DWORD, &cpu_state._cycles,        REG_INTEGER},
	
	[IREG_CS_base] = {REG_DWORD, &cpu_state.seg_cs.base, REG_INTEGER},
	[IREG_DS_base] = {REG_DWORD, &cpu_state.seg_ds.base, REG_INTEGER},
	[IREG_ES_base] = {REG_DWORD, &cpu_state.seg_es.base, REG_INTEGER},
	[IREG_FS_base] = {REG_DWORD, &cpu_state.seg_fs.base, REG_INTEGER},
	[IREG_GS_base] = {REG_DWORD, &cpu_state.seg_gs.base, REG_INTEGER},
	[IREG_SS_base] = {REG_DWORD, &cpu_state.seg_ss.base, REG_INTEGER},

	[IREG_CS_seg] = {REG_WORD, &cpu_state.seg_cs.seg, REG_INTEGER},
	[IREG_DS_seg] = {REG_WORD, &cpu_state.seg_ds.seg, REG_INTEGER},
	[IREG_ES_seg] = {REG_WORD, &cpu_state.seg_es.seg, REG_INTEGER},
	[IREG_FS_seg] = {REG_WORD, &cpu_state.seg_fs.seg, REG_INTEGER},
	[IREG_GS_seg] = {REG_WORD, &cpu_state.seg_gs.seg, REG_INTEGER},
	[IREG_SS_seg] = {REG_WORD, &cpu_state.seg_ss.seg, REG_INTEGER},
	
	[IREG_FPU_TOP] = {REG_DWORD, &cpu_state.TOP, REG_INTEGER},

	[IREG_ST0] = {REG_FPU_ST_DOUBLE, &cpu_state.ST[0], REG_FP},
	[IREG_ST1] = {REG_FPU_ST_DOUBLE, &cpu_state.ST[0], REG_FP},
	[IREG_ST2] = {REG_FPU_ST_DOUBLE, &cpu_state.ST[0], REG_FP},
	[IREG_ST3] = {REG_FPU_ST_DOUBLE, &cpu_state.ST[0], REG_FP},
	[IREG_ST4] = {REG_FPU_ST_DOUBLE, &cpu_state.ST[0], REG_FP},
	[IREG_ST5] = {REG_FPU_ST_DOUBLE, &cpu_state.ST[0], REG_FP},
	[IREG_ST6] = {REG_FPU_ST_DOUBLE, &cpu_state.ST[0], REG_FP},
	[IREG_ST7] = {REG_FPU_ST_DOUBLE, &cpu_state.ST[0], REG_FP},
	
	[IREG_tag0] = {REG_FPU_ST_BYTE, &cpu_state.tag[0], REG_INTEGER},
	[IREG_tag1] = {REG_FPU_ST_BYTE, &cpu_state.tag[0], REG_INTEGER},
	[IREG_tag2] = {REG_FPU_ST_BYTE, &cpu_state.tag[0], REG_INTEGER},
	[IREG_tag3] = {REG_FPU_ST_BYTE, &cpu_state.tag[0], REG_INTEGER},
	[IREG_tag4] = {REG_FPU_ST_BYTE, &cpu_state.tag[0], REG_INTEGER},
	[IREG_tag5] = {REG_FPU_ST_BYTE, &cpu_state.tag[0], REG_INTEGER},
	[IREG_tag6] = {REG_FPU_ST_BYTE, &cpu_state.tag[0], REG_INTEGER},
	[IREG_tag7] = {REG_FPU_ST_BYTE, &cpu_state.tag[0], REG_INTEGER},

	[IREG_ST0_i64] = {REG_FPU_ST_QWORD, &cpu_state.MM[0], REG_FP},
	[IREG_ST1_i64] = {REG_FPU_ST_QWORD, &cpu_state.MM[0], REG_FP},
	[IREG_ST2_i64] = {REG_FPU_ST_QWORD, &cpu_state.MM[0], REG_FP},
	[IREG_ST3_i64] = {REG_FPU_ST_QWORD, &cpu_state.MM[0], REG_FP},
	[IREG_ST4_i64] = {REG_FPU_ST_QWORD, &cpu_state.MM[0], REG_FP},
	[IREG_ST5_i64] = {REG_FPU_ST_QWORD, &cpu_state.MM[0], REG_FP},
	[IREG_ST6_i64] = {REG_FPU_ST_QWORD, &cpu_state.MM[0], REG_FP},
	[IREG_ST7_i64] = {REG_FPU_ST_QWORD, &cpu_state.MM[0], REG_FP},

	[IREG_MM0x] = {REG_QWORD, &cpu_state.MM[0], REG_FP},
	[IREG_MM1x] = {REG_QWORD, &cpu_state.MM[1], REG_FP},
	[IREG_MM2x] = {REG_QWORD, &cpu_state.MM[2], REG_FP},
	[IREG_MM3x] = {REG_QWORD, &cpu_state.MM[3], REG_FP},
	[IREG_MM4x] = {REG_QWORD, &cpu_state.MM[4], REG_FP},
	[IREG_MM5x] = {REG_QWORD, &cpu_state.MM[5], REG_FP},
	[IREG_MM6x] = {REG_QWORD, &cpu_state.MM[6], REG_FP},
	[IREG_MM7x] = {REG_QWORD, &cpu_state.MM[7], REG_FP},
	
	[IREG_NPXCx] = {REG_WORD, &cpu_state.npxc, REG_INTEGER},
	[IREG_NPXSx] = {REG_WORD, &cpu_state.npxs, REG_INTEGER},

	[IREG_flagsx] = {REG_WORD, &cpu_state.flags, REG_INTEGER},
	[IREG_eflagsx] = {REG_WORD, &cpu_state.eflags, REG_INTEGER},

	/*Temporary registers are stored on the stack, and are not guaranteed to
          be preserved across uOPs. They will not be written back if they will
          not be read again.*/
	[IREG_temp0] = {REG_DWORD, (void *)16, REG_INTEGER},
	[IREG_temp1] = {REG_DWORD, (void *)20, REG_INTEGER},
	[IREG_temp2] = {REG_DWORD, (void *)24, REG_INTEGER},
	[IREG_temp3] = {REG_DWORD, (void *)28, REG_INTEGER},
	
	[IREG_temp0d] = {REG_DOUBLE, (void *)40, REG_FP},
	[IREG_temp1d] = {REG_DOUBLE, (void *)48, REG_FP},
};

static int reg_is_native_size(ir_reg_t ir_reg)
{
        int native_size = ireg_data[IREG_GET_REG(ir_reg.reg)].native_size;
        int requested_size = IREG_GET_SIZE(ir_reg.reg);
        
        switch (native_size)
        {
                case REG_BYTE: case REG_FPU_ST_BYTE:
                return (requested_size == IREG_SIZE_B);
                case REG_WORD:
                return (requested_size == IREG_SIZE_W);
                case REG_DWORD:
                return (requested_size == IREG_SIZE_L);
                case REG_QWORD: case REG_FPU_ST_QWORD: case REG_DOUBLE: case REG_FPU_ST_DOUBLE:
                return ((requested_size == IREG_SIZE_D) || (requested_size == IREG_SIZE_Q));
                case REG_POINTER:
                if (sizeof(void *) == 4)
                        return (requested_size == IREG_SIZE_L);
                return (requested_size == IREG_SIZE_Q);
                
                default:
                fatal("get_reg_is_native_size: unknown native size %i\n", native_size);
        }
        
        return 0;
}

void codegen_reg_reset()
{
        int c;

        host_reg_set.regs = _host_regs;
        host_reg_set.dirty = _host_reg_dirty;
        host_reg_set.reg_list = codegen_host_reg_list;
        host_reg_set.locked = 0;
        host_reg_set.nr_regs = CODEGEN_HOST_REGS;
        host_fp_reg_set.regs = host_fp_regs;
        host_fp_reg_set.dirty = host_fp_reg_dirty;
        host_fp_reg_set.reg_list = codegen_host_fp_reg_list;
        host_fp_reg_set.locked = 0;
        host_fp_reg_set.nr_regs = CODEGEN_HOST_FP_REGS;

        for (c = 0; c < IREG_COUNT; c++)
        {
                reg_last_version[c] = 0;
                reg_version_refcount[c][0] = 0;
        }
        for (c = 0; c < CODEGEN_HOST_REGS; c++)
        {
                host_reg_set.regs[c] = invalid_ir_reg;
                host_reg_set.dirty[c] = 0;
        }
        for (c = 0; c < CODEGEN_HOST_FP_REGS; c++)
        {
                host_fp_reg_set.regs[c] = invalid_ir_reg;
                host_fp_reg_set.dirty[c] = 0;
        }
}

static inline int ir_reg_is_invalid(ir_reg_t ir_reg)
{
        return (IREG_GET_REG(ir_reg.reg) == IREG_INVALID);
}

static inline int ir_get_get_refcount(ir_reg_t ir_reg)
{
        return reg_version_refcount[IREG_GET_REG(ir_reg.reg)][ir_reg.version];
}

static inline host_reg_set_t *get_reg_set(ir_reg_t ir_reg)
{
        if (ireg_data[IREG_GET_REG(ir_reg.reg)].type == REG_INTEGER)
                return &host_reg_set;
        else
                return &host_fp_reg_set;
}

static void codegen_reg_load(host_reg_set_t *reg_set, codeblock_t *block, int c, ir_reg_t ir_reg)
{
        switch (ireg_data[IREG_GET_REG(ir_reg.reg)].native_size)
        {
                case REG_WORD:
                if (ireg_data[IREG_GET_REG(ir_reg.reg)].type != REG_INTEGER)
                        fatal("codegen_reg_load - REG_WORD !REG_INTEGER\n");
                if ((uintptr_t)ireg_data[IREG_GET_REG(ir_reg.reg)].p < 256)
                        codegen_direct_read_16_stack(block, reg_set->reg_list[c], (int)ireg_data[IREG_GET_REG(ir_reg.reg)].p);
                else
                        codegen_direct_read_16(block, reg_set->reg_list[c], ireg_data[IREG_GET_REG(ir_reg.reg)].p);
                break;

                case REG_DWORD:
                if (ireg_data[IREG_GET_REG(ir_reg.reg)].type != REG_INTEGER)
                        fatal("codegen_reg_load - REG_DWORD !REG_INTEGER\n");
                if ((uintptr_t)ireg_data[IREG_GET_REG(ir_reg.reg)].p < 256)
                        codegen_direct_read_32_stack(block, reg_set->reg_list[c], (int)ireg_data[IREG_GET_REG(ir_reg.reg)].p);
                else
                        codegen_direct_read_32(block, reg_set->reg_list[c], ireg_data[IREG_GET_REG(ir_reg.reg)].p);
                break;

                case REG_QWORD:
                if (ireg_data[IREG_GET_REG(ir_reg.reg)].type != REG_FP)
                        fatal("codegen_reg_load - REG_QWORD !REG_FP\n");
                if ((uintptr_t)ireg_data[IREG_GET_REG(ir_reg.reg)].p < 256)
                        codegen_direct_read_64_stack(block, reg_set->reg_list[c], (int)ireg_data[IREG_GET_REG(ir_reg.reg)].p);
                else
                        codegen_direct_read_64(block, reg_set->reg_list[c], ireg_data[IREG_GET_REG(ir_reg.reg)].p);
                break;
                
                case REG_POINTER:
                if (ireg_data[IREG_GET_REG(ir_reg.reg)].type != REG_INTEGER)
                        fatal("codegen_reg_load - REG_POINTER !REG_INTEGER\n");
                if ((uintptr_t)ireg_data[IREG_GET_REG(ir_reg.reg)].p < 256)
                        codegen_direct_read_pointer_stack(block, reg_set->reg_list[c], (int)ireg_data[IREG_GET_REG(ir_reg.reg)].p);
                else
                        codegen_direct_read_pointer(block, reg_set->reg_list[c], ireg_data[IREG_GET_REG(ir_reg.reg)].p);
                break;

                case REG_DOUBLE:
                if (ireg_data[IREG_GET_REG(ir_reg.reg)].type != REG_FP)
                        fatal("codegen_reg_load - REG_DOUBLE !REG_FP\n");
                if ((uintptr_t)ireg_data[IREG_GET_REG(ir_reg.reg)].p < 256)
                        codegen_direct_read_double_stack(block, reg_set->reg_list[c], (int)ireg_data[IREG_GET_REG(ir_reg.reg)].p);
                else
                        codegen_direct_read_double(block, reg_set->reg_list[c], ireg_data[IREG_GET_REG(ir_reg.reg)].p);
                break;
                
                case REG_FPU_ST_BYTE:
                if (ireg_data[IREG_GET_REG(ir_reg.reg)].type != REG_INTEGER)
                        fatal("codegen_reg_load - REG_FPU_ST_BYTE !REG_INTEGER\n");
                if (block->flags & CODEBLOCK_STATIC_TOP)
                        codegen_direct_read_8(block, reg_set->reg_list[c], &cpu_state.tag[ir_reg.reg & 7]);
                else
                        codegen_direct_read_st_8(block, reg_set->reg_list[c], &cpu_state.tag[0], ir_reg.reg & 7);
                break;

                case REG_FPU_ST_QWORD:
                if (ireg_data[IREG_GET_REG(ir_reg.reg)].type != REG_FP)
                        fatal("codegen_reg_load - REG_FPU_ST_QWORD !REG_FP\n");
                if (block->flags & CODEBLOCK_STATIC_TOP)
                        codegen_direct_read_64(block, reg_set->reg_list[c], &cpu_state.MM[ir_reg.reg & 7]);
                else
                        codegen_direct_read_st_64(block, reg_set->reg_list[c], &cpu_state.MM[0], ir_reg.reg & 7);
                break;

                case REG_FPU_ST_DOUBLE:
                if (ireg_data[IREG_GET_REG(ir_reg.reg)].type != REG_FP)
                        fatal("codegen_reg_load - REG_FPU_ST_DOUBLE !REG_FP\n");
                if (block->flags & CODEBLOCK_STATIC_TOP)
                        codegen_direct_read_double(block, reg_set->reg_list[c], &cpu_state.ST[ir_reg.reg & 7]);
                else
                        codegen_direct_read_st_double(block, reg_set->reg_list[c], &cpu_state.ST[0], ir_reg.reg & 7);
                break;

                default:
                fatal("codegen_reg_load - native_size=%i reg=%i\n", ireg_data[IREG_GET_REG(ir_reg.reg)].native_size, IREG_GET_REG(ir_reg.reg));
        }

        reg_set->regs[c] = ir_reg;

//pclog("       codegen_reg_load: c=%i reg=%02x.%i\n", c, host_regs[c].reg,host_regs[c].version);
}

static void codegen_reg_writeback(host_reg_set_t *reg_set, codeblock_t *block, int c, int invalidate)
{
        int ir_reg = IREG_GET_REG(reg_set->regs[c].reg);
        void *p = ireg_data[ir_reg].p;

        switch (ireg_data[ir_reg].native_size)
        {
                case REG_BYTE:
                if (ireg_data[ir_reg].type != REG_INTEGER)
                        fatal("codegen_reg_writeback - REG_BYTE !REG_INTEGER\n");
                if ((uintptr_t)p < 256)
                        fatal("codegen_reg_writeback - REG_BYTE %p\n", p);
                codegen_direct_write_8(block, p, reg_set->reg_list[c]);
                break;

                case REG_WORD:
                if (ireg_data[ir_reg].type != REG_INTEGER)
                        fatal("codegen_reg_writeback - REG_WORD !REG_INTEGER\n");
                if ((uintptr_t)p < 256)
                        fatal("codegen_reg_writeback - REG_WORD %p\n", p);
                codegen_direct_write_16(block, p, reg_set->reg_list[c]);
                break;

                case REG_DWORD:
                if (ireg_data[ir_reg].type != REG_INTEGER)
                        fatal("codegen_reg_writeback - REG_DWORD !REG_INTEGER\n");
                if ((uintptr_t)p < 256)
                        codegen_direct_write_32_stack(block, (int)p, reg_set->reg_list[c]);
                else
                        codegen_direct_write_32(block, p, reg_set->reg_list[c]);
                break;

                case REG_QWORD:
                if (ireg_data[ir_reg].type != REG_FP)
                        fatal("codegen_reg_writeback - REG_QWORD !REG_FP\n");
                if ((uintptr_t)p < 256)
                        codegen_direct_write_64_stack(block, (int)p, reg_set->reg_list[c]);
                else
                        codegen_direct_write_64(block, p, reg_set->reg_list[c]);
                break;

                case REG_POINTER:
                if (ireg_data[ir_reg].type != REG_INTEGER)
                        fatal("codegen_reg_writeback - REG_POINTER !REG_INTEGER\n");
                if ((uintptr_t)p < 256)
                        fatal("codegen_reg_writeback - REG_POINTER %p\n", p);
                codegen_direct_write_ptr(block, p, reg_set->reg_list[c]);
                break;

                case REG_DOUBLE:
                if (ireg_data[ir_reg].type != REG_FP)
                        fatal("codegen_reg_writeback - REG_DOUBLE !REG_FP\n");
                if ((uintptr_t)p < 256)
                        codegen_direct_write_double_stack(block, (int)p, reg_set->reg_list[c]);
                else
                        codegen_direct_write_double(block, p, reg_set->reg_list[c]);
                break;

                case REG_FPU_ST_BYTE:
                if (ireg_data[ir_reg].type != REG_INTEGER)
                        fatal("codegen_reg_writeback - REG_FPU_ST_BYTE !REG_INTEGER\n");
                if (block->flags & CODEBLOCK_STATIC_TOP)
                        codegen_direct_write_8(block, &cpu_state.tag[reg_set->regs[c].reg & 7], reg_set->reg_list[c]);
                else
                        codegen_direct_write_st_8(block, &cpu_state.tag[0], reg_set->regs[c].reg & 7, reg_set->reg_list[c]);
                break;

                case REG_FPU_ST_QWORD:
                if (ireg_data[ir_reg].type != REG_FP)
                        fatal("codegen_reg_writeback - REG_FPU_ST_QWORD !REG_FP\n");
                if (block->flags & CODEBLOCK_STATIC_TOP)
                        codegen_direct_write_64(block, &cpu_state.MM[reg_set->regs[c].reg & 7], reg_set->reg_list[c]);
                else
                        codegen_direct_write_st_64(block, &cpu_state.MM[0], reg_set->regs[c].reg & 7, reg_set->reg_list[c]);
                break;

                case REG_FPU_ST_DOUBLE:
                if (ireg_data[ir_reg].type != REG_FP)
                        fatal("codegen_reg_writeback - REG_FPU_ST_DOUBLE !REG_FP\n");
                if (block->flags & CODEBLOCK_STATIC_TOP)
                        codegen_direct_write_double(block, &cpu_state.ST[reg_set->regs[c].reg & 7], reg_set->reg_list[c]);
                else
                        codegen_direct_write_st_double(block, &cpu_state.ST[0], reg_set->regs[c].reg & 7, reg_set->reg_list[c]);
                break;

                default:
                fatal("codegen_reg_flush - native_size=%i\n", ireg_data[ir_reg].native_size);
        }

        if (invalidate)
                reg_set->regs[c] = invalid_ir_reg;
        reg_set->dirty[c] = 0;
}

static void alloc_reg(ir_reg_t ir_reg)
{
        host_reg_set_t *reg_set = get_reg_set(ir_reg);
        int nr_regs = (reg_set == &host_reg_set) ? CODEGEN_HOST_REGS : CODEGEN_HOST_FP_REGS;
        int c;
        
        for (c = 0; c < nr_regs; c++)
        {
                if (IREG_GET_REG(reg_set->regs[c].reg) == IREG_GET_REG(ir_reg.reg))
                {
                        if (reg_set->regs[c].version != ir_reg.version)
                                fatal("alloc_reg - host_regs[c].version != ir_reg.version  %i %p %p  %i %i\n", c, reg_set, &host_reg_set, reg_set->regs[c].reg, ir_reg.reg);
                        reg_set->locked |= (1 << c);
                        return;
                }
        }
}

static void alloc_dest_reg(ir_reg_t ir_reg, int dest_reference)
{
        host_reg_set_t *reg_set = get_reg_set(ir_reg);
        int nr_regs = (reg_set == &host_reg_set) ? CODEGEN_HOST_REGS : CODEGEN_HOST_FP_REGS;
        int c;

        for (c = 0; c < nr_regs; c++)
        {
                if (IREG_GET_REG(reg_set->regs[c].reg) == IREG_GET_REG(ir_reg.reg))
                {
                        if (reg_set->regs[c].version == ir_reg.version || (reg_set->regs[c].version == (ir_reg.version-1) && reg_version_refcount[IREG_GET_REG(reg_set->regs[c].reg)][reg_set->regs[c].version] == dest_reference))
                                reg_set->locked |= (1 << c);
                        else
                                fatal("codegen_reg_alloc_register - host_regs[c].version != dest_reg_a.version  %i,%i %i\n", reg_set->regs[c].version, ir_reg.version, dest_reference);
                        return;
                }
        }
}

void codegen_reg_alloc_register(ir_reg_t dest_reg_a, ir_reg_t src_reg_a, ir_reg_t src_reg_b, ir_reg_t src_reg_c)
{
        int dest_reference = 0;
        
        host_reg_set.locked = 0;
        host_fp_reg_set.locked = 0;
        
/*        pclog("alloc_register: dst=%i.%i src_a=%i.%i src_b=%i.%i\n", dest_reg_a.reg, dest_reg_a.version,
                src_reg_a.reg, src_reg_a.version,
                src_reg_b.reg, src_reg_b.version);*/
        
        if (!ir_reg_is_invalid(dest_reg_a))
        {
                if (!ir_reg_is_invalid(src_reg_a) && IREG_GET_REG(src_reg_a.reg) == IREG_GET_REG(dest_reg_a.reg) && src_reg_a.version == dest_reg_a.version-1)
                        dest_reference++;
                if (!ir_reg_is_invalid(src_reg_b) && IREG_GET_REG(src_reg_b.reg) == IREG_GET_REG(dest_reg_a.reg) && src_reg_b.version == dest_reg_a.version-1)
                        dest_reference++;
                if (!ir_reg_is_invalid(src_reg_c) && IREG_GET_REG(src_reg_c.reg) == IREG_GET_REG(dest_reg_a.reg) && src_reg_c.version == dest_reg_a.version-1)
                        dest_reference++;
        }
        if (!ir_reg_is_invalid(src_reg_a))
                alloc_reg(src_reg_a);
        if (!ir_reg_is_invalid(src_reg_b))
                alloc_reg(src_reg_b);
        if (!ir_reg_is_invalid(src_reg_c))
                alloc_reg(src_reg_c);
        if (!ir_reg_is_invalid(dest_reg_a))
                alloc_dest_reg(dest_reg_a, dest_reference);
}

ir_host_reg_t codegen_reg_alloc_read_reg(codeblock_t *block, ir_reg_t ir_reg, int *host_reg_idx)
{
        host_reg_set_t *reg_set = get_reg_set(ir_reg);
        int c;

        /*Search for required register*/
        for (c = 0; c < reg_set->nr_regs; c++)
        {
                if (!ir_reg_is_invalid(reg_set->regs[c]) && IREG_GET_REG(reg_set->regs[c].reg) == IREG_GET_REG(ir_reg.reg) && reg_set->regs[c].version == ir_reg.version)
                        break;

                if (!ir_reg_is_invalid(reg_set->regs[c]) && IREG_GET_REG(reg_set->regs[c].reg) == IREG_GET_REG(ir_reg.reg) && reg_version_refcount[IREG_GET_REG(reg_set->regs[c].reg)][reg_set->regs[c].version])
                        fatal("codegen_reg_alloc_read_reg - version mismatch!\n");
        }

        if (c == reg_set->nr_regs)
        {
                /*No unused registers. Search for an unlocked register with no pending reads*/
                for (c = 0; c < reg_set->nr_regs; c++)
                {
                        if (!(reg_set->locked & (1 << c)) && !ir_get_get_refcount(reg_set->regs[c]))
                                break;
                }
                if (c == reg_set->nr_regs)
                {
                        /*Search for any unlocked register*/
                        for (c = 0; c < reg_set->nr_regs; c++)
                        {
                                if (!(reg_set->locked & (1 << c)))
                                        break;
                        }
                        if (c == reg_set->nr_regs)
                                fatal("codegen_reg_alloc_read_reg - out of registers\n");
                }
                if (reg_set->dirty[c])
                        codegen_reg_writeback(reg_set, block, c, 1);
//                pclog("   load %i\n", c);
                codegen_reg_load(reg_set, block, c, ir_reg);
//                fatal("codegen_reg_alloc_read_reg - read %i.%i to %i\n", ir_reg.reg,ir_reg.version, c);
                reg_set->locked |= (1 << c);
//                codegen_reg_writeback(block, c);
                reg_set->dirty[c] = 0;
        }
//        else
//                pclog("   already loaded %i\n", c);

        reg_version_refcount[IREG_GET_REG(reg_set->regs[c].reg)][reg_set->regs[c].version]--;
        if (reg_version_refcount[IREG_GET_REG(reg_set->regs[c].reg)][reg_set->regs[c].version] < 0)
                fatal("codegen_reg_alloc_read_reg - refcount < 0\n");

        if (host_reg_idx)
                *host_reg_idx = c;
//        pclog(" codegen_reg_alloc_read_reg: %i.%i %i  %02x.%i  %i\n", ir_reg.reg, ir_reg.version, codegen_host_reg_list[c],  host_regs[c].reg,host_regs[c].version,  c);
        return reg_set->reg_list[c] | IREG_GET_SIZE(ir_reg.reg);
}

ir_host_reg_t codegen_reg_alloc_write_reg(codeblock_t *block, ir_reg_t ir_reg)
{
        host_reg_set_t *reg_set = get_reg_set(ir_reg);
        int c;
        
        if (!reg_is_native_size(ir_reg))
        {
                /*Read in parent register so we can do partial accesses to it*/
                ir_reg_t parent_reg;
                
                parent_reg.reg = IREG_GET_REG(ir_reg.reg) | IREG_SIZE_L;
                parent_reg.version = ir_reg.version - 1;
                
                codegen_reg_alloc_read_reg(block, parent_reg, &c);

                if (IREG_GET_REG(reg_set->regs[c].reg) != IREG_GET_REG(ir_reg.reg) || reg_set->regs[c].version != ir_reg.version-1)
                        fatal("codegen_reg_alloc_write_reg sub_reg - doesn't match  %i %02x.%i %02x.%i\n", c,
                                        reg_set->regs[c].reg,reg_set->regs[c].version,
                                        ir_reg.reg,ir_reg.version);
                        
                reg_set->regs[c].reg = ir_reg.reg;
                reg_set->regs[c].version = ir_reg.version;
                reg_set->dirty[c] = 1;
//        pclog(" codegen_reg_alloc_write_reg: partial %i.%i %i\n", ir_reg.reg, ir_reg.version, codegen_host_reg_list[c]);
                return reg_set->reg_list[c] | IREG_GET_SIZE(ir_reg.reg);
        }
        
        /*Search for previous version in host register*/
        for (c = 0; c < reg_set->nr_regs; c++)
        {
                if (!ir_reg_is_invalid(reg_set->regs[c]) && IREG_GET_REG(reg_set->regs[c].reg) == IREG_GET_REG(ir_reg.reg))
                {
                        if (reg_set->regs[c].version == ir_reg.version-1)
                        {
                                if (reg_version_refcount[IREG_GET_REG(reg_set->regs[c].reg)][reg_set->regs[c].version] != 0)
                                        fatal("codegen_reg_alloc_write_reg - previous version refcount != 0\n");
                                break;
                        }
                }
        }
        
        if (c == reg_set->nr_regs)
        {
                /*Search for unused registers*/
                for (c = 0; c < reg_set->nr_regs; c++)
                {
                        if (ir_reg_is_invalid(reg_set->regs[c]))
                                break;
                }
        
                if (c == reg_set->nr_regs)
                {
                        /*No unused registers. Search for an unlocked register*/
                        for (c = 0; c < reg_set->nr_regs; c++)
                        {
                                if (!(reg_set->locked & (1 << c)))
                                        break;
                        }
                        if (c == reg_set->nr_regs)
                                fatal("codegen_reg_alloc_write_reg - out of registers\n");
                        if (reg_set->dirty[c])
                                codegen_reg_writeback(reg_set, block, c, 1);
                }
        }
        
        reg_set->regs[c].reg = ir_reg.reg;
        reg_set->regs[c].version = ir_reg.version;
        reg_set->dirty[c] = 1;
//        pclog(" codegen_reg_alloc_write_reg: %i.%i %i\n", ir_reg.reg, ir_reg.version, codegen_host_reg_list[c]);
        return reg_set->reg_list[c] | IREG_GET_SIZE(ir_reg.reg);
}

void codegen_reg_flush(ir_data_t *ir, codeblock_t *block)
{
        host_reg_set_t *reg_set;
        int c;
        
        reg_set = &host_reg_set;
        for (c = 0; c < reg_set->nr_regs; c++)
        {
                if (!ir_reg_is_invalid(reg_set->regs[c]) && reg_set->dirty[c])
                {
                        codegen_reg_writeback(reg_set, block, c, 0);
                }
        }

        reg_set = &host_fp_reg_set;
        for (c = 0; c < reg_set->nr_regs; c++)
        {
                if (!ir_reg_is_invalid(reg_set->regs[c]) && reg_set->dirty[c])
                {
                        codegen_reg_writeback(reg_set, block, c, 0);
                }
        }
}

void codegen_reg_flush_invalidate(ir_data_t *ir, codeblock_t *block)
{
        host_reg_set_t *reg_set;
        int c;
        
        reg_set = &host_reg_set;
        for (c = 0; c < reg_set->nr_regs; c++)
        {
                if (!ir_reg_is_invalid(reg_set->regs[c]) && reg_set->dirty[c])
                {
                        codegen_reg_writeback(reg_set, block, c, 1);
                }
                reg_set->regs[c] = invalid_ir_reg;
                reg_set->dirty[c] = 0;
        }

        reg_set = &host_fp_reg_set;
        for (c = 0; c < reg_set->nr_regs; c++)
        {
                if (!ir_reg_is_invalid(reg_set->regs[c]) && reg_set->dirty[c])
                {
                        codegen_reg_writeback(reg_set, block, c, 1);
                }
                reg_set->regs[c] = invalid_ir_reg;
                reg_set->dirty[c] = 0;
        }
}
