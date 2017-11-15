/******************************************************************************
*                                                                             *
*   Module:     interrupt.c                                                   *
*                                                                             *
*   Date:       04/28/00                                                      *
*                                                                             *
*   Copyright (c) 2000 Goran Devic                                            *
*                                                                             *
*   Author:     Goran Devic                                                   *
*                                                                             *
*   This source code and produced executable is copyrighted by Goran Devic.   *
*   This source, portions or complete, and its derivatives can not be given,  *
*   copied, or distributed by any means without explicit written permission   *
*   of the copyright owner. All other rights, including intellectual          *
*   property rights, are implicitly reserved. There is no guarantee of any    *
*   kind that this software would perform, and nobody is liable for the       *
*   consequences of running it. Use at your own risk.                         *
*                                                                             *
*******************************************************************************

    Module Description:

        This module contains debugger interrupt handlers and functions.

*******************************************************************************
*                                                                             *
*   Changes:                                                                  *
*                                                                             *
*   DATE     DESCRIPTION OF CHANGES                               AUTHOR      *
* --------   ---------------------------------------------------  ----------- *
* 04/28/00   Original                                             Goran Devic *
* 09/11/00   Second edition                                       Goran Devic *
* --------   ---------------------------------------------------  ----------- *
*******************************************************************************
*   Include Files                                                             *
******************************************************************************/

#include "module-header.h"              // Versatile module header file

#include "clib.h"                       // Include C library header file
#include "ice.h"                        // Include main debugger structures
#include "ibm-pc.h"                     // Include PC architecture defines
#include "debug.h"                      // Include our dprintk()
#include "intel.h"                      // Include processor specific stuff
#include "iceface.h"                    // Include iceface module stub protos

/******************************************************************************
*                                                                             *
*   Global Variables                                                          *
*                                                                             *
******************************************************************************/

extern void MemAccess_START();
extern void MemAccess_END();
extern void MemAccess_FAULT();

extern DWORD IceIntHandlers[0x30];
extern DWORD IceIntHandler80;

extern void DebuggerEnterBreak(void);
extern void DebuggerEnterDelayedTrace(void);
extern void DebuggerEnterDelayedArm(void);

typedef void (*TDEBUGGER_STATE_FN)(void);

TDEBUGGER_STATE_FN DebuggerEnter[MAX_DEBUGGER_STATE] =
{
    DebuggerEnterBreak,
    DebuggerEnterDelayedTrace,
    DebuggerEnterDelayedArm
};


// From apic.c

extern int IrqRedirect(int nIrq);
extern void GetIrqRedirection(void);
extern int ReverseMapIrq(int nInt);
extern void IoApicClamp(int cpu);
extern void IoApicUnclamp();
extern void smpSpinOtherCpus(void);


/******************************************************************************
*                                                                             *
*   Local Defines, Variables and Macros                                       *
*                                                                             *
******************************************************************************/

// Array where we store the original Linux IDT entries at init time

TIDT_Gate LinuxIdt[256];

// Array that holds debugger private IDT table for the duration of its run

TIDT_Gate IceIdt[256];
TDescriptor IceIdtDescriptor;

/******************************************************************************
*                                                                             *
*   Functions                                                                 *
*                                                                             *
******************************************************************************/

extern void LoadIDT(PTDescriptor pIdt);
extern void KeyboardHandler(void);
extern void SerialHandler(int port);
extern void MouseHandler(void);

extern DWORD TestAndReset(DWORD *pSpinlock);
extern DWORD SpinlockTest(DWORD *pSpinlock);
extern void  SpinlockSet(DWORD *pSpinlock);
extern void  SpinlockReset(DWORD *pSpinlock);

extern void IntAck(int nInt);


