/** @file
 * Generic GDB stub library
 */

/*
 * Copyright (C) 2020 Alexander Eichner <alexander.eichner@campus.tu-berlin.de>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdarg.h>

#include "libgdbstub.h"


/** Character indicating the start of a packet. */
#define GDBSTUB_PKT_START       '$'
/** Character indicating the end of a packet (excluding the checksum). */
#define GDBSTUB_PKT_END         '#'
/** The escape character. */
#define GDBSTUB_PKT_ESCAPE      '{'
/** The out-of-band interrupt character. */
#define GDBSTUB_OOB_INTERRUPT   0x03

/** Returns the number of elements from a static array. */
#define ELEMENTS(a_Array) (sizeof(a_Array)/sizeof(a_Array[0]))
/** Returns the minimum of two given values. */
#define MIN(a_Val1, a_Val2) ((a_Val1) < (a_Val2) ? (a_Val1) : (a_Val2))
/** Sets the specified bit. */
#define BIT(a_Bit) (1 << (a_Bit))
/** Return the absolute value of a given number .*/
#define ABS(a) ((a) < 0 ? -(a) : (a))

/** Our own bool type. */
typedef uint8_t bool;
/** true value. */
#define true 1
/** false value. */
#define false 0


/**
 * GDB stub receive state.
 */
typedef enum GDBSTUBRECVSTATE
{
    /** Invalid state. */
    GDBSTUBRECVSTATE_INVALID = 0,
    /** Waiting for the start character. */
    GDBSTUBRECVSTATE_PACKET_WAIT_FOR_START,
    /** Reiceiving the packet body up until the END character. */
    GDBSTUBRECVSTATE_PACKET_RECEIVE_BODY,
    /** Receiving the checksum. */
    GDBSTUBRECVSTATE_PACKET_RECEIVE_CHECKSUM,
    /** Blow up the enum to 32bits for easier alignment of members in structs. */
    GDBSTUBRECVSTATE_32BIT_HACK = 0x7fffffff
} GDBSTUBRECVSTATE;


/**
 * Command output context.
 */
typedef struct GDBSTUBOUTCTX
{
    /** The helper structure, MUST come first!. */
    GDBSTUBCMDOUTHLP            Hlp;
    /** Current offset into the scratch buffer. */
    uint32_t                    offScratch;
    /** Scratch buffer. */
    uint8_t                     abScratch[512];
} GDBSTUBOUTCTX;
/** Pointer to a command output context. */
typedef GDBSTUBOUTCTX *PGDBSTUBOUTCTX;
/** Pointer to a const command output context. */
typedef const GDBSTUBOUTCTX *PCGDBSTUBOUTCTX;


/**
 * Internal PSP proxy context.
 */
typedef struct GDBSTUBCTXINT
{
    /** The I/O interface callback table. */
    PCGDBSTUBIOIF               pIoIf;
    /** The interface callback table. */
    PCGDBSTUBIF                 pIf;
    /** Opaque user data passed in the callbacks. */
    void                        *pvUser;
    /** The current state when receiving a new packet. */
    GDBSTUBRECVSTATE            enmState;
    /** Maximum number of bytes the packet buffer can hold. */
    size_t                      cbPktBufMax;
    /** Current offset into the packet buffer. */
    uint32_t                    offPktBuf;
    /** The size of the packet (minus the start, end characters and the checksum). */
    uint32_t                    cbPkt;
    /** Pointer to the packet buffer data. */
    uint8_t                     *pbPktBuf;
    /** Number of bytes left for the checksum. */
    size_t                      cbChksumRecvLeft;
    /** Last target state seen. */
    GDBSTUBTGTSTATE             enmTgtStateLast;
    /** Number of registers this architecture has. */
    uint32_t                    cRegs;
    /** Overall size to return all registers. */
    size_t                      cbRegs;
    /** Register scratch space (for reading writing registers). */
    void                        *pvRegsScratch;
    /** Register index array for querying setting. */
    uint32_t                    *paidxRegs;
    /** Feature flags supported we negotiated with the remote end. */
    uint32_t                    fFeatures;
    /** Pointer to the XML target description. */
    uint8_t                     *pbTgtXmlDesc;
    /** Size of the XML target description. */
    size_t                      cbTgtXmlDesc;
    /** Flag whether the stub is in extended mode. */
    bool                        fExtendedMode;
    /** Output context. */
    GDBSTUBOUTCTX               OutCtx;
} GDBSTUBCTXINT;
/** Pointer to an internal PSP proxy context. */
typedef GDBSTUBCTXINT *PGDBSTUBCTXINT;

/** Indicate support for the 'qXfer:features:read' packet to support the target description. */
#define GDBSTUBCTX_FEATURES_F_TGT_DESC      BIT(0)

/**
 * Specific query packet processor callback.
 *
 * @returns Status code.
 * @param   pThis               The GDB stub context.
 * @param   pbArgs              Pointer to the arguments.
 * @param   cbArgs              Size of the arguments in bytes.
 */
typedef int (FNGDBSTUBQPKTPROC) (PGDBSTUBCTXINT pThis, const uint8_t *pbArgs, size_t cbArgs);
typedef FNGDBSTUBQPKTPROC *PFNGDBSTUBQPKTPROC;


/**
 * 'q' packet processor.
 */
typedef struct GDBSTUBQPKTPROC
{
    /** Name */
    const char                  *pszName;
    /** Length of name in characters (without \0 terminator). */
    uint32_t                    cchName;
    /** The callback to call for processing the particular query. */
    PFNGDBSTUBQPKTPROC          pfnProc;
} GDBSTUBQPKTPROC;
/** Pointer to a 'q' packet processor entry. */
typedef GDBSTUBQPKTPROC *PGDBSTUBQPKTPROC;
/** Pointer to a const 'q' packet processor entry. */
typedef const GDBSTUBQPKTPROC *PCGDBSTUBQPKTPROC;


/**
 * 'v' packet processor.
 */
typedef struct GDBSTUBVPKTPROC
{
    /** Name */
    const char                  *pszName;
    /** Length of name in characters (without \0 terminator). */
    uint32_t                    cchName;
    /** Replay to a query packet (ends with ?). */
    const char                  *pszReplyQ;
    /** Length of the query reply (without \0 terminator). */
    uint32_t                    cchReplyQ;
    /** The callback to call for processing the particular query. */
    PFNGDBSTUBQPKTPROC          pfnProc;
} GDBSTUBVPKTPROC;
/** Pointer to a 'q' packet processor entry. */
typedef GDBSTUBVPKTPROC *PGDBSTUBVPKTPROC;
/** Pointer to a const 'q' packet processor entry. */
typedef const GDBSTUBVPKTPROC *PCGDBSTUBVPKTPROC;


/**
 * Feature callback.
 *
 * @returns Status code.
 * @param   pThis               The GDB stub context.
 * @param   pbVal               Pointer to the value.
 * @param   cbVal               Size of the value in bytes.  
 */
typedef int (FNGDBSTUBFEATHND) (PGDBSTUBCTXINT pThis, const uint8_t *pbVal, size_t cbVal);
typedef FNGDBSTUBFEATHND *PFNGDBSTUBFEATHND;


/**
 * GDB feature descriptor.
 */
typedef struct GDBSTUBFEATDESC
{
    /** Feature name */
    const char                  *pszName;
    /** Length of the feature name in characters (without \0 terminator). */
    uint32_t                    cchName;
    /** The callback to call for processing the particular feature. */
    PFNGDBSTUBFEATHND           pfnHandler;
    /** Flag whether the feature requires a value. */
    bool                        fVal;
} GDBSTUBFEATDESC;
/** Pointer to a GDB feature descriptor. */
typedef GDBSTUBFEATDESC *PGDBSTUBFEATDESC;
/** Pointer to a const GDB feature descriptor. */
typedef const GDBSTUBFEATDESC *PCGDBSTUBFEATDESC;


/**
 * GDB architecture names.
 */
static const char *s_aGdbArchMapping[] =
{
    NULL,   /* GDBSTUBTGTARCH_INVALID */
    "arm",  /* GDBSTUBTGTARCH_ARM     */
    "i386", /* GDBSTUBTGTARCH_X86     */
    "i386", /* GDBSTUBTGTARCH_AMD64   */
};


/**
 * Core feature mapping names for each architecture.
 */
static const char *s_aGdbArchFeatMapping[] =
{
    NULL,                    /* GDBSTUBTGTARCH_INVALID */
    "org.gnu.gdb.arm.core",  /* GDBSTUBTGTARCH_ARM     */
    "org.gnu.gdb.i386.core", /* GDBSTUBTGTARCH_X86     */
    "org.gnu.gdb.arm.core",  /* GDBSTUBTGTARCH_AMD64   */
};


/**
 * Wrapper for allocating memory using the callback interface.
 *
 * @returns Pointer to the allocated memory on success or NULL if out of memory.
 * @param   pThis               The GDB stub context.
 * @param   cbAlloc             Number of bytes to allocate.
 */
static inline void *gdbStubCtxIfMemAlloc(PGDBSTUBCTXINT pThis, size_t cbAlloc)
{
    return pThis->pIf->pfnMemAlloc(pThis, pThis->pvUser, cbAlloc);
}


/**
 * Wrapper for freeing memory using the callback interface.
 *
 * @returns nothing.
 * @param   pThis               The GDB stub context.
 * @param   pv                  The pointer to free.
 */
static inline void gdbStubCtxIfMemFree(PGDBSTUBCTXINT pThis, void *pv)
{
    return pThis->pIf->pfnMemFree(pThis, pThis->pvUser, pv);
}


/**
 * Wrapper for the interface target get state callback.
 *
 * @returns Target state.
 * @param   pThis               The GDB stub context.
 */
static inline GDBSTUBTGTSTATE gdbStubCtxIfTgtGetState(PGDBSTUBCTXINT pThis)
{
    return pThis->pIf->pfnTgtGetState(pThis, pThis->pvUser);
}


/**
 * Wrapper for the interface target stop callback.
 *
 * @returns Status code.
 * @param   pThis               The GDB stub context.
 */
static inline int gdbStubCtxIfTgtStop(PGDBSTUBCTXINT pThis)
{
    return pThis->pIf->pfnTgtStop(pThis, pThis->pvUser);
}


/**
 * Wrapper for the interface target restart callback.
 *
 * @returns Status code.
 * @param   pThis               The GDB stub context.
 */
static inline int gdbStubCtxIfTgtRestart(PGDBSTUBCTXINT pThis)
{
    return pThis->pIf->pfnTgtRestart(pThis, pThis->pvUser);
}


/**
 * Wrapper for the interface target kill callback.
 *
 * @returns Status code.
 * @param   pThis               The GDB stub context.
 */
static inline int gdbStubCtxIfTgtKill(PGDBSTUBCTXINT pThis)
{
    return pThis->pIf->pfnTgtKill(pThis, pThis->pvUser);
}


/**
 * Wrapper for the interface target step callback.
 *
 * @returns Status code.
 * @param   pThis               The GDB stub context.
 */
static inline int gdbStubCtxIfTgtStep(PGDBSTUBCTXINT pThis)
{
    return pThis->pIf->pfnTgtStep(pThis, pThis->pvUser);
}


/**
 * Wrapper for the interface target continue callback.
 *
 * @returns Status code.
 * @param   pThis               The GDB stub context.
 */
static inline int gdbStubCtxIfTgtContinue(PGDBSTUBCTXINT pThis)
{
    return pThis->pIf->pfnTgtCont(pThis, pThis->pvUser);
}


/**
 * Wrapper for the interface target read memory callback.
 *
 * @returns Status code.
 * @param   pThis               The GDB stub context.
 * @param   GdbTgtMemAddr       The target memory address to read.
 * @param   pvDst               Where to store the read data.
 * @param   cbRead              Number of bytes to read.
 */
static inline int gdbStubCtxIfTgtMemRead(PGDBSTUBCTXINT pThis, GDBTGTMEMADDR GdbTgtMemAddr, void *pvDst, size_t cbRead)
{
    return pThis->pIf->pfnTgtMemRead(pThis, pThis->pvUser, GdbTgtMemAddr, pvDst, cbRead);
}


/**
 * Wrapper for the interface target write memory callback.
 *
 * @returns Status code.
 * @param   pThis               The GDB stub context.
 * @param   GdbTgtMemAddr       The target memory address to write to.
 * @param   pvSrc               The data to write.
 * @param   cbWrite             Number of bytes to write.
 */
