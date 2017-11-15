/******************************************************************************
*                                                                             *
*   Module:     symbolTable.c                                                 *
*                                                                             *
*   Date:       10/21/00                                                      *
*                                                                             *
*   Copyright (c) 2001 Goran Devic                                            *
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

        This module contains code for symbol table management

*******************************************************************************
*                                                                             *
*   Major changes:                                                            *
*                                                                             *
*   DATE     DESCRIPTION OF CHANGES                               AUTHOR      *
* --------   ---------------------------------------------------  ----------- *
* 10/21/00   Initial version                                      Goran Devic *
* --------   ---------------------------------------------------  ----------- *
*******************************************************************************
*   Include Files                                                             *
******************************************************************************/

#include "module-header.h"              // Versatile module header file

#include <asm/uaccess.h>                // User space memory access functions
#include <linux/string.h>

#define __NO_VERSION__
#include <linux/module.h>

#include "clib.h"                       // Include C library header file
#include "ice.h"                        // Include main debugger structures

#include "debug.h"                      // Include our dprintk()

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
*   Functions                                                                 *
*                                                                             *
******************************************************************************/

extern BYTE *ice_malloc(DWORD size);
extern void ice_free(BYTE *p);
extern void ice_free_heap(BYTE *pHeap);
extern struct module *FindModule(const char *name);

BOOL SymbolTableRemove(char *pTableName, TSYMTAB *pRemove);
void SymTabRelocate(TSYMTAB *pSymTab, int offCode, int offData);

/******************************************************************************
*                                                                             *
*   int CheckSymtab(TSYMTAB *pSymtab)                                         *
*                                                                             *
*******************************************************************************
*
*   Checks the consistency of the symbol table structrure
*
*   Returns: Numerical value of erroreous field (look source) or 0 if ok
*
******************************************************************************/

// TODO: Need a smarter way to check the address sanity...
#define CHKADDR(address) (((DWORD)(address)>0xC0000000)&&((DWORD)(address)<0xD0000000))

int CheckSymtab(TSYMTAB *pSymtab)
{

    return(0);
}


