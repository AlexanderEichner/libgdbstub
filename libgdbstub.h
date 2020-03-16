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
#ifndef __libgdbstub_h
#define __libgdbstub_h

#include <stdint.h>
#include <stddef.h>


/** Target address space address. */
typedef uint64_t GDBTGTMEMADDR;

/** Opaque GDB stub context handle. */
typedef struct GDBSTUBCTXINT *GDBSTUBCTX;
/** Pointer to a GDB stub context handle. */
typedef GDBSTUBCTX *PGDBSTUBCTX;

/** Informational status code - success. */
#define GDBSTUB_INF_SUCCESS            (0)
/** Error code - invalid parameter. */
#define GDBSTUB_ERR_INVALID_PARAMETER  (-1)
/** Error code - out of memory. */
#define GDBSTUB_ERR_NO_MEMORY          (-2)
/** Informational status code - no data available */
#define GDBSTUB_INF_TRY_AGAIN          (3)
/** Error code - internal error (bug in the library). */
#define GDBSTUB_ERR_INTERNAL_ERROR     (-4)
/** Error code - the GDB peer disconnected. */
#define GDBSTUB_ERR_PEER_DISCONNECTED  (-5)
/** Error code - the action is not supported. */
#define GDBSTUB_ERR_NOT_SUPPORTED      (-6)
/** Error code - protocol error. */
#define GDBSTUB_ERR_PROTOCOL_VIOLATION (-7)
/** Error code - buffer would overflow. */
#define GDBSTUB_ERR_BUFFER_OVERFLOW    (-8)
/** Error code - not found. */
#define GDBSTUB_ERR_NOT_FOUND          (-9)


/**
 * The stub target architecture.
 */
typedef enum GDBSTUBTGTARCH
{
    /** Invalid architecture, do not use. */
    GDBSTUBTGTARCH_INVALID = 0,
    /** ARM architecture. */
    GDBSTUBTGTARCH_ARM,
    /** x86 architecture. */
    GDBSTUBTGTARCH_X86,
    /** x86-64 (amd64) architecture. */
    GDBSTUBTGTARCH_AMD64,
    /** 32bit hack. */
    GDBSTUBTGTARCH_32BIT_HACK = 0x7fffffff
} GDBSTUBTGTARCH;


/**
 * The current target state.
 */
typedef enum GDBSTUBTGTSTATE
{
    /** Invalid state, do not use. */
    GDBSTUBTGTSTATE_INVALID = 0,
    /** Target is currently running. */
    GDBSTUBTGTSTATE_RUNNING,
    /** Target is stopped. */
    GDBSTUBTGTSTATE_STOPPED,
    /** 32bit hack. */
    GDBSTUBTGTSTATE_32BIT_HACK = 0x7fffffff
} GDBSTUBTGTSTATE;


/**
 * Trace point type.
 */
typedef enum GDBSTUBTPTYPE
{
    /** Invalid type, do not use. */
    GDBSTUBTPTYPE_INVALID = 0,
    /** An instruction software trace point. */
    GDBSTUBTPTYPE_EXEC_SW,
    /** An instruction hardware trace point. */
    GDBSTUBTPTYPE_EXEC_HW,
    /** A memory read trace point. */
    GDBSTUBTPTYPE_MEM_READ,
    /** A memory write trace point. */
    GDBSTUBTPTYPE_MEM_WRITE,
    /** A memory access trace point. */
    GDBSTUBTPTYPE_MEM_ACCESS,
    /** 32bit hack. */
    GDBSTUBTPTYPE_32BIT_HACK = 0x7fffffff
} GDBSTUBTPTYPE;


/**
 * Trace point action.
 */
typedef enum GDBSTUBTPACTION
{
    /** Invalid action, do not use. */
    GDBSTUBTPACTION_INVALID = 0,
    /** Stops execution of the target and returns control to the debugger. */
    GDBSTUBTPACTION_STOP,
    /** @todo Dump data, execute commands, etc. */
    GDBSTUBTPACTION_32BIT_HACK = 0x7fffffff
} GDBSTUBTPACTION;


/**
 * Register type.
 */
typedef enum GDBSTUBREGTYPE
{
    /** Invalid type, do not use. */
    GDBSTUBREGTYPE_INVALID = 0,
    /** General purpose register. */
    GDBSTUBREGTYPE_GP,
    /** Program counter. */
    GDBSTUBREGTYPE_PC,
    /** Stack pointer. */
    GDBSTUBREGTYPE_STACK_PTR,
    /** Code pointer. */
    GDBSTUBREGTYPE_CODE_PTR,
    /** Status register. */
    GDBSTUBREGTYPE_STATUS,
    /** 32bit hack. */
    GDBSTUBREGTYPE_32BIT_HACK = 0x7fffffff
} GDBSTUBREGTYPE;


