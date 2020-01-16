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
} GDBSTUBCTXINT;
/** Pointer to an internal PSP proxy context. */
typedef GDBSTUBCTXINT *PGDBSTUBCTXINT;



int GDBStubCtxCreate(PGDBSTUBCTX phCtx, PCGDBSTUBIOIF pIoIf, PCGDBSTUBIF pIf, void *pvUser)
{
    if (!phCtx || !pIoIf || !pIf)
        return GDBSTUB_ERR_INVALID_PARAMETER;

    int rc = GDBSTUB_INF_SUCCESS;
    PGDBSTUBCTXINT pThis = (PGDBSTUBCTXINT)pIf->pfnMemAlloc(NULL, pvUser, sizeof(*pThis));
    if (pThis)
    {
        pThis->pIoIf  = pIoIf;
        pThis->pIf    = pIf;
        pThis->pvUser = pvUser;
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

    return GDBSTUB_INF_TRY_AGAIN;
}