/******************************************************************************
*                                                                             *
*   int UserAddSymbolTable(void *pSymUser)                                    *
*                                                                             *
*******************************************************************************
*
*   Adds a symbol table from the header.
*
*   Where:
*       pSymUser is the address of the symbol table header in the user space
*
*   Returns:
*       0 init ok
*       -EINVAL Bad symbol table
*       -EFAULT general failure
*       -ENOMEM not enough memory
*
******************************************************************************/
int UserAddSymbolTable(void *pSymUser)
{
    int retval = -EINVAL;
    TSYMTAB SymHeader;
    TSYMTAB *pSymTab;                   // Symbol table in debugger buffer
    TSYMPRIV *pPriv;
    BYTE *pInitFunctionOffset;
    DWORD dwInitFunctionSymbol;
    DWORD dwDataReloc;
    TSYMRELOC  *pReloc;                 // Symbol table relocation header
    struct module *image;               // *this* module kernel descriptor

    // Copy the header of the symbol table into the local structur to examine it
    if( copy_from_user(&SymHeader, pSymUser, sizeof(TSYMTAB))==0 )
    {
        // Check if we should allocate memory for the symbol table from the allowed pool
        if( pIce->nSymbolBufferAvail >= SymHeader.dwSize )
        {
            // Allocate memory for complete symbol table from the private pool
            pSymTab = (TSYMTAB *) ice_malloc((unsigned int)SymHeader.dwSize);
            if( pSymTab )
            {
                INFO(("Allocated %d bytes at %X for symbol table\n", (int) SymHeader.dwSize, (int) pSymTab));

                // Allocate memory for the private section of the symbol table
                pPriv = (TSYMPRIV *) ice_malloc(sizeof(TSYMPRIV));
                if( pPriv )
                {
                    INFO(("Allocated %d bytes for private symbol table structure\n", sizeof(TSYMPRIV) ));

                    // Copy the complete symbol table from the use space
                    if( copy_from_user(pSymTab, pSymUser, SymHeader.dwSize)==0 )
                    {
                        // Make sure we are really loading a symbol table
                        if( !strcmp(pSymTab->sSig, SYMSIG) )
                        {
                            // Compare the symbol table version - major number has to match
                            if( (pSymTab->Version>>8)==(SYMVER>>8) )
                            {
                                // Call the remove function that will remove this particular
                                // symbol table if we are reloading it
                                SymbolTableRemove(pSymTab->sTableName, NULL);

                                dprinth(1, "Loaded symbols for module `%s' size %d (ver %d.%d)",
                                    pSymTab->sTableName, pSymTab->dwSize, pSymTab->Version>>8, pSymTab->Version&0xFF);

                                // Link the private data structure to the symbol table
                                pSymTab->pPriv = pPriv;

                                pIce->nSymbolBufferAvail -= SymHeader.dwSize;

                                // Link this symbol table in the linked list and also make it current
                                pSymTab->pPriv->next = (struct TSYMTAB *) pIce->pSymTab;

                                pIce->pSymTab = pSymTab;
                                pIce->pSymTabCur = pIce->pSymTab;

                                // Initialize elements of the private symbol table structure

                                pPriv->pStrings = (char*)((DWORD) pSymTab + pSymTab->dStrings);

                                // If the symbol table being loaded describes a kernel module, we need to
                                // see if that module is already loaded, and if so, relocate its symbols

                                pPriv->relocCode = 0;
                                pPriv->relocData = 0;

                                // Try to find if a module with that symbol name has already been loaded
                                image = FindModule(pSymTab->sTableName);
                                if( image )
                                {
                                    // Module is already loaded - we need to relocate symbol table based on it

                                    // Get the offset of the init_module global symbol from this symbol table
                                    if( SymbolName2Value(pSymTab, &dwInitFunctionSymbol, "init_module") )
                                    {
                                        dprinth(1, "Relocating symbols for `%s'", pSymTab->sTableName);

                                        // Details of relocation scheme are explained in ParseReloc.c file
                        
                                        // --- relocating code section ---
                        
                                        // Get the real kernel address of the init_module function after relocation
                                        pInitFunctionOffset = (BYTE *) image->init;
                            
                                        // Store private reloc adjustment values
                                        pSymTab->pPriv->relocCode = (int)(pInitFunctionOffset - dwInitFunctionSymbol);
                        
                                        // --- relocating data section ---
                        
                                        // Find the symbol table relocation block
                                        pReloc = (TSYMRELOC *) SymTabFindSection(pSymTab, HTYPE_RELOC);
                                        if( pReloc )
                                        {
//                                            dprinth(1, "refOffset=%08X symOffset=%08X", pReloc->refOffset, pReloc->symOffset );
                        
                                            // Find the address within the code segment from which we will read the offset to
                                            // our data. Relocation block contains the relative offset from the init_module function
                                            // to our dword sample that we need to take.
                        
                                            dwDataReloc = *(DWORD *) ((DWORD)image->init - pReloc->refOffset);
                        
                                            pSymTab->pPriv->relocData = dwDataReloc - pReloc->symOffset;
                        
                                        }
                                        else
                                        {
                                            // There was not a single global variable to use for relocation. Odd, but
                                            // possible... In that case it does not really matter not to relocate data...
                        
                                            dprinth(1, "Symbol table missing HTYPE_RELOC so setting data==code");
                        
                                            pSymTab->pPriv->relocData = pSymTab->pPriv->relocCode;
                                        }
                                
                                        // Relocate symbol table by the required offset
                                        SymTabRelocate(pSymTab, pSymTab->pPriv->relocCode, pSymTab->pPriv->relocData);
                                    }
                                }

                                // Return OK
                                return( 0 );
                            }
                            else
                            {
                                dprinth(1, "Error: Symbol table has incompatible version number!");
                            }
                        }
                        else
                        {
                            dprinth(1, "Invalid symbol table signature!");
                        }
                    }
                    else
                    {
                        ERROR(("Error copying symbol table"));
                        retval = -EFAULT;
                    }

                    // Deallocate memory for the private symbol table structure
                    ice_free_heap((void *) pPriv);
                }
                else
                {
                    ERROR(("Unable to allocate %d for private symbol table structure!\n", sizeof(TSYMPRIV)));
                    retval = -ENOMEM;
                }

                // Deallocate memory for symbol table
                ice_free_heap((void *) pSymTab);
            }
            else
            {
                ERROR(("Unable to allocate %d for symbol table!\n", (int) SymHeader.dwSize));
                retval = -ENOMEM;
            }
        }
        else
        {
            dprinth(1, "Symbol table memory pool too small to load this table!");
            retval = -ENOMEM;
        }
    }
    else
    {
        ERROR(("Invalid IOCTL packet address\n"));
        retval = -EFAULT;
    }

    return( retval );
}