/**
 * A register entry.
 */
typedef struct GDBSTUBREG
{
    /** Register name. */
    const char                  *pszName;
    /** Register bitsize. */
    uint32_t                    cRegBits;
    /** Register type. */
    GDBSTUBREGTYPE              enmType;
} GDBSTUBREG;
/** Pointer to a register entry. */
typedef GDBSTUBREG *PGDBSTUBREG;
/** Pointer to a const register entry. */
typedef const GDBSTUBREG *PCGDBSTUBREG;


/** Forward decleration of a const output helper structure. */
typedef const struct GDBSTUBOUTHLP *PCGDBSTUBOUTHLP;

/**
 * Output helper callback table.
 */
typedef struct GDBSTUBOUTHLP
{

    /**
     * Print formatted string.
     *
     * @returns Status code.
     * @param   pHlp                Pointer to this structure.
     * @param   pszFmt              The format string.
     * @param   ...                 Variable number of arguments depending on the foramt string.
     */
    int (*pfnPrintf) (PCGDBSTUBOUTHLP pHlp, const char *pszFmt, ...);

} GDBSTUBCMDOUTHLP;


/**
 * Custom command descriptor.
 */
typedef struct GDBSTUBCMD
{
    /** Command name. */
    const char                  *pszCmd;
    /** Command description, optional. */
    const char                  *pszDesc;

    /**
     * Command callback.
     *
     * @returns Status code.
     * @param   hGdbStubCtx         The GDB stub context handle invoking the command.
     * @param   pHlp                Pointer to output formatting helpers.
     * @param   pszArgs             Command arguments.
     * @param   pvUser              Opaque user data passed during stub context creation.
     */
    int (*pfnCmd) (GDBSTUBCTX hGdbStubCtx, PCGDBSTUBOUTHLP pHlp, const char *pszArgs, void *pvUser);
} GDBSTUBCMD;
/** Pointer to a custom command descriptor. */
typedef GDBSTUBCMD *PGDBSTUBCMD;
/** Pointer to a const custom command descriptor. */
typedef const GDBSTUBCMD *PCGDBSTUBCMD;


/**
 * GDB stub interface callback table.
 */