static inline int gdbStubCtxIfTgtMemWrite(PGDBSTUBCTXINT pThis, GDBTGTMEMADDR GdbTgtMemAddr, void *pvSrc, size_t cbWrite)
{
    return pThis->pIf->pfnTgtMemWrite(pThis, pThis->pvUser, GdbTgtMemAddr, pvSrc, cbWrite);
}


/**
 * Wrapper for the interface target read registers callback.
 *
 * @returns Status code.
 * @param   pThis               The GDB stub context.
 * @param   paRegs              Register indizes to read.
 * @param   cRegs               Number of registers to read.
 * @param   pvDst               Where to store the register content (caller makes sure there is enough space
 *                              to store cRegs * GDBSTUBIF::cbReg bytes of data).
 */
static inline int gdbStubCtxIfTgtRegsRead(PGDBSTUBCTXINT pThis, uint32_t *paRegs, uint32_t cRegs, void *pvDst)
{
    return pThis->pIf->pfnTgtRegsRead(pThis, pThis->pvUser, paRegs, cRegs, pvDst);
}


/**
 * Wrapper for the interface target write registers callback.
 *
 * @returns Status code.
 * @param   pThis               The GDB stub context.
 * @param   paRegs              Register indizes to write.
 * @param   cRegs               Number of registers to write.
 * @param   pvSrc               The register content to write (caller makes sure there is enough space
 *                              to store cRegs * GDBSTUBIF::cbReg bytes of data).
 */
static inline int gdbStubCtxIfTgtRegsWrite(PGDBSTUBCTXINT pThis, uint32_t *paRegs, uint32_t cRegs, void *pvSrc)
{
    return pThis->pIf->pfnTgtRegsWrite(pThis, pThis->pvUser, paRegs, cRegs, pvSrc);
}


/**
 * Wrapper for the interface target tracepoint set callback.
 *
 * @returns Status code.
 * @param   pThis               The GDB stub context.
 * @param   GdbTgtTpAddr        The target address space memory address to set the trace point at.
 * @param   enmTpType           The tracepoint type (working on instructions or memory accesses).
 * @param   enmTpAction         The action to execute if the tracepoint is hit.
 */
static inline int gdbStubCtxIfTgtTpSet(PGDBSTUBCTXINT pThis, GDBTGTMEMADDR GdbTgtTpAddr, GDBSTUBTPTYPE enmTpType, GDBSTUBTPACTION enmTpAction)
{
    if (pThis->pIf->pfnTgtTpSet)
        return pThis->pIf->pfnTgtTpSet(pThis, pThis->pvUser, GdbTgtTpAddr, enmTpType, enmTpAction);

    return GDBSTUB_ERR_NOT_SUPPORTED;
}


/**
 * Wrapper for the interface target tracepoint clear callback.
 *
 * @returns Status code.
 * @param   pThis               The GDB stub context.
 * @param   GdbTgtTpAddr        The target address space memory address to remove the trace point from.
 */
static inline int gdbStubCtxIfTgtTpClear(PGDBSTUBCTXINT pThis, GDBTGTMEMADDR GdbTgtTpAddr)
{
    if (pThis->pIf->pfnTgtTpClear)
        return pThis->pIf->pfnTgtTpClear(pThis, pThis->pvUser, GdbTgtTpAddr);

    return GDBSTUB_ERR_NOT_SUPPORTED;
}


/**
 * Wrapper for the I/O interface peek callback.
 *
 * @returns Number of bytes available for reading.
 * @param   pThis               The GDB stub context.
 */
static inline size_t gdbStubCtxIoIfPeek(PGDBSTUBCTXINT pThis)
{
    return pThis->pIoIf->pfnPeek(pThis, pThis->pvUser);
}


/**
 * Wrapper for the I/O interface read callback.
 *
 * @returns Status code.
 * @param   pThis               The GDB stub context.
 * @param   pvDst               Where to store the read data.
 * @param   cbRead              How much to read.
 * @param   pcbRead             Where to store the number of bytes read on success.
 */
static inline int gdbStubCtxIoIfRead(PGDBSTUBCTXINT pThis, void *pvDst, size_t cbRead, size_t *pcbRead)
{
    return pThis->pIoIf->pfnRead(pThis, pThis->pvUser, pvDst, cbRead, pcbRead);
}


/**
 * Wrapper for the I/O interface write callback.
 *
 * @returns Status code.
 * @param   pThis               The GDB stub context.
 * @param   pvPkt               The packet data to send.
 * @param   cbPkt               Size of the packet in bytes.
 */
static inline int gdbStubCtxIoIfWrite(PGDBSTUBCTXINT pThis, const void *pvPkt, size_t cbPkt)
{
    return pThis->pIoIf->pfnWrite(pThis, pThis->pvUser, pvPkt, cbPkt);
}


/**
 * Wrapper for the I/O interface poll callback.
 *
 * @returns Status code.
 * @param   pThis               The GDB stub context.
 */
static inline size_t gdbStubCtxIoIfPoll(PGDBSTUBCTXINT pThis)
{
    return pThis->pIoIf->pfnPoll(pThis, pThis->pvUser);
}


/**
 * Internal memcpy.
 *
 * @returns Pointer to the destination buffer.
 * @param   pvDst               Destination to copy the buffer to.
 * @param   pvSrc               Where to copy the data from.
 * @param   cb                  Amount of bytes to copy.
 *
 * @todo Allow using optimized variants of memcpy.
 */
static void *gdbStubCtxMemcpy(void *pvDst, const void *pvSrc, size_t cb)
{
    uint8_t *pbDst = (uint8_t *)pvDst;
    const uint8_t *pbSrc = (const uint8_t *)pvSrc;

    while (cb--)
        *pbDst++ = *pbSrc++;

    return pvDst;
}


/**
 * Internal memset.
 *
 * @returns Pointer to the destination buffer.
 * @param   pvDst               Destination to copy the buffer to.
 * @param   bVal                Byte value to set the buffer to.
 * @param   cb                  Amount of bytes to set.
 *
 * @todo Allow using optimized variants of memset.
 */
static void *gdbStubCtxMemset(void *pvDst, uint8_t bVal, size_t cb)
{
    uint8_t *pbDst = (uint8_t *)pvDst;

    while (cb--)
        *pbDst++ = bVal;

    return pvDst;
}


/**
 * Converts a given to the hexadecimal value if valid.
 *
 * @returns The hexadecimal value the given character represents 0-9,a-f,A-F or 0xff on error.
 * @param   ch                  The character to convert.
 */
static inline uint8_t gdbStubCtxChrToHex(char ch)
{
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >= 'A' && ch <= 'F')
        return ch - 'A' + 0xa;
    if (ch >= 'a' && ch <= 'f')
        return ch - 'a' + 0xa;

    return 0xff;
}


/**
 * Converts a 4bit hex number to the appropriate character.
 *
 * @returns Character representing the 4bit hex number.
 * @param   uHex                The 4 bit hex number.
 */
static inline char gdbStubCtxHexToChr(uint8_t uHex)
{
    if (uHex < 0xa)
        return '0' + uHex;
    if (uHex <= 0xf)
        return 'A' + uHex - 0xa;

    return 'X';
}


/**
 * Internal memchr.
 *
 * @returns Pointer to the first byte containing the given character or NULL if not found.
 * @param   pv                  The data buffer to search in.
 * @param   ch                  The character/byte to search for.
 * @param   cb                  Maximum number of bytes to search.
 *
 * @todo Allow using optimized variants of memchr.
 */
static void *gdbStubCtxMemchr(const void *pv, uint8_t ch, size_t cb)
{
    uint8_t *pb = (uint8_t *)pv;

    while (cb--)
    {
        if (*pb == ch)
            return pb;
        pb++;
    }

    return NULL;
}


/**
 * Internal memmove.
 *
 * @returns Pointer to the original destination buffer pointer.
 * @param   pvDst               The pointer of the memory region the data should be moved to.
 * @param   pvSrc               The data to move.
 * @param   cb                  Number of bytes to move.
 *
 * @todo Allow using optimized variants of memmove.
 */
static void *gdbStubCtxMemmove(void *pvDst, const void *pvSrc, size_t cb)
{
    uint8_t *pbDst = (uint8_t *)pvDst;
    const uint8_t *pbSrc = (const uint8_t *)pvSrc;

    while (cb--)
        *pbDst++ = *pbSrc++;

    return pvDst;
}


/**
 * Compares two memory buffers.
 *
 * @returns Status of the comparison.
 * @retval 0 if both memory buffers are identical.
 * @retval difference between the first two differing bytes.
 * @param   pvBuf1                  First buffer.
 * @param   pvBuf2                  Second buffer.
 * @param   cb                      How many bytes to compare.
 *
 * @todo Allow using optimized variants of memcmp.
 */
static int gdbStubMemcmp(const void *pvBuf1, const void *pvBuf2, size_t cb)
{
    const uint8_t *pbBuf1 = (const uint8_t *)pvBuf1;
    const uint8_t *pbBuf2 = (const uint8_t *)pvBuf2;

    while (cb)
    {
        if (*pbBuf1 != *pbBuf2)
            return *pbBuf1 - *pbBuf2;

        pbBuf1++;
        pbBuf2++;
        cb--;
    }

    return 0;
}


/**
 * Calculates the length of the given string excluding the zero terminator.
 *
 * @returns Number of characters in the given string.
 * @param   psz                     Pointer to the string.
 *
 * @todo Allow using optimized variants of strlen.
 */
static size_t gdbStubStrlen(const char *psz)
{
    size_t cchStr = 0;

    while (*psz != '\0')
    {
        cchStr++;
        psz++;
    }

    return cchStr;
}


/**
 * Compares two strings.
 *
 * @returns 0 if both strings are equal,
 *          < 0 if psz1 is less than psz2,
 *          > 0 if psz1 is less than psz2,
 * @param   psz1                    Pointer to the first string.
 * @param   psz2                    Pointer to the first string.
 *
 * @todo Allow using optimized variants of strcmp.
 */
static int gdbStubStrcmp(const char *psz1, const char *psz2)
{
    for (;;)
    {
        if (*psz1 < *psz2)
            return -1;
        if (*psz1 > *psz2)
            return 1;

        if (*psz1 == '\0' && *psz2 == '\0')
            break;

        psz1++;
        psz2++;
    }

    return 0;
}


/**
 * Appends a single character to the given outptu context.
 *
 * @returns nothing.
 * @param   pThis      The output context instance.
 * @param   ch         The character to append.
 */
static void gdbStubOutCtxAppendChar(PGDBSTUBOUTCTX pThis, const char ch)
{
    if (pThis->offScratch < sizeof(pThis->abScratch))
    {
        pThis->abScratch[pThis->offScratch] = ch;
        pThis->offScratch++;
    }
}


/**
 * Converts a given unsigned 32bit integer into a string and appends it to the scratch buffer.
 *
 * @returns nothing.
 * @param   pThis      The output context instance.
 * @param   u32        The value to log.
 * @param   cDigits    Minimum number of digits to log, if the number has fewer
 *                     the gap is prepended with 0.
 */
static void gdbStubOutCtxAppendU32(PGDBSTUBOUTCTX pThis, uint32_t u32, uint32_t cDigits)
{
    char achDigits[] = "0123456789";
    char aszBuf[32];
    unsigned offBuf = 0;

    /** @todo: Optimize. */

    while (u32)
    {
        uint8_t u8Val = u32 % 10;
        u32 /= 10;

        aszBuf[offBuf++] = achDigits[u8Val];
    }

    /* Prepend 0. */
    if (offBuf < cDigits)
    {
        while (cDigits - offBuf > 0)
        {
            gdbStubOutCtxAppendChar(pThis, '0');
            cDigits--;
        }
    }

    while (offBuf-- > 0)
        gdbStubOutCtxAppendChar(pThis, aszBuf[offBuf]);
}


/**
 * Converts a given unsigned 64bit integer into a string and appends it to the scratch buffer.
 *
 * @returns nothing.
 * @param   pThis      The output context instance.
 * @param   u64        The value to log.
 * @param   cDigits    Minimum number of digits to log, if the number has fewer
 *                     the gap is prepended with 0.
 */
static void gdbStubOutCtxAppendU64(PGDBSTUBOUTCTX pThis, uint64_t u64, uint32_t cDigits)
{
    char achDigits[] = "0123456789";
    char aszBuf[32];
    unsigned offBuf = 0;

    /** @todo: Optimize. */

    while (u64)
    {
        uint8_t u8Val = u64 % 10;
        u64 /= 10;

        aszBuf[offBuf++] = achDigits[u8Val];
    }

    /* Prepend 0. */
    if (offBuf < cDigits)
    {
        while (cDigits - offBuf > 0)
        {
            gdbStubOutCtxAppendChar(pThis, '0');
            cDigits--;
        }
    }

    while (offBuf-- > 0)
        gdbStubOutCtxAppendChar(pThis, aszBuf[offBuf]);
}


