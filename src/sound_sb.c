#include <stdlib.h>
#include "ibm.h"
#include "device.h"
#include "io.h"
#include "mca.h"
#include "mem.h"
#include "rom.h"
#include "sound.h"
#include "sound_emu8k.h"
#include "sound_mpu401_uart.h"
#include "sound_opl.h"
#include "sound_sb.h"
#include "sound_sb_dsp.h"

#include "filters.h"

typedef struct sb_mixer_t
{
        int master_l, master_r;
        int voice_l,  voice_r;
        int fm_l,     fm_r;
        int cd_l,     cd_r;
        int bass_l,   bass_r;
        int treble_l, treble_r;
        int filter;

        int index;
        uint8_t regs[256];
} sb_mixer_t;

typedef struct sb_t
{
        opl_t           opl;
        sb_dsp_t        dsp;
        sb_mixer_t      mixer;
        mpu401_uart_t   mpu;
        emu8k_t         emu8k;

        int pos;
        
        uint8_t pos_regs[8];
        
        int opl_emu;
} sb_t;
/* 0 to 7 -> -14dB to 0dB i 2dB steps. 8 to 15 -> 0 to +14dB in 2dB steps.
  Note that for positive dB values, this is not amplitude, it is amplitude-1. */
const float sb_bass_treble_4bits[]= {
   0.199526231, 0.25, 0.316227766, 0.398107170, 0.5, 0.63095734, 0.794328234, 1, 
    0, 0.25892541, 0.584893192, 1, 1.511886431, 2.16227766, 3, 4.011872336
};

static int sb_att[]=
{
        50,65,82,103,130,164,207,260,328,413,520,655,825,1038,1307,
        1645,2072,2608,3283,4134,5205,6553,8250,10385,13075,16461,20724,26089,
        32845,41349,52055,65535
};

static void sb_get_buffer_opl2(int32_t *buffer, int len, void *p)
{
        sb_t *sb = (sb_t *)p;
        sb_mixer_t *mixer = &sb->mixer;
                
        int c;

        opl2_update2(&sb->opl);
        sb_dsp_update(&sb->dsp);
        for (c = 0; c < len * 2; c += 2)
        {
                int32_t out_l, out_r;
                
                out_l = ((((sb->opl.buffer[c]     * mixer->fm_l) >> 16) * 51000) >> 16);
                out_r = ((((sb->opl.buffer[c + 1] * mixer->fm_r) >> 16) * 51000) >> 16);

                if (sb->mixer.filter)
                {
                        out_l += (int)(((sb_iir(0, (float)sb->dsp.buffer[c])     / 1.3) * mixer->voice_l) / 3) >> 16;
                        out_r += (int)(((sb_iir(1, (float)sb->dsp.buffer[c + 1]) / 1.3) * mixer->voice_r) / 3) >> 16;
                }
                else
                {
                        out_l += ((int32_t)(sb->dsp.buffer[c]     * mixer->voice_l) / 3) >> 16;
                        out_r += ((int32_t)(sb->dsp.buffer[c + 1] * mixer->voice_r) / 3) >> 16;
                }
                
                out_l = (out_l * mixer->master_l) >> 16;
                out_r = (out_r * mixer->master_r) >> 16;

                if (mixer->bass_l != 8 || mixer->bass_r != 8 || mixer->treble_l != 8 || mixer->treble_r != 8)
                {
                        // This is not exactly how one does bass/treble controls, but the end result is like it. A better implementation would reduce the cpu usage
                        if (mixer->bass_l>8) out_l += (int32_t)(low_iir(0, (float)out_l)*sb_bass_treble_4bits[mixer->bass_l]);
                        if (mixer->bass_r>8)  out_r += (int32_t)(low_iir(1, (float)out_r)*sb_bass_treble_4bits[mixer->bass_r]);
                        if (mixer->treble_l>8) out_l += (int32_t)(high_iir(0, (float)out_l)*sb_bass_treble_4bits[mixer->treble_l]);
                        if (mixer->treble_r>8) out_r += (int32_t)(high_iir(1, (float)out_r)*sb_bass_treble_4bits[mixer->treble_r]);
                        if (mixer->bass_l<8)   out_l = (int32_t)((out_l )*sb_bass_treble_4bits[mixer->bass_l] + low_cut_iir(0, (float)out_l)*(1.0-sb_bass_treble_4bits[mixer->bass_l]));
                        if (mixer->bass_r<8)   out_r = (int32_t)((out_r )*sb_bass_treble_4bits[mixer->bass_r] + low_cut_iir(1, (float)out_r)*(1.0-sb_bass_treble_4bits[mixer->bass_r])); 
                        if (mixer->treble_l<8) out_l = (int32_t)((out_l )*sb_bass_treble_4bits[mixer->treble_l] + high_cut_iir(0, (float)out_l)*(1.0-sb_bass_treble_4bits[mixer->treble_l]));
                        if (mixer->treble_r<8) out_r = (int32_t)((out_r )*sb_bass_treble_4bits[mixer->treble_r] + high_cut_iir(1, (float)out_r)*(1.0-sb_bass_treble_4bits[mixer->treble_r]));
                }
                        
                buffer[c]     += out_l;
                buffer[c + 1] += out_r;
        }

        sb->pos = 0;
        sb->opl.pos = 0;
        sb->dsp.pos = 0;
}