/******************************************************************************
*                                                                             *
*   void HookIdt(PTIDT_Gate pGate, int nIntNumber)                            *
*                                                                             *
*******************************************************************************
*
*   Hooks a single IDT entry
*
*   Where:
*       pGate is the pointer to an int entry to hook
*       nIntNumber is the interrupt number function to hook
*
******************************************************************************/
static void HookIdt(TIDT_Gate IDT[], int nEntryNumber, int nIntNumber)
{
    DWORD IntHandlerFunction;
    TIDT_Gate *pGate;

    IntHandlerFunction = IceIntHandlers[nIntNumber];

    nEntryNumber = IrqRedirect(nEntryNumber);

    pGate = &IDT[nEntryNumber];

    pGate->offsetLow  = LOWORD(IntHandlerFunction);
    pGate->offsetHigh = HIWORD(IntHandlerFunction);
    pGate->selector   = GetKernelCS();
    pGate->type       = INT_TYPE_INT32;
    pGate->dpl        = 3;
    pGate->present    = TRUE;
}


/******************************************************************************
*                                                                             *
*   void InterruptInit(void)                                                  *
*                                                                             *
*******************************************************************************
*
*   Initializes interrupt tables. Called once from init.c
*
******************************************************************************/
void InterruptInit(void)
{
    GET_IDT(deb.idt);

    // Initialize our IRQ redirection table for cases when IO APIC is present
    GetIrqRedirection();

    // Copy original Linux IDT to our local buffer

    memcpy(LinuxIdt, (void *)deb.idt.base, deb.idt.limit+1);

    // Copy the same IDT into our private IDT and initialize its descriptor

    memcpy(IceIdt, (void *)deb.idt.base, deb.idt.limit+1);
    IceIdtDescriptor.base = (DWORD) IceIdt;
    IceIdtDescriptor.limit = deb.idt.limit;

    // Hook private IDT with debuger-run hooks - this IDT is switched to
    // during the debugger run, so we want to "hook" most of them

    // These are mostly BAD if they happen:

    HookIdt(IceIdt,0x00, 0x0);        // Divide error
    HookIdt(IceIdt,0x01, 0x1);        // INT 1
    HookIdt(IceIdt,0x03, 0x3);        // INT 3
    HookIdt(IceIdt,0x04, 0x4);        // Overflow error
    HookIdt(IceIdt,0x06, 0x6);        // Invalid opcode
    HookIdt(IceIdt,0x08, 0x8);        // Double-fault
    HookIdt(IceIdt,0x0A, 0xA);        // Invalid TSS
    HookIdt(IceIdt,0x0B, 0xB);        // Segment not present
    HookIdt(IceIdt,0x0C, 0xC);        // Stack exception
    HookIdt(IceIdt,0x0D, 0xD);        // GP fault (also used with memaccess)

    // These we expect to happen:
    HookIdt(IceIdt,0x0E, 0x0E);       // Page fault (used with memaccess)
    HookIdt(IceIdt,0x20, 0x20);       // PIT
    HookIdt(IceIdt,0x21, 0x21);       // Keyboard
    HookIdt(IceIdt,0x23, 0x23);       // COM2
    HookIdt(IceIdt,0x24, 0x24);       // COM1
    HookIdt(IceIdt,0x2C, 0x2C);       // PS/2 Mouse
}


/******************************************************************************
*                                                                             *
*   void HookDebuger(void)                                                    *
*                                                                             *
*******************************************************************************
*
*   Hooks IDT entries of monitoring debugee. Modifies individual entries
*   of Linux kernel IDT in place.
*
******************************************************************************/
void HookDebuger(void)
{
    PTIDT_Gate pIdt = (PTIDT_Gate) deb.idt.base;

    // We hook selected CPU interrupts and traps. This makes the debugger armed.

    HookIdt(pIdt, 0x01, 0x1);          // Int 1
    HookIdt(pIdt, 0x03, 0x3);          // Int 3
    HookIdt(pIdt, 0x08, 0x8);          // Double-fault
    //
    // TODO: Hooking GPF breaks X on 2.4.x kernel
    //
//    HookIdt(pIdt, 0x0D, 0xD);          // GP fault
//    HookIdt(pIdt, 0x0E, 0xE);          // Page fault
    HookIdt(pIdt, 0x20, 0x20);         // PIT

//    HookIdt(pIdt, 0x21, 0x21);         // Keyboard
//    HookIdt(pIdt, 0x23, 0x23);         // COM2
//    HookIdt(pIdt, 0x24, 0x24);         // COM1

//    HookIdt(pIdt, 0x80, 0x80);         // System call interrupt
}