/**
 * Converts a given unsigned 32bit integer into a string as hex and appends it to the scratch buffer.
 *
 * @returns nothing.
 * @param   pThis      The output context instance.
 * @param   u32        The value to log.
 * @param   cDigits    Minimum number of digits to log, if the number has fewer
 *                     the gap is prepended with 0.
 */
static void gdbStubOutCtxAppendHexU32(PGDBSTUBOUTCTX pThis, uint32_t u32, uint32_t cDigits)
{
    char achDigits[] = "0123456789abcdef";
    char aszBuf[10];
    unsigned offBuf = 0;

    /** @todo: Optimize. */

    while (u32)
    {
        uint8_t u8Val = u32 & 0xf;
        u32 >>= 4;

        aszBuf[offBuf++] = achDigits[u8Val];
    }

    /* Prepend 0. */
    if (offBuf < cDigits)
    {
        while (cDigits - offBuf > 0)
        {
            gdbStubOutCtxAppendChar(pThis, '0');
            cDigits--;
        }
    }

    while (offBuf-- > 0)
        gdbStubOutCtxAppendChar(pThis, aszBuf[offBuf]);
}

/**
 * Converts a given unsigned 64bit integer into a string as hex and appends it to the scratch buffer.
 *
 * @returns nothing.
 * @param   pThis      The output context instance.
 * @param   u64        The value to log.
 * @param   cDigits    Minimum number of digits to log, if the number has fewer
 *                     the gap is prepended with 0.
 */
static void gdbStubOutCtxAppendHexU64(PGDBSTUBOUTCTX pThis, uint64_t u64, uint32_t cDigits)
{
    char achDigits[] = "0123456789abcdef";
    char aszBuf[20];
    unsigned offBuf = 0;

    /** @todo: Optimize. */

    while (u64)
    {
        uint8_t u8Val = u64 & 0xf;
        u64 >>= 4;

        aszBuf[offBuf++] = achDigits[u8Val];
    }

    /* Prepend 0. */
    if (offBuf < cDigits)
    {
        while (cDigits - offBuf > 0)
        {
            gdbStubOutCtxAppendChar(pThis, '0');
            cDigits--;
        }
    }

    while (offBuf-- > 0)
        gdbStubOutCtxAppendChar(pThis, aszBuf[offBuf]);
}


/**
 * Converts a given signed 32bit integer into a string and appends it to the scratch buffer.
 *
 * @returns nothing.
 * @param   pThis      The output context instance.
 * @param   i32        The value to log.
 * @param   cDigits    Minimum number of digits to log, if the number has fewer
 *                     the gap is prepended with 0.
 */
static void gdbStubOutCtxAppendS32(PGDBSTUBOUTCTX pThis, int32_t i32, uint32_t cDigits)
{
    /* Add sign? */
    if (i32 < 0)
    {
        gdbStubOutCtxAppendChar(pThis, '-');
        i32 = ABS(i32);
    }

    /* Treat as unsigned from here on. */
    gdbStubOutCtxAppendU32(pThis, (uint32_t)i32, cDigits);
}


/**
 * Appends a given string to the logger instance.
 *
 * @returns nothing.
 * @param   pThis      The output context instance.
 * @param   psz        The string to append.
 */
static void gdbStubOutCtxAppendString(PGDBSTUBOUTCTX pThis, const char *psz)
{
    /** @todo: Optimize */
    if (!psz)
    {
        gdbStubOutCtxAppendString(pThis, "<null>");
        return;
    }

    while (*psz)
        gdbStubOutCtxAppendChar(pThis, *psz++);
}


/**
 * @copydoc{GDBSTUBOUTHLP,pfnPrintf}
 */
static int gdbStubOutCtxPrintf(PCGDBSTUBOUTHLP pHlp, const char *pszFmt, ...)
{
    int rc = GDBSTUB_INF_SUCCESS;
    PGDBSTUBOUTCTX pThis = (PGDBSTUBOUTCTX)pHlp;
    va_list hArgs;
    va_start(hArgs, pszFmt);

    while (*pszFmt)
    {
        char ch = *pszFmt++;

        switch (ch)
        {
            case '%':
            {
                /* Format specifier. */
                char chFmt = *pszFmt;
                pszFmt++;

                if (chFmt == '#')
                {
                    gdbStubOutCtxAppendString(pThis, "0x");
                    chFmt = *pszFmt++;
                }

                switch (chFmt)
                {
                    case '%':
                    {
                        gdbStubOutCtxAppendChar(pThis, '%');
                        break;
                    }
                    case 'u':
                    {
                        uint32_t u32 = va_arg(hArgs, uint32_t);
                        gdbStubOutCtxAppendU32(pThis, u32, 1);
                        break;
                    }
                    case 'd':
                    {
                        int32_t i32 = va_arg(hArgs, int32_t);
                        gdbStubOutCtxAppendS32(pThis, i32, 1);
                        break;
                    }
                    case 's':
                    {
                        const char *psz = va_arg(hArgs, const char *);
                        gdbStubOutCtxAppendString(pThis, psz);
                        break;
                    }
                    case 'x':
                    {
                        uint32_t u32 = va_arg(hArgs, uint32_t);
                        gdbStubOutCtxAppendHexU32(pThis, u32, 1);
                        break;
                    }
                    case 'X':
                    {
                        uint64_t u64 = va_arg(hArgs, uint64_t);
                        gdbStubOutCtxAppendHexU64(pThis, u64, 1);
                        break;
                    }
                    case 'p': /** @todo: Works only on 32bit... */
                    {
                        void *pv = va_arg(hArgs, void *);
                        gdbStubOutCtxAppendString(pThis, "0x");
                        if (sizeof(void *) == 4)
                            gdbStubOutCtxAppendHexU32(pThis, (uint32_t)(uintptr_t)pv, 0);
                        else if (sizeof(void *) == 8)
                            gdbStubOutCtxAppendHexU64(pThis, (uint64_t)(uintptr_t)pv, 0);
                        else
                            gdbStubOutCtxAppendString(pThis, "<Unrecognised pointer width>");
                    }
                    default:
                        /** @todo: Ignore or assert? */
                        ;
                }
                break;
            }
            default:
                gdbStubOutCtxAppendChar(pThis, ch);
        }
    }

    va_end(hArgs);
    return rc;
}


/**
 * Resets the given output context.
 *
 * @returns nothing.
 * @param   pThis               The output context instance.
 */
static void gdbStubOutCtxReset(PGDBSTUBOUTCTX pThis)
{
    pThis->offScratch = 0;
    gdbStubCtxMemset(&pThis->abScratch[0], 0, sizeof(pThis->abScratch));
}


/**
 * Initializes the given output context.
 *
 * @returns nothing.
 * @param   pThis               The output context instance.
 */
static void gdbStubOutCtxInit(PGDBSTUBOUTCTX pThis)
{
    pThis->Hlp.pfnPrintf = gdbStubOutCtxPrintf;
    gdbStubOutCtxReset(pThis);
}


/**
 * Encodes the given buffer as a hexstring string it into the given destination buffer.
 *
 * @returns Status code.
 * @param   pbDst               Where store the resulting hex string on success.
 * @param   cbDst               Size of the destination buffer in bytes.
 * @param   pvSrc               The data to encode.
 * @param   cbSrc               Number of bytes to encode.
 */
static int gdbStubCtxEncodeBinaryAsHex(uint8_t *pbDst, size_t cbDst, void *pvSrc, size_t cbSrc)
{
    if (cbSrc * 2 > cbDst)
        return GDBSTUB_ERR_INVALID_PARAMETER;

    uint8_t *pbSrc = (uint8_t *)pvSrc;
    for (size_t i = 0; i < cbSrc; i++)
    {
        uint8_t bSrc = *pbSrc++;

        *pbDst++ = gdbStubCtxHexToChr(bSrc >> 4);
        *pbDst++ = gdbStubCtxHexToChr(bSrc & 0xf);
    }

    return GDBSTUB_INF_SUCCESS;
}


/**
 * Decodes the given ASCII hexstring as binary data up until the given separator is found or the end of the string is reached.
 *
 * @returns Status code.
 * @param   pbBuf               The buffer containing the hexstring to convert.
 * @param   cbBuf               Size of the buffer in bytes.
 * @param   pvDst               Where to store the decoded data.
 * @param   cbDst               Maximum buffer sizein bytes.
 * @param   chSep               The character to stop conversion at.
 * @param   ppbSep              Where to store the pointer in the buffer where the separator was found, optional.
 */
static int gdbStubCtxParseHexStringAsInteger(const uint8_t *pbBuf, size_t cbBuf, uint64_t *puVal, uint8_t chSep, const uint8_t **ppbSep)
{
    uint64_t uVal = 0;

    while (   cbBuf
           && *pbBuf != chSep)
    {
        uVal = uVal * 16 + gdbStubCtxChrToHex(*pbBuf++);
        cbBuf--;
    }

    *puVal = uVal;

    if (ppbSep)
        *ppbSep = pbBuf;

    return GDBSTUB_INF_SUCCESS;
}


/**
 * Decodes the given ASCII hexstring as a byte buffer up until the given separator is found or the end of the string is reached.
 *
 * @returns Status code.
 * @param   pbBuf               The buffer containing the hexstring to convert.
 * @param   cbBuf               Size of the buffer in bytes.
 * @param   pvDst               Where to store the decoded data.
 * @param   cbDst               Maximum buffer size in bytes.
 * @param   pcbDecoded          Where to store the number of consumed bytes from the input.
 */
static int gdbStubCtxParseHexStringAsByteBuf(const uint8_t *pbBuf, size_t cbBuf, void *pvDst, size_t cbDst, size_t *pcbDecoded)
{
    size_t cbDecode = MIN(cbBuf, cbDst * 2);

    /* A single byte is constructed from two hex digits. */
    if (cbDecode % 2 != 0)
        return GDBSTUB_ERR_INVALID_PARAMETER;

    if (pcbDecoded)
        *pcbDecoded = cbDecode;

    uint8_t *pbDst = (uint8_t *)pvDst;
    while (   cbBuf
           && cbDst)
    {
        uint8_t bByte = (gdbStubCtxChrToHex(*pbBuf) << 4) | gdbStubCtxChrToHex(*(pbBuf + 1));
        *pbDst++ = bByte;
        pbBuf += 2;
        cbBuf -= 2;
        cbDst--;
    }

    return GDBSTUB_INF_SUCCESS;
}


/**
 * Ensures that there is at least the given amount of bytes of free space left in the packet buffer.
 *
 * @returns Status code (error when increasing the buffer failed).
 * @param   pThis               The GDB stub context.
 * @param   cbSpace             Number of bytes required.
 */
static int gdbStubCtxEnsurePktBufSpace(PGDBSTUBCTXINT pThis, size_t cbSpace)
{
    if (pThis->cbPktBufMax - pThis->offPktBuf >= cbSpace)
        return GDBSTUB_INF_SUCCESS;

    /* Slow path allocate new buffer and copy content over. */
    int rc = GDBSTUB_INF_SUCCESS;
    size_t cbPktBufMaxNew = pThis->cbPktBufMax - pThis->offPktBuf + cbSpace;
    void *pvNew = gdbStubCtxIfMemAlloc(pThis, cbPktBufMaxNew);
    if (pvNew)
    {
        gdbStubCtxMemcpy(pvNew, pThis->pbPktBuf, pThis->offPktBuf);
        if (pThis->pbPktBuf)
            gdbStubCtxIfMemFree(pThis, pThis->pbPktBuf);
        pThis->pbPktBuf    = (uint8_t *)pvNew;
        pThis->cbPktBufMax = cbPktBufMaxNew;
    }
    else
        rc = GDBSTUB_ERR_NO_MEMORY;

    return rc;
}


/**
 * Sends the given reply packet, doing the framing, checksumming, etc.
 *
 * @returns Status code.
 * @param   pThis               The GDB stub context.
 * @param   pbReplyPkt          The reply packet to send.
 * @param   cbReplyPkt          Size of the reply packet in bytes.
 */