static void sb_get_buffer_opl3(int32_t *buffer, int len, void *p)
{
        sb_t *sb = (sb_t *)p;
        sb_mixer_t *mixer = &sb->mixer;
                
        int c;

        opl3_update2(&sb->opl);
        sb_dsp_update(&sb->dsp);
        for (c = 0; c < len * 2; c += 2)
        {
                int32_t out_l, out_r;
                
                out_l = ((((sb->opl.buffer[c]     * mixer->fm_l) >> 16) * (sb->opl_emu ? 47000 : 51000)) >> 16);
                out_r = ((((sb->opl.buffer[c + 1] * mixer->fm_r) >> 16) * (sb->opl_emu ? 47000 : 51000)) >> 16);

                if (sb->mixer.filter)
                {
                        out_l += (int)(((sb_iir(0, (float)sb->dsp.buffer[c])     / 1.3) * mixer->voice_l) / 3) >> 16;
                        out_r += (int)(((sb_iir(1, (float)sb->dsp.buffer[c + 1]) / 1.3) * mixer->voice_r) / 3) >> 16;
                }
                else
                {
                        out_l += ((int32_t)(sb->dsp.buffer[c]     * mixer->voice_l) / 3) >> 16;
                        out_r += ((int32_t)(sb->dsp.buffer[c + 1] * mixer->voice_r) / 3) >> 16;
                }
                
                out_l = (out_l * mixer->master_l) >> 16;
                out_r = (out_r * mixer->master_r) >> 16;

                if (mixer->bass_l != 8 || mixer->bass_r != 8 || mixer->treble_l != 8 || mixer->treble_r != 8)
                {
                        // This is not exactly how one does bass/treble controls, but the end result is like it. A better implementation would reduce the cpu usage
                        if (mixer->bass_l>8) out_l += (int32_t)(low_iir(0, (float)out_l)*sb_bass_treble_4bits[mixer->bass_l]);
                        if (mixer->bass_r>8)  out_r += (int32_t)(low_iir(1, (float)out_r)*sb_bass_treble_4bits[mixer->bass_r]);
                        if (mixer->treble_l>8) out_l += (int32_t)(high_iir(0, (float)out_l)*sb_bass_treble_4bits[mixer->treble_l]);
                        if (mixer->treble_r>8) out_r += (int32_t)(high_iir(1, (float)out_r)*sb_bass_treble_4bits[mixer->treble_r]);
                        if (mixer->bass_l<8)   out_l = (int32_t)((out_l )*sb_bass_treble_4bits[mixer->bass_l] + low_cut_iir(0, (float)out_l)*(1.0-sb_bass_treble_4bits[mixer->bass_l]));
                        if (mixer->bass_r<8)   out_r = (int32_t)((out_r )*sb_bass_treble_4bits[mixer->bass_r] + low_cut_iir(1, (float)out_r)*(1.0-sb_bass_treble_4bits[mixer->bass_r])); 
                        if (mixer->treble_l<8) out_l = (int32_t)((out_l )*sb_bass_treble_4bits[mixer->treble_l] + high_cut_iir(0, (float)out_l)*(1.0-sb_bass_treble_4bits[mixer->treble_l]));
                        if (mixer->treble_r<8) out_r = (int32_t)((out_r )*sb_bass_treble_4bits[mixer->treble_r] + high_cut_iir(1, (float)out_r)*(1.0-sb_bass_treble_4bits[mixer->treble_r]));
                }
                        
                buffer[c]     += out_l;
                buffer[c + 1] += out_r;
        }

        sb->pos = 0;
        sb->opl.pos = 0;
        sb->dsp.pos = 0;
        sb->emu8k.pos = 0;
}

static void sb_get_buffer_emu8k(int32_t *buffer, int len, void *p)
{
        sb_t *sb = (sb_t *)p;
        sb_mixer_t *mixer = &sb->mixer;
                
        int c;

        opl3_update2(&sb->opl);
        sb_dsp_update(&sb->dsp);
        emu8k_update(&sb->emu8k);
        for (c = 0; c < len * 2; c += 2)
        {
                int c_emu8k = (((c/2) * 44100) / 48000)*2;
                int32_t out_l, out_r;
                
                out_l = ((((sb->opl.buffer[c]     * mixer->fm_l) >> 16) * (sb->opl_emu ? 47000 : 51000)) >> 16);
                out_r = ((((sb->opl.buffer[c + 1] * mixer->fm_r) >> 16) * (sb->opl_emu ? 47000 : 51000)) >> 16);

                out_l += ((sb->emu8k.buffer[c_emu8k]     * mixer->fm_l) >> 16);
                out_r += ((sb->emu8k.buffer[c_emu8k + 1] * mixer->fm_l) >> 16);

                if (sb->mixer.filter)
                {
                        out_l += (int)(((sb_iir(0, (float)sb->dsp.buffer[c])     / 1.3) * mixer->voice_l) / 3) >> 16;
                        out_r += (int)(((sb_iir(1, (float)sb->dsp.buffer[c + 1]) / 1.3) * mixer->voice_r) / 3) >> 16;
                }
                else
                {
                        out_l += ((int32_t)(sb->dsp.buffer[c]     * mixer->voice_l) / 3) >> 16;
                        out_r += ((int32_t)(sb->dsp.buffer[c + 1] * mixer->voice_r) / 3) >> 16;
                }
                
                out_l = (out_l * mixer->master_l) >> 16;
                out_r = (out_r * mixer->master_r) >> 16;

                if (mixer->bass_l != 8 || mixer->bass_r != 8 || mixer->treble_l != 8 || mixer->treble_r != 8)
                {
                        // This is not exactly how one does bass/treble controls, but the end result is like it. A better implementation would reduce the cpu usage
                        if (mixer->bass_l>8) out_l += (int32_t)(low_iir(0, (float)out_l)*sb_bass_treble_4bits[mixer->bass_l]);
                        if (mixer->bass_r>8)  out_r += (int32_t)(low_iir(1, (float)out_r)*sb_bass_treble_4bits[mixer->bass_r]);
                        if (mixer->treble_l>8) out_l += (int32_t)(high_iir(0, (float)out_l)*sb_bass_treble_4bits[mixer->treble_l]);
                        if (mixer->treble_r>8) out_r += (int32_t)(high_iir(1, (float)out_r)*sb_bass_treble_4bits[mixer->treble_r]);
                        if (mixer->bass_l<8)   out_l = (int32_t)((out_l )*sb_bass_treble_4bits[mixer->bass_l] + low_cut_iir(0, (float)out_l)*(1.0-sb_bass_treble_4bits[mixer->bass_l]));
                        if (mixer->bass_r<8)   out_r = (int32_t)((out_r )*sb_bass_treble_4bits[mixer->bass_r] + low_cut_iir(1, (float)out_r)*(1.0-sb_bass_treble_4bits[mixer->bass_r])); 
                        if (mixer->treble_l<8) out_l = (int32_t)((out_l )*sb_bass_treble_4bits[mixer->treble_l] + high_cut_iir(0, (float)out_l)*(1.0-sb_bass_treble_4bits[mixer->treble_l]));
                        if (mixer->treble_r<8) out_r = (int32_t)((out_r )*sb_bass_treble_4bits[mixer->treble_r] + high_cut_iir(1, (float)out_r)*(1.0-sb_bass_treble_4bits[mixer->treble_r]));
                }
                        
                buffer[c]     += out_l;
                buffer[c + 1] += out_r;
        }

        sb->pos = 0;
        sb->opl.pos = 0;
        sb->dsp.pos = 0;
        sb->emu8k.pos = 0;
}