typedef struct GDBSTUBIF
{
    /** Architecture supported by this interface. */
    GDBSTUBTGTARCH              enmArch;
    /** Register entries for the target (the index will be used by the getter/setter callbacks),
     * ended by an entry with a NULL name and 0 register bit size. */
    PCGDBSTUBREG                paRegs;
    /** Custom command descriptors, terminated by a NULL entry. */
    PCGDBSTUBCMD                paCmds;

    /**
     * Allocate memory.
     *
     * @returns Pointer to the allocated memory on success or NULL if out of memory.
     * @param   hGdbStubCtx         The GDB stub context handle invoking the callback,
     *                              can be NULL during allocation and freeing of the context structure itself.
     * @param   pvUser              Opaque user data passed during creation of the stub context.
     * @param   cb                  How much to allocate.
     */
    void * (*pfnMemAlloc) (GDBSTUBCTX hGdbStubCtx, void *pvUser, size_t cb);

    /**
     * Frees memory allocated with pfnMemAlloc().
     *
     * @returns nothing.
     * @param   hGdbStubCtx         The GDB stub context handle invoking the callback.
     * @param   pvUser              Opaque user data passed during creation of the stub context.
     * @param   pv                  The pointer of the memory region to free.
     */
    void   (*pfnMemFree) (GDBSTUBCTX hGdbStubCtx, void *pvUser, void *pv);

    /**
     * Returns the current target state.
     *
     * @returns Target state.
     * @param   hGdbStubCtx         The GDB stub context handle invoking the callback.
     * @param   pvUser              Opaque user data passed during creation of the stub context.
     */
    GDBSTUBTGTSTATE (*pfnTgtGetState) (GDBSTUBCTX hGdbStubCtx, void *pvUser);

    /**
     * Stop the target.
     *
     * @returns Status code.
     * @param   hGdbStubCtx         The GDB stub context handle invoking the callback.
     * @param   pvUser              Opaque user data passed during creation of the stub context.
     */
    int    (*pfnTgtStop) (GDBSTUBCTX hGdbStubCtx, void *pvUser);

    /**
     * Restarts the target, optional.
     *
     * @returns Status code.
     * @param   hGdbStubCtx         The GDB stub context handle invoking the callback.
     * @param   pvUser              Opaque user data passed during creation of the stub context.
     */
    int    (*pfnTgtRestart) (GDBSTUBCTX hGdbStubCtx, void *pvUser);

    /**
     * Kills the target, optional.
     *
     * @returns Status code.
     * @param   hGdbStubCtx         The GDB stub context handle invoking the callback.
     * @param   pvUser              Opaque user data passed during creation of the stub context.
     */
    int    (*pfnTgtKill) (GDBSTUBCTX hGdbStubCtx, void *pvUser);

    /**
     * Step a single instruction in the target.
     *
     * @returns Status code.
     * @param   hGdbStubCtx         The GDB stub context handle invoking the callback.
     * @param   pvUser              Opaque user data passed during creation of the stub context.
     */
    int    (*pfnTgtStep) (GDBSTUBCTX hGdbStubCtx, void *pvUser);

    /**
     * Continue the target.
     *
     * @returns Status code.
     * @param   hGdbStubCtx         The GDB stub context handle invoking the callback.
     * @param   pvUser              Opaque user data passed during creation of the stub context.
     */
    int    (*pfnTgtCont) (GDBSTUBCTX hGdbStubCtx, void *pvUser);

    /**
     * Read data from the target address space.
     *
     * @returns Status code.
     * @param   hGdbStubCtx         The GDB stub context handle invoking the callback.
     * @param   pvUser              Opaque user data passed during creation of the stub context.
     * @param   GdbTgtMemAddr       The target address space memory address to start reading from.
     * @param   pvDst               Where to store the read data on success.
     * @param   cbRead              How much to read.
     */
    int    (*pfnTgtMemRead) (GDBSTUBCTX hGdbStubCtx, void *pvUser, GDBTGTMEMADDR GdbTgtMemAddr, void *pvDst, size_t cbRead);

    /**
     * Write data to the target address space.
     *
     * @returns Status code.
     * @param   hGdbStubCtx         The GDB stub context handle invoking the callback.
     * @param   pvUser              Opaque user data passed during creation of the stub context.
     * @param   GdbTgtMemAddr       The target address space memory address to start writing to.
     * @param   pvSrc               The data to write.
     * @param   cbWrite             How much to write.
     */
    int    (*pfnTgtMemWrite) (GDBSTUBCTX hGdbStubCtx, void *pvUser, GDBTGTMEMADDR GdbTgtMemAddr, const void *pvSrc, size_t cbWrite);

    /**
     * Reads the given registers of the target.
     *
     * @returns Status code.
     * @param   hGdbStubCtx         The GDB stub context handle invoking the callback.
     * @param   pvUser              Opaque user data passed during creation of the stub context.
     * @param   paRegs              Register indizes to read.
     * @param   cRegs               Number of registers to read.
     * @param   pvDst               Where to store the register content (caller makes sure there is enough space
     *                              to store cRegs * GDBSTUBIF::cbReg bytes of data).
     */
    int    (*pfnTgtRegsRead) (GDBSTUBCTX hGdbStubCtx, void *pvUser, uint32_t *paRegs, uint32_t cRegs, void *pvDst);

    /**
     * Writes the given registers of the target.
     *
     * @returns Status code.
     * @param   hGdbStubCtx         The GDB stub context handle invoking the callback.
     * @param   pvUser              Opaque user data passed during creation of the stub context.
     * @param   paRegs              Register indizes to write.
     * @param   cRegs               Number of registers to write.
     * @param   pvSrc               The register content to write (caller makes sure there is enough space
     *                              to store cRegs * GDBSTUBIF::cbReg bytes of data).
     */
    int    (*pfnTgtRegsWrite) (GDBSTUBCTX hGdbStubCtx, void *pvUser, uint32_t *paRegs, uint32_t cRegs, const void *pvSrc);

    /**
     * Sets a new trace point at the given address - optional.
     *
     * @returns Status code.
     * @param   hGdbStubCtx         The GDB stub context handle invoking the callback.
     * @param   pvUser              Opaque user data passed during creation of the stub context.
     * @param   GdbTgtTpAddr        The target address space memory address to set the trace point at.
     * @param   enmTpType           The tracepoint type (working on instructions or memory accesses).
     * @param   enmTpAction         The action to execute if the tracepoint is hit.
     */
    int    (*pfnTgtTpSet) (GDBSTUBCTX hGdbStubCtx, void *pvUser, GDBTGTMEMADDR GdbTgtTpAddr, GDBSTUBTPTYPE enmTpType, GDBSTUBTPACTION enmTpAction);

    /**
     * Clears a previously set trace point at the given address - optional.
     *
     * @returns Status code.
     * @param   hGdbStubCtx         The GDB stub context handle invoking the callback.
     * @param   pvUser              Opaque user data passed during creation of the stub context.
     * @param   GdbTgtTpAddr        The target address space memory address to remove the trace point from.
     */
    int    (*pfnTgtTpClear) (GDBSTUBCTX hGdbStubCtx, void *pvUser, GDBTGTMEMADDR GdbTgtTpAddr);

} GDBSTUBIF;
/** Pointer to a interface callback table. */
typedef GDBSTUBIF *PGDBSTUBIF;
/** Pointer to a const interface callback table. */
typedef const GDBSTUBIF *PCGDBSTUBIF;