/******************************************************************************
*                                                                             *
*   int UserRemoveSymbolTable(void *pSymtab)                                  *
*                                                                             *
*******************************************************************************
*
*   Removes a symbol table from the debugger.
*
*   Where:
*       pSymtab is the address of the symbol table name string in the user space
*
*   Returns:
*       0 remove ok
*       -EINVAL Invalid symbol table name; nonexistent table
*       -EFAULT general failure
*
******************************************************************************/
int UserRemoveSymbolTable(void *pSymtab)
{
    int retval;
    char sName[MAX_MODULE_NAME];

    // Copy the symbol table name from the user address space
    if( copy_from_user(sName, pSymtab, MAX_MODULE_NAME)==0 )
    {
        sName[MAX_MODULE_NAME-1] = 0;

        // Search for the symbol table with that specific table name
        if( SymbolTableRemove(sName, NULL)==FALSE )
            retval = -EINVAL;
        retval = -4;
    }
    else
    {
        ERROR(("Invalid IOCTL packet address\n"));
        retval = -EFAULT;
    }

    return(retval);
}

/******************************************************************************
*                                                                             *
*   BOOL SymbolTableRemove(char *pTableName, TSYMTAB *pRemove)                *
*                                                                             *
*******************************************************************************
*
*   Low level function to remove a symbol table from the debugger.
*
*   Where:
*       pTable name is the name string. It can be NULL, in which case...
*       pRemove address of the table to be removed
*
*   Returns:
*       TRUE - table found & removed
*       FALSE - table not found
*
******************************************************************************/
BOOL SymbolTableRemove(char *pTableName, TSYMTAB *pRemove)
{
    TSYMTAB *pSym, *pPrev = NULL;
    BOOL fMatch = FALSE;

    // Traverse the linked list of symbol tables and find which one matched
    pSym = pIce->pSymTab;
    while( pSym )
    {
        if( pTableName!=NULL )
        {
            if( !strcmp(pSym->sTableName, pTableName) )
                fMatch = TRUE;
        }
        else
            if( pRemove==pSym )
                fMatch = TRUE;

        if( fMatch )
        {
            dprinth(1, "Removing symbol table `%s'", pSym->sTableName);

            // Found it - unlink, free and return success
            if( pSym==pIce->pSymTab )
                pIce->pSymTab = (TSYMTAB *) pSym->pPriv->next;     // First in the linked list
            else
                pPrev->pPriv->next = pSym->pPriv->next;       // Not the first in the linked list

            // Add the memory block to the free pool
            pIce->nSymbolBufferAvail += pSym->dwSize;

            // Release the private symbol table data
            ice_free((BYTE *)pSym->pPriv);

            // Release the symbol table itself
            ice_free((BYTE *)pSym);

            // Leave no dangling pointers...
            pIce->pSymTabCur = pIce->pSymTab;

            return(TRUE);
        }

        pPrev = pSym;
        pSym = (TSYMTAB *) pSym->pPriv->next;
    }

    return(FALSE);
}


