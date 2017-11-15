/******************************************************************************
*                                                                             *
*   Module:     flow.c                                                        *
*                                                                             *
*   Date:       10/16/00                                                      *
*                                                                             *
*   Copyright (c) 2001 - 2001 Goran Devic                                     *
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

        Flow control commands

*******************************************************************************
*                                                                             *
*   Changes:                                                                  *
*                                                                             *
*   DATE     DESCRIPTION OF CHANGES                               AUTHOR      *
* --------   ---------------------------------------------------  ----------- *
* 10/16/00   Original                                             Goran Devic *
* --------   ---------------------------------------------------  ----------- *
*******************************************************************************
*   Include Files                                                             *
******************************************************************************/

#include "module-header.h"              // Versatile module header file

#include "clib.h"                       // Include C library header file
#include "ice.h"                        // Include main debugger structures

#include "debug.h"                      // Include our dprintk()
#include "disassembler.h"               // Include disassembler

/******************************************************************************
*                                                                             *
*   Global Variables                                                          *
*                                                                             *
******************************************************************************/

/******************************************************************************
*                                                                             *
*   Local Defines, Variables and Macros                                       *
*                                                                             *
******************************************************************************/

/******************************************************************************
*                                                                             *
*   External Functions                                                        *
*                                                                             *
******************************************************************************/

extern DWORD SymFnLinAddress2NextAddress(TSYMFNLIN *pFnLin, DWORD dwAddress);
extern void SetSymbolContext(WORD wSel, DWORD dwOffset);
extern void SetNonStickyBreakpoint(TADDRDESC Addr);

// From linux/reboot.h

extern void machine_restart(char *cmd);
extern void machine_halt(void);
extern void machine_power_off(void);


/******************************************************************************
*                                                                             *
*   Functions                                                                 *
*                                                                             *
******************************************************************************/

/******************************************************************************
*                                                                             *
*   BOOL cmdXit(char *args, int subClass)                                     *
*                                                                             *
*******************************************************************************
*
*   Returns to the debugee
*
******************************************************************************/
BOOL cmdXit(char *args, int subClass)
{

    return( FALSE );
}


/******************************************************************************
*                                                                             *
*   BOOL cmdGo(char *args, int subClass)                                      *
*                                                                             *
*******************************************************************************
*
*   Runs the interrupted program
*
*   G [=start-address] [break-address]
*
*   If you specify start-address, eip is set to it before executing
*   If you spcify break-address, a one-time breakpoint is set
*
******************************************************************************/
BOOL cmdGo(char *args, int subClass)
{
    TADDRDESC StartAddr, BpAddr;
    BOOL fStart, fBreak;

    fStart = fBreak = FALSE;

    if( *args )
    {
        // Arguments present. Is it start-address?
        if( *args=='=' )
        {
            // Set the default selector to current CS
            evalSel = deb.r->cs;

            if( Expression(&StartAddr.offset, args, &args) )
            {
                // Assign the complete start address
                StartAddr.sel = evalSel;
                fStart = TRUE;
            }
            else
            {
                // Something was bad with the address
                dprinth(1, "Syntax error");
                return( TRUE );
            }
        }

        // See if we have the break address
        if( *args )
        {
            // Set the default selector to current CS
            evalSel = deb.r->cs;

            if( Expression(&BpAddr.offset, args, &args) )
            {
                // Assign the complete bp address
                BpAddr.sel = evalSel;
                fBreak = TRUE;
            }
            else
            {
                // Something was bad with the address
                dprinth(1, "Syntax error");
                return( TRUE );
            }
        }

        // If we specified start address, modify cs:eip
        if( fStart )
        {
            deb.r->cs = StartAddr.sel;
            deb.r->eip = StartAddr.offset;
        }

        // If we specified break address, set a one-time bp
        if( fBreak )
        {
            SetNonStickyBreakpoint(BpAddr);
        }
    }

    return( FALSE );
}


