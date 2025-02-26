/*******************************************************************************
 * Copyright 2019-2022 Microchip FPGA Embedded Systems Solutions.
 *
 * SPDX-License-Identifier: MIT
 *
 * MPFS HSS Embedded Software
 *
 */

/*!
 * \file QSPI (null) Service
 * \brief Setup QSPI
 */

#include "config.h"
#include "hss_types.h"
#include "hss_state_machine.h"
#include "hss_progress.h"
#include "hss_debug.h"

#include <assert.h>
#include <string.h>

#include "qspi_service.h"
#include "encoding.h"
#include "mss_qspi.h"
#include "mss_sys_services.h"

#include "mss_peripherals.h"
#include "winbond_w25n01gv.h"

/*
 * QSPI doesn't need a "service" to run every super-loop, but it does need to be
 * initialized early...
 */

struct FlashDescriptor
{
    uint32_t jedecId;
    uint32_t pageSize;
    uint32_t pagesPerBlock;
    uint32_t blocksPerDie;
    const char * const name;
} qspiFlashes[] = {
    { 0xEFAA21u, 2048u, 64u, 1024u, "Winbond W25N01GV" }, // EFh => Winbond, AA21h => W25N01GV
};

static uint32_t pageSize, blockSize, dieSize, eraseSize, pageCount, blockCount;
static uint32_t numBadBlocks;

static uint16_t *pLogicalToPhysicalMap = NULL;
static uint16_t *pBadBlocksMap = NULL;
static struct HSS_QSPI_Cache_Descriptor
{
    bool dirtyCache;
    bool inCache;
} *pLogicalBlockDesc = NULL;
static uint8_t *pCacheDataBuffer = NULL;

bool cacheDirtyFlag = false;

////////////////////////////////////////////////////////////////////////////////////////
//
// Generic Local module functions
//
__attribute__((nonnull)) static bool flash_id_to_descriptor_(const uint32_t jedec_id, size_t *indexOut)
{
    bool result = false;
    assert(indexOut);

    for (size_t i = 0u; i < ARRAY_SIZE(qspiFlashes); i++) {
        if (qspiFlashes[i].jedecId == jedec_id) {
            *indexOut = i;
            result = true;
            break;
        }
    }

    return result;
}

static void build_bad_block_map_(void)
{
    numBadBlocks = Flash_scan_for_bad_blocks(pBadBlocksMap);

    size_t badBlockIndex = 0u;
    size_t logicalBlockIndex = 0u;

    uint32_t badBlocksRemaining = numBadBlocks;

    for (size_t physicalBlockIndex = 0u; physicalBlockIndex < blockCount; physicalBlockIndex++) {
        if (badBlocksRemaining && (pBadBlocksMap[badBlockIndex] == physicalBlockIndex)) { // skip bad physical block
            badBlockIndex++;
            badBlocksRemaining--;
        } else { // good physical block, so use
            pLogicalToPhysicalMap[logicalBlockIndex] = physicalBlockIndex;
            logicalBlockIndex++;
        }
    }
}

__attribute__((pure)) static inline uint32_t column_to_block_(const uint32_t column_addr)
{
    assert(blockSize); // avoid divide by zero

    const uint32_t result = (column_addr / blockSize);
    return result;
}

__attribute__((pure)) static inline uint32_t logical_to_physical_address_(const uint32_t logical_addr)
{
    assert(blockSize); // avoid divide by zero

    const uint16_t logical_block_num = logical_addr / blockSize;
    const uint32_t remainder = logical_addr % blockSize;
    const uint16_t physical_block_number = pLogicalToPhysicalMap[logical_block_num];

    const uint32_t result = (physical_block_number * blockSize) + remainder;

    return result;
}

__attribute__((pure)) static inline uint32_t logical_to_physical_block_(const uint32_t logical_block)
{
    const uint32_t result = pLogicalToPhysicalMap[logical_block];

    return result;
}

