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
    /** Register scratch space (for reading writing registers). */
    void                        *pvRegsScratch;
    /** Register index array for querying setting. */
    uint32_t                    *paidxRegs;
} GDBSTUBCTXINT;
/** Pointer to an internal PSP proxy context. */
typedef GDBSTUBCTXINT *PGDBSTUBCTXINT;


/**
 * Specific query packet processor callback.
 *
 * @returns Status code.
 * @param   pThis               The GDB stub context.
 * @param   pbArgs              Pointer to the arguments.
 * @param   cbArgs              Size of the arguments in bytes.
 */
typedef int (FNGDBTUBQPKTPROC) (PGDBSTUBCTXINT pThis, const uint8_t *pbArgs, size_t cbArgs);
typedef FNGDBTUBQPKTPROC *PFNGDBTUBQPKTPROC;


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
    PFNGDBTUBQPKTPROC           pfnProc;
} GDBSTUBQPKTPROC;
/** Pointer to a 'q' packet processor entry. */
typedef GDBSTUBQPKTPROC *PGDBSTUBQPKTPROC;
/** Pointer to a const 'q' packet processor entry. */
typedef const GDBSTUBQPKTPROC *PCGDBSTUBQPKTPROC;


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
 * Wrapper for the interface target read registers callback.
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
        uVal = uVal * 16 + gdbStubCtxChrToHex(*pbBuf++);

    *puVal = uVal;

    if (ppbSep)
        *ppbSep = pbBuf;

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


#if 0 /* Later */
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
}
#endif


/**
 * List of supported query packets.
 */
static const GDBSTUBQPKTPROC g_aQPktProcs[] =
{
#define GDBSTUBQPKTPROC_INIT(a_Name, a_pfnProc) { a_Name, sizeof(a_Name) - 1, a_pfnProc }
    GDBSTUBQPKTPROC_INIT("TStatus",   gdbStubCtxPktProcessQueryTStatus),
    /*GDBSTUBQPKTPROC_INIT("Supported", gdbStubCtxPktProcessQuerySupported),*/
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
                    size_t cbReplyPkt = pThis->cRegs * pThis->pIf->cbReg * 2; /* One byte needs two characters. */

                    /* Encode data and send. */
                    rc = gdbStubCtxEnsurePktBufSpace(pThis, cbReplyPkt);
                    if (rc == GDBSTUB_INF_SUCCESS)
                    {
                        rc = gdbStubCtxEncodeBinaryAsHex(pThis->pbPktBuf, pThis->cbPktBufMax, pThis->pvRegsScratch, pThis->cRegs * pThis->pIf->cbReg);
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
            case 'p': /* Read a single register */
            {
                uint64_t uReg = 0;
                int rc = gdbStubCtxParseHexStringAsInteger(&pThis->pbPktBuf[2], pThis->cbPkt - 1, &uReg,
                                                           GDBSTUB_PKT_END, NULL);
                if (rc == GDBSTUB_INF_SUCCESS)
                {
                    uint32_t idxReg = (uint32_t)uReg;

                    rc = gdbStubCtxIfTgtRegsRead(pThis, &idxReg, 1, pThis->pvRegsScratch);
                    if (rc == GDBSTUB_INF_SUCCESS)
                    {
                        size_t cbReplyPkt = pThis->pIf->cbReg * 2; /* One byte needs two characters. */

                        /* Encode data and send. */
                        rc = gdbStubCtxEnsurePktBufSpace(pThis, cbReplyPkt);
                        if (rc == GDBSTUB_INF_SUCCESS)
                        {
                            rc = gdbStubCtxEncodeBinaryAsHex(pThis->pbPktBuf, pThis->cbPktBufMax, pThis->pvRegsScratch, pThis->pIf->cbReg);
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

        uint32_t cRegs = 0;
        while (pIf->papszRegs[cRegs] != NULL)
            cRegs++;

        pThis->cRegs = cRegs;

        /* Allocate scratch space for register content and index array. */
        void *pvRegsScratch = gdbStubCtxIfMemAlloc(pThis, cRegs * pIf->cbReg + cRegs * sizeof(uint32_t));
        if (pvRegsScratch)
        {
            pThis->pvRegsScratch = pvRegsScratch;
            pThis->paidxRegs     = (uint32_t *)((uint8_t *)pvRegsScratch + (cRegs * pIf->cbReg));

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