void sb_pro_mixer_write(uint16_t addr, uint8_t val, void *p)
{
        sb_t *sb = (sb_t *)p;
        sb_mixer_t *mixer = &sb->mixer;
        
        if (!(addr & 1))
                mixer->index = val & 0xff;
        else
        {
                mixer->regs[mixer->index] = val;
  
                mixer->master_l = sb_att[(mixer->regs[0x22] >> 4)  | 0x11];
                mixer->master_r = sb_att[(mixer->regs[0x22] & 0xf) | 0x11];
                mixer->voice_l  = sb_att[(mixer->regs[0x04] >> 4)  | 0x11];
                mixer->voice_r  = sb_att[(mixer->regs[0x04] & 0xf) | 0x11];
                mixer->fm_l     = sb_att[(mixer->regs[0x26] >> 4)  | 0x11];
                mixer->fm_r     = sb_att[(mixer->regs[0x26] & 0xf) | 0x11];
                mixer->cd_l     = sb_att[(mixer->regs[0x28] >> 4)  | 0x11];
                mixer->cd_r     = sb_att[(mixer->regs[0x28] & 0xf) | 0x11];
                mixer->filter   = !(mixer->regs[0xe] & 0x20);
                mixer->bass_l   = mixer->bass_r   = 8;
                mixer->treble_l = mixer->treble_r = 8;
                sound_set_cd_volume(((uint32_t)mixer->master_l * (uint32_t)mixer->cd_l) / 65535,
                                    ((uint32_t)mixer->master_r * (uint32_t)mixer->cd_r) / 65535);
//                pclog("%02X %02X %02X\n", mixer->regs[0x04], mixer->regs[0x22], mixer->regs[0x26]);
//                pclog("Mixer - %04X %04X %04X %04X %04X %04X\n", mixer->master_l, mixer->master_r, mixer->voice_l, mixer->voice_r, mixer->fm_l, mixer->fm_r);
                if (mixer->index == 0xe)
                        sb_dsp_set_stereo(&sb->dsp, val & 2);
        }
}

uint8_t sb_pro_mixer_read(uint16_t addr, void *p)
{
        sb_t *sb = (sb_t *)p;
        sb_mixer_t *mixer = &sb->mixer;

        if (!(addr & 1))
                return mixer->index;

        switch (mixer->index)
        {
                case 0x00: case 0x04: case 0x0a: case 0x0c: case 0x0e:
                case 0x22: case 0x26: case 0x28: case 0x2e:
                return mixer->regs[mixer->index];
        }
        
        return 0xff;
}

void sb_16_mixer_write(uint16_t addr, uint8_t val, void *p)
{
        sb_t *sb = (sb_t *)p;
        sb_mixer_t *mixer = &sb->mixer;
        
        if (!(addr & 1))
                mixer->index = val;
        else
        {
                mixer->regs[mixer->index] = val;
                switch (mixer->index)
                {
                        case 0x22:
                        mixer->regs[0x30] = ((mixer->regs[0x22] >> 4)  | 0x11) << 3;
                        mixer->regs[0x31] = ((mixer->regs[0x22] & 0xf) | 0x11) << 3;
                        break;
                        case 0x04:
                        mixer->regs[0x32] = ((mixer->regs[0x04] >> 4)  | 0x11) << 3;
                        mixer->regs[0x33] = ((mixer->regs[0x04] & 0xf) | 0x11) << 3;
                        break;
                        case 0x26:
                        mixer->regs[0x34] = ((mixer->regs[0x26] >> 4)  | 0x11) << 3;
                        mixer->regs[0x35] = ((mixer->regs[0x26] & 0xf) | 0x11) << 3;
                        break;
                        case 0x28:
                        mixer->regs[0x36] = ((mixer->regs[0x28] >> 4)  | 0x11) << 3;
                        mixer->regs[0x37] = ((mixer->regs[0x28] & 0xf) | 0x11) << 3;
                        break;
                        case 0x80:
                        if (val & 1) sb->dsp.sb_irqnum = 2;
                        if (val & 2) sb->dsp.sb_irqnum = 5;
                        if (val & 4) sb->dsp.sb_irqnum = 7;
                        if (val & 8) sb->dsp.sb_irqnum = 10;
                        break;
                }
                mixer->master_l = sb_att[mixer->regs[0x30] >> 3];
                mixer->master_r = sb_att[mixer->regs[0x31] >> 3];
                mixer->voice_l  = sb_att[mixer->regs[0x32] >> 3];
                mixer->voice_r  = sb_att[mixer->regs[0x33] >> 3];
                mixer->fm_l     = sb_att[mixer->regs[0x34] >> 3];
                mixer->fm_r     = sb_att[mixer->regs[0x35] >> 3];
                mixer->cd_l     = sb_att[mixer->regs[0x36] >> 3];
                mixer->cd_r     = sb_att[mixer->regs[0x37] >> 3];
                mixer->bass_l   = mixer->regs[0x46] >> 4;
                mixer->bass_r   = mixer->regs[0x47] >> 4;
                mixer->treble_l = mixer->regs[0x44] >> 4;
                mixer->treble_r = mixer->regs[0x45] >> 4;
                mixer->filter = 0;
                sound_set_cd_volume(((uint32_t)mixer->master_l * (uint32_t)mixer->cd_l) / 65535,
                                    ((uint32_t)mixer->master_r * (uint32_t)mixer->cd_r) / 65535);
//                pclog("%02X %02X %02X %02X %02X %02X\n", mixer->regs[0x30], mixer->regs[0x31], mixer->regs[0x32], mixer->regs[0x33], mixer->regs[0x34], mixer->regs[0x35]);
//                pclog("Mixer - %04X %04X %04X %04X %04X %04X\n", mixer->master_l, mixer->master_r, mixer->voice_l, mixer->voice_r, mixer->fm_l, mixer->fm_r);
        }
}

