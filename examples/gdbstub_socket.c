/** @file
 * GDB stub examples - network socket.
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
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

#include <poll.h>
#include <sys/ioctl.h>

#include <libgdbstub.h>

/**
 * The stub instance.
 */
typedef struct GDBSOCKSTUB
{
    /** The socket for this stub. */
    int                     iFdSock;
    /** The GDB stub context. */
    GDBSTUBCTX              hGdbStubCtx;
} GDBSOCKSTUB;
/** Pointer to the GDB socket stub instance. */
typedef GDBSOCKSTUB *PGDBSOCKSTUB;
/** Pointer to a const GDB socket stub instance. */
typedef const GDBSOCKSTUB *PCGDBSOCKSTUB;


/**
 * GDB stub register names (for ARM).
 */
static const GDBSTUBREG g_aGdbStubRegs[] =
{
    { "r0",   32, GDBSTUBREGTYPE_GP        },
    { "r1",   32, GDBSTUBREGTYPE_GP        },
    { "r2",   32, GDBSTUBREGTYPE_GP        },
    { "r3",   32, GDBSTUBREGTYPE_GP        },
    { "r4",   32, GDBSTUBREGTYPE_GP        },
    { "r5",   32, GDBSTUBREGTYPE_GP        },
    { "r6",   32, GDBSTUBREGTYPE_GP        },
    { "r7",   32, GDBSTUBREGTYPE_GP        },
    { "r8",   32, GDBSTUBREGTYPE_GP        },
    { "r9",   32, GDBSTUBREGTYPE_GP        },
    { "r10",  32, GDBSTUBREGTYPE_GP        },
    { "r11",  32, GDBSTUBREGTYPE_GP        },
    { "r12",  32, GDBSTUBREGTYPE_GP        },
    { "sp",   32, GDBSTUBREGTYPE_STACK_PTR },
    { "lr",   32, GDBSTUBREGTYPE_CODE_PTR  },
    { "pc",   32, GDBSTUBREGTYPE_PC        },
    { "cpsr", 32, GDBSTUBREGTYPE_STATUS    },
    { NULL,    0, GDBSTUBREGTYPE_INVALID   }
};


/**
 * @copydoc{GDBSTUBCMD,pfnCmd}
 */
static int gdbStubCmdHelp(GDBSTUBCTX hGdbStubCtx, PCGDBSTUBOUTHLP pHlp, const char *pszArgs, void *pvUser)
{
    pHlp->pfnPrintf(pHlp, "Test: %s %p %#x\n", "help", pHlp, 0xdeadbeef);
    return GDBSTUB_INF_SUCCESS;
}


/**
 * Custom commands descriptors.
 */
static const GDBSTUBCMD g_aGdbCmds[] =
{
    { "help", "Print help about supported commands", gdbStubCmdHelp },
    { NULL,   NULL,                                  NULL           }
};


/**
 * @copydoc{GDBSTUBIF,pfnMemAlloc}
 */
static void *gdbStubIfMemAlloc(GDBSTUBCTX hGdbStubCtx, void *pvUser, size_t cb)
{
    (void)hGdbStubCtx;
    (void)pvUser;

    return calloc(1, cb);
}


/**
 * @copydoc{GDBSTUBIF,pfnMemFree}
 */
static void gdbStubIfMemFree(GDBSTUBCTX hGdbStubCtx, void *pvUser, void *pv)
{
    (void)hGdbStubCtx;
    (void)pvUser;

    free(pv);
}


/**
 * @copydoc{GDBSTUBIF,pfnTgtGetState}
 */
static GDBSTUBTGTSTATE gdbStubIfTgtGetState(GDBSTUBCTX hGdbStubCtx, void *pvUser)
{
    return GDBSTUBTGTSTATE_STOPPED;
}


/**
 * @copydoc{GDBSTUBIF,pfnTgtStop}
 */
static int gdbStubIfTgtStop(GDBSTUBCTX hGdbStubCtx, void *pvUser)
{
    printf("gdbStubIfTgtStop: hGdbStubCtx=%p pvUser=%p\n", hGdbStubCtx, pvUser);
    return GDBSTUB_INF_SUCCESS;
}


/**
 * @copydoc{GDBSTUBIF,pfnTgtRestart}
 */
static int gdbStubIfTgtRestart(GDBSTUBCTX hGdbStubCtx, void *pvUser)
{
    printf("gdbStubIfTgtRestart: hGdbStubCtx=%p pvUser=%p\n", hGdbStubCtx, pvUser);
    return GDBSTUB_INF_SUCCESS;
}


/**
 * @copydoc{GDBSTUBIF,pfnTgtKill}
 */
static int gdbStubIfTgtKill(GDBSTUBCTX hGdbStubCtx, void *pvUser)
{
    printf("gdbStubIfTgtKill: hGdbStubCtx=%p pvUser=%p\n", hGdbStubCtx, pvUser);
    return GDBSTUB_INF_SUCCESS;
}


/**
 * @copydoc{GDBSTUBIF,pfnTgtStep}
 */