/******************************************************************************
*                                                                             *
*   BOOL cmdTrace(char *args, int subClass)                                   *
*                                                                             *
*******************************************************************************
*
*   Trace one instruction. Optional parameter is the number of instructions
*   that need to single step. Pressing ESC key stops single tracing of multiple
*   instructions.
*
*   This command follows the flow into subroutine calls.
*
*   Keyboard mapped to F8
*
******************************************************************************/
BOOL cmdTrace(char *args, int subClass)
{
    TDISASM Dis;                        // Disassembler interface structure
    TADDRDESC BpAddr;                   // Address of a non-sticky breakpoint
    DWORD count;
    DWORD dwNextLine;                   // Address of the next line's code
    DWORD dwAddress;                    // Current scanning address

    if( *args != 0 )
    {
        // Optional parameter is the number of instructions to single step

        if( Expression(&count, args, &args) )
        {
            if( count==0 )
                count = 1;

            deb.TraceCount = count;
        }
        else
        {
            // Invalid expression - abort trace
            dprinth(1, "Syntax error");
            return( TRUE );
        }
    }
    else
    {
        // Single instruction
        deb.TraceCount = 1;
    }

    // Depending on the source mode, we can simply use Trace flag or we need
    // to scan a block of instructions

    // First, set the current symbol content to make sure our cs:eip is at the
    // right place, since we may have it set differenly
    SetSymbolContext(deb.r->cs, deb.r->eip);

    if( (deb.eSrc==1 || deb.eSrc==2) && deb.pFnScope && deb.pFnLin )
    {
        // SOURCE CODE ACTIVE

        // Get the machine code address of the next line in the source code
        dwNextLine = SymFnLinAddress2NextAddress(deb.pFnLin, deb.r->eip);

        // Scan the block between current address/line and the next line
        // and stop if we hit a jump, ret or call
        for(dwAddress=deb.r->eip; dwAddress<dwNextLine; dwAddress += Dis.bInstrLen)
        {
            Dis.bState   = DIS_DATA32 | DIS_ADDRESS32;
            Dis.wSel     = deb.r->cs;
            Dis.dwOffset = dwAddress;
            DisassemblerLen(&Dis);
    
            Dis.bFlags &= SCAN_MASK;            // Mask the scan bits

            if( Dis.bFlags==SCAN_JUMP && Dis.dwTargetAddress )
            {
                // Jumps that are always taken are followed

                BpAddr.sel    = deb.r->cs;
                BpAddr.offset = Dis.dwTargetAddress;
                SetNonStickyBreakpoint(BpAddr);

                return( FALSE );
            }
            else
            if( Dis.bFlags==SCAN_RET || Dis.bFlags==SCAN_COND_JUMP )
            {
                // Returns and conditional jumps are delayed traced

                // Set a delayed Trace
                deb.fDelayedTrace = TRUE;
                break;
            }
            else
            if( Dis.bFlags==SCAN_CALL )
            {
                // If the instruction is a CALL to subroutine, we will follow only if we
                // have a symbol for that function in our symbol table (not exports!).
                // That will avoid tracing into common globals such is 'printk'

                if( SymAddress2FunctionName(deb.r->cs, Dis.dwTargetAddress) != NULL )
                {
                    // We have our function defined at that address, so place a bp there
                    BpAddr.sel    = deb.r->cs;
                    BpAddr.offset = Dis.dwTargetAddress;
                    SetNonStickyBreakpoint(BpAddr);

                    return( FALSE );
                }
            }
        }

        // If the address is the current cs:eip, we execute a single step, otherwise
        // set a non-sticky breakpoint at this address
        if( deb.r->eip==dwAddress )
        {
            deb.fTrace = TRUE;
        }
        else
        {
            BpAddr.sel    = deb.r->cs;
            BpAddr.offset = dwAddress;
            SetNonStickyBreakpoint(BpAddr);
        }
    }
    else
    {
        // MACHINE CODE OR MIXED - Use CPU Trace Flag

        deb.fTrace = TRUE;
    }

    // Exit into debugee...
    return( FALSE );
}