/******************************************************************************
*                                                                             *
*   BOOL cmdTable(char *args, int subClass)                                   *
*                                                                             *
*******************************************************************************
*
*   Display or set current symbol table. Table name may be specified --
*   partial name would suffice.
*
*   Optional argument 'R' removes a specified table (or all tables if 'R *')
*
******************************************************************************/
BOOL cmdTable(char *args, int subClass)
{
    TSYMTAB *pSymTab = pIce->pSymTab;
//  DWORD offset;
    int nLine = 2;

    if( pSymTab==NULL )
    {
        dprinth(1, "No symbol table loaded.");
    }
    else
    {
        // If we specified a table name, find that table and set it as current
        if( *args )
        {
            // Remove a symbol table?
            if( toupper(*args)=='R' && *(args+1)==' ' )
            {
                // Remove a symbol table whose name is given (or '*' for all)
                args += 2;
                while(*args==' ') args++;

                if( *args=='*' )
                {
                    // Remove all symbol tables

                    dprinth(nLine++, "Removing all symbol tables...");

                    while( pIce->pSymTab )
                    {
                        SymbolTableRemove(NULL, pIce->pSymTab);
                    }
                }
                else
                {
                    // Remove a particular symbol table

                    if( SymbolTableRemove(args, NULL)==FALSE )
                        dprinth(1, "Nonexisting symbol table `%s'", args);

                    return(TRUE);
                }
            }
            else
            {
                // Set current symbol table

                while( pSymTab )
                {
                    // Compare internal table name to our argument string
                    if( !strcmp(pSymTab->sTableName, args) )
                    {
                        pIce->pSymTabCur = pSymTab;

                        return(TRUE);
                    }

                    pSymTab = (TSYMTAB *) pSymTab->pPriv->next;
                }

                dprinth(1, "Nonexisting symbol table `%s'", args);

                return(TRUE);
            }
        }
        else
        {
            // No arguments - List all symbol tables that are loaded

            dprinth(1, "Symbol tables:");
            while( pSymTab )
            {
                // Print a symbol table name. If a table is current, highlight it.
                dprinth(nLine++, " %c%c%6d  %s",
                    DP_SETCOLINDEX, pSymTab==pIce->pSymTabCur? COL_BOLD:COL_NORMAL,
                    pSymTab->dwSize, pSymTab->sTableName);
                pSymTab = (TSYMTAB *) pSymTab->pPriv->next;
            }
        }
    }

    dprinth(nLine, "%d bytes of symbol table memory available", pIce->nSymbolBufferAvail);

    return( TRUE );
}


/******************************************************************************
*                                                                             *
*   TSYMTAB *SymTabFind(char *name)                                           *
*                                                                             *
*******************************************************************************
*
*   Searches for the named symbol table.
*
*   Where:
*       name is the internal symbol table name
*
*   Returns:
*       Pointer to a symbol table
*       NULL if the symbol table of that name is not loaded
*
******************************************************************************/
TSYMTAB *SymTabFind(char *name)
{
    TSYMTAB *pSymTab = pIce->pSymTab;   // Pointer to all symbol tables

    // Make sure name is given as a valid pointer
    if( name!=NULL )
    {
        while( pSymTab )
        {
            if( !strcmp(pSymTab->sTableName, name) )
                return( pSymTab );

            pSymTab = (TSYMTAB *) pSymTab->pPriv->next;
        }
    }

    return(NULL);
}


/******************************************************************************
*                                                                             *
*   void *SymTabFindSection(TSYMTAB *pSymTab, BYTE hType)                     *
*                                                                             *
*******************************************************************************
*
*   Searches for the named section within the given symbol table.
*
*   Where:
*       pSymTab is the address of the symbol table to search
*       hType is the section number
*
*   Returns:
*       Pointer to a section header
*       NULL if the section can not be located
*
******************************************************************************/
void *SymTabFindSection(TSYMTAB *pSymTab, BYTE hType)
{
    TSYMHEADER *pHead;                  // Generic section header

    if( pSymTab )
    {
        pHead = pSymTab->header;

        while( pHead->hType != HTYPE__END )
        {
            if( pHead->hType == hType )
                return( (void *)pHead );

            pHead = (TSYMHEADER*)((DWORD)pHead + pHead->dwSize);
        }
    }

    return(NULL);
}