/******************************************************************************
*                                                                             *
*   void UnHookDebuger(void)                                                  *
*                                                                             *
*******************************************************************************
*
*   Unhooks all Linux IDT entries by reverting to the original saved values.
*
******************************************************************************/
void UnHookDebuger(void)
{
    // Copy back the original Linux IDT

    if( deb.idt.base )
        memcpy((void *)deb.idt.base, LinuxIdt, deb.idt.limit+1);
}


/******************************************************************************
*                                                                             *
*   void Panic(void)                                                          *
*                                                                             *
*******************************************************************************
*
*   This is a panic handler. It does the best to try to print some info,
*   and then hungs the system.
*
******************************************************************************/
void Panic(void)
{
    DWORD *p = (DWORD *)deb.r;
    int i;

    pOut->x = 0;
    pOut->y = 0;
    dprint("ICE-PANIC: Int: %d\r\n", deb.nInterrupt);
    dprint("EAX=%08X EBX=%08X ECX=%08X EDX=%08X ESI=%08X EDI=%08X\r\n",
            deb.r->eax, deb.r->ebx, deb.r->ecx, deb.r->edx, deb.r->esi, deb.r->edi);
    dprint("CS:EIP=%04X:%08X  SS:ESP=%04X:%08X EBP=%08X\r\n",
            deb.r->cs, deb.r->eip, deb.r->ss, deb.r->esp, deb.r->ebp);
    dprint("DS=%04X ES=%04X FS=%04X GS=%04X\r\n",
            deb.r->ds, deb.r->es, deb.r->fs, deb.r->gs );

    for( i=0; i<13; i++)
    {
        dprint("%08X  \n", *p++);
    }

#if 1
    LocalCLI();
    while( TRUE );
#endif
}