uint8_t sb_16_mixer_read(uint16_t addr, void *p)
{
        sb_t *sb = (sb_t *)p;
        sb_mixer_t *mixer = &sb->mixer;

        if (!(addr & 1))
                return mixer->index;

        switch (mixer->index)
        {
                case 0x40: return 0xff;
                case 0x80:
                switch (sb->dsp.sb_irqnum)
                {
                        case 2: return 1; /*IRQ 7*/
                        case 5: return 2; /*IRQ 7*/
                        case 7: return 4; /*IRQ 7*/
                        case 10: return 8; /*IRQ 7*/
                }
                break;
                case 0x81:
                return 0x22; /*DMA 1 and 5*/
                case 0x82:
                return ((sb->dsp.sb_irq8) ? 1 : 0) | ((sb->dsp.sb_irq16) ? 2 : 0);
        }
        return mixer->regs[mixer->index];                
}

void sb_mixer_init(sb_mixer_t *mixer)
{
        mixer->master_l = mixer->master_r = 65535;
        mixer->voice_l  = mixer->voice_r  = 65535;
        mixer->fm_l     = mixer->fm_r     = 65535;
        mixer->bass_l   = mixer->bass_r   = 8;
        mixer->treble_l = mixer->treble_r = 8;
        mixer->filter = 1;
}

static uint16_t sb_mcv_addr[8] = {0x200, 0x210, 0x220, 0x230, 0x240, 0x250, 0x260, 0x270};

uint8_t sb_mcv_read(int port, void *p)
{
        sb_t *sb = (sb_t *)p;
        
        pclog("sb_mcv_read: port=%04x\n", port);
        
        return sb->pos_regs[port & 7];
}

void sb_mcv_write(int port, uint8_t val, void *p)
{
        uint16_t addr;
        sb_t *sb = (sb_t *)p;

        if (port < 0x102)
                return;
        
        pclog("sb_mcv_write: port=%04x val=%02x\n", port, val);

        addr = sb_mcv_addr[sb->pos_regs[4] & 7];
        io_removehandler(addr+8, 0x0002, opl2_read, NULL, NULL, opl2_write, NULL, NULL, &sb->opl);
        io_removehandler(0x0388, 0x0002, opl2_read, NULL, NULL, opl2_write, NULL, NULL, &sb->opl);
        sb_dsp_setaddr(&sb->dsp, 0);

        sb->pos_regs[port & 7] = val;

        if (sb->pos_regs[2] & 1)
        {
                addr = sb_mcv_addr[sb->pos_regs[4] & 7];
                
                io_sethandler(addr+8, 0x0002, opl2_read, NULL, NULL, opl2_write, NULL, NULL, &sb->opl);
                io_sethandler(0x0388, 0x0002, opl2_read, NULL, NULL, opl2_write, NULL, NULL, &sb->opl);
                sb_dsp_setaddr(&sb->dsp, addr);
        }
}

static int sb_pro_mcv_irqs[4] = {7, 5, 3, 3};

uint8_t sb_pro_mcv_read(int port, void *p)
{
        sb_t *sb = (sb_t *)p;
        
        pclog("sb_pro_mcv_read: port=%04x\n", port);
        
        return sb->pos_regs[port & 7];
}

void sb_pro_mcv_write(int port, uint8_t val, void *p)
{
        uint16_t addr;
        sb_t *sb = (sb_t *)p;

        if (port < 0x102)
                return;
        
        pclog("sb_pro_mcv_write: port=%04x val=%02x\n", port, val);

        addr = (sb->pos_regs[2] & 0x20) ? 0x220 : 0x240;
        io_removehandler(addr+0, 0x0004, opl3_read,   NULL, NULL, opl3_write,   NULL, NULL, &sb->opl);
        io_removehandler(addr+8, 0x0002, opl3_read,   NULL, NULL, opl3_write,   NULL, NULL, &sb->opl);
        io_removehandler(0x0388, 0x0004, opl3_read,   NULL, NULL, opl3_write,   NULL, NULL, &sb->opl);
        io_removehandler(addr+4, 0x0002, sb_pro_mixer_read, NULL, NULL, sb_pro_mixer_write, NULL, NULL, sb);
        sb_dsp_setaddr(&sb->dsp, 0);

        sb->pos_regs[port & 7] = val;

        if (sb->pos_regs[2] & 1)
        {
                addr = (sb->pos_regs[2] & 0x20) ? 0x220 : 0x240;

                io_sethandler(addr+0, 0x0004, opl3_read,   NULL, NULL, opl3_write,   NULL, NULL, &sb->opl);
                io_sethandler(addr+8, 0x0002, opl3_read,   NULL, NULL, opl3_write,   NULL, NULL, &sb->opl);
                io_sethandler(0x0388, 0x0004, opl3_read,   NULL, NULL, opl3_write,   NULL, NULL, &sb->opl);
                io_sethandler(addr+4, 0x0002, sb_pro_mixer_read, NULL, NULL, sb_pro_mixer_write, NULL, NULL, sb);
                
                sb_dsp_setaddr(&sb->dsp, addr);
        }
        sb_dsp_setirq(&sb->dsp, sb_pro_mcv_irqs[(sb->pos_regs[5] >> 4) & 3]);
        sb_dsp_setdma8(&sb->dsp, sb->pos_regs[4] & 3);
}
        