static int gdbStubCtxReplySend(PGDBSTUBCTXINT pThis, const uint8_t *pbReplyPkt, size_t cbReplyPkt)
{
    uint8_t chPktStart = GDBSTUB_PKT_START;
    int rc = gdbStubCtxIoIfWrite(pThis, &chPktStart, sizeof(chPktStart));
    if (rc == GDBSTUB_INF_SUCCESS)
    {
        uint8_t uChkSum = 0;
        for (uint32_t i = 0; i < cbReplyPkt; i++)
            uChkSum += pbReplyPkt[i];

        if (cbReplyPkt)
            rc = gdbStubCtxIoIfWrite(pThis, pbReplyPkt, cbReplyPkt);
        if (rc == GDBSTUB_INF_SUCCESS)
        {
            uint8_t chPktEnd = GDBSTUB_PKT_END;
            rc = gdbStubCtxIoIfWrite(pThis, &chPktEnd, sizeof(chPktEnd));
            if (rc == GDBSTUB_INF_SUCCESS)
            {
                uint8_t achChkSum[2];

                achChkSum[0] = gdbStubCtxHexToChr(uChkSum >> 4);
                achChkSum[1] = gdbStubCtxHexToChr(uChkSum & 0xf);
                rc = gdbStubCtxIoIfWrite(pThis, &achChkSum[0], sizeof(achChkSum));
            }
        }
    }

    return rc;
}


/**
 * Sends a 'OK' reply packet.
 *
 * @returns Status code.
 * @param   pThis               The GDB stub context.
 */
static int gdbStubCtxReplySendOk(PGDBSTUBCTXINT pThis)
{
    char achOk[2] = { 'O', 'K' };
    return gdbStubCtxReplySend(pThis, &achOk[0], sizeof(achOk));
}


/**
 * Sends a 'E NN' reply packet.
 *
 * @returns Status code.
 * @param   pThis               The GDB stub context.
 * @param   uErr                The error code to send.
 */
static int gdbStubCtxReplySendErr(PGDBSTUBCTXINT pThis, uint8_t uErr)
{
    char achErr[3] = { 'E', 0, 0 };
    achErr[1] = gdbStubCtxHexToChr(uErr >> 4);
    achErr[2] = gdbStubCtxHexToChr(uErr & 0xf);
    return gdbStubCtxReplySend(pThis, &achErr[0], sizeof(achErr));
}


/**
 * Sends a signal trap (S 05) packet to indicate that the target has stopped.
 *
 * @returns Status code.
 * @param   pThis               The GDB stub context.
 */
static int gdbStubCtxReplySendSigTrap(PGDBSTUBCTXINT pThis)
{
    uint8_t achSigTrap[3] = { 'S', '0', '5' };
    return gdbStubCtxReplySend(pThis, &achSigTrap[0], sizeof(achSigTrap));
}


/**
 * Sends a GDB stub status code indicating an error using the error reply packet.
 *
 * @returns Status code.
 * @param   pThis               The GDB stub context.
 * @param   rc                  The status code to send.
 */
static int gdbStubCtxReplySendErrSts(PGDBSTUBCTXINT pThis, int rc)
{
    /** Todo convert error codes maybe. */
    return gdbStubCtxReplySendErr(pThis, (-rc) & 0xff);
}


/**
 * Parses the arguments of a 'Z' and 'z' packet.
 *
 * @returns Status code.
 * @param   pbArgs                  Pointer to the start of the first argument.
 * @param   cbArgs                  Number of argument bytes.
 * @param   penmTpType              Where to store the tracepoint type on success.
 * @param   pGdbTgtAddr             Where to store the address on success.
 * @param   puKind                  Where to store the kind argument on success.
 */
static int gdbStubCtxParseTpPktArgs(const uint8_t *pbArgs, size_t cbArgs, GDBSTUBTPTYPE *penmTpType, GDBTGTMEMADDR *pGdbTgtAddr, uint64_t *puKind)
{
    const uint8_t *pbPktSep = NULL;
    uint64_t uType = 0;

    int rc = gdbStubCtxParseHexStringAsInteger(pbArgs, cbArgs, &uType,
                                               ',', &pbPktSep);
    if (rc == GDBSTUB_INF_SUCCESS)
    {
        cbArgs -= (uintptr_t)(pbPktSep - pbArgs) - 1;
        rc = gdbStubCtxParseHexStringAsInteger(pbPktSep + 1, cbArgs, pGdbTgtAddr,
                                               ',', &pbPktSep);
        if (rc == GDBSTUB_INF_SUCCESS)
        {
            cbArgs -= (uintptr_t)(pbPktSep - pbArgs) - 1;
            rc = gdbStubCtxParseHexStringAsInteger(pbPktSep + 1, cbArgs, puKind,
                                                   GDBSTUB_PKT_END, NULL);
            if (rc == GDBSTUB_INF_SUCCESS)
            {
                switch (uType)
                {
                    case 0:
                        *penmTpType = GDBSTUBTPTYPE_EXEC_SW;
                        break;
                    case 1:
                        *penmTpType = GDBSTUBTPTYPE_EXEC_HW;
                        break;
                    case 2:
                        *penmTpType = GDBSTUBTPTYPE_MEM_WRITE;
                        break;
                    case 3:
                        *penmTpType = GDBSTUBTPTYPE_MEM_READ;
                        break;
                    case 4:
                        *penmTpType = GDBSTUBTPTYPE_MEM_ACCESS;
                        break;
                    default:
                        rc = GDBSTUB_ERR_INVALID_PARAMETER;
                        break;
                }
            }
        }
    }

    return rc;
}


/**
 * Processes the 'TStatus' query.
 *
 * @returns Status code.
 * @param   pThis               The GDB stub context.
 * @param   pbArgs              Pointer to the start of the arguments in the packet.
 * @param   cbArgs              Size of arguments in bytes.
 */
static int gdbStubCtxPktProcessQueryTStatus(PGDBSTUBCTXINT pThis, const uint8_t *pbArgs, size_t cbArgs)
{
    char achReply[2] = { 'T', '0' };
    return gdbStubCtxReplySend(pThis, &achReply[0], sizeof(achReply));
}


/**
 * @copydoc{FNGDBSTUBQPKTPROC}
 */
static int gdbStubCtxPktProcessFeatXmlRegs(PGDBSTUBCTXINT pThis, const uint8_t *pbVal, size_t cbVal)
{
    /*
     * xmlRegisters contain a list of supported architectures delimited by ','.
     * Check that the architecture is in the supported list.
     */
    int rc = GDBSTUB_INF_SUCCESS;
    while (cbVal)
    {
        /* Find the next delimiter. */
        size_t cbThisVal = cbVal;
        const uint8_t *pbDelim = gdbStubCtxMemchr(pbVal, ',', cbVal);
        if (pbDelim)
            cbThisVal = pbDelim - pbVal;

        size_t cchArch = gdbStubStrlen(s_aGdbArchMapping[pThis->pIf->enmArch]);
        if (!gdbStubMemcmp(pbVal, s_aGdbArchMapping[pThis->pIf->enmArch], MIN(cbVal, cchArch)))
        {
            /* Set the flag to support the qXfer:features:read packet. */
            pThis->fFeatures |= GDBSTUBCTX_FEATURES_F_TGT_DESC;
            break;
        }

        cbVal -= cbThisVal + (pbDelim ? 1 : 0);
        pbVal = pbDelim + (pbDelim ? 1 : 0);
    }

    return rc;
}


/**
 * Features which can be reported by the remote GDB which we might support.
 *
 * @note The sorting matters for features which start the same, the longest must come first.
 */
static const GDBSTUBFEATDESC g_aGdbFeatures[] =
{
#define GDBSTUBFEATDESC_INIT(a_Name, a_pfnHnd, a_fVal) { a_Name, sizeof(a_Name) - 1, a_pfnHnd, a_fVal }
    GDBSTUBFEATDESC_INIT("xmlRegisters",   gdbStubCtxPktProcessFeatXmlRegs, true),
#undef GDBSTUBFEATDESC_INIT
};


/**
 * Calculates the feature length of the next feature pointed to by the given arguments buffer.
 *
 * @returns Status code.
 * @param   pbArgs              Pointer to the start of the arguments in the packet.
 * @param   cbArgs              Size of arguments in bytes.
 * @param   pcbArg              Where to store the size of the argument in bytes on success (excluding the delimiter).
 * @param   pfTerminator        Whereto store the flag whether the packet terminator (#) was seen as a delimiter.
 */
static int gdbStubCtxQueryPktQueryFeatureLen(const uint8_t *pbArgs, size_t cbArgs, size_t *pcbArg, bool *pfTerminator)
{
    const uint8_t *pbArgCur = pbArgs;

    while (   cbArgs
           && *pbArgCur != ';'
           && *pbArgCur != GDBSTUB_PKT_END)
    {
        cbArgs--;
        pbArgCur++;
    }

    if (   !cbArgs
        && *pbArgCur != ';'
        && *pbArgCur != GDBSTUB_PKT_END)
        return GDBSTUB_ERR_PROTOCOL_VIOLATION;

    *pcbArg       = pbArgCur - pbArgs;
    *pfTerminator = *pbArgCur == GDBSTUB_PKT_END ? true : false;

    return GDBSTUB_INF_SUCCESS;
}


/**
 * Sends the reply to the 'qSupported' packet.
 *
 * @returns Status code.
 * @param   pThis               The GDB stub context.
 */
static int gdbStubCtxPktProcessQuerySupportedReply(PGDBSTUBCTXINT pThis)
{
    /** @todo Enhance. */
    if (pThis->fFeatures & GDBSTUBCTX_FEATURES_F_TGT_DESC)
        return gdbStubCtxReplySend(pThis, "qXfer:features:read+", sizeof("qXfer:features:read+") - 1);

    return gdbStubCtxReplySend(pThis, NULL, 0);
}


/**
 * Processes the 'Supported' query.
 *
 * @returns Status code.
 * @param   pThis               The GDB stub context.
 * @param   pbArgs              Pointer to the start of the arguments in the packet.
 * @param   cbArgs              Size of arguments in bytes.
 */
static int gdbStubCtxPktProcessQuerySupported(PGDBSTUBCTXINT pThis, const uint8_t *pbArgs, size_t cbArgs)
{
    /* Skip the : following the qSupported start. */
    if (   cbArgs < 1
        || pbArgs[0] != ':')
        return GDBSTUB_ERR_PROTOCOL_VIOLATION;

    cbArgs--;
    pbArgs++;

    /*
     * Each feature but the last one are separated by ; and the last one is delimited by the # packet end symbol.
     * We first determine the boundaries of the reported feature and pass it to the appropriate handler.
     */
    int rc = GDBSTUB_INF_SUCCESS;
    while (   cbArgs
           && rc == GDBSTUB_INF_SUCCESS)
    {
        bool fTerminator = false;
        size_t cbArg = 0;
        rc = gdbStubCtxQueryPktQueryFeatureLen(pbArgs, cbArgs, &cbArg, &fTerminator);
        if (rc == GDBSTUB_INF_SUCCESS)
        {
            /* Search for the feature handler. */
            for (uint32_t i = 0; i < ELEMENTS(g_aGdbFeatures); i++)
            {
                PCGDBSTUBFEATDESC pFeatDesc = &g_aGdbFeatures[i];

                if (   cbArg > pFeatDesc->cchName /* At least one character must come after the feature name ('+', '-' or '='). */
                    && !gdbStubMemcmp(pFeatDesc->pszName, pbArgs, pFeatDesc->cchName))
                {
                    /* Found, execute handler after figuring out whether there is a value attached. */
                    const uint8_t *pbVal = pbArgs + pFeatDesc->cchName;
                    size_t cbVal = cbArg - pFeatDesc->cchName;

                    if (pFeatDesc->fVal)
                    {
                        if (   *pbVal == '='
                            && cbVal > 1)
                        {
                            pbVal++;
                            cbVal--;
                        }
                        else
                            rc = GDBSTUB_ERR_PROTOCOL_VIOLATION;
                    }
                    else if (   cbVal != 1
                             || (   *pbVal != '+'
                                 && *pbVal != '-')) /* '+' and '-' are allowed to indicate support for a particular feature. */
                        rc = GDBSTUB_ERR_PROTOCOL_VIOLATION;

                    if (rc == GDBSTUB_INF_SUCCESS)
                        rc = pFeatDesc->pfnHandler(pThis, pbVal, cbVal);
                    break;
                }
            }

            cbArgs -= cbArg;
            pbArgs += cbArg;
            if (!fTerminator)
            {
                cbArgs--;
                pbArgs++;
            }
            else
                break;
        }
    }

    /* If everything went alright send the reply with our supported features. */
    if (rc == GDBSTUB_INF_SUCCESS)
        rc = gdbStubCtxPktProcessQuerySupportedReply(pThis);

    return rc;
}