/**
 * GDB stub I/O interface callback table.
 */
typedef struct GDBSTUBIOIF
{
    /** 
     * Returns amount of data available for reading (for optimized buffer allocations).
     *
     * @returns Amount of bytes available for reading.
     * @param   hGdbStubCtx         The GDB stub context handle invoking the callback.
     * @param   pvUser              Opaque user data passed during creation of the stub context.
     */
    size_t (*pfnPeek) (GDBSTUBCTX hGdbStubCtx, void *pvUser);

    /**
     * Read data from the underlying transport layer - non blocking.
     *
     * @returns Status code.
     * @param   hGdbStubCtx         The GDB stub context handle invoking the callback.
     * @param   pvUser              Opaque user data passed during creation of the stub context.
     * @param   pvDst               Where to store the read data.
     * @param   cbRead              Maximum number of bytes to read.
     * @param   pcbRead             Where to store the number of bytes actually read.
     */
    int    (*pfnRead) (GDBSTUBCTX hGdbStubCtx, void *pvUser, void *pvDst, size_t cbRead, size_t *pcbRead);

    /** 
     * Write a packet to the underlying transport layer.
     *
     * @returns Status code.
     * @param   hGdbStubCtx         The GDB stub context handle invoking the callback.
     * @param   pvUser              Opaque user data passed during creation of the stub context.
     * @param   pvPkt               The packet data to write.
     * @param   cbPkt               The number of bytes to write.
     *
     * @note Unlike the read callback this should only return when the whole packet has been written
     *       or an unrecoverable error occurred.
     */
    int    (*pfnWrite) (GDBSTUBCTX hGdbStubCtx, void *pvUser, const void *pvPkt, size_t cbPkt);

    /**
     * Blocks until data is available for reading - optional.
     *
     * @returns Status code.
     * @param   hGdbStubCtx         The GDB stub context handle invoking the callback.
     * @param   pvUser              Opaque user data passed during creation of the stub context.
     *
     * @note This is an optional callback, if not available and there is no data to read from the underlying
     *       transport layer GDBStubCtxRun() will return and must be invoked again when there is something to read.
     */
    int    (*pfnPoll) (GDBSTUBCTX hGdbStubCtx, void *pvUser);
} GDBSTUBIOIF;
/** Pointer to a I/O interface callback table. */
typedef GDBSTUBIOIF *PGDBSTUBIOIF;
/** Pointer to a const I/O interface callback table. */
typedef const GDBSTUBIOIF *PCGDBSTUBIOIF;


/**
 * Creates a new GDB stub context with the given callback table.
 *
 * @returns Status code.
 * @param   phCtx                   Where to store the handle to the GDB stub context on success.
 * @param   pIoIf                   The I/O interface callback table pointer.
 * @param   pIf                     The interface callback table pointer.
 * @param   pvUser                  Opaque user data to pass to the interface callbacks.
 */
int GDBStubCtxCreate(PGDBSTUBCTX phCtx, PCGDBSTUBIOIF pIoIf, PCGDBSTUBIF pIf, void *pvUser);

/**
 * Destroys a given GDB stub context.
 *
 * @returns nothing.
 * @param   hCtx                    The GDB stub context handle to destroy.
 */
void GDBStubCtxDestroy(GDBSTUBCTX hCtx);

/**
 * Runs the GDB stub runloop until there is nothing to read from the underlying I/O device.
 *
 * @returns Status code.
 * @param   hCtx                    The GDB stub context handle.
 */
int GDBStubCtxRun(GDBSTUBCTX hCtx);

/**
 * Resets the given GDB stub context to an initial state without freeing allocated scratch buffers.
 *
 * @returns Status code.
 * @param   hCtx                    The GDB stub context handle.
 */
int GDBStubCtxReset(GDBSTUBCTX hCtx);

#endif /* __libgdbstub_h */