/******************************************************************************
*                                                                             *
*   BOOL cmdStep(char *args, int subClass)                                    *
*                                                                             *
*******************************************************************************
*
*   Execute one logical program step. Command P.  This skips over the calls,
*   interrupts, loops and repeated string instructions.
*
*   Keyboard mapped to F10
*
*   Optional parameter [RET] will single step until a RET or IRET instruction
*   is hit.
*
******************************************************************************/
BOOL cmdStep(char *args, int subClass)
{
    TDISASM Dis;                        // Disassembler interface structure
    TADDRDESC BpAddr;                   // Breakpoint address descriptor
    DWORD dwNextLine;                   // Address of the next line's code
    DWORD dwAddress;                    // Current scanning address

    if( *args != 0 )
    {
        // Argument must be 'RET' - step until the ret instruction
        if( strnicmp(args, "ret", 3)==0 )
        {
            deb.fStepRet = TRUE;
        }
        else
        {
            dprinth(1, "Syntax error");
            return( TRUE );
        }
    }

    deb.fStep = TRUE;

    // First, set the current symbol content to make sure our cs:eip is at the
    // right place, since we may have it set differenly
    SetSymbolContext(deb.r->cs, deb.r->eip);

    if( (deb.eSrc==1 || deb.eSrc==2) && deb.pFnScope && deb.pFnLin )
    {
        // SOURCE CODE ACTIVE

        // Get the machine code address of the next line in the source code
        dwNextLine = SymFnLinAddress2NextAddress(deb.pFnLin, deb.r->eip);

        // Scan the block between current address/line and the next line
        // and stop if we hit a jump or ret
        for(dwAddress=deb.r->eip; dwAddress<dwNextLine; dwAddress += Dis.bInstrLen)
        {
            Dis.bState   = DIS_DATA32 | DIS_ADDRESS32;
            Dis.wSel     = deb.r->cs;
            Dis.dwOffset = dwAddress;
            DisassemblerLen(&Dis);
    
            Dis.bFlags &= SCAN_MASK;            // Mask the scan bits

            if( Dis.bFlags==SCAN_RET || Dis.bFlags==SCAN_JUMP || Dis.bFlags==SCAN_COND_JUMP )
            {
                // Set a delayed Trace
                deb.fDelayedTrace = TRUE;
                break;
            }
        }

        // If the address is the current cs:eip, we execute a single step, otherwise
        // set a non-sticky breakpoint at this address
        if( deb.r->eip==dwAddress )
        {
            deb.TraceCount = 1;
            deb.fTrace = TRUE;
        }
        else
        {
            BpAddr.sel    = deb.r->cs;
            BpAddr.offset = dwAddress;
            SetNonStickyBreakpoint(BpAddr);
        }
    }
    else
    {
        // MACHINE CODE OR MIXED - Use CPU Trace Flag or skip calls and ints

        // Get the size in bytes of the current instruction and its flags
        Dis.bState   = DIS_DATA32 | DIS_ADDRESS32;
        Dis.wSel     = deb.r->cs;
        Dis.dwOffset = deb.r->eip;
        DisassemblerLen(&Dis);

        Dis.bFlags &= SCAN_MASK;            // Mask the scan bits

        if( Dis.bFlags==SCAN_CALL || Dis.bFlags==SCAN_INT )
        {
            // Call and INT instructions we skip

            // Set a non-sticky breakpoint at the next instruction
            BpAddr.sel = deb.r->cs;
            BpAddr.offset = deb.r->eip + Dis.bInstrLen;
            SetNonStickyBreakpoint(BpAddr);
        }
        else
        {
            // All other instructions we single step

            deb.TraceCount = 1;
            deb.fTrace = TRUE;
        }
    }

    // Exit into debugee...
    return( FALSE );
}