static int gdbStubIfTgtStep(GDBSTUBCTX hGdbStubCtx, void *pvUser)
{
    printf("gdbStubIfTgtStep: hGdbStubCtx=%p pvUser=%p\n", hGdbStubCtx, pvUser);
    return GDBSTUB_INF_SUCCESS;
}


/**
 * @copydoc{GDBSTUBIF,pfnTgtCont}
 */
static int gdbStubIfTgtCont(GDBSTUBCTX hGdbStubCtx, void *pvUser)
{
    printf("gdbStubIfTgtCont: hGdbStubCtx=%p pvUser=%p\n", hGdbStubCtx, pvUser);
    return GDBSTUB_INF_SUCCESS;
}


/**
 * @copydoc{GDBSTUBIF,pfnTgtMemRead}
 */
static int gdbStubIfTgtMemRead(GDBSTUBCTX hGdbStubCtx, void *pvUser, GDBTGTMEMADDR GdbTgtMemAddr, void *pvDst, size_t cbRead)
{
    memset(pvDst, 0, cbRead);
    return GDBSTUB_INF_SUCCESS;
}


/**
 * @copydoc{GDBSTUBIF,pfnTgtMemWrite}
 */
static int gdbStubIfTgtMemWrite(GDBSTUBCTX hGdbStubCtx, void *pvUser, GDBTGTMEMADDR GdbTgtMemAddr, const void *pvSrc, size_t cbWrite)
{
    printf("gdbStubIfTgtMemWrite: hGdbStubCtx=%p pvUser=%p GdbTgtMemAddr=%#llx pvSrc=%p cbWrite=%zu\n",
           hGdbStubCtx, pvUser, GdbTgtMemAddr, pvSrc, cbWrite);
    return GDBSTUB_INF_SUCCESS;
}


/**
 * @copydoc{GDBSTUBIF,pfnTgtRegsRead}
 */
static int gdbStubIfTgtRegsRead(GDBSTUBCTX hGdbStubCtx, void *pvUser, uint32_t *paRegs, uint32_t cRegs, void *pvDst)
{
    uint32_t *pau32Regs = (uint32_t *)pvDst;

    for (uint32_t i = 0; i < cRegs; i++)
            *pau32Regs++ = paRegs[i];

    return GDBSTUB_INF_SUCCESS;
}


/**
 * @copydoc{GDBSTUBIF,pfnTgtRegsWrite}
 */
static int gdbStubIfTgtRegsWrite(GDBSTUBCTX hGdbStubCtx, void *pvUser, uint32_t *paRegs, uint32_t cRegs, const void *pvSrc)
{
    printf("gdbStubIfTgtRegsWrite: hGdbStubCtx=%p pvUser=%p paRegs=%p cRegs=%u pvSrc=%p\n",
           hGdbStubCtx, pvUser, paRegs, cRegs, pvSrc);
    return GDBSTUB_INF_SUCCESS;
}


/**
 * @copydoc{GDBSTUBIF,pfnTgtTpSet}
 */
static int gdbStubIfTgtTpSet(GDBSTUBCTX hGdbStubCtx, void *pvUser, GDBTGTMEMADDR GdbTgtTpAddr, GDBSTUBTPTYPE enmTpType, GDBSTUBTPACTION enmTpAction)
{
    printf("gdbStubIfTgtTpSet: hGdbStubCtx=%p pvUser=%p GdbTgtTpAddr=%#llx enmTpType=%u enmTpAction=%u\n",
           hGdbStubCtx, pvUser, GdbTgtTpAddr, enmTpType, enmTpAction);
    return GDBSTUB_INF_SUCCESS;
}


/**
 * @copydoc{GDBSTUBIF,pfnTgtTpClear}
 */
static int gdbStubIfTgtTpClear(GDBSTUBCTX hGdbStubCtx, void *pvUser, GDBTGTMEMADDR GdbTgtTpAddr)
{
    printf("gdbStubIfTgtTpClear: hGdbStubCtx=%p pvUser=%p GdbTgtTpAddr=%#llx\n",
           hGdbStubCtx, pvUser, GdbTgtTpAddr);
    return GDBSTUB_INF_SUCCESS;
}


/**
 * GDB stub interface callback table.
 */
const GDBSTUBIF g_GdbStubIf =
{
    /** enmArch */
    GDBSTUBTGTARCH_ARM,
    /** paRegs */
    &g_aGdbStubRegs[0],
    /** paCmds */
    &g_aGdbCmds[0],
    /** pfnMemAlloc */
    gdbStubIfMemAlloc,
    /** pfnMemFree */
    gdbStubIfMemFree,
    /** pfnTgtGetState */
    gdbStubIfTgtGetState,
    /** pfnTgtStop */
    gdbStubIfTgtStop,
    /** pfnTgtRestart */
    gdbStubIfTgtRestart,
    /** pfnTgtKill */
    gdbStubIfTgtKill,
    /** pfnTgtStep */
    gdbStubIfTgtStep,
    /** pfnTgtCont */
    gdbStubIfTgtCont,
    /** pfnTgtMemRead */
    gdbStubIfTgtMemRead,
    /** pfnTgtMemWrite */
    gdbStubIfTgtMemWrite,
    /** pfnTgtRegsRead */
    gdbStubIfTgtRegsRead,
    /** pfnTgtRegsWrite */
    gdbStubIfTgtRegsWrite,
    /** pfnTgtTpSet */
    gdbStubIfTgtTpSet,
    /** pfnTgtTpClear */
    gdbStubIfTgtTpClear
};