void *sb_1_init()
{
        sb_t *sb = malloc(sizeof(sb_t));
        uint16_t addr = device_get_config_int("addr");        
        memset(sb, 0, sizeof(sb_t));
        
        opl2_init(&sb->opl);
        sb_dsp_init(&sb->dsp, SB1);
        sb_dsp_setaddr(&sb->dsp, addr);
        sb_dsp_setirq(&sb->dsp, device_get_config_int("irq"));
        sb_dsp_setdma8(&sb->dsp, device_get_config_int("dma"));
        sb_mixer_init(&sb->mixer);
        io_sethandler(addr+8, 0x0002, opl2_read, NULL, NULL, opl2_write, NULL, NULL, &sb->opl);
        io_sethandler(0x0388, 0x0002, opl2_read, NULL, NULL, opl2_write, NULL, NULL, &sb->opl);
        sound_add_handler(sb_get_buffer_opl2, sb);
        return sb;
}
void *sb_15_init()
{
        sb_t *sb = malloc(sizeof(sb_t));
        uint16_t addr = device_get_config_int("addr");
        memset(sb, 0, sizeof(sb_t));

        opl2_init(&sb->opl);
        sb_dsp_init(&sb->dsp, SB15);
        sb_dsp_setaddr(&sb->dsp, addr);
        sb_dsp_setirq(&sb->dsp, device_get_config_int("irq"));
        sb_dsp_setdma8(&sb->dsp, device_get_config_int("dma"));
        sb_mixer_init(&sb->mixer);
        io_sethandler(addr+8, 0x0002, opl2_read, NULL, NULL, opl2_write, NULL, NULL, &sb->opl);
        io_sethandler(0x0388, 0x0002, opl2_read, NULL, NULL, opl2_write, NULL, NULL, &sb->opl);
        sound_add_handler(sb_get_buffer_opl2, sb);
        return sb;
}

void *sb_mcv_init()
{
        sb_t *sb = malloc(sizeof(sb_t));
        memset(sb, 0, sizeof(sb_t));

        opl2_init(&sb->opl);
        sb_dsp_init(&sb->dsp, SB15);
        sb_dsp_setaddr(&sb->dsp, 0);//addr);
        sb_dsp_setirq(&sb->dsp, device_get_config_int("irq"));
        sb_dsp_setdma8(&sb->dsp, device_get_config_int("dma"));
        sb_mixer_init(&sb->mixer);
        sound_add_handler(sb_get_buffer_opl2, sb);
        mca_add(sb_mcv_read, sb_mcv_write, sb);
        sb->pos_regs[0] = 0x84;
        sb->pos_regs[1] = 0x50;
        return sb;
}
void *sb_2_init()
{
        sb_t *sb = malloc(sizeof(sb_t));
        uint16_t addr = device_get_config_int("addr");
        memset(sb, 0, sizeof(sb_t));

        opl2_init(&sb->opl);
        sb_dsp_init(&sb->dsp, SB2);
        sb_dsp_setaddr(&sb->dsp, addr);
        sb_dsp_setirq(&sb->dsp, device_get_config_int("irq"));
        sb_dsp_setdma8(&sb->dsp, device_get_config_int("dma"));
        sb_mixer_init(&sb->mixer);
        io_sethandler(addr+8, 0x0002, opl2_read, NULL, NULL, opl2_write, NULL, NULL, &sb->opl);
        io_sethandler(0x0388, 0x0002, opl2_read, NULL, NULL, opl2_write, NULL, NULL, &sb->opl);
        sound_add_handler(sb_get_buffer_opl2, sb);
        return sb;
}

void *sb_pro_v1_init()
{
        sb_t *sb = malloc(sizeof(sb_t));
        uint16_t addr = device_get_config_int("addr");
        memset(sb, 0, sizeof(sb_t));

        opl2_init(&sb->opl);
        sb_dsp_init(&sb->dsp, SBPRO);
        sb_dsp_setaddr(&sb->dsp, addr);
        sb_dsp_setirq(&sb->dsp, device_get_config_int("irq"));
        sb_dsp_setdma8(&sb->dsp, device_get_config_int("dma"));
        sb_mixer_init(&sb->mixer);
        io_sethandler(addr+0, 0x0002, opl2_l_read, NULL, NULL, opl2_l_write, NULL, NULL, &sb->opl);
        io_sethandler(addr+2, 0x0002, opl2_r_read, NULL, NULL, opl2_r_write, NULL, NULL, &sb->opl);
        io_sethandler(addr+8, 0x0002, opl2_read,   NULL, NULL, opl2_write,   NULL, NULL, &sb->opl);
        io_sethandler(0x0388, 0x0002, opl2_read,   NULL, NULL, opl2_write,   NULL, NULL, &sb->opl);
        io_sethandler(addr+4, 0x0002, sb_pro_mixer_read, NULL, NULL, sb_pro_mixer_write, NULL, NULL, sb);
        sound_add_handler(sb_get_buffer_opl2, sb);

        sb->mixer.regs[0x22] = 0xff;
        sb->mixer.regs[0x04] = 0xff;
        sb->mixer.regs[0x26] = 0xff;
        sb->mixer.regs[0xe]  = 0;

        return sb;
}

