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
} GDBSTUBCTXINT;
/** Pointer to an internal PSP proxy context. */
typedef GDBSTUBCTXINT *PGDBSTUBCTXINT;


/**
 * Wrapper for allocating memory using the callback interface.
 *
 * @returns Pointer to the allocated memory on success or NULL if out of memory.
 * @param   pThis               The GDB stub context.
 * @param   cbAlloc             Number of bytes to allocate.
 */
inline void *gdbStubCtxIfMemAlloc(PGDBSTUBCTXINT pThis, size_t cbAlloc)
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
inline void gdbStubCtxIfMemFree(PGDBSTUBCTXINT pThis, void *pv)
{
    return pThis->pIf->pfnMemFree(pThis, pThis->pvUser, pv);
}


/**
 * Wrapper for the I/O interface peek callback.
 *
 * @returns Number of bytes available for reading.
 * @param   pThis               The GDB stub context.
 */
inline size_t gdbStubCtxIoIfPeek(PGDBSTUBCTXINT pThis)
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
inline int gdbStubCtxIoIfRead(PGDBSTUBCTXINT pThis, void *pvDst, size_t cbRead, size_t *pcbRead)
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
inline int gdbStubCtxIoIfWrite(PGDBSTUBCTXINT pThis, const void *pvPkt, size_t cbPkt)
{
    return pThis->pIoIf->pfnWrite(pThis, pThis->pvUser, pvPkt, cbPkt);
}


/**
 * Wrapper for the I/O interface poll callback.
 *
 * @returns Status code.
 * @param   pThis               The GDB stub context.
 */
inline size_t gdbStubCtxIoIfPoll(PGDBSTUBCTXINT pThis)
{
    return pThis->pIoIf->pfnPeek(pThis, pThis->pvUser);
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
inline uint8_t gdbStubCtxChrToHex(char ch)
{
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >= 'A' && ch <= 'F')
        return ch - 'A';
    if (ch >= 'a' && ch <= 'f')
        return ch - 'a';

    return 0xff;
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
 * Processes a completely received packet.
 *
 * @returns Status code.
 * @param   pThis               The GDB stub context.
 */
static int gdbStubCtxPktProcess(PGDBSTUBCTXINT pThis)
{
    /** @todo Continue here next */
    return GDBSTUB_ERR_INTERNAL_ERROR;
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
 * Searches for the start character in the current data buffer.
 *
 * @returns Status code.
 * @param   pThis               The GDB stub context.
 * @param   cbData              Number of new bytes in the packet buffer.
 * @param   pcbProcessed        Where to store the amount of bytes processed.
 */
static int gdbStubCtxPktBufSearchStart(PGDBSTUBCTXINT pThis, size_t cbData, size_t *pcbProcessed)
{
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
        /* Not found, ignore the received data and reset the packet buffer. */
        /** @todo Look for out of band characters. */
        gdbStubCtxPktBufReset(pThis);
        *pcbProcessed = cbData;
    }

    return GDBSTUB_INF_SUCCESS;
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
        *pcbProcessed     = (uintptr_t)(pbEnd - &pThis->pbPktBuf[pThis->offPktBuf]);
        pThis->offPktBuf += *pcbProcessed;
        pThis->cbPkt      = pThis->offPktBuf - 1 - 1; /* Don't account for the start and end character. */
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
        uint8_t uChkSum =   gdbStubCtxChrToHex(pThis->pbPktBuf[pThis->offPktBuf - 2]) << 4
                          | gdbStubCtxChrToHex(pThis->pbPktBuf[pThis->offPktBuf - 1]);

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
        pThis->pIoIf       = pIoIf;
        pThis->pIf         = pIf;
        pThis->pvUser      = pvUser;
        pThis->cbPktBufMax = 0;
        pThis->pbPktBuf    = NULL;
        gdbStubCtxReset(pThis);
        *phCtx = pThis;
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

    /** @todo Destroy all allocated resources. */

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
