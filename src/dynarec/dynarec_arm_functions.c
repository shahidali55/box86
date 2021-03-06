#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <errno.h>
#include <string.h>
#include <math.h>
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>

#include "debug.h"
#include "box86context.h"
#include "dynarec.h"
#include "emu/x86emu_private.h"
#include "tools/bridge_private.h"
#include "x86run.h"
#include "x86emu.h"
#include "box86stack.h"
#include "callback.h"
#include "emu/x86run_private.h"
#include "emu/x87emu_private.h"
#include "x86trace.h"
#include "dynarec_arm.h"
#include "dynarec_arm_private.h"
#include "dynarec_arm_functions.h"

void arm_popf(x86emu_t* emu, uint32_t f)
{
    emu->packed_eflags.x32 = (f & 0x3F7FD7) | 0x2; // mask off res2 and res3 and on res1
    UnpackFlags(emu);
    RESET_FLAGS(emu);
}

void arm_fstp(x86emu_t* emu, void* p)
{
    if(ST0.ll!=STld(0).ref)
        D2LD(&ST0.d, p);
    else
        memcpy(p, &STld(0).ld, 10);
}

void arm_print_armreg(x86emu_t* emu, uintptr_t reg, uintptr_t n)
{
    dynarec_log(LOG_DEBUG, "R%d=0x%x (%d)\n", n, reg, reg);
}

void arm_f2xm1(x86emu_t* emu)
{
    ST0.d = exp2(ST0.d) - 1.0;
}
void arm_fyl2x(x86emu_t* emu)
{
    ST(1).d = log2(ST0.d)*ST(1).d;
}
void arm_ftan(x86emu_t* emu)
{
    ST0.d = tan(ST0.d);
}
void arm_fpatan(x86emu_t* emu)
{
    ST1.d = atan2(ST1.d, ST0.d);
}
void arm_fxtract(x86emu_t* emu)
{
    int32_t tmp32s = (ST1.ll&0x7ff0000000000000LL)>>52;
    tmp32s -= 1023;
    ST1.d /= exp2(tmp32s);
    ST0.d = tmp32s;
}
void arm_fprem(x86emu_t* emu)
{
    int32_t tmp32s = ST0.d / ST1.d;
    ST0.d -= ST1.d * tmp32s;
    emu->sw.f.F87_C2 = 0;
    emu->sw.f.F87_C0 = (tmp32s&1);
    emu->sw.f.F87_C3 = ((tmp32s>>1)&1);
    emu->sw.f.F87_C1 = ((tmp32s>>2)&1);
}
void arm_fyl2xp1(x86emu_t* emu)
{
    ST(1).d = log2(ST0.d + 1.0)*ST(1).d;
}
void arm_fsincos(x86emu_t* emu)
{
    sincos(ST1.d, &ST1.d, &ST0.d);
}
void arm_frndint(x86emu_t* emu)
{
    ST0.d = fpu_round(emu, ST0.d);
}
void arm_fscale(x86emu_t* emu)
{
    ST0.d *= exp2(trunc(ST1.d));
}
void arm_fsin(x86emu_t* emu)
{
    ST0.d = sin(ST0.d);
}
void arm_fcos(x86emu_t* emu)
{
    ST0.d = cos(ST0.d);
}

void arm_fbld(x86emu_t* emu, uint8_t* ed)
{
    fpu_fbld(emu, ed);
}

void arm_fild64(x86emu_t* emu, int64_t* ed)
{
    ST0.d = *ed;
    STll(0).ll = *ed;
    STll(0).ref = ST0.ll;
}

void arm_fbstp(x86emu_t* emu, uint8_t* ed)
{
    fpu_fbst(emu, ed);
}

void arm_fistp64(x86emu_t* emu, int64_t* ed)
{
    // used of memcpy to avoid aligments issues
    if(STll(0).ref==ST(0).ll) {
        memcpy(ed, &STll(0).ll, sizeof(int64_t));
    } else {
        int64_t tmp;
        if(isgreater(ST0.d, (double)(int64_t)0x7fffffffffffffffLL) || isless(ST0.d, (double)(int64_t)0x8000000000000000LL))
            tmp = 0x8000000000000000LL;
        else
            tmp = (int64_t)ST0.d;
        memcpy(ed, &tmp, sizeof(tmp));
    }
}

void arm_fld(x86emu_t* emu, uint8_t* ed)
{
    memcpy(&STld(0).ld, ed, 10);
    LD2D(&STld(0), &ST(0).d);
    STld(0).ref = ST0.ll;
}

void arm_ud(x86emu_t* emu)
{
    kill(getpid(), SIGILL);
}

void arm_fxsave(x86emu_t* emu, uint8_t* ed)
{
    // should save flags & all
    // copy MMX regs...
    for(int i=0; i<8; ++i)
        memcpy(ed+32+i*16, &emu->mmx[0], sizeof(emu->mmx[0]));
    // copy SSE regs
    memcpy(ed+160, &emu->xmm[0], sizeof(emu->xmm));
    // put also FPU regs in a reserved area...
    for(int i=0; i<8; ++i)
        memcpy(ed+416+i*8, &emu->fpu[0], sizeof(emu->fpu[0]));
}