void *sb_pro_v2_init()
{
        sb_t *sb = malloc(sizeof(sb_t));
        uint16_t addr = device_get_config_int("addr");
        memset(sb, 0, sizeof(sb_t));

        sb->opl_emu = device_get_config_int("opl_emu");
        opl3_init(&sb->opl, sb->opl_emu);
        sb_dsp_init(&sb->dsp, SBPRO2);
        sb_dsp_setaddr(&sb->dsp, addr);
        sb_dsp_setirq(&sb->dsp, device_get_config_int("irq"));
        sb_dsp_setdma8(&sb->dsp, device_get_config_int("dma"));
        sb_mixer_init(&sb->mixer);
        io_sethandler(addr+0, 0x0004, opl3_read,   NULL, NULL, opl3_write,   NULL, NULL, &sb->opl);
        io_sethandler(addr+8, 0x0002, opl3_read,   NULL, NULL, opl3_write,   NULL, NULL, &sb->opl);
        io_sethandler(0x0388, 0x0004, opl3_read,   NULL, NULL, opl3_write,   NULL, NULL, &sb->opl);
        io_sethandler(addr+4, 0x0002, sb_pro_mixer_read, NULL, NULL, sb_pro_mixer_write, NULL, NULL, sb);
        sound_add_handler(sb_get_buffer_opl3, sb);

        sb->mixer.regs[0x22] = 0xff;
        sb->mixer.regs[0x04] = 0xff;
        sb->mixer.regs[0x26] = 0xff;
        sb->mixer.regs[0xe]  = 0;

        return sb;
}

void *sb_pro_mcv_init()
{
        sb_t *sb = malloc(sizeof(sb_t));
        memset(sb, 0, sizeof(sb_t));

        sb->opl_emu = device_get_config_int("opl_emu");
        opl3_init(&sb->opl, sb->opl_emu);
        sb_dsp_init(&sb->dsp, SBPRO2);
        sb_mixer_init(&sb->mixer);
        io_sethandler(0x0388, 0x0004, opl3_read,   NULL, NULL, opl3_write,   NULL, NULL, &sb->opl);
        sound_add_handler(sb_get_buffer_opl3, sb);

        sb->mixer.regs[0x22] = 0xff;
        sb->mixer.regs[0x04] = 0xff;
        sb->mixer.regs[0x26] = 0xff;
        sb->mixer.regs[0xe]  = 0;

        mca_add(sb_pro_mcv_read, sb_pro_mcv_write, sb);
        sb->pos_regs[0] = 0x03;
        sb->pos_regs[1] = 0x51;

        return sb;
}

void *sb_16_init()
{
        sb_t *sb = malloc(sizeof(sb_t));
        memset(sb, 0, sizeof(sb_t));

        sb->opl_emu = device_get_config_int("opl_emu");
        opl3_init(&sb->opl, sb->opl_emu);
        sb_dsp_init(&sb->dsp, SB16);
        sb_dsp_setaddr(&sb->dsp, 0x0220);
        sb_mixer_init(&sb->mixer);
        io_sethandler(0x0220, 0x0004, opl3_read,   NULL, NULL, opl3_write,   NULL, NULL, &sb->opl);
        io_sethandler(0x0228, 0x0002, opl3_read,   NULL, NULL, opl3_write,   NULL, NULL, &sb->opl);
        io_sethandler(0x0388, 0x0004, opl3_read,   NULL, NULL, opl3_write,   NULL, NULL, &sb->opl);
        io_sethandler(0x0224, 0x0002, sb_16_mixer_read, NULL, NULL, sb_16_mixer_write, NULL, NULL, sb);
        sound_add_handler(sb_get_buffer_opl3, sb);
        mpu401_uart_init(&sb->mpu, 0x330);

        sb->mixer.regs[0x30] = 31 << 3;
        sb->mixer.regs[0x31] = 31 << 3;
        sb->mixer.regs[0x32] = 31 << 3;
        sb->mixer.regs[0x33] = 31 << 3;
        sb->mixer.regs[0x34] = 31 << 3;
        sb->mixer.regs[0x35] = 31 << 3;
        sb->mixer.regs[0x44] =  8 << 4;
        sb->mixer.regs[0x45] =  8 << 4;
        sb->mixer.regs[0x46] =  8 << 4;
        sb->mixer.regs[0x47] =  8 << 4;
        sb->mixer.regs[0x22] = (sb->mixer.regs[0x30] & 0xf0) | (sb->mixer.regs[0x31] >> 4);
        sb->mixer.regs[0x04] = (sb->mixer.regs[0x32] & 0xf0) | (sb->mixer.regs[0x33] >> 4);
        sb->mixer.regs[0x26] = (sb->mixer.regs[0x34] & 0xf0) | (sb->mixer.regs[0x35] >> 4);

        return sb;
}

int sb_awe32_available()
{
        return rom_present("awe32.raw");
}