/**
 * Sends the reply to a 'qXfer:<object>:read:...' request.
 *
 * @returns Status code.
 * @param   pThis               The GDB stub context.
 * @param   offRead             Where to start reading from within the object.
 * @param   cbRead              How much to read.
 * @param   pbObj               The start of the object.
 * @param   cbObj               Size of the object.
 */
static int gdbStubCtxQueryXferReadReply(PGDBSTUBCTXINT pThis, uint32_t offRead, size_t cbRead, const uint8_t *pbObj, size_t cbObj)
{
    int rc = GDBSTUB_INF_SUCCESS;

    if (offRead < cbObj)
    {
        /** @todo Escaping */
        size_t cbThisRead = offRead + cbRead < cbObj ? cbRead : cbObj - offRead;

        rc = gdbStubCtxEnsurePktBufSpace(pThis, cbThisRead + 1);
        if (rc == GDBSTUB_INF_SUCCESS)
        {
            uint8_t *pbPktBuf = pThis->pbPktBuf;
            *pbPktBuf++ = cbThisRead < cbRead ? 'l' : 'm';
            gdbStubCtxMemcpy(pbPktBuf, pbObj + offRead, cbThisRead);
            rc = gdbStubCtxReplySend(pThis, pThis->pbPktBuf, cbThisRead + 1);
        }
        else
            rc = gdbStubCtxReplySendErr(pThis, GDBSTUB_ERR_NO_MEMORY);
    }
    else if (offRead == cbObj)
        rc = gdbStubCtxReplySend(pThis, "l", sizeof("l") - 1);
    else
        rc = gdbStubCtxReplySendErr(pThis, GDBSTUB_ERR_PROTOCOL_VIOLATION);

    return rc;
}


/**
 * Parses the annex:offset,length part of a 'qXfer:<object>:read:...' request.
 *
 * @returns Status code.
 * @param   pbArgs              Start of the arguments beginning with <annex>.
 * @param   cbArgs              Number of bytes remaining for the arguments.
 * @param   ppchAnnex           Where to store the pointer to the beginning of the annex on success.
 * @param   pcchAnnex           Where to store the number of characters for the annex on success.
 * @param   poff                Where to store the offset on success.
 * @param   pcb                 Where to store the length on success.
 */
static int gdbStubCtxPktProcessQueryXferParseAnnexOffLen(const uint8_t *pbArgs, size_t cbArgs, const char **ppchAnnex, size_t *pcchAnnex,
                                                         uint32_t *poffRead, size_t *pcbRead)
{
    int rc = GDBSTUB_INF_SUCCESS;
    const uint8_t *pbSep = gdbStubCtxMemchr(pbArgs, ':', cbArgs);
    if (pbSep)
    {
        *ppchAnnex = (const char *)pbArgs;
        *pcchAnnex = pbSep - pbArgs;

        pbSep++;
        cbArgs -= *pcchAnnex + 1;

        uint64_t u64Tmp = 0;
        const uint8_t *pbLenStart = NULL;
        rc = gdbStubCtxParseHexStringAsInteger(pbSep, cbArgs, &u64Tmp, ',', &pbLenStart);
        if (   rc == GDBSTUB_INF_SUCCESS
            && (uint32_t)u64Tmp == u64Tmp)
        {
            *poffRead = (uint32_t)u64Tmp;
            cbArgs -= pbLenStart - pbSep;

            rc = gdbStubCtxParseHexStringAsInteger(pbLenStart + 1, cbArgs, &u64Tmp, '#', &pbLenStart);
            if (   rc == GDBSTUB_INF_SUCCESS
                && (size_t)u64Tmp == u64Tmp)
                *pcbRead = (size_t)u64Tmp;
            else
                rc = GDBSTUB_ERR_PROTOCOL_VIOLATION;
        }
        else
            rc = GDBSTUB_ERR_PROTOCOL_VIOLATION;
    }
    else
        rc = GDBSTUB_ERR_PROTOCOL_VIOLATION;

    return rc;
}


/**
 * Creates the target XML description.
 *
 * @returns Status code.
 * @param   pThis               The GDB stub context.
 */
static int gdbStubCtxTgtXmlDescCreate(PGDBSTUBCTXINT pThis)
{
    static const char s_szXmlTgtHdr[] =
        "<?xml version=\"1.0\"?>\n"
        "<!DOCTYPE target SYSTEM \"gdb-target.dtd\">\n"
        "<target version=\"1.0\">\n";
    static const char s_szXmlTgtFooter[] =
        "</target>\n";

    /** @todo Redo ASAP, this is crappy as hell. */
    int rc = GDBSTUB_INF_SUCCESS;
    size_t cbXmlTgtDesc = sizeof(s_szXmlTgtHdr) + sizeof(s_szXmlTgtFooter);

    /* Add length for architecture. */
    cbXmlTgtDesc +=   (sizeof("<architecture>") - 1)
                    + (sizeof("</architecture>\n") - 1)
                    + gdbStubStrlen(s_aGdbArchMapping[pThis->pIf->enmArch]);

    /* Add length for the <feature></feature>. */
    cbXmlTgtDesc +=  (sizeof("<feature name=\"\">\n") - 1)
                    + (sizeof("</feature>\n") - 1)
                    + gdbStubStrlen(s_aGdbArchFeatMapping[pThis->pIf->enmArch]);

    /* Add the length for each register. */
    for (uint32_t i = 0; i < pThis->cRegs; i++)
    {
        PCGDBSTUBREG pReg = &pThis->pIf->paRegs[i];

        size_t cchRegName = gdbStubStrlen(pReg->pszName);
        cbXmlTgtDesc +=   (sizeof("<reg name=\"\" bitsize=\"\"/>\n") - 1)
                        + cchRegName + 2; /* Up to two characters for bitsize for now. */

        if (   pReg->enmType == GDBSTUBREGTYPE_PC
            || pReg->enmType == GDBSTUBREGTYPE_STACK_PTR
            || pReg->enmType == GDBSTUBREGTYPE_CODE_PTR)
        {
            size_t cchTypeName = pReg->enmType == GDBSTUBREGTYPE_STACK_PTR ? sizeof("data_ptr") - 1 : sizeof("code_ptr") - 1;
            cbXmlTgtDesc += sizeof(" type=\"\"") + cchTypeName - 1;
        }
    }

    pThis->pbTgtXmlDesc = (uint8_t *)gdbStubCtxIfMemAlloc(pThis, cbXmlTgtDesc);
    if (pThis->pbTgtXmlDesc)
    {
        uint8_t *pbXmlCur = pThis->pbTgtXmlDesc;
        pThis->cbTgtXmlDesc = cbXmlTgtDesc;

        gdbStubCtxMemcpy(pbXmlCur, &s_szXmlTgtHdr[0], sizeof(s_szXmlTgtHdr) - 1);
        pbXmlCur += sizeof(s_szXmlTgtHdr) - 1;

        gdbStubCtxMemcpy(pbXmlCur, "<architecture>", sizeof("<architecture>") - 1);
        pbXmlCur += sizeof("<architecture>") - 1;

        size_t cch = gdbStubStrlen(s_aGdbArchMapping[pThis->pIf->enmArch]);
        gdbStubCtxMemcpy(pbXmlCur, s_aGdbArchMapping[pThis->pIf->enmArch], cch);
        pbXmlCur += cch;

        gdbStubCtxMemcpy(pbXmlCur, "</architecture>\n", sizeof("</architecture>\n") - 1);
        pbXmlCur += sizeof("</architecture>\n") - 1;

        gdbStubCtxMemcpy(pbXmlCur, "<feature name=\"", sizeof("<feature name=\"") - 1);
        pbXmlCur += sizeof("<feature name=\"") - 1;

        cch = gdbStubStrlen(s_aGdbArchFeatMapping[pThis->pIf->enmArch]);
        gdbStubCtxMemcpy(pbXmlCur, s_aGdbArchFeatMapping[pThis->pIf->enmArch], cch);
        pbXmlCur += cch;

        gdbStubCtxMemcpy(pbXmlCur, "\">\n", sizeof("\">\n") - 1);
        pbXmlCur += sizeof("\">\n") - 1;

        /* Register */
        for (uint32_t i = 0; i < pThis->cRegs; i++)
        {
            PCGDBSTUBREG pReg = &pThis->pIf->paRegs[i];
            size_t cchRegName = gdbStubStrlen(pReg->pszName);

            gdbStubCtxMemcpy(pbXmlCur, "<reg name=\"", sizeof("<reg name=\"") - 1);
            pbXmlCur += sizeof("<reg name=\"") - 1;

            gdbStubCtxMemcpy(pbXmlCur, pReg->pszName, cchRegName);
            pbXmlCur += cchRegName;

            gdbStubCtxMemcpy(pbXmlCur, "\" bitsize=\"", sizeof("\" bitsize=\"") - 1);
            pbXmlCur += sizeof("\" bitsize=\"") - 1;

            *pbXmlCur++ = '0' + (pReg->cRegBits / 10);
            *pbXmlCur++ = '0' + (pReg->cRegBits % 10);

            if (   pReg->enmType == GDBSTUBREGTYPE_PC
                || pReg->enmType == GDBSTUBREGTYPE_STACK_PTR
                || pReg->enmType == GDBSTUBREGTYPE_CODE_PTR)
            {
                size_t cchTypeName = pReg->enmType == GDBSTUBREGTYPE_STACK_PTR ? sizeof("data_ptr") - 1 : sizeof("code_ptr") - 1;
                const char *pszTypeName = pReg->enmType == GDBSTUBREGTYPE_STACK_PTR ? "data_ptr" : "code_ptr";

                gdbStubCtxMemcpy(pbXmlCur, "\" type=\"", sizeof("\" type=\"") - 1);
                pbXmlCur += sizeof("\" type=\"") - 1;

                gdbStubCtxMemcpy(pbXmlCur, pszTypeName, cchTypeName);
                pbXmlCur += cchTypeName;
            }

            gdbStubCtxMemcpy(pbXmlCur, "\"/>\n", sizeof("\"/>\n") - 1);
            pbXmlCur += sizeof("\"/>\n") - 1;
        }

        gdbStubCtxMemcpy(pbXmlCur, "</feature>\n", sizeof("</feature>\n") - 1);
        pbXmlCur += sizeof("</feature>\n") - 1;

        gdbStubCtxMemcpy(pbXmlCur, &s_szXmlTgtFooter[0], sizeof(s_szXmlTgtFooter) - 1);
        pbXmlCur += sizeof(s_szXmlTgtFooter) - 1;
    }
    else
        rc = GDBSTUB_ERR_NO_MEMORY;

    return rc;
}


/**
 * Processes the 'Xfer:features:read' query.
 *
 * @returns Status code.
 * @param   pThis               The GDB stub context.
 * @param   pbArgs              Pointer to the start of the arguments in the packet.
 * @param   cbArgs              Size of arguments in bytes.
 */
static int gdbStubCtxPktProcessQueryXferFeatRead(PGDBSTUBCTXINT pThis, const uint8_t *pbArgs, size_t cbArgs)
{
    int rc = GDBSTUB_INF_SUCCESS;

    /* Skip the : following the Xfer:features:read start. */
    if (   cbArgs < 1
        || pbArgs[0] != ':')
        return GDBSTUB_ERR_PROTOCOL_VIOLATION;

    cbArgs--;
    pbArgs++;

    if (pThis->fFeatures & GDBSTUBCTX_FEATURES_F_TGT_DESC)
    {
        /* Create the target XML description if not existing. */
        if (!pThis->pbTgtXmlDesc)
            rc = gdbStubCtxTgtXmlDescCreate(pThis);

        if (rc == GDBSTUB_INF_SUCCESS)
        {
            /* Parse annex, offset and length and return the data. */
            const char *pchAnnex = NULL;
            size_t cchAnnex = 0;
            uint32_t offRead = 0;
            size_t cbRead = 0;

            rc = gdbStubCtxPktProcessQueryXferParseAnnexOffLen(pbArgs, cbArgs,
                                                               &pchAnnex, &cchAnnex,
                                                               &offRead, &cbRead);
            if (rc == GDBSTUB_INF_SUCCESS)
            {
                /* Check whether the annex is supported. */
                if (   cchAnnex == sizeof("target.xml") - 1
                    && !gdbStubMemcmp(pchAnnex, "target.xml", cchAnnex))
                    rc = gdbStubCtxQueryXferReadReply(pThis, offRead, cbRead, pThis->pbTgtXmlDesc, pThis->cbTgtXmlDesc);
                else
                    rc = gdbStubCtxReplySendErr(pThis, 0);
            }
            else
                rc = gdbStubCtxReplySendErrSts(pThis, rc);
        }
        else
            rc = gdbStubCtxReplySendErrSts(pThis, rc);
    }
    else
        rc = gdbStubCtxReplySend(pThis, NULL, 0); /* Not supported. */

    return rc;
}


