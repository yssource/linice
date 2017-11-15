/******************************************************************************
*                                                                             *
*   Module:     output.c                                                      *
*                                                                             *
*   Date:       09/11/00                                                      *
*                                                                             *
*   Copyright (c) 1997, 2001 Goran Devic                                      *
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

    This module contains the printing code that is independent on the
    type of output. All printing goes through this module.

    If the first character of a string is '@', a line will be stored in the
    history buffer as well.

    The cache output buffer is also here and some utility functions to deal
    with it. Cache output buffer is a simple ASCII text buffer to which
    shadowed print is made. It is used with high-resolution scrolling to
    read characters to scroll and serial line not to send redundant codes.
    (TODO)

*******************************************************************************
*                                                                             *
*   Changes:                                                                  *
*                                                                             *
*   DATE     REV   DESCRIPTION OF CHANGES                         AUTHOR      *
* --------   ----  ---------------------------------------------  ----------- *
* 09/11/00         Original                                       Goran Devic *
* --------   ----  ---------------------------------------------  ----------- *
*******************************************************************************
*   Include Files                                                             *
******************************************************************************/

#include "module-header.h"              // Versatile module header file

#include "clib.h"                       // Include C library header file
#include "ice.h"                        // Include main debugger structures

/******************************************************************************
*                                                                             *
*   Global Variables                                                          *
*                                                                             *
******************************************************************************/

BYTE cacheText[MAX_Y][MAX_X];           // Cache output buffer

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

void CacheTextScrollUp(DWORD top, DWORD bottom)
{
    // Scroll up all requested lines
    memmove(&cacheText[top][0], &cacheText[top+1][0], MAX_X * (bottom-top));

    // Clear the last line
    memset(&cacheText[bottom][0], 0, MAX_X);
}

void CacheTextCls()
{
    // Clear complete cache to spaces
    memset(cacheText, ' ', MAX_X * MAX_Y);
}

/******************************************************************************
*                                                                             *
*   void dputc(UCHAR c)                                                       *
*                                                                             *
*******************************************************************************
*
*   A simple counterpart of the dprint().  This one sends a single character
*   to an output device.
*
******************************************************************************/
void dputc(UCHAR c)
{
    // This works fine with little-endian...
    WORD printBuf = (WORD) c;

    pOut->sprint((char *) &printBuf);
}


/******************************************************************************
*                                                                             *
*   int PrintLine(char *format,...)                                           *
*                                                                             *
*******************************************************************************
*
*   Prints a loosely formatted header line.
*
*   Where:
*       format is the standard printf() format string
*       ... standard printf() list of arguments.
*
*   Returns:
*       number of characters actually printed.
*
******************************************************************************/
int PrintLine(char *format,...)
{
    char printBuf[256];
    char *pBuf = printBuf, *p;
    int written;
    va_list arg;

    // Print the line into a string
    va_start( arg, format );
    written = vsprintf(pBuf, format, arg);
    va_end(arg);

    // Append enough spaces to fill in a current line width
    memset(pBuf+written, ' ', pOut->sizeX - written);
    pBuf[pOut->sizeX] = '\n';
    pBuf[pOut->sizeX+1]   = 0;

    // Change spaces into the graphics line
    p = pBuf;
    while( *p )
    {
        if( *p==' ' )
            *p = 0xC4;                  // Horizontal line extended code
        p++;
    }

    // Set the color that is assigned for a header line
    dprint("%c%c", DP_SETCOLINDEX, COL_LINE);

    // Send the string to a current output device driver
    pOut->sprint(pBuf);

    return( pOut->sizeX );
}


/******************************************************************************
*                                                                             *
*   int dprint( char *format, ... )                                           *
*                                                                             *
*******************************************************************************
*
*   This is the main print function for debugger output.
*
*   Where:
*       format is the standard printf() format string
*       ... standard printf() list of arguments.
*
*   Returns:
*       number of characters actually printed.
*
******************************************************************************/
int dprint( char *format, ... )
{
    char printBuf[256];
    char *pBuf = printBuf;
    int written;
    va_list arg;

    // Print the line into a string
    va_start( arg, format );
    written = vsprintf(pBuf, format, arg);
    va_end(arg);

    // Send the string to a current output device driver
    pOut->sprint(pBuf);

    return( written );
}


/******************************************************************************
*                                                                             *
*   BOOL dprinth( int nLineCount, char *format, ... )                         *
*                                                                             *
*******************************************************************************
*
*   This print function should be used by all commands. If a command prints
*   also in a window, line
*
*   Where:
*       nLineCount is the line count for printing in the history buffer
*           Start with 1 and increment for every line.
*       format is the standard printf() format string
*       ... standard printf() list of arguments.
*
*   Returns:
*       TRUE if continue printing
*       FALSE if abort further printing
*
******************************************************************************/
BOOL dprinth( int nLineCount, char *format, ... )
{
    char printBuf[256];
    CHAR Key;
    char *pBuf = printBuf;
    int written;
    va_list arg;

    // Print the line into a string
    va_start( arg, format );
    written = vsprintf(pBuf, format, arg);
    va_end(arg);

    // If we are printing in the history buffer, store it there as well
    if( pOut->y > pWin->h.Top )
    {
        HistoryAdd(pBuf);
        pOut->sprint(pBuf);

        // If we are printing to a history buffer, and the line count is reached,
        // print the help line and wait for a keypress
        if( nLineCount % ((pWin->h.nLines-1) )==0 )
        {
            dprint("%c%c%c%c%c%c    Press any key to continue; Esc to cancel\r%c",
                DP_SAVEXY,
                DP_SETCURSORXY, 1+0, 1+pOut->sizeY-1,
                DP_SETCOLINDEX, COL_HELP,
                DP_RESTOREXY);
            Key = GetKey(TRUE);

            // Move cursor to a new line
            dprint("\n");

            if( Key==ESC )
                return( FALSE );
        }
        else
            dprint("\n");
    }
    else
    {
        // We are printing within a window
        // Append new line
        strcat(pBuf, "\n");
        pOut->sprint(pBuf);
    }

    return( TRUE );
}