void *sb_awe32_init()
{
        sb_t *sb = malloc(sizeof(sb_t));
        int onboard_ram = device_get_config_int("onboard_ram");
        memset(sb, 0, sizeof(sb_t));

        sb->opl_emu = device_get_config_int("opl_emu");
        opl3_init(&sb->opl, sb->opl_emu);
        sb_dsp_init(&sb->dsp, SB16 + 1);
        sb_dsp_setaddr(&sb->dsp, 0x0220);
        sb_mixer_init(&sb->mixer);
        io_sethandler(0x0220, 0x0004, opl3_read,   NULL, NULL, opl3_write,   NULL, NULL, &sb->opl);
        io_sethandler(0x0228, 0x0002, opl3_read,   NULL, NULL, opl3_write,   NULL, NULL, &sb->opl);
        io_sethandler(0x0388, 0x0004, opl3_read,   NULL, NULL, opl3_write,   NULL, NULL, &sb->opl);
        io_sethandler(0x0224, 0x0002, sb_16_mixer_read, NULL, NULL, sb_16_mixer_write, NULL, NULL, sb);
        sound_add_handler(sb_get_buffer_emu8k, sb);
        mpu401_uart_init(&sb->mpu, 0x330);       
        emu8k_init(&sb->emu8k, onboard_ram);

        sb->mixer.regs[0x30] = 31 << 3;
        sb->mixer.regs[0x31] = 31 << 3;
        sb->mixer.regs[0x32] = 31 << 3;
        sb->mixer.regs[0x33] = 31 << 3;
        sb->mixer.regs[0x34] = 31 << 3;
        sb->mixer.regs[0x35] = 31 << 3;
        sb->mixer.regs[0x44] =  8 << 4;
        sb->mixer.regs[0x45] =  8 << 4;
        sb->mixer.regs[0x46] =  8 << 4;
        sb->mixer.regs[0x47] =  8 << 4;
        sb->mixer.regs[0x22] = (sb->mixer.regs[0x30] & 0xf0) | (sb->mixer.regs[0x31] >> 4);
        sb->mixer.regs[0x04] = (sb->mixer.regs[0x32] & 0xf0) | (sb->mixer.regs[0x33] >> 4);
        sb->mixer.regs[0x26] = (sb->mixer.regs[0x34] & 0xf0) | (sb->mixer.regs[0x35] >> 4);
        
        return sb;
}

void sb_close(void *p)
{
        sb_t *sb = (sb_t *)p;
        
        free(sb);
}

void sb_awe32_close(void *p)
{
        sb_t *sb = (sb_t *)p;
        
        emu8k_close(&sb->emu8k);

        free(sb);
}

void sb_speed_changed(void *p)
{
        sb_t *sb = (sb_t *)p;
        
        sb_dsp_speed_changed(&sb->dsp);
}

void sb_add_status_info(char *s, int max_len, void *p)
{
        sb_t *sb = (sb_t *)p;
        
        sb_dsp_add_status_info(s, max_len, &sb->dsp);
}

static device_config_t sb_config[] =
{
        {
                .name = "addr",
                .description = "Address",
                .type = CONFIG_BINARY,
                .type = CONFIG_SELECTION,
                .selection =
                {
                        {
                                .description = "0x220",
                                .value = 0x220
                        },
                        {
                                .description = "0x240",
                                .value = 0x240
                        },
                        {
                                .description = ""
                        }
                },
                .default_int = 0x220
        },
        {
                .name = "irq",
                .description = "IRQ",
                .type = CONFIG_SELECTION,
                .selection =
                {
                        {
                                .description = "IRQ 2",
                                .value = 2
                        },
                        {
                                .description = "IRQ 3",
                                .value = 3
                        },
                        {
                                .description = "IRQ 5",
                                .value = 5
                        },
                        {
                                .description = "IRQ 7",
                                .value = 7
                        },
                        {
                                .description = ""
                        }
                },
                .default_int = 7
        },
        {
                .name = "dma",
                .description = "DMA",
                .type = CONFIG_SELECTION,
                .selection =
                {
                        {
                                .description = "DMA 1",
                                .value = 1
                        },
                        {
                                .description = "DMA 3",
                                .value = 3
                        },
                        {
                                .description = ""
                        }
                },
                .default_int = 1
        },
        {
                .type = -1
        }
};

static device_config_t sb_mcv_config[] =
{
        {
                .name = "irq",
                .description = "IRQ",
                .type = CONFIG_SELECTION,
                .selection =
                {
                        {
                                .description = "IRQ 3",
                                .value = 3
                        },
                        {
                                .description = "IRQ 5",
                                .value = 5
                        },
                        {
                                .description = "IRQ 7",
                                .value = 7
                        },
                        {
                                .description = ""
                        }
                },
                .default_int = 7
        },
        {
                .name = "dma",
                .description = "DMA",
                .type = CONFIG_SELECTION,
                .selection =
                {
                        {
                                .description = "DMA 1",
                                .value = 1
                        },
                        {
                                .description = "DMA 3",
                                .value = 3
                        },
                        {
                                .description = ""
                        }
                },
                .default_int = 1
        },
        {
                .type = -1
        }
};

static device_config_t sb_pro_v1_config[] =
{
        {
                .name = "addr",
                .description = "Address",
                .type = CONFIG_BINARY,
                .type = CONFIG_SELECTION,
                .selection =
                {
                        {
                                .description = "0x220",
                                .value = 0x220
                        },
                        {
                                .description = "0x240",
                                .value = 0x240
                        },
                        {
                                .description = ""
                        }
                },
                .default_int = 0x220
        },
        {
                .name = "irq",
                .description = "IRQ",
                .type = CONFIG_SELECTION,
                .selection =
                {
                        {
                                .description = "IRQ 2",
                                .value = 2
                        },
                        {
                                .description = "IRQ 5",
                                .value = 5
                        },
                        {
                                .description = "IRQ 7",
                                .value = 7
                        },
                        {
                                .description = "IRQ 10",
                                .value = 10
                        },
                        {
                                .description = ""
                        }
                },
                .default_int = 7
        },
        {
                .name = "dma",
                .description = "DMA",
                .type = CONFIG_SELECTION,
                .selection =
                {
                        {
                                .description = "DMA 1",
                                .value = 1
                        },
                        {
                                .description = "DMA 3",
                                .value = 3
                        },
                        {
                                .description = ""
                        }
                },
                .default_int = 1
        },
        {
                .type = -1
        }
};