/**
 * Calls the given command handler and processes the reply.
 *
 * @returns Status code.
 * @param   pThis               The GDB stub context.
 * @param   pCmd                The command to call.
 * @param   pszArgs             Argument string to call the command with.
 */
static int gdbStubCtxCmdProcess(PGDBSTUBCTXINT pThis, PCGDBSTUBCMD pCmd, const char *pszArgs)
{
    int rc = GDBSTUB_INF_SUCCESS;

    gdbStubOutCtxReset(&pThis->OutCtx);
    int rcCmd = pCmd->pfnCmd(pThis, &pThis->OutCtx.Hlp, pszArgs, pThis->pvUser);
    if (rcCmd == GDBSTUB_INF_SUCCESS)
    {
        if (!pThis->OutCtx.offScratch) /* No output, just send OK reply. */
            rc = gdbStubCtxReplySendOk(pThis);
        else
        {
            rc = gdbStubCtxEnsurePktBufSpace(pThis, pThis->OutCtx.offScratch * 2);
            if (rc == GDBSTUB_INF_SUCCESS)
            {
                uint8_t *pbPktBuf = pThis->pbPktBuf;
                size_t cbPktBuf = pThis->OutCtx.offScratch * 2;

                rc = gdbStubCtxEncodeBinaryAsHex(pbPktBuf, cbPktBuf, &pThis->OutCtx.abScratch[0], pThis->OutCtx.offScratch);
                if (rc == GDBSTUB_INF_SUCCESS)
                    rc = gdbStubCtxReplySend(pThis, pThis->pbPktBuf, cbPktBuf);
                else
                    rc = gdbStubCtxReplySendErrSts(pThis, rc);
            }
            else
                rc = gdbStubCtxReplySendErrSts(pThis, rc);
        }
    }
    else
        rc = gdbStubCtxReplySendErrSts(pThis, rcCmd);

    return rc;
}


/**
 * Processes the 'Rcmd' query.
 *
 * @returns Status code.
 * @param   pThis               The GDB stub context.
 * @param   pbArgs              Pointer to the start of the arguments in the packet.
 * @param   cbArgs              Size of arguments in bytes.
 */
static int gdbStubCtxPktProcessQueryRcmd(PGDBSTUBCTXINT pThis, const uint8_t *pbArgs, size_t cbArgs)
{
    int rc = GDBSTUB_INF_SUCCESS;

    /* Skip the , following the qRcmd start. */
    if (   cbArgs < 1
        || pbArgs[0] != ',')
        return GDBSTUB_ERR_PROTOCOL_VIOLATION;

    if (!pThis->pIf->paCmds)
        return GDBSTUB_ERR_NOT_FOUND;

    cbArgs--;
    pbArgs++;

    /* Decode the command. */
    /** @todo Make this dynamic. */
    char szCmd[4096];
    if (cbArgs / 2 >= sizeof(szCmd))
        return GDBSTUB_ERR_BUFFER_OVERFLOW;

    size_t cbDecoded = 0;
    rc = gdbStubCtxParseHexStringAsByteBuf(pbArgs, cbArgs - 1, &szCmd[0], sizeof(szCmd), &cbDecoded);
    if (rc == GDBSTUB_INF_SUCCESS)
    {
        const char *pszArgs = NULL;
        szCmd[cbDecoded] = '\0'; /* Ensure zero termination. */

        /** @todo Sanitize string. */

        /* Look for the first space and take that as the separator between command identifier. */
        uint8_t *pbDelim = gdbStubCtxMemchr(&szCmd[0], ' ', cbDecoded);
        if (pbDelim)
        {
            *pbDelim = '\0';
            pszArgs = pbDelim + 1;
        }

        /* Search for the command. */
        PCGDBSTUBCMD pCmd = &pThis->pIf->paCmds[0];
        rc = GDBSTUB_ERR_NOT_FOUND;
        while (pCmd->pszCmd)
        {
            if (!gdbStubStrcmp(pCmd->pszCmd, &szCmd[0]))
            {
                rc = gdbStubCtxCmdProcess(pThis, pCmd, pszArgs);
                break;
            }
            pCmd++;
        }

        if (rc == GDBSTUB_ERR_NOT_FOUND)
            rc = gdbStubCtxReplySendErrSts(pThis, rc); /** @todo Send string. */
    }

    return rc;
}


/**
 * List of supported query packets.
 */
static const GDBSTUBQPKTPROC g_aQPktProcs[] =
{
#define GDBSTUBQPKTPROC_INIT(a_Name, a_pfnProc) { a_Name, sizeof(a_Name) - 1, a_pfnProc }
    GDBSTUBQPKTPROC_INIT("TStatus",            gdbStubCtxPktProcessQueryTStatus),
    GDBSTUBQPKTPROC_INIT("Supported",          gdbStubCtxPktProcessQuerySupported),
    GDBSTUBQPKTPROC_INIT("Xfer:features:read", gdbStubCtxPktProcessQueryXferFeatRead),
    GDBSTUBQPKTPROC_INIT("Rcmd",               gdbStubCtxPktProcessQueryRcmd),
#undef GDBSTUBQPKTPROC_INIT
};


/**
 * Processes a 'q' packet, sending the appropriate reply.
 *
 * @returns Status code.
 * @param   pThis               The GDB stub context.
 * @param   pbQuery             The query packet data (without the 'q').
 * @param   cbQuery             Size of the remaining query packet in bytes.
 */
static int gdbStubCtxPktProcessQuery(PGDBSTUBCTXINT pThis, const uint8_t *pbQuery, size_t cbQuery)
{
    int rc = GDBSTUB_INF_SUCCESS;

    /* Search the query and execute the processor or return an empty reply if not supported. */
    for (uint32_t i = 0; i < ELEMENTS(g_aQPktProcs); i++)
    {
        size_t cbCmp = g_aQPktProcs[i].cchName < cbQuery ? g_aQPktProcs[i].cchName : cbQuery;

        if (!gdbStubMemcmp(pbQuery, g_aQPktProcs[i].pszName, cbCmp))
            return g_aQPktProcs[i].pfnProc(pThis, pbQuery + cbCmp, cbQuery - cbCmp);
    }

    return gdbStubCtxReplySend(pThis, NULL, 0);
}


/**
 * Processes a 'vCont[;action[:thread-id]]' packet.
 *
 * @returns Status code.
 * @param   pThis               The GDB stub context.
 * @param   pbArgs              Pointer to the start of the arguments in the packet.
 * @param   cbArgs              Size of arguments in bytes.
 */
static int gdbStubCtxPktProcessVCont(PGDBSTUBCTXINT pThis, const uint8_t *pbArgs, size_t cbArgs)
{
    int rc = GDBSTUB_INF_SUCCESS;

    /* Skip the ; following the identifier. */
    if (   cbArgs < 2
        || pbArgs[0] != ';')
        return gdbStubCtxReplySendErrSts(pThis, GDBSTUB_ERR_PROTOCOL_VIOLATION);

    pbArgs++;
    cbArgs--;

    /** @todo For now we don't care about multiple threads and ignore thread IDs and multiple actions. */
    switch (pbArgs[0])
    {
        case 'c':
        {
            rc = gdbStubCtxIfTgtContinue(pThis);
            if (rc == GDBSTUB_INF_SUCCESS)
                pThis->enmTgtStateLast = GDBSTUBTGTSTATE_RUNNING;
            break;
        }
        case 's':
        {
            rc = gdbStubCtxIfTgtStep(pThis);
            if (rc == GDBSTUB_INF_SUCCESS)
                rc = gdbStubCtxReplySendSigTrap(pThis);
            break;
        }
        case 't':
        {
            rc = gdbStubCtxIfTgtStop(pThis);
            if (rc == GDBSTUB_INF_SUCCESS)
                rc = gdbStubCtxReplySendSigTrap(pThis);
            break;
        }
        default:
            rc = gdbStubCtxReplySendErrSts(pThis, GDBSTUB_ERR_PROTOCOL_VIOLATION);
    }

    return rc;
}


/**
 * List of supported 'v<identifier>' packets.
 */
static const GDBSTUBVPKTPROC g_aVPktProcs[] =
{
#define GDBSTUBVPKTPROC_INIT(a_Name, a_pszReply, a_pfnProc) { a_Name, sizeof(a_Name) - 1, a_pszReply, sizeof(a_pszReply) - 1, a_pfnProc }
    GDBSTUBVPKTPROC_INIT("Cont", "vCont;s;c;t", gdbStubCtxPktProcessVCont)
#undef GDBSTUBVPKTPROC_INIT
};


/**
 * Processes a 'v<identifier>' packet, sending the appropriate reply.
 *
 * @returns Status code.
 * @param   pThis               The GDB stub context.
 * @param   pbPktRem            The remaining packet data (without the 'v').
 * @param   cbPktRem            Size of the remaining packet in bytes.
 */
static int gdbStubCtxPktProcessV(PGDBSTUBCTXINT pThis, const uint8_t *pbPktRem, size_t cbPktRem)
{
    int rc = GDBSTUB_INF_SUCCESS;

    /* Determine the end of the identifier, delimiters are '?', ';' or end of packet. */
    bool fQuery = false;
    const uint8_t *pbDelim = gdbStubCtxMemchr(pbPktRem, '?', cbPktRem);
    if (!pbDelim)
        pbDelim = gdbStubCtxMemchr(pbPktRem, ';', cbPktRem);
    else
        fQuery = true;

    size_t cchId = 0;
    if (pbDelim) /* Delimiter found, calculate length. */
        cchId = pbDelim - pbPktRem;
    else /* Not found, size goes till end of packet. */
        cchId = cbPktRem;

    /* Search the query and execute the processor or return an empty reply if not supported. */
    for (uint32_t i = 0; i < ELEMENTS(g_aVPktProcs); i++)
    {
        PCGDBSTUBVPKTPROC pVProc = &g_aVPktProcs[i];

        if (   pVProc->cchName == cchId
            && !gdbStubMemcmp(pbPktRem, pVProc->pszName, cchId))
        {
            /* Just send the static reply for a query and execute the processor for everything else. */
            if (fQuery)
                return gdbStubCtxReplySend(pThis, pVProc->pszReplyQ, pVProc->cchReplyQ);

            /* Execute the handler. */
            return pVProc->pfnProc(pThis, pbPktRem + cchId, cbPktRem - cchId);
        }
    }

    return gdbStubCtxReplySend(pThis, NULL, 0);
}


/**
 * Processes a completely received packet.
 *
 * @returns Status code.
 * @param   pThis               The GDB stub context.
 */
