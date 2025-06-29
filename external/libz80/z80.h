/* =============================================================================
 *  libz80 - Z80 emulation library
 * =============================================================================
 *
 * (C) Gabriel Gambetta (gabriel.gambetta@gmail.com) 2000 - 2012
 *
 * Version 2.1.0
 *
 * -----------------------------------------------------------------------------
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#ifndef _Z80_H_
#define _Z80_H_

#if _WIN32
#    define _CRT_SECURE_NO_WARNINGS
#endif

#include <stddef.h>
#include <stdint.h>

/* If this file is included inside a C++ program, use explicit C linkage for the following functions
 * to avoid name mangling and liker issues
 */
#ifdef __cplusplus
extern "C" {
#endif

/** Function type to emulate data read. */
typedef uint8_t (*Z80DataIn)(uintptr_t param, uint16_t address);

/** Function type to emulate data write. */
typedef void (*Z80DataOut)(uintptr_t param, uint16_t address, uint8_t data);

/**
 * A Z80 register set.
 * An union is used since we want independent access to the high and low bytes of the 16-bit registers.
 */
typedef union {
    /** Word registers. */
    struct
    {
        uint16_t AF, BC, DE, HL, IX, IY, SP;
    } wr;

    /** Byte registers. Note that SP can't be accessed partially. */
    struct
    {
        uint8_t F, A, C, B, E, D, L, H, IXl, IXh, IYl, IYh;
    } br;
} Z80Regs;

/** The Z80 flags */
typedef enum {
    F_C  = 1,  /**< Carry */
    F_N  = 2,  /**< Sub / Add */
    F_PV = 4,  /**< Parity / Overflow */
    F_3  = 8,  /**< Reserved */
    F_H  = 16, /**< Half carry */
    F_5  = 32, /**< Reserved */
    F_Z  = 64, /**< Zero */
    F_S  = 128 /**< Sign */
} Z80Flags;

/** A Z80 execution context. */
typedef struct
{
    Z80Regs  R1; /**< Main register set (R) */
    Z80Regs  R2; /**< Alternate register set (R') */
    uint16_t PC; /**< Program counter */
    uint8_t  R;  /**< Refresh */
    uint8_t  I;
    uint8_t  IFF1; /**< Interrupt Flipflop 1 */
    uint8_t  IFF2; /**< Interrupt Flipflop 2 */
    uint8_t  IM;   /**< Instruction mode */

    Z80DataIn  instrRead;
    Z80DataIn  memRead;
    Z80DataOut memWrite;
    uintptr_t  memParam;

    Z80DataIn  ioRead;
    Z80DataOut ioWrite;
    uintptr_t  ioParam;

    uint8_t  halted;
    unsigned tstates;

    /* Below are implementation details which may change without
     * warning; they should not be relied upon by any user of this
     * library.
     */

    /* If true, an NMI has been requested. */

    uint8_t nmi_req;

    /* If true, a maskable interrupt has been requested. */

    uint8_t int_req;

    /* If true, defer checking maskable interrupts for one
     * instruction.  This is used to keep an interrupt from happening
     * immediately after an IE instruction. */

    uint8_t defer_int;

    /* When a maskable interrupt has been requested, the interrupt
     * vector.  For interrupt mode 1, it's the opcode to execute.  For
     * interrupt mode 2, it's the LSB of the interrupt vector address.
     * Not used for interrupt mode 0.
     */

    uint8_t int_vector;

    /* If true, then execute the opcode in int_vector. */

    uint8_t exec_int_vector;

} Z80Context;

/** Execute the next instruction. */
void Z80Execute(Z80Context *ctx);

/** Execute enough instructions to use at least tstates cycles.
 * Returns the number of tstates actually executed.  Note: Resets
 * ctx->tstates.*/
unsigned Z80ExecuteTStates(Z80Context *ctx, unsigned tstates);

/** Decode the next instruction to be executed.
 * dump and decode can be NULL if such information is not needed
 *
 * @param dump A buffer which receives the hex dump
 * @param decode A buffer which receives the decoded instruction
 */
void Z80Debug(Z80Context *ctx, char *dump, char *decode);

/** Resets the processor. */
void Z80RESET(Z80Context *ctx);

/** Generates a hardware interrupt.
 * Some interrupt modes read a value from the data bus; this value must be provided in this function call, even
 * if the processor ignores that value in the current interrupt mode.
 *
 * @param value The value to read from the data bus
 */
void Z80INT(Z80Context *ctx, uint8_t value);

/** Generates a non-maskable interrupt. */
void Z80NMI(Z80Context *ctx);

#ifdef __cplusplus
} /*close extern "C" block*/
#endif

#endif