/******************************************************************************
*                                                                             *
*   BOOL cmdZap(char *args, int subClass)                                     *
*                                                                             *
*******************************************************************************
*
*   Replaces embedded INT1 or INT3 instruction with a NOP
*
******************************************************************************/
BOOL cmdZap(char *args, int subClass)
{
    TADDRDESC Addr;                     // Address to zap
    int b0;

    // Make sure the address is valid
    b0 = GetByte(deb.r->cs, deb.r->eip - 1);
    if( b0==0xCC )
    {
        Addr.sel = deb.r->cs;
        Addr.offset = deb.r->eip - 1;

        // Zap the INT3
        AddrSetByte(&Addr, 0x90);
    }
    else
    if( b0==0x01 || b0==0x03 )
    {
        if( GetByte(deb.r->cs, deb.r->eip - 2)==0xCD )
        {
            Addr.sel = deb.r->cs;

            // Zap the 2-byte INT 1 or INT 3
            Addr.offset = deb.r->eip - 1;
            AddrSetByte(&Addr, 0x90);

            Addr.offset = deb.r->eip - 2;
            AddrSetByte(&Addr, 0x90);
        }
    }

    return( TRUE );
}


/******************************************************************************
*                                                                             *
*   BOOL cmdI1here(char *args, int subClass)                                  *
*                                                                             *
*******************************************************************************
*
*   Pop up on embedded INT1 instruction.
*
******************************************************************************/
BOOL cmdI1here(char *args, int subClass)
{
    switch( GetOnOff(args) )
    {
        case 1:         // On
            deb.fI1Here = TRUE;
            deb.fI1Kernel = FALSE;
        break;

        case 2:         // Off
            deb.fI1Here = FALSE;
            deb.fI1Kernel = FALSE;
        break;

        case 3:         // Display the state of the variable
            dprinth(1, "I1Here is %s", deb.fI1Here? (deb.fI1Kernel? "kernel" : "on") : "off");
        break;

        case 4:         // Only on KERNEL code
            deb.fI1Here = TRUE;
            deb.fI1Kernel = TRUE;
        break;
    }

    return( TRUE );
}


/******************************************************************************
*                                                                             *
*   BOOL cmdI3here(char *args, int subClass)                                  *
*                                                                             *
*******************************************************************************
*
*   Pop up on embedded INT3 instruction.
*
******************************************************************************/
BOOL cmdI3here(char *args, int subClass)
{
    switch( GetOnOff(args) )
    {
        case 1:         // On
            deb.fI3Here = TRUE;
            deb.fI3Kernel = FALSE;
        break;

        case 2:         // Off
            deb.fI3Here = FALSE;
        break;

        case 3:         // Display the state of the variable
            dprinth(1, "I3Here is %s", deb.fI3Here? (deb.fI3Kernel? "kernel" : "on") : "off");
        break;

        case 4:         // Only on KERNEL code
            deb.fI3Here = TRUE;
            deb.fI3Kernel = TRUE;
        break;
    }

    return( TRUE );
}


/******************************************************************************
*                                                                             *
*   BOOL cmdHboot(char *args, int subClass)                                   *
*                                                                             *
*******************************************************************************
*
*   Total computer reset
*
******************************************************************************/
BOOL cmdHboot(char *args, int subClass)
{
    // Linux: process.c

    machine_restart(NULL);

    return( TRUE );
}


/******************************************************************************
*                                                                             *
*   BOOL cmdHalt(char *args, int subClass)                                    *
*                                                                             *
*******************************************************************************
*
*   Powers computer off
*
******************************************************************************/
BOOL cmdHalt(char *args, int subClass)
{
    // Use APM power off:
    machine_power_off();

    // If that fails, we hboot-it
    machine_restart(NULL);

    return( TRUE );
}