static void demandCopyFlashBlocksToCache_(size_t byteOffset, size_t byteCount, bool markDirty)
{
    for (size_t offset = byteOffset; offset < (byteOffset + byteCount); offset += blockSize) {
        const size_t physicalBlockOffset = logical_to_physical_block_(column_to_block_(offset));

        if (!pLogicalBlockDesc[physicalBlockOffset].inCache) {
            //mHSS_DEBUG_PRINTF(LOG_NORMAL, "Reading block %u into cache\n", physicalBlockOffset);

            const size_t physicalBlockByteOffset = physicalBlockOffset * blockSize;
            Flash_read(pCacheDataBuffer + physicalBlockByteOffset, physicalBlockByteOffset, blockSize);
            pLogicalBlockDesc[physicalBlockOffset].inCache = true;
        }

        if (markDirty) {
            pLogicalBlockDesc[physicalBlockOffset].dirtyCache = true;
        }
    }
}

static void copyCacheToFlashBlocks_(size_t byteOffset, size_t byteCount)
{
    const size_t endOffset = byteOffset + byteCount;
    size_t dirtyBlockCount = 0u;
    mHSS_DEBUG_PRINTF_EX("\n");

    for (size_t blockOffset = 0u; blockOffset < blockCount; blockOffset++) {
        if (pLogicalBlockDesc[blockOffset].dirtyCache) {
            dirtyBlockCount++;
        }
    }

    const size_t initialDirtyBlockCount = dirtyBlockCount;
    for (size_t offset = byteOffset; dirtyBlockCount && (offset < endOffset); offset += blockSize) {
        HSS_ShowProgress(initialDirtyBlockCount, dirtyBlockCount);

        const size_t physicalBlockOffset = logical_to_physical_block_(column_to_block_(offset));

        if (pLogicalBlockDesc[physicalBlockOffset].inCache) {
            if (pLogicalBlockDesc[physicalBlockOffset].dirtyCache) {
                //mHSS_DEBUG_PRINTF(LOG_NORMAL, "Writing block %u from cache\n", physicalBlockOffset);

                const size_t physicalBlockByteOffset = physicalBlockOffset * blockSize;
                uint8_t status = Flash_erase_block(physicalBlockOffset);

                if (status) {
                    mHSS_DEBUG_PRINTF(LOG_ERROR, "Error erasing block %u\n", physicalBlockOffset);
                    break;
                }

                status = Flash_program(pCacheDataBuffer + physicalBlockByteOffset, physicalBlockByteOffset, blockSize);
                if (status) {
                    mHSS_DEBUG_PRINTF(LOG_ERROR, "Error programming block %u\n", physicalBlockOffset);

                }
                pLogicalBlockDesc[physicalBlockOffset].dirtyCache = false;
                dirtyBlockCount--;
            }
        }
    }

    HSS_ShowProgress(initialDirtyBlockCount, 0u);
}

////////////////////////////////////////////////////////////////////////////////////////
//
// QSPI Uncached Functions
//
//