static device_config_t sb_pro_v2_config[] =
{
        {
                .name = "addr",
                .description = "Address",
                .type = CONFIG_BINARY,
                .type = CONFIG_SELECTION,
                .selection =
                {
                        {
                                .description = "0x220",
                                .value = 0x220
                        },
                        {
                                .description = "0x240",
                                .value = 0x240
                        },
                        {
                                .description = ""
                        }
                },
                .default_int = 0x220
        },
        {
                .name = "irq",
                .description = "IRQ",
                .type = CONFIG_SELECTION,
                .selection =
                {
                        {
                                .description = "IRQ 2",
                                .value = 2
                        },
                        {
                                .description = "IRQ 5",
                                .value = 5
                        },
                        {
                                .description = "IRQ 7",
                                .value = 7
                        },
                        {
                                .description = "IRQ 10",
                                .value = 10
                        },
                        {
                                .description = ""
                        }
                },
                .default_int = 7
        },
        {
                .name = "dma",
                .description = "DMA",
                .type = CONFIG_SELECTION,
                .selection =
                {
                        {
                                .description = "DMA 1",
                                .value = 1
                        },
                        {
                                .description = "DMA 3",
                                .value = 3
                        },
                        {
                                .description = ""
                        }
                },
                .default_int = 1
        },
        {
                .name = "opl_emu",
                .description = "OPL emulator",
                .type = CONFIG_SELECTION,
                .selection =
                {
                        {
                                .description = "DBOPL",
                                .value = OPL_DBOPL
                        },
                        {
                                .description = "NukedOPL",
                                .value = OPL_NUKED
                        },
                },
                .default_int = OPL_DBOPL
        },
        {
                .type = -1
        }
};

static device_config_t sb_pro_mcv_config[] =
{
        {
                .name = "opl_emu",
                .description = "OPL emulator",
                .type = CONFIG_SELECTION,
                .selection =
                {
                        {
                                .description = "DBOPL",
                                .value = OPL_DBOPL
                        },
                        {
                                .description = "NukedOPL",
                                .value = OPL_NUKED
                        },
                },
                .default_int = OPL_DBOPL
        },
        {
                .type = -1
        }
};

static device_config_t sb_16_config[] =
{
        {
                .name = "midi",
                .description = "MIDI out device",
                .type = CONFIG_MIDI,
                .default_int = 0
        },
        {
                .name = "opl_emu",
                .description = "OPL emulator",
                .type = CONFIG_SELECTION,
                .selection =
                {
                        {
                                .description = "DBOPL",
                                .value = OPL_DBOPL
                        },
                        {
                                .description = "NukedOPL",
                                .value = OPL_NUKED
                        },
                },
                .default_int = OPL_DBOPL
        },
        {
                .type = -1
        }
};

static device_config_t sb_awe32_config[] =
{
        {
                .name = "midi",
                .description = "MIDI out device",
                .type = CONFIG_MIDI,
                .default_int = 0
        },
        {
                .name = "onboard_ram",
                .description = "Onboard RAM",
                .type = CONFIG_SELECTION,
                .selection =
                {
                        {
                                .description = "None",
                                .value = 0
                        },
                        {
                                .description = "512 KB",
                                .value = 512
                        },
                        {
                                .description = "2 MB",
                                .value = 2048
                        },
                        {
                                .description = "8 MB",
                                .value = 8192
                        },
                        {
                                .description = "28 MB",
                                .value = 28*1024
                        },
                        {
                                .description = ""
                        }
                },
                .default_int = 512
        },
        {
                .name = "opl_emu",
                .description = "OPL emulator",
                .type = CONFIG_SELECTION,
                .selection =
                {
                        {
                                .description = "DBOPL",
                                .value = OPL_DBOPL
                        },
                        {
                                .description = "NukedOPL",
                                .value = OPL_NUKED
                        },
                },
                .default_int = OPL_DBOPL
        },
        {
                .type = -1
        }
};

device_t sb_1_device =
{
        "Sound Blaster v1.0",
        0,
        sb_1_init,
        sb_close,
        NULL,
        sb_speed_changed,
        NULL,
        sb_add_status_info,
        sb_config
};
device_t sb_15_device =
{
        "Sound Blaster v1.5",
        0,
        sb_15_init,
        sb_close,
        NULL,
        sb_speed_changed,
        NULL,
        sb_add_status_info,
        sb_config
};
device_t sb_mcv_device =
{
        "Sound Blaster MCV",
        DEVICE_MCA,
        sb_mcv_init,
        sb_close,
        NULL,
        sb_speed_changed,
        NULL,
        sb_add_status_info,
        sb_mcv_config
};
device_t sb_2_device =
{
        "Sound Blaster v2.0",
        0,
        sb_2_init,
        sb_close,
        NULL,
        sb_speed_changed,
        NULL,
        sb_add_status_info,
        sb_config
};
device_t sb_pro_v1_device =
{
        "Sound Blaster Pro v1",
        0,
        sb_pro_v1_init,
        sb_close,
        NULL,
        sb_speed_changed,
        NULL,
        sb_add_status_info,
        sb_pro_v1_config
};
device_t sb_pro_v2_device =
{
        "Sound Blaster Pro v2",
        0,
        sb_pro_v2_init,
        sb_close,
        NULL,
        sb_speed_changed,
        NULL,
        sb_add_status_info,
        sb_pro_v2_config
};
device_t sb_pro_mcv_device =
{
        "Sound Blaster Pro MCV",
        DEVICE_MCA,
        sb_pro_mcv_init,
        sb_close,
        NULL,
        sb_speed_changed,
        NULL,
        sb_add_status_info,
        sb_pro_mcv_config
};
device_t sb_16_device =
{
        "Sound Blaster 16",
        0,
        sb_16_init,
        sb_close,
        NULL,
        sb_speed_changed,
        NULL,
        sb_add_status_info,
        sb_16_config
};
device_t sb_awe32_device =
{
        "Sound Blaster AWE32",
        0,
        sb_awe32_init,
        sb_close,
        sb_awe32_available,
        sb_speed_changed,
        NULL,
        sb_add_status_info,
        sb_awe32_config
};