void arm_fxrstor(x86emu_t* emu, uint8_t* ed)
{
    // should restore flags & all
    // copy back MMX regs...
    for(int i=0; i<8; ++i)
        memcpy(&emu->mmx[i], ed+32+i*16, sizeof(emu->mmx[0]));
    // copy SSE regs
    memcpy(&emu->xmm[0], ed+160/4, sizeof(emu->xmm));
    for(int i=0; i<8; ++i)
        memcpy(&emu->fpu[0], ed+416+i*8, sizeof(emu->fpu[0]));
}

void arm_fsave(x86emu_t* emu, uint8_t* ed)
{
    fpu_savenv(emu, (char*)ed, 0);

    uint8_t* p = ed;
    p += 28;
    for (int i=0; i<8; ++i) {
        #ifdef USE_FLOAT
        d = ST(i).f;
        LD2D(p, &d);
        #else
        LD2D(p, &ST(i).d);
        #endif
        p+=10;
    }
}
void arm_frstor(x86emu_t* emu, uint8_t* ed)
{
    fpu_loadenv(emu, (char*)ed, 0);

    uint8_t* p = ed;
    p += 28;
    for (int i=0; i<8; ++i) {
        #ifdef USE_FLOAT
        D2LD(&d, p);
        ST(i).f = d;
        #else
        D2LD(&ST(i).d, p);
        #endif
        p+=10;
    }

}


// Get a FPU single scratch reg
int fpu_get_scratch_single(dynarec_arm_t* dyn)
{
    return dyn->fpu_scratch++;  // return an Sx
}
// Get a FPU double scratch reg
int fpu_get_scratch_double(dynarec_arm_t* dyn)
{
    int i = (dyn->fpu_scratch+1)&(~1);
    dyn->fpu_scratch = i+2;
    return i/2; // return a Dx
}
// Get a FPU quad scratch reg
int fpu_get_scratch_quad(dynarec_arm_t* dyn)
{
    int i = (dyn->fpu_scratch+3)&(~3);
    dyn->fpu_scratch = i+4;
    return i/2; // Return a Dx, not a Qx
}
// Reset scratch regs counter
void fpu_reset_scratch(dynarec_arm_t* dyn)
{
    dyn->fpu_scratch = 0;
}
#define FPUFIRST    8
// Get a FPU double reg
int fpu_get_reg_double(dynarec_arm_t* dyn)
{
    // TODO: check upper limit?
    int i=0;
    while (dyn->fpuused[i]) ++i;
    dyn->fpuused[i] = 1;
    return i+FPUFIRST; // return a Dx
}
// Free a FPU double reg
void fpu_free_reg_double(dynarec_arm_t* dyn, int reg)
{
    // TODO: check upper limit?
    int i=reg-FPUFIRST;
    dyn->fpuused[i] = 0;
}
// Get a FPU quad reg
int fpu_get_reg_quad(dynarec_arm_t* dyn)
{
    int i=0;
    while (dyn->fpuused[i] || dyn->fpuused[i+1]) i+=2;
    dyn->fpuused[i] = dyn->fpuused[i+1] = 1;
    return i+FPUFIRST; // Return a Dx, not a Qx
}
// Free a FPU quad reg
void fpu_free_reg_quad(dynarec_arm_t* dyn, int reg)
{
    int i=reg-FPUFIRST;
    dyn->fpuused[i] = dyn->fpuused[i+1] = 0;
}
// Reset fpu regs counter
void fpu_reset_reg(dynarec_arm_t* dyn)
{
    dyn->fpu_reg = 0;
    for (int i=0; i<24; ++i)
        dyn->fpuused[i]=0;
}

// Get if ED will have the correct parity. Not emiting anything. Parity is 2 for DWORD or 3 for QWORD
int getedparity(dynarec_arm_t* dyn, int ninst, uintptr_t addr, uint8_t nextop, int parity)
{
#define F8      *(uint8_t*)(addr++)
#define F32     *(uint32_t*)(addr+=4, addr-4)

    uint32_t tested = (1<<parity)-1;
    if((nextop&0xC0)==0xC0)
        return 0;   // direct register, no parity...
    if(!(nextop&0xC0)) {
        if((nextop&7)==4) {
            uint8_t sib = F8;
            int sib_reg = (sib>>3)&7;
            if((sib&0x7)==5) {
                uint32_t tmp = F32;
                if (sib_reg!=4) {
                    // if XXXXXX+reg<<N then check parity of XXXXX and N should be enough
                    return ((tmp&tested)==0 && (sib>>6)>=parity)?1:0;
                } else {
                    // just a constant...
                    return (tmp&tested)?0:1;
                }
            } else {
                if(sib_reg==4)
                    return 0;   // simple [reg]
                // don't try [reg1 + reg2<<N], unless reg1 is ESP
                return ((sib&0x7)==4 && (sib>>6)>=parity)?1:0;
            }
        } else if((nextop&7)==5) {
            uint32_t tmp = F32;
            return (tmp&tested)?0:1;
        } else {
            return 0;
        }
    } else {
        return 0; //Form [reg1 + reg2<<N + XXXXXX]
    }
#undef F8
#undef F32
}