/******************************************************************************
*                                                                             *
*   DWORD InterruptHandler( DWORD nInt, TRegs *pRegs )                        *
*                                                                             *
*******************************************************************************
*
*   This function is called on an exception/interrupt/trap.  It may originate
*   either in the debugee or while running debugger.
*
*   Where:
*       nInt is the interrupt number that happened
*       pRegs is the register structure on the stack
*
*   Returns:
*       NULL to return to the original interrupted application/kernel
*       ADDRESS to chain the interrupt to that specified handler address
*
******************************************************************************/
DWORD InterruptHandler( DWORD nInt, PTREGS pRegs )
{
    //------------------------------------------------------------------------
    DWORD chain;
//    BYTE savePIC1;

    // Depending on the execution context, branch

    if( SpinlockTest(&pIce->fRunningIce) )
    {
        //---------------------------------------------
        //  Exception occurred during the Linice run
        //---------------------------------------------
        chain = 0;                      // By default, do not chain from this handler

        pIce->nIntsIce[nInt & 0x3F]++;

        // Handle some limited number of interrupts, puke on the rest

        switch( nInt )
        {
            // ---------------- Hardware Interrupts ----------------------

            case 0x20:      // Timer
                // Put all required timers here
                if( pIce->timer[0] )   pIce->timer[0]--;        // Serial polling
                if( pIce->timer[1] )   pIce->timer[1]--;        // Cursor carret blink
                break;

            case 0x21:      // Keyboard interrupt
                KeyboardHandler();
                break;

            case 0x23:      // COM2 / COM4
                SerialHandler(1);
                break;

            case 0x24:      // COM1 / COM3
                SerialHandler(0);
                break;

            case 0x2C:      // PS/2 Mouse - handled within the kbd function
                KeyboardHandler();
                break;

            // ---------------- CPU Traps and Faults ---------------------

            case 0x0E:      // PAGE FAULT - we handle internal page faults
                            // from the very specific functions differently:
                            //  GetByte() and GetDWORD()
                if( (pRegs->eip > (DWORD)MemAccess_START) && (pRegs->eip < (DWORD)MemAccess_END) )
                {
                    // Set the default value for invalid data / page fault into eax and
                    // position EIP to the function epilogue

                    pRegs->eax = MEMACCESS_PF;          // Page not present error code
                    pRegs->eip = (DWORD)MemAccess_FAULT;

                    break;
                }
                chain = GET_IDT_BASE( &LinuxIdt[ReverseMapIrq(nInt)] );
                break;

            case 0x0D:      // GP FAULT - we handle internal GP faults
                            // from the very specific functions differently:
                            //  GetByte() and GetDWORD()
                if( (pRegs->eip > (DWORD)MemAccess_START) && (pRegs->eip < (DWORD)MemAccess_END) )
                {
                    // Set the default value for invalid data into eax and
                    // position EIP to the function epilogue

                    pRegs->eax = MEMACCESS_GPF;         // Invalid selector error code
                    pRegs->eip = (DWORD)MemAccess_FAULT;

                    break;
                }

                // TODO: Chanining to kernel GPF will most likely cause oops, so we want to
                // do something different eventually...

                chain = GET_IDT_BASE( &LinuxIdt[ReverseMapIrq(nInt)] );
                break;

            // -----------------------------------------------------------
            // Default case still catches IRQs and traps that we dont process,
            // such is HDD interrupt etc. We simply bounce them back to kernel
            // interrupt handler
            default:
                chain = GET_IDT_BASE( &LinuxIdt[ReverseMapIrq(nInt)] );
        }

        // Acknowledge the interrupt controller only if we processed interrupt
        if( !chain )
            IntAck(nInt);
    }
    else
    {
        //---------------------------------------------
        //  Exception occurred during the debugee run
        //---------------------------------------------
        pIce->nIntsPass[nInt & 0x3F]++;

        chain = GET_IDT_BASE( &LinuxIdt[ReverseMapIrq(nInt)] );

        // We break into the debugger for all interrupts except the
        // timer, in which case we need to check for the keyboard activation flag

        if( nInt!=0x20 || (nInt==0x20 && TestAndReset(&pIce->fKbdBreak)) )
        {
            // Store the CPU number that we happen to be using
            deb.cpu = ice_smp_processor_id();

            // Limit all IRQ's to the current CPU
            IoApicClamp( deb.cpu );

            SpinlockSet(&pIce->fRunningIce);

            // Acknowledge the interrupt controller
            IntAck(nInt);

            // Explicitly enable interrupts on serial ports
//                    savePIC1 = inp(0x21);
//                    outp(0x21, savePIC1 & ~((1<<4) | (1<<3)));

//            RunDebugger();      // Run the debugger now
            //------------------------------------------------------------------------
            // Continues running within the debugger. Returns when the debugger
            // issues an execute instruction
            //
            {
                // Store the address of the registers into the debugee state structure
                // and the current interrupt number

                deb.r = pRegs;
                deb.nInterrupt = nInt;

                // Get the current GDT table
                GET_GDT(deb.gdt);

                // Restore original Linux IDT table since we may want to examine it
                // using the debugger
                UnHookDebuger();

                // Switch to our private IDT that is already hooked
                SET_IDT(IceIdtDescriptor);

                // Be sure to enable interrupts so we can operate
                LocalSTI();

                // Now that we have enabled local interrupts, we can send a message to
                // all other CPUs to interrupt what they've been doing and start spinning
                // on our `in debugger' semaphore. That we do so they dont execute other
                // code in the 'background'
                //
                smpSpinOtherCpus();

                // Call the debugger entry function of the current state
                if( pIce->eDebuggerState < MAX_DEBUGGER_STATE )
                    (DebuggerEnter[pIce->eDebuggerState])();
                else
                    (DebuggerEnter[DEB_STATE_BREAK])();

                // We are ready to continue execution of the system. The controlling CPU
                // had been armed, but other CPUs also need to have their debug registers
                // set up
        //        smpSetSysRegs();

                // Disable interrupts so we can mess with IDT
                LocalCLI();

                // Load debugee IDT
                SET_IDT(deb.idt);

                // Hook again the debugee IDT
                HookDebuger();
            }


            chain = 0;          // Continue into the debugee, do not chain

            // Restore the state of the PIC1
//                    outp(0x21, savePIC1);

            SpinlockReset(&pIce->fRunningIce);

            IoApicUnclamp();
        }
    }

    return( chain );
}