/******************************************************************************
*                                                                             *
*   TSYMSOURCE *SymTabFindSource(TSYMTAB *pSymTab, WORD fileID)               *
*                                                                             *
*******************************************************************************
*
*   Searches for the fileID descriptor of the given file id number
*
*   Where:
*       pSymTab is the address of the symbol table to search
*       fileID is the file ID to search for
*
*   Returns:
*       Pointer to a source descriptor
*       NULL if the file id descriptor can not be located
*
******************************************************************************/
TSYMSOURCE *SymTabFindSource(TSYMTAB *pSymTab, WORD fileID)
{
    TSYMHEADER *pHead;                  // Generic section header
    TSYMSOURCE *pSource;

    if( pSymTab )
    {
        pHead = pSymTab->header;

        while( pHead->hType != HTYPE__END )
        {
            if( (pHead->hType == HTYPE_SOURCE) )
            {
                pSource = (TSYMSOURCE *)pHead;

                if( pSource->file_id==fileID )
                    return( pSource );
            }

            pHead = (TSYMHEADER*)((DWORD)pHead + pHead->dwSize);
        }
    }

    return( NULL );
}


/******************************************************************************
*                                                                             *
*   void SymTabRelocate(TSYMTAB *pSymTab, int offCode, int offData)           *
*                                                                             *
*******************************************************************************
*
*   Relocates all address references within a symbol table by a set of 
*   code/data offsets.
*
*   Where:
*       pSymTab is the pointer to a symbol table to relocate
*       offCode is the code symbols address offset value
*       offData is the data symbols address offset value
*
******************************************************************************/
void SymTabRelocate(TSYMTAB *pSymTab, int offCode, int offData)
{
    TSYMHEADER *pHead;                  // Generic section header
    DWORD count;

    TSYMGLOBAL  *pGlobals;              // Globals section pointer
    TSYMGLOBAL1 *pGlobal;               // Single global item
    TSYMFNLIN   *pFnLin;                // Function lines section pointer
    TSYMFNSCOPE *pFnScope;              // Function scope section pointer
    TSYMSTATIC  *pStatic;               // Static symbols section pointer
    TSYMSTATIC1 *pStatic1;              // Single static item

    if( pSymTab )
    {
        pHead = pSymTab->header;

        // Loop over the complete symbol table and relocate each appropriate section
        while( pHead->hType != HTYPE__END )
        {
            switch( pHead->hType )
            {
                case HTYPE_GLOBALS:

                    // Data and code are relocated differently

                    pGlobals = (TSYMGLOBAL *) pHead;
                    pGlobal  = &pGlobals->global[0];

                    for(count=0; count<pGlobals->nGlobals; count++, pGlobal++ )
                    {
                        if( (pGlobal->bFlags & 1)==0 )
                        {
                            // Relocating a code entry
                            pGlobal->dwStartAddress += offCode;
                            pGlobal->dwEndAddress   += offCode;
                        }
                        else
                        {
                            // Relocating a data entry
                            pGlobal->dwStartAddress += offData;
                            pGlobal->dwEndAddress   += offData;
                        }
                    }
                    break;

                case HTYPE_SOURCE:
                    // This section does not need relocation
                    break;

                case HTYPE_FUNCTION_LINES:

                    pFnLin = (TSYMFNLIN *) pHead;
                    pFnLin->dwStartAddress += offCode;
                    pFnLin->dwEndAddress   += offCode;
                    break;

                case HTYPE_FUNCTION_SCOPE:

                    pFnScope = (TSYMFNSCOPE *) pHead;
                    pFnScope->dwStartAddress += offCode;
                    pFnScope->dwEndAddress   += offCode;
                    break;

                case HTYPE_STATIC:

                    pStatic  = (TSYMSTATIC *) pHead;
                    pStatic1 = &pStatic->list[0];

                    for(count=0; count<pStatic->nStatics; count++, pStatic1++ )
                    {
                        pStatic1->dwAddress += offData;
                    }
                    break;

                case HTYPE_TYPEDEF:
                    // This section does not need relocation
                    break;

                case HTYPE_IGNORE:
                    // This section does not need relocation
                    break;

                case HTYPE_SYMBOL_HASH:
                    // This section does not need relocation
                    break;

                default:
                    // We could catch a corrupted symbols error here if we want to...
                    break;
            }

            // Next section
            pHead = (TSYMHEADER*)((DWORD)pHead + pHead->dwSize);
        }
    }
}