static bool qspiInitialized = false;
static size_t qspiIndex = 0u;
bool HSS_QSPIInit(void)
{

    if (!qspiInitialized) {
        /* read and output Flash ID as a sanity test */
        (void)mss_config_clk_rst(MSS_PERIPH_QSPIXIP, (uint8_t) 0u, PERIPHERAL_ON);

        uint8_t rd_buf[10] __attribute__ ((aligned(4)));

        Flash_init(MSS_QSPI_DUAL_FULL);
        Flash_readid(rd_buf);

        uint32_t jedec_id = ((rd_buf[0] << 16) | (rd_buf[1] <<8) | (rd_buf[2]));

        if (flash_id_to_descriptor_(jedec_id, &qspiIndex)) {
            mHSS_DEBUG_PRINTF(LOG_NORMAL, "%s detected (JEDEC %06X)\n", qspiFlashes[qspiIndex].name, jedec_id);

            pageSize = qspiFlashes[qspiIndex].pageSize;
            blockSize = qspiFlashes[qspiIndex].pageSize * qspiFlashes[qspiIndex].pagesPerBlock;
            eraseSize = blockSize;
            blockCount = qspiFlashes[qspiIndex].blocksPerDie;
            pageCount = qspiFlashes[qspiIndex].pagesPerBlock * blockCount;
            dieSize = blockSize * blockCount;

            // mHSS_DEBUG_PRINTF(LOG_NORMAL, "pageSize: %u\n", pageSize);
            // mHSS_DEBUG_PRINTF(LOG_NORMAL, "blockSize: %u\n", blockSize);
            // mHSS_DEBUG_PRINTF(LOG_NORMAL, "eraseSize: %u\n", eraseSize);
            // mHSS_DEBUG_PRINTF(LOG_NORMAL, "pageCount: %u\n", pageCount);
            // mHSS_DEBUG_PRINTF(LOG_NORMAL, "blockCount: %u\n", blockCount);
            // mHSS_DEBUG_PRINTF(LOG_NORMAL, "dieSize: %u\n", dieSize);

            //
            // we're going to place buffers in DDR for
            //   * a set of logical to physical block qspiIndex mappings;
            //   * a list of bad blocks;
            //   * a set of logical block descriptors;
            //   * a data cache the same size as the QSPI Flash device
            //
            extern const uint64_t __ddr_start;
#define DDR_START              (&__ddr_start)
            uint8_t *pU8Buffer = (uint8_t *)DDR_START; // start of cached DDR, as good a place as any
            pLogicalToPhysicalMap = (uint16_t *)pU8Buffer;
            memset(pLogicalToPhysicalMap, 0, (sizeof(*pLogicalToPhysicalMap) * blockCount));
            pU8Buffer += (sizeof(*pLogicalToPhysicalMap) * blockCount);

            pBadBlocksMap = (uint16_t *)pU8Buffer;
            memset(pBadBlocksMap, 0, (sizeof(*pBadBlocksMap) * blockCount));
            pU8Buffer += (sizeof(*pBadBlocksMap) * blockCount);

            pLogicalBlockDesc = (struct HSS_QSPI_Cache_Descriptor*)pU8Buffer;
            memset(pLogicalBlockDesc, 0, (sizeof(*pLogicalBlockDesc) * blockCount));
            pU8Buffer += (sizeof(*pLogicalBlockDesc) * blockCount);

            pCacheDataBuffer = (uint8_t *)pU8Buffer;

            // mHSS_DEBUG_PRINTF(LOG_NORMAL, "pLogicalToPhysicalMap: %p\n", pLogicalToPhysicalMap);
            // mHSS_DEBUG_PRINTF(LOG_NORMAL, "pLogicalBlockDesc: %p\n", pLogicalBlockDesc);
            // mHSS_DEBUG_PRINTF(LOG_NORMAL, "pCacheDataBuffer: %p\n", pCacheDataBuffer);

            //
            // check for bad blocks and reduce the number of blocks accordingly...
            // our caches and logical block descriptors above may now be slightly too large, but this
            // is of no consequence
            build_bad_block_map_();
            blockCount -= numBadBlocks; // adjust block count to take account of bad blocks
            pageCount = qspiFlashes[qspiIndex].pagesPerBlock * blockCount;
            dieSize = blockSize * blockCount;

            // mHSS_DEBUG_PRINTF(LOG_NORMAL, "blockCount (after bad blocks): %u\n", blockCount);

            mHSS_DEBUG_PRINTF(LOG_NORMAL, "Initialized Flash\n");
            qspiInitialized = true;

        } else {
            mHSS_DEBUG_PRINTF(LOG_NORMAL, "Initialized Flash (JEDEC %06X)\n", jedec_id);
        }
    }

    return qspiInitialized;
}

__attribute__((nonnull)) bool HSS_QSPI_ReadBlock(void *pDest, size_t srcOffset, size_t byteCount)
{
    bool result = true;

    const uint32_t read_addr = logical_to_physical_address_((uint32_t)srcOffset);
    Flash_read((uint8_t *)pDest, read_addr, (uint32_t) byteCount);

    return result;
}

__attribute__((nonnull)) bool HSS_QSPI_WriteBlock(size_t dstOffset, void *pSrc, size_t byteCount)
{
    bool result = true;

    const uint32_t write_addr = logical_to_physical_address_((uint32_t)dstOffset);
    Flash_program((uint8_t *)pSrc, write_addr, (uint32_t)byteCount);
    return result;
}