static size_t gdbStubIoIfPeek(GDBSTUBCTX hGdbStubCtx, void *pvUser)
{
    (void)hGdbStubCtx;

    PCGDBSOCKSTUB pStub = (PCGDBSOCKSTUB)pvUser;
    int cbAvail = 0;
    int rc = ioctl(pStub->iFdSock, FIONREAD, &cbAvail);
    if (rc)
        return 0;

    return cbAvail;
}


static int gdbStubIoIfRead(GDBSTUBCTX hGdbStubCtx, void *pvUser, void *pvDst, size_t cbRead, size_t *pcbRead)
{
    (void)hGdbStubCtx;

    PCGDBSOCKSTUB pStub = (PCGDBSOCKSTUB)pvUser;
    ssize_t cbRet = recv(pStub->iFdSock, pvDst, cbRead, MSG_DONTWAIT);
    if (cbRet > 0)
    {
        *pcbRead = cbRead;
        return GDBSTUB_INF_SUCCESS;
    }

    if (!cbRet)
        return GDBSTUB_ERR_PEER_DISCONNECTED;

    if (errno == EAGAIN || errno == EWOULDBLOCK)
        return GDBSTUB_INF_TRY_AGAIN;

    return GDBSTUB_ERR_INTERNAL_ERROR; /** @todo Better status codes for the individual errors. */
}


static int gdbStubIoIfWrite(GDBSTUBCTX hGdbStubCtx, void *pvUser, const void *pvPkt, size_t cbPkt)
{
    (void)hGdbStubCtx;

    PCGDBSOCKSTUB pStub = (PCGDBSOCKSTUB)pvUser;
    ssize_t cbRet = send(pStub->iFdSock, pvPkt, cbPkt, 0);
    if (cbRet == cbPkt)
        return GDBSTUB_INF_SUCCESS;

    return GDBSTUB_ERR_INTERNAL_ERROR; /** @todo Better status codes for the individual errors. */
}


static int gdbStubIoIfPoll(GDBSTUBCTX hGdbStubCtx, void *pvUser)
{
    (void)hGdbStubCtx;
    PCGDBSOCKSTUB pStub = (PCGDBSOCKSTUB)pvUser;
    struct pollfd PollFd;

    PollFd.fd      = pStub->iFdSock;
    PollFd.events  = POLLIN | POLLHUP | POLLERR;
    PollFd.revents = 0;

    int rc = GDBSTUB_INF_SUCCESS;
    for (;;)
    {
        int rcPsx = poll(&PollFd, 1, INT32_MAX);
        if (rcPsx == 1)
            break; /* Stop polling if the single descriptor has events. */
        if (rcPsx == -1)
            rc = GDBSTUB_ERR_INTERNAL_ERROR; /** @todo Better status codes for the individual errors. */
    }

    return rc;
}


/**
 * GDB stub I/O interface callback table.
 */
const GDBSTUBIOIF g_GdbStubIoIf =
{
    /** pfnPeek */
    gdbStubIoIfPeek,
    /** pfnRead */
    gdbStubIoIfRead,
    /** pfnWrite */
    gdbStubIoIfWrite,
    /** pfnPoll */
    gdbStubIoIfPoll
};


int main(int argc, char *argv[])
{
    if (argc != 2)
    {
        printf("Usage: %s <port>\n", argv[0]);
        return 1;
    }

    uint32_t uPort = strtoul(argv[1], NULL, 10);
    int iFdListen = 0;
    struct sockaddr_in SockAddr;

    /** @todo Error handling. */
    iFdListen = socket(AF_INET, SOCK_STREAM, 0);
    memset(&SockAddr, 0, sizeof(SockAddr));

    SockAddr.sin_family      = AF_INET;
    SockAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    SockAddr.sin_port        = htons(uPort);
    bind(iFdListen, (struct sockaddr *)&SockAddr, sizeof(SockAddr));

    for (;;)
    {
        listen(iFdListen, 5);

        int iFdSock = accept(iFdListen, (struct sockaddr *)NULL, NULL);
        printf("Got new connection\n");

        GDBSTUBCTX hGdbStubCtx = NULL;
        GDBSOCKSTUB GdbStub;

        GdbStub.iFdSock = iFdSock;
        int rc = GDBStubCtxCreate(&hGdbStubCtx, &g_GdbStubIoIf, &g_GdbStubIf, &GdbStub);
        if (rc == GDBSTUB_INF_SUCCESS)
        {
            rc = GDBStubCtxRun(hGdbStubCtx);
            printf("GDB stub context runloop exited with %d\n", rc);
            GDBStubCtxDestroy(hGdbStubCtx);
        }
        else
            printf("Creating the GDB stub context failed with: %d\n", rc);

        close(iFdSock);
    }
    return 0;
}