static int gdbStubCtxPktProcess(PGDBSTUBCTXINT pThis)
{
    int rc = GDBSTUB_INF_SUCCESS;

    if (pThis->cbPkt >= 1)
    {
        switch (pThis->pbPktBuf[1])
        {
            case '!': /* Enabled extended mode. */
            {
                if (pThis->pIf->pfnTgtRestart)
                {
                    pThis->fExtendedMode = true;
                    rc = gdbStubCtxReplySendOk(pThis);
                }
                else /* Send empty reply to indicate extended mode is unsupported. */
                    rc = gdbStubCtxReplySend(pThis, NULL, 0);
                break;
            }
            case '?':
            {
                /* Return signal state. */
                rc = gdbStubCtxReplySendSigTrap(pThis);
                break;
            }
            case 's': /* Single step, target stopped immediately again. */
            {
                rc = gdbStubCtxIfTgtStep(pThis);
                if (rc == GDBSTUB_INF_SUCCESS)
                    rc = gdbStubCtxReplySendSigTrap(pThis);
                break;
            }
            case 'c': /* Continue, no response */
            {
                rc = gdbStubCtxIfTgtContinue(pThis);
                if (rc == GDBSTUB_INF_SUCCESS)
                    pThis->enmTgtStateLast = GDBSTUBTGTSTATE_RUNNING;
                break;
            }
            case 'g': /* Read general registers. */
            {
                rc = gdbStubCtxIfTgtRegsRead(pThis, pThis->paidxRegs, pThis->cRegs, pThis->pvRegsScratch);
                if (rc == GDBSTUB_INF_SUCCESS)
                {
                    size_t cbReplyPkt = pThis->cbRegs * 2; /* One byte needs two characters. */

                    /* Encode data and send. */
                    rc = gdbStubCtxEnsurePktBufSpace(pThis, cbReplyPkt);
                    if (rc == GDBSTUB_INF_SUCCESS)
                    {
                        rc = gdbStubCtxEncodeBinaryAsHex(pThis->pbPktBuf, pThis->cbPktBufMax, pThis->pvRegsScratch, pThis->cbRegs);
                        if (rc == GDBSTUB_INF_SUCCESS)
                            rc = gdbStubCtxReplySend(pThis, pThis->pbPktBuf, cbReplyPkt);
                        else
                            rc = gdbStubCtxReplySendErrSts(pThis, rc);
                    }
                    else
                        rc = gdbStubCtxReplySendErrSts(pThis, rc);
                }
                else
                    rc = gdbStubCtxReplySendErrSts(pThis, rc);
                break;
            }
            case 'm': /* Read memory. */
            {
                GDBTGTMEMADDR GdbTgtAddr = 0;
                const uint8_t *pbPktSep = NULL;

                int rc = gdbStubCtxParseHexStringAsInteger(&pThis->pbPktBuf[2], pThis->cbPkt - 1, &GdbTgtAddr,
                                                           ',', &pbPktSep);
                if (rc == GDBSTUB_INF_SUCCESS)
                {
                    size_t cbProcessed = pbPktSep - &pThis->pbPktBuf[2];
                    size_t cbRead = 0;
                    rc = gdbStubCtxParseHexStringAsInteger(pbPktSep + 1, pThis->cbPkt - 1 - cbProcessed - 1, &cbRead, GDBSTUB_PKT_END, NULL);
                    if (rc == GDBSTUB_INF_SUCCESS)
                    {
                        size_t cbReplyPkt = cbRead * 2; /* One byte needs two characters. */

                        rc = gdbStubCtxEnsurePktBufSpace(pThis, cbReplyPkt);
                        if (rc == GDBSTUB_INF_SUCCESS)
                        {
                            uint8_t *pbPktBuf = pThis->pbPktBuf;
                            size_t cbPktBufLeft = cbReplyPkt;

                            while (   cbRead
                                   && rc == GDBSTUB_INF_SUCCESS)
                            {
                                size_t cbThisRead = cbRead < 1024 ? cbRead : 1024;
                                uint8_t abTmp[1024];

                                rc = gdbStubCtxIfTgtMemRead(pThis, GdbTgtAddr, &abTmp[0], cbThisRead);
                                if (rc != GDBSTUB_INF_SUCCESS)
                                    break;

                                rc = gdbStubCtxEncodeBinaryAsHex(pbPktBuf, cbPktBufLeft, &abTmp[0], cbThisRead);
                                if (rc != GDBSTUB_INF_SUCCESS)
                                    break;

                                GdbTgtAddr   += cbThisRead;
                                cbRead       -= cbThisRead;
                                pbPktBuf     += cbThisRead;
                                cbPktBufLeft -= cbThisRead;
                            }

                            if (rc == GDBSTUB_INF_SUCCESS)
                                rc = gdbStubCtxReplySend(pThis, pThis->pbPktBuf, cbReplyPkt);
                            else
                                rc = gdbStubCtxReplySendErrSts(pThis, rc);
                        }
                        else
                            rc = gdbStubCtxReplySendErrSts(pThis, rc);
                    }
                    else
                        rc = gdbStubCtxReplySendErrSts(pThis, rc);
                }
                else
                    rc = gdbStubCtxReplySendErrSts(pThis, rc);
                break;
            }
            case 'M': /* Write memory. */
            {
                GDBTGTMEMADDR GdbTgtAddr = 0;
                const uint8_t *pbPktSep = NULL;

                int rc = gdbStubCtxParseHexStringAsInteger(&pThis->pbPktBuf[2], pThis->cbPkt - 1, &GdbTgtAddr,
                                                           ',', &pbPktSep);
                if (rc == GDBSTUB_INF_SUCCESS)
                {
                    size_t cbProcessed = pbPktSep - &pThis->pbPktBuf[2];
                    size_t cbWrite = 0;
                    rc = gdbStubCtxParseHexStringAsInteger(pbPktSep + 1, pThis->cbPkt - 1 - cbProcessed - 1, &cbWrite, ':', &pbPktSep);
                    if (rc == GDBSTUB_INF_SUCCESS)
                    {
                        cbProcessed = pbPktSep - &pThis->pbPktBuf[2];
                        const uint8_t *pbDataCur = pbPktSep + 1;
                        size_t cbDataLeft = pThis->cbPkt - 1 - cbProcessed - 1 - 1;

                        while (   cbWrite
                               && !rc)
                        {
                            uint8_t abTmp[4096];
                            size_t cbThisWrite = MIN(cbWrite, sizeof(abTmp));
                            size_t cbDecoded = 0;

                            rc = gdbStubCtxParseHexStringAsByteBuf(pbDataCur, cbDataLeft, &abTmp[0], cbThisWrite, &cbDecoded);
                            if (!rc)
                                rc = gdbStubCtxIfTgtMemWrite(pThis, GdbTgtAddr, &abTmp[0], cbThisWrite);

                            GdbTgtAddr += cbThisWrite;
                            cbWrite    -= cbThisWrite;
                            pbDataCur  += cbDecoded;
                            cbDataLeft -= cbDecoded;
                        }

                        if (rc == GDBSTUB_INF_SUCCESS)
                            rc = gdbStubCtxReplySendOk(pThis);
                        else
                            rc = gdbStubCtxReplySendErrSts(pThis, rc);
                    }
                    else
                        rc = gdbStubCtxReplySendErrSts(pThis, rc);
                }
                else
                    rc = gdbStubCtxReplySendErrSts(pThis, rc);
                break;
            }
            case 'p': /* Read a single register */
            {
                uint64_t uReg = 0;
                int rc = gdbStubCtxParseHexStringAsInteger(&pThis->pbPktBuf[2], pThis->cbPkt - 1, &uReg,
                                                           GDBSTUB_PKT_END, NULL);
                if (rc == GDBSTUB_INF_SUCCESS)
                {
                    uint32_t idxReg = (uint32_t)uReg;

                    if (idxReg < pThis->cRegs)
                    {
                        rc = gdbStubCtxIfTgtRegsRead(pThis, &idxReg, 1, pThis->pvRegsScratch);
                        if (rc == GDBSTUB_INF_SUCCESS)
                        {
                            size_t cbReg = pThis->pIf->paRegs[idxReg].cRegBits / 8;
                            size_t cbReplyPkt = cbReg * 2; /* One byte needs two characters. */

                            /* Encode data and send. */
                            rc = gdbStubCtxEnsurePktBufSpace(pThis, cbReplyPkt);
                            if (rc == GDBSTUB_INF_SUCCESS)
                            {
                                rc = gdbStubCtxEncodeBinaryAsHex(pThis->pbPktBuf, pThis->cbPktBufMax, pThis->pvRegsScratch, cbReg);
                                if (rc == GDBSTUB_INF_SUCCESS)
                                    rc = gdbStubCtxReplySend(pThis, pThis->pbPktBuf, cbReplyPkt);
                                else
                                    rc = gdbStubCtxReplySendErrSts(pThis, rc);
                            }
                            else
                                rc = gdbStubCtxReplySendErrSts(pThis, rc);
                        }
                        else
                            rc = gdbStubCtxReplySendErrSts(pThis, rc);
                    }
                    else
                        rc = gdbStubCtxReplySendErrSts(pThis, GDBSTUB_ERR_PROTOCOL_VIOLATION);
                }
                else
                    rc = gdbStubCtxReplySendErrSts(pThis, rc);
                break;
            }
            case 'P': /* Write a single register */
            {
                uint64_t uReg = 0;
                const uint8_t *pbPktSep = NULL;
                int rc = gdbStubCtxParseHexStringAsInteger(&pThis->pbPktBuf[2], pThis->cbPkt - 1, &uReg,
                                                           '=', &pbPktSep);
                if (rc == GDBSTUB_INF_SUCCESS)
                {
                    uint32_t idxReg = (uint32_t)uReg;

                    if (idxReg < pThis->cRegs)
                    {
                        size_t cbProcessed = pbPktSep - &pThis->pbPktBuf[2];
                        uint32_t u32RegVal = 0;
                        rc = gdbStubCtxParseHexStringAsByteBuf(pbPktSep + 1, pThis->cbPkt - 1 - cbProcessed - 1, &u32RegVal, sizeof(u32RegVal), NULL);
                        if (rc == GDBSTUB_INF_SUCCESS)
                        {
                            rc = gdbStubCtxIfTgtRegsWrite(pThis, &idxReg, 1, &u32RegVal);
                            if (rc == GDBSTUB_INF_SUCCESS)
                                rc = gdbStubCtxReplySendOk(pThis);
                            else if (rc == GDBSTUB_ERR_NOT_SUPPORTED)
                                rc = gdbStubCtxReplySend(pThis, NULL, 0);
                            else
                                rc = gdbStubCtxReplySendErrSts(pThis, rc);
                        }
                    }
                    else
                        rc = gdbStubCtxReplySendErrSts(pThis, GDBSTUB_ERR_PROTOCOL_VIOLATION);
                }
                else
                    rc = gdbStubCtxReplySendErrSts(pThis, rc);
                break;
            }
            case 'Z': /* Insert a breakpoint/watchpoint. */
            {
                GDBSTUBTPTYPE enmTpType = 0;
                GDBTGTMEMADDR GdbTgtTpAddr = 0;
                uint64_t      uKind = 0;

                int rc = gdbStubCtxParseTpPktArgs(&pThis->pbPktBuf[2], pThis->cbPkt - 1, &enmTpType, &GdbTgtTpAddr, &uKind);
                if (rc == GDBSTUB_INF_SUCCESS)
                {
                    rc = gdbStubCtxIfTgtTpSet(pThis, GdbTgtTpAddr, enmTpType, GDBSTUBTPACTION_STOP);
                    if (rc == GDBSTUB_INF_SUCCESS)
                        rc = gdbStubCtxReplySendOk(pThis);
                    else if (rc == GDBSTUB_ERR_NOT_SUPPORTED)
                        rc = gdbStubCtxReplySend(pThis, NULL, 0);
                    else
                        rc = gdbStubCtxReplySendErrSts(pThis, rc);
                }
                else
                    rc = gdbStubCtxReplySendErrSts(pThis, rc);
                break;
            }
            case 'z': /* Remove a breakpoint/watchpoint. */
            {
                GDBSTUBTPTYPE enmTpType = 0;
                GDBTGTMEMADDR GdbTgtTpAddr = 0;
                uint64_t      uKind = 0;

                int rc = gdbStubCtxParseTpPktArgs(&pThis->pbPktBuf[2], pThis->cbPkt - 1, &enmTpType, &GdbTgtTpAddr, &uKind);
                if (rc == GDBSTUB_INF_SUCCESS)
                {
                    rc = gdbStubCtxIfTgtTpClear(pThis, GdbTgtTpAddr);
                    if (rc == GDBSTUB_INF_SUCCESS)
                        rc = gdbStubCtxReplySendOk(pThis);
                    else if (rc == GDBSTUB_ERR_NOT_SUPPORTED)
                        rc = gdbStubCtxReplySend(pThis, NULL, 0);
                    else
                        rc = gdbStubCtxReplySendErrSts(pThis, rc);
                }
                else
                    rc = gdbStubCtxReplySendErrSts(pThis, rc);
                break;
            }
            case 'q': /* Query packet */
            {
                rc = gdbStubCtxPktProcessQuery(pThis, &pThis->pbPktBuf[2], pThis->cbPkt - 1);
                break;
            }
            case 'v': /* Multiletter identifier (verbose?) */
            {
                rc = gdbStubCtxPktProcessV(pThis, &pThis->pbPktBuf[2], pThis->cbPkt - 1);
                break;
            }
            case 'R': /* Restart target. */
            {
                if (pThis->fExtendedMode) /* No reply if supported. */
                    rc = gdbStubCtxIfTgtRestart(pThis);
                else
                    rc = gdbStubCtxReplySend(pThis, NULL, 0);
                break;
            }
            case 'k': /* Kill target. */
            {
                rc = gdbStubCtxIfTgtKill(pThis);
                break;
            }
            default:
                /* Not supported, send empty reply. */
                rc = gdbStubCtxReplySend(pThis, NULL, 0);
        }
    }

    return rc;
}


/**
 * Resets the packet buffer.
 *
 * @returns nothing.
 * @param   pThis               The GDB stub context.
 */