__attribute__((nonnull)) void HSS_QSPI_GetInfo(uint32_t *pBlockSize, uint32_t *pEraseSize, uint32_t *pBlockCount)
{
    *pBlockSize = pageSize;
    *pEraseSize = eraseSize;
    *pBlockCount = pageCount;
}

void HSS_QSPI_FlushWriteBuffer(void)
{
}

void HSS_QSPI_FlashChipErase(void)
{
    for (uint32_t blockIndex = 0u; blockIndex < blockCount; blockIndex++) {
        HSS_ShowProgress(blockCount, blockCount - blockIndex);
        Flash_erase_block(blockIndex);
    }
    HSS_ShowProgress(blockCount, 0u);
}

void HSS_QSPI_BadBlocksInfo(void)
{
    if (qspiInitialized)
    {
        blockCount = qspiFlashes[qspiIndex].blocksPerDie;
        pageCount = qspiFlashes[qspiIndex].pagesPerBlock * blockCount;
        dieSize = blockSize * blockCount;

        build_bad_block_map_(); // update bad blocks mapping and numBadBlocks

        mHSS_DEBUG_PRINTF(LOG_ERROR, "QSPI Flash: %u bad block%s found\n", numBadBlocks,
            numBadBlocks == 1 ? "":"s");
        if (numBadBlocks) {
            blockCount -= numBadBlocks; // adjust block count to take account of bad blocks
            pageCount = qspiFlashes[qspiIndex].pagesPerBlock * blockCount;
            dieSize = blockSize * blockCount;

            mHSS_DEBUG_PRINTF_EX("Bad Block%s: %u", numBadBlocks == 1 ? "":"s", pBadBlocksMap[0]);
            for (size_t i = 1u; i < numBadBlocks; i++) {
                mHSS_DEBUG_PRINTF_EX(", %u", pBadBlocksMap[i]);
            }
        }
    }
}

////////////////////////////////////////////////////////////////////////////////////////
//
// QSPI Cached Functions
//
//  To make QSPI Flash available as a USB drive, we need to cache the contents
//  in DDR. This allows us to reduce wear on the flash by minimizing the number of
//  block erases performed, and it also makes operation quicker...
//

bool HSS_CachedQSPIInit(void)
{
    bool result = HSS_QSPIInit();
    return result;
}

__attribute__((nonnull)) bool HSS_CachedQSPI_ReadBlock(void *pDest, size_t srcOffset, size_t byteCount)
{
    bool result = true;

    assert(pDest);
    assert((srcOffset + byteCount) <= dieSize);

    demandCopyFlashBlocksToCache_(srcOffset, byteCount, false);

    memcpy(pDest, pCacheDataBuffer + srcOffset, byteCount);

    return result;
}

__attribute__((nonnull)) bool HSS_CachedQSPI_WriteBlock(size_t dstOffset, void *pSrc, size_t byteCount)
{
    bool result = true;

    assert(pSrc);
    assert((dstOffset + byteCount) <= dieSize);

    cacheDirtyFlag = true;
    demandCopyFlashBlocksToCache_(dstOffset, byteCount, true);

    memcpy(pCacheDataBuffer + dstOffset, pSrc, byteCount);
    return result;
}

__attribute__((nonnull)) void HSS_CachedQSPI_GetInfo(uint32_t *pBlockSize, uint32_t *pEraseSize, uint32_t *pBlockCount)
{
    HSS_QSPI_GetInfo(pBlockSize, pEraseSize, pBlockCount);
}

void HSS_CachedQSPI_FlushWriteBuffer(void)
{
    if (cacheDirtyFlag) {
        copyCacheToFlashBlocks_(0u, dieSize);
        mHSS_DEBUG_PRINTF_EX("\n");
        mHSS_DEBUG_PRINTF(LOG_NORMAL, "Synchronized Cache with Flash ...\n");

        cacheDirtyFlag = false;
    }
}
