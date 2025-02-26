/*******************************************************************************
 * Copyright 2019-2022 Microchip FPGA Embedded Systems Solutions.
 *
 * SPDX-License-Identifier: MIT
 *
 * MPFS HAL Embedded Software
 *
 */

/***************************************************************************
 * @file mss_util.h
 * @author Microchip-FPGA Embedded Systems Solutions
 * @brief MACROs defines and prototypes associated with utility functions
 *
 */
#ifndef MSS_UTIL_H
#define MSS_UTIL_H

#include <stdint.h>
#include <stdbool.h>
#include "encoding.h"
#include "mss_hart_ints.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Useful macros
 */
#define WRITE_REG8(x, y) (*((volatile uint8_t *)(x)) = (y))
#define READ_REG8(x)     (*((volatile uint8_t *)(x)))

#define WRITE_REG32(x, y) (*((volatile uint32_t *)(x)) = (y))
#define READ_REG32(x)     (*((volatile uint32_t *)(x)))

#define WRITE_REG64(x, y) (*((volatile uint64_t *)(x)) = (y))
#define READ_REG64(x)     (*((volatile uint64_t *)(x)))

/*
 * Local defines
 */
#define    LOCAL_INT_OFFSET_IN_MIE          16U  /* Offset from start of MIE for local irq enables */
#define    LOCAL_INT_F2M_OFFSET             16U  /* Offset from 0 for fabric to MSS local interrupts */

/*
 * return mcycle
 */
uint64_t readmcycle(void);

void sleep_ms(uint64_t msecs);
void sleep_cycles(uint64_t ncycles);


uint64_t get_stack_pointer(void);
uint64_t get_tp_reg(void);
uint64_t get_program_counter(void) __attribute__((aligned(16)));

#ifdef MPFS_PRINTF_DEBUG_SUPPORTED
void display_address_of_interest(uint64_t * address_of_interest, int nb_locations);
#endif

void exit_simulation(void);

void enable_interrupts(void);
uint64_t disable_interrupts(void);
void restore_interrupts(uint64_t saved_psr);
void __disable_irq(void);
void __disable_all_irqs(void);
void __enable_irq(void);
void __enable_local_irq(uint8_t local_interrupt);
void __disable_local_irq(uint8_t local_interrupt);

static inline void spinunlock(volatile long *pLock)
{
    __sync_lock_release(pLock);
}

static inline void spinlock(volatile long *pLock)
{
    while(__sync_lock_test_and_set(pLock, 1))
    {
        /* add yield if OS */
#if defined USING_FREERTOS
        taskYIELD();
#endif
    }
}


#ifdef __cplusplus
}
#endif

#endif  /* MSS_UTIL_H */