static void gdbStubCtxPktBufReset(PGDBSTUBCTXINT pThis)
{
    pThis->offPktBuf        = 0;
    pThis->cbPkt            = 0;
    pThis->cbChksumRecvLeft = 2;
}


/**
 * Resets the given GDB stub context to the initial state.
 *
 * @returns nothing.
 * @param   pThis               The GDB stub context.
 */
static void gdbStubCtxReset(PGDBSTUBCTXINT pThis)
{
    pThis->enmState = GDBSTUBRECVSTATE_PACKET_WAIT_FOR_START;
    gdbStubCtxPktBufReset(pThis);
}


/**
 * Searches for the start character in the current data buffer.
 *
 * @returns Status code.
 * @param   pThis               The GDB stub context.
 * @param   cbData              Number of new bytes in the packet buffer.
 * @param   pcbProcessed        Where to store the amount of bytes processed.
 */
static int gdbStubCtxPktBufSearchStart(PGDBSTUBCTXINT pThis, size_t cbData, size_t *pcbProcessed)
{
    int rc = GDBSTUB_INF_SUCCESS;
    uint8_t *pbStart = gdbStubCtxMemchr(pThis->pbPktBuf, GDBSTUB_PKT_START, cbData);
    if (pbStart)
    {
        /* Found the start character, align the start to the beginning of the packet buffer and advance the state machine. */
        gdbStubCtxMemmove(pThis->pbPktBuf, pbStart, cbData - (pbStart - pThis->pbPktBuf));
        pThis->enmState = GDBSTUBRECVSTATE_PACKET_RECEIVE_BODY;
        *pcbProcessed = (uintptr_t)(pbStart - pThis->pbPktBuf);
        pThis->offPktBuf = 0;
    }
    else
    {
        /* Check for out of band characters. */
        if (gdbStubCtxMemchr(pThis->pbPktBuf, GDBSTUB_OOB_INTERRUPT, cbData) != NULL)
        {
            /* Stop target and send packet to indicate the target has stopped. */
            rc = gdbStubCtxIfTgtStop(pThis);
            if (rc == GDBSTUB_INF_SUCCESS)
                rc = gdbStubCtxReplySendSigTrap(pThis);
        }

        /* Not found, ignore the received data and reset the packet buffer. */
        gdbStubCtxPktBufReset(pThis);
        *pcbProcessed = cbData;
    }

    return rc;
}


/**
 * Searches for the end character in the current data buffer.
 *
 * @returns Status code.
 * @param   pThis               The GDB stub context.
 * @param   cbData              Number of new bytes in the packet buffer.
 * @param   pcbProcessed        Where to store the amount of bytes processed.
 */
static int gdbStubCtxPktBufSearchEnd(PGDBSTUBCTXINT pThis, size_t cbData, size_t *pcbProcessed)
{
    uint8_t *pbEnd = gdbStubCtxMemchr(&pThis->pbPktBuf[pThis->offPktBuf], GDBSTUB_PKT_END, cbData);
    if (pbEnd)
    {
#if 0 /* This should not be necessary as the escaped character is xored with 0x20 so the search should not trigger. */
        /*
         * Check whether this is an escaped character not denoting the end of a packet.
         * The pointer arithmetic is safe because we always store the start character of a packet
         * in the buffer as well so the worst case with the smallest packet $#hh is properly detected.
         * Just sending # before the start character will be ignored in gdbStubCtxPktBufSearchStart()
         * and we don't end up here at all.
         */
        if (*(pbEnd - 1) != GDBSTUB_PKT_ESCAPE)
#endif
        {
            /* Found the end character, next comes the checksum. */
            pThis->enmState = GDBSTUBRECVSTATE_PACKET_RECEIVE_CHECKSUM;
        }
        *pcbProcessed     = (uintptr_t)(pbEnd - &pThis->pbPktBuf[pThis->offPktBuf]) + 1;
        pThis->offPktBuf += *pcbProcessed;
        pThis->cbPkt      = pThis->offPktBuf - 1; /* Don't account for the start and end character. */
    }
    else
    {
        /* Not found, still in the middle of a packet. */
        /** @todo Look for out of band characters. */
        *pcbProcessed    = cbData;
        pThis->offPktBuf += cbData;
    }

    return GDBSTUB_INF_SUCCESS;
}


/**
 * Processes the checksum.
 *
 * @returns Status code.
 * @param   pThis               The GDB stub context.
 * @param   cbData              Number of new bytes in the packet buffer.
 * @param   pcbProcessed        Where to store the amount of bytes processed.
 */
static int gdbStubCtxPktBufProcessChksum(PGDBSTUBCTXINT pThis, size_t cbData, size_t *pcbProcessed)
{
    int rc = GDBSTUB_INF_SUCCESS;
    size_t cbChksumProcessed = (cbData < pThis->cbChksumRecvLeft) ? cbData : pThis->cbChksumRecvLeft;

    pThis->cbChksumRecvLeft -= cbChksumProcessed;
    if (!pThis->cbChksumRecvLeft)
    {
        /* Verify checksum of the whole packet. */
        uint8_t uChkSum =   gdbStubCtxChrToHex(pThis->pbPktBuf[pThis->offPktBuf]) << 4
                          | gdbStubCtxChrToHex(pThis->pbPktBuf[pThis->offPktBuf + 1]);

        uint8_t uSum = 0;
        for (uint32_t i = 1; i < pThis->cbPkt; i++)
            uSum += pThis->pbPktBuf[i];

        if (uSum == uChkSum)
        {
            /* Checksum matches, send acknowledge and continue processing the complete payload. */
            char chAck = '+';
            rc = gdbStubCtxIoIfWrite(pThis, &chAck, sizeof(chAck));
            if (rc == GDBSTUB_INF_SUCCESS)
                rc = gdbStubCtxPktProcess(pThis);
        }
        else
        {
            /* Send NACK and reset for the next packet. */
            char chAck = '-';
            rc = gdbStubCtxIoIfWrite(pThis, &chAck, sizeof(chAck));
        }

        gdbStubCtxReset(pThis);
    }

    *pcbProcessed += cbChksumProcessed;
    return rc;
}


/**
 * Process read data in the packet buffer based on the current state.
 *
 * @returns Status code.
 * @param   pThis               The GDB stub context.
 * @param   cbData              Number of new bytes in the packet buffer.
 */
static int gdbStubCtxPktBufProcess(PGDBSTUBCTXINT pThis, size_t cbData)
{
    int rc = GDBSTUB_INF_SUCCESS;

    while (   cbData
           && rc == GDBSTUB_INF_SUCCESS)
    {
        size_t cbProcessed = 0;

        switch (pThis->enmState)
        {
            case GDBSTUBRECVSTATE_PACKET_WAIT_FOR_START:
            {
                rc = gdbStubCtxPktBufSearchStart(pThis, cbData, &cbProcessed);
                break;
            }
            case GDBSTUBRECVSTATE_PACKET_RECEIVE_BODY:
            {
                rc = gdbStubCtxPktBufSearchEnd(pThis, cbData, &cbProcessed);
                break;
            }
            case GDBSTUBRECVSTATE_PACKET_RECEIVE_CHECKSUM:
            {
                rc = gdbStubCtxPktBufProcessChksum(pThis, cbData, &cbProcessed);
                break;
            }
            default:
                /* Should never happen. */
                rc = GDBSTUB_ERR_INTERNAL_ERROR;
        }

        cbData -= cbProcessed;
    }

    return rc;
}


/**
 * The main receive loop.
 *
 * @returns Status code.
 * @param   pThis               The GDB stub context.
 */
static int gdbStubCtxRecv(PGDBSTUBCTXINT pThis)
{
    int rc = GDBSTUB_INF_SUCCESS;

    GDBSTUBTGTSTATE enmTgtState = gdbStubCtxIfTgtGetState(pThis);
    if (   enmTgtState == GDBSTUBTGTSTATE_STOPPED
        && pThis->enmTgtStateLast != GDBSTUBTGTSTATE_STOPPED)
        rc = gdbStubCtxReplySendSigTrap(pThis);

    pThis->enmTgtStateLast = enmTgtState;

    while (rc == GDBSTUB_INF_SUCCESS)
    {
        size_t cbRead = gdbStubCtxIoIfPeek(pThis);

        if (cbRead)
        {
            rc = gdbStubCtxEnsurePktBufSpace(pThis, cbRead);
            if (rc == GDBSTUB_INF_SUCCESS)
            {
                size_t cbThisRead = 0;
                rc = gdbStubCtxIoIfRead(pThis, &pThis->pbPktBuf[pThis->offPktBuf], cbRead, &cbThisRead);
                if (rc == GDBSTUB_INF_SUCCESS)
                    rc = gdbStubCtxPktBufProcess(pThis, cbThisRead);
            }
        }
        else
        {
            /* Block when poll is available. */
            if (pThis->pIoIf->pfnPoll)
                rc = gdbStubCtxIoIfPoll(pThis);
            else
                rc = GDBSTUB_INF_TRY_AGAIN;
        }
    }

    return rc;
}


int GDBStubCtxCreate(PGDBSTUBCTX phCtx, PCGDBSTUBIOIF pIoIf, PCGDBSTUBIF pIf, void *pvUser)
{
    if (!phCtx || !pIoIf || !pIf)
        return GDBSTUB_ERR_INVALID_PARAMETER;

    int rc = GDBSTUB_INF_SUCCESS;
    PGDBSTUBCTXINT pThis = (PGDBSTUBCTXINT)pIf->pfnMemAlloc(NULL, pvUser, sizeof(*pThis));
    if (pThis)
    {
        pThis->pIoIf           = pIoIf;
        pThis->pIf             = pIf;
        pThis->pvUser          = pvUser;
        pThis->cbPktBufMax     = 0;
        pThis->pbPktBuf        = NULL;
        pThis->enmTgtStateLast = GDBSTUBTGTSTATE_INVALID;
        pThis->fFeatures       = GDBSTUBCTX_FEATURES_F_TGT_DESC;
        pThis->pbTgtXmlDesc    = NULL;
        pThis->cbTgtXmlDesc    = 0;
        pThis->fExtendedMode   = false;
        gdbStubOutCtxInit(&pThis->OutCtx);

        uint32_t cRegs = 0;
        size_t cbRegs = 0;
        while (pIf->paRegs[cRegs].pszName != NULL)
        {
            cbRegs += pIf->paRegs[cRegs].cRegBits / 8;
            cRegs++;
        }

        pThis->cRegs = cRegs;
        pThis->cbRegs = cbRegs;

        /* Allocate scratch space for register content and index array. */
        void *pvRegsScratch = gdbStubCtxIfMemAlloc(pThis, cRegs * cbRegs + cRegs * sizeof(uint32_t));
        if (pvRegsScratch)
        {
            pThis->pvRegsScratch = pvRegsScratch;
            pThis->paidxRegs     = (uint32_t *)((uint8_t *)pvRegsScratch + (cRegs * cbRegs));

            /* GDB always sets or queries all registers so we can statically initialize the index array. */
            for (uint32_t i = 0; i < pThis->cRegs; i++)
                pThis->paidxRegs[i] = i;

            gdbStubCtxReset(pThis);
            *phCtx = pThis;
            return GDBSTUB_INF_SUCCESS;
        }
        else
            rc = GDBSTUB_ERR_NO_MEMORY;

        pIf->pfnMemFree(NULL, pvUser, pThis);
    }
    else
        rc = GDBSTUB_ERR_NO_MEMORY;

    return rc;
}

void GDBStubCtxDestroy(GDBSTUBCTX hCtx)
{
    PGDBSTUBCTXINT pThis = hCtx;
    PCGDBSTUBIF pIf = pThis->pIf;
    void *pvUser = pThis->pvUser;

    if (pThis->pbPktBuf)
        pIf->pfnMemFree(pThis, pvUser, pThis->pbPktBuf);
    pIf->pfnMemFree(NULL, pvUser, pThis);
}

int GDBStubCtxRun(GDBSTUBCTX hCtx)
{
    PGDBSTUBCTXINT pThis = hCtx;

    if (!pThis)
        return GDBSTUB_ERR_INVALID_PARAMETER;

    return gdbStubCtxRecv(pThis);
}

int GDBStubCtxReset(GDBSTUBCTX hCtx)
{
    PGDBSTUBCTXINT pThis = hCtx;

    if (!pThis)
        return GDBSTUB_ERR_INVALID_PARAMETER;

    gdbStubCtxReset(pThis);
    return GDBSTUB_INF_SUCCESS;
}
