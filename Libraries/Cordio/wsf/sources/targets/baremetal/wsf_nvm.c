/*************************************************************************************************/
/*!
 *  \file   wsf_nvm.c
 *
 *  \brief  NVM service.
 *
 *  Copyright (c) 2019 Arm Ltd. All Rights Reserved.
 *
 *  Copyright (c) 2019-2020 Packetcraft, Inc.
 *  
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *  
 *      http://www.apache.org/licenses/LICENSE-2.0
 *  
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */
/*************************************************************************************************/
#include <string.h>
#include <stdint.h>

#include "wsf_cs.h"
#include "wsf_types.h"
#include "wsf_nvm.h"
#include "wsf_assert.h"
#include "pal_flash.h"
#include "util/crc32.h"
#include "wsf_buf.h"
#include "wsf_queue.h"
#include "mxc_device.h"

/**************************************************************************************************
  Macros
**************************************************************************************************/

/*! NVM data start address. */
#define WSF_NVM_START_ADDR 0x0000

/*! Reserved filecode. */
#define WSF_NVM_RESERVED_FILECODE ((uint64_t)0)

/* Unused (erased) filecode. */
/* TODO: May depend on flash type */
#define WSF_NVM_UNUSED_FILECODE ((uint64_t)0xFFFFFFFFFFFFFFFF)

/*! Flash word size. */
#define WSF_FLASH_WORD_SIZE 4

/*! Align value to word boundary. */
#define WSF_NVM_WORD_ALIGN(x) (((x) + (WSF_FLASH_WORD_SIZE - 1)) & ~(WSF_FLASH_WORD_SIZE - 1))

#define WSF_NVM_CRC_INIT_VALUE 0xFEDCBA98

/**************************************************************************************************
  Data Types
**************************************************************************************************/

/*! \brief      Header. */

typedef struct {
    uint64_t id; /*!< Stored data ID. */
    uint32_t len; /*!< Stored data length. */
    uint32_t headerCrc; /*!< CRC of this header. */
    uint32_t dataCrc; /*!< CRC of subsequent data. */
} WsfNvmHeader_t;

#define WSF_NVM_HEADER_SIZE sizeof(WsfNvmHeader_t)
#define WSF_NVM_FILE_SIZE(header_len) (WSF_NVM_HEADER_SIZE + WSF_NVM_WORD_ALIGN(header_len))
static struct {
    uint32_t availAddr; /*!< Next available address for NVM write. */
    uint32_t sectorSize; /*!< Size of erase sector. */
    uint32_t totalSize; /*!< Total size of NVM storage. */
} wsfNvmCb;

extern uint32_t __pal_nvm_db_start__, __pal_nvm_db_end__;

/**************************************************************************************************
  Global Functions
**************************************************************************************************/
/*************************************************************************************************/
/*!
 *  \brief  Get remaining space in WSF Allcoated NVM.
 *  \return Bytes left.
 */
/*************************************************************************************************/
uint32_t WsfNvmGetRemainingSpace(void)
{
    const int32_t bytesLeft = wsfNvmCb.totalSize - (wsfNvmCb.availAddr - WSF_NVM_START_ADDR);


    WSF_ASSERT(bytesLeft >= 0);

    if(bytesLeft < 0)
    {
        return 0;
    }

    
    return (uint32_t)bytesLeft;
}
/*************************************************************************************************/
/*!
 *  \brief  Check if NVM space is full.
 *  \return TRUE if all NVM space is taken, FALSE otherwise.
 */
/*************************************************************************************************/
bool_t WsfNvmIsFull(void)
{
    return WsfNvmGetRemainingSpace() <= 0;
}

/*************************************************************************************************/
/*!
 *  \brief  Initialize the WSF NVM.
 */
/*************************************************************************************************/
void WsfNvmInit(void)
{
    PalFlashInit(NULL);
    wsfNvmCb.totalSize = PalNvmGetTotalSize();
    wsfNvmCb.sectorSize = PalNvmGetSectorSize();

    WsfNvmHeader_t header;
    uint32_t storageAddr = WSF_NVM_START_ADDR;
    uint32_t headerCrc;
    bool_t corruptData = FALSE;

    do {
        /* Read header. */
        PalFlashRead(&header, sizeof(header), storageAddr);

        if (header.id == WSF_NVM_UNUSED_FILECODE) {
            /* Found unused entry at end of used storage. */
            break;
        }

        /* Iterate through stored data headers, looking for existing matching stored data header. */
        if (header.id != WSF_NVM_RESERVED_FILECODE) {
            /* Calculate CRC of header itself. */
            headerCrc = CalcCrc32(WSF_NVM_CRC_INIT_VALUE, sizeof(header.id) + sizeof(header.len),
                                  (uint8_t *)&header);

            if (headerCrc != header.headerCrc) {
                /* Corrupt header. */
                corruptData = TRUE;
                break;
            }
        } else {
            if ((header.headerCrc != 0) || (header.dataCrc != 0)) {
                /* Corrupt header. */
                corruptData = TRUE;
                break;
            }
        }

        /* Move to next stored data block and read header. */
        storageAddr += WSF_NVM_WORD_ALIGN(header.len) + sizeof(header);

        if (!((storageAddr - WSF_NVM_START_ADDR) < wsfNvmCb.totalSize)) {
            WSF_TRACE_INFO0("WSF -> NVM IS FULL!");
            storageAddr =  WSF_NVM_START_ADDR + wsfNvmCb.totalSize;
            break;
        }

    } while ((storageAddr - WSF_NVM_START_ADDR) < wsfNvmCb.totalSize);

    wsfNvmCb.availAddr = storageAddr;

    /* Check for corrupt data. */
    if (corruptData == TRUE) {
        /* Search for the first available location */
        while ((storageAddr - WSF_NVM_START_ADDR) < wsfNvmCb.totalSize) {
            PalFlashRead(&header.id, sizeof(header.id), storageAddr);

            if (header.id == WSF_NVM_UNUSED_FILECODE) {
                break;
            }

            storageAddr += sizeof(header.id);
        }

        /* Update the address of the first available location. align to sector boundary. */
        wsfNvmCb.availAddr = (storageAddr + wsfNvmCb.sectorSize - 1) & ~(wsfNvmCb.sectorSize - 1);

        /* Erase all data. */
        WsfNvmEraseDataAll(NULL);
    }
}

/*************************************************************************************************/
/*!
 *  \brief  Read data.
 *
 *  \param  id         Stored data ID.
 *  \param  pData      Buffer to read to.
 *  \param  len        Data length to read.
 *  \param  compCback  Read callback.
 *
 *  \return TRUE if NVM operation is successful, FALSE otherwise.
 */
/*************************************************************************************************/
bool_t WsfNvmReadData(uint64_t id, uint8_t *pData, uint16_t len, WsfNvmCompEvent_t compCback)
{
    WsfNvmHeader_t header;
    uint32_t headerCrc, dataCrc;
    uint32_t storageAddr = WSF_NVM_START_ADDR;
    bool_t findId = FALSE;

    WSF_ASSERT(!((id == WSF_NVM_RESERVED_FILECODE) || (id == WSF_NVM_UNUSED_FILECODE)));

    /* Read first header. */
    PalFlashRead(&header, sizeof(header), storageAddr);

    do {
        if (header.id == WSF_NVM_UNUSED_FILECODE) {
            /* Found unused entry at end of used storage. */
            break;
        }

        /* Iterate through stored data headers, looking for existing matching stored data header. */
        if (header.id != WSF_NVM_RESERVED_FILECODE) {
            /* Calculate CRC of header itself. */
            headerCrc = CalcCrc32(WSF_NVM_CRC_INIT_VALUE, sizeof(header.id) + sizeof(header.len),
                                  (uint8_t *)&header);

            if (headerCrc != header.headerCrc) {
                /* Corrupt header. */
                /* TODO: Catastrophic failure? */
                break;
            } else if ((header.id == id) && (header.len == len)) {
                /* Valid header and matching ID - read data after header. */
                storageAddr += sizeof(header);
                PalFlashRead(pData, header.len, storageAddr);
                dataCrc = CalcCrc32(WSF_NVM_CRC_INIT_VALUE, header.len, pData);
                if (dataCrc == header.dataCrc) {
                    findId = TRUE;
                }
                break;
            }
        }

        /* Move to next stored data block and read header. */
        storageAddr += WSF_NVM_WORD_ALIGN(header.len) + sizeof(header);
        PalFlashRead(&header, sizeof(header), storageAddr);
    } while (1);

    if (compCback) {
        compCback(findId);
    }
    return findId;
}

/*************************************************************************************************/
/*!
 *  \brief  Check whether or not the amount of data has space to fit into NVM
 *
 *  \param  lenNeeded         Number of bytes wanting to store
 *
 *  \return TRUE if NVM operation is successful, FALSE otherwise.
 */
/*************************************************************************************************/
static inline bool_t wsfNvmHaveEnoughSpace(uint32_t lenNeeded)
{
    if (WSF_NVM_FILE_SIZE(lenNeeded) > WsfNvmGetRemainingSpace()) {
        return FALSE;
    } else {
        return TRUE;
    }
}
/*************************************************************************************************/
/*!
 *  \brief  Write data.
 *
 *  \param  id         Stored data ID.
 *  \param  pData      Buffer to write.
 *  \param  len        Data length to write.
 *  \param  compCback  Write callback.
 *
 *  \return TRUE if NVM operation is successful, FALSE otherwise.
 */
/*************************************************************************************************/
bool_t WsfNvmWriteData(uint64_t id, const uint8_t *pData, uint16_t len, WsfNvmCompEvent_t compCback)
{
    WsfNvmHeader_t header;
    uint32_t headerCrc, dataCrc;
    uint32_t storageAddr = WSF_NVM_START_ADDR;

    if (wsfNvmCb.totalSize == 0) {
        return FALSE;
    }

    WSF_ASSERT(!((id == WSF_NVM_RESERVED_FILECODE) || (id == WSF_NVM_UNUSED_FILECODE)));

    if (!wsfNvmHaveEnoughSpace(len)) {
        WSF_TRACE_INFO0("WsfNvm: Failed to write flash! Out of space.");
        return FALSE;
    }

    /* Read first header. */
    PalFlashRead(&header, sizeof(header), storageAddr);

    do {
        if (header.id == WSF_NVM_UNUSED_FILECODE) {
            /* Found unused entry at end of used storage. */
            break;
        }

        /* Iterate through stored data headers, looking for existing matching stored data header. */
        if (header.id != WSF_NVM_RESERVED_FILECODE) {
            /* Calculate CRC of header itself. */
            headerCrc = CalcCrc32(WSF_NVM_CRC_INIT_VALUE, sizeof(header.id) + sizeof(header.len),
                                  (uint8_t *)&header);

            if (headerCrc != header.headerCrc) {
                /* Corrupt header. */
                /* TODO: Catastrophic failure? */
                break;
            } else if (header.id == id) {
                dataCrc = CalcCrc32(WSF_NVM_CRC_INIT_VALUE, len, pData);
                if (dataCrc == header.dataCrc) {
                    if (compCback) {
                        compCback(TRUE);
                    }
                    return TRUE;
                } else {
                    /* Valid header and matching ID - scratch header out. */
                    header.id = WSF_NVM_RESERVED_FILECODE;
                    header.headerCrc = 0;
                    header.dataCrc = 0;
                    PalFlashWrite(&header, sizeof(header), storageAddr);
                }
            }
        }

        /* Move to next stored data block and read header. */
        storageAddr += WSF_NVM_WORD_ALIGN(header.len) + sizeof(header);
        PalFlashRead(&header, sizeof(header), storageAddr);
    } while (1);

    /* After cycling through all headers, create a new stored data header and store data */
    header.id = id;
    header.len = len;
    header.headerCrc = CalcCrc32(WSF_NVM_CRC_INIT_VALUE, sizeof(header.id) + sizeof(header.len),
                                 (uint8_t *)&header);
    header.dataCrc = CalcCrc32(WSF_NVM_CRC_INIT_VALUE, len, pData);

    PalFlashWrite(&header, sizeof(header), storageAddr);
    PalFlashWrite((void *)pData, len, storageAddr + sizeof(header));

    /* Move to next empty flash. */
    storageAddr += WSF_NVM_WORD_ALIGN(header.len) + sizeof(header);
    wsfNvmCb.availAddr = storageAddr;

    if (compCback) {
        compCback((wsfNvmCb.availAddr - WSF_NVM_START_ADDR) <= wsfNvmCb.totalSize);
    }

    return TRUE;
}

/*************************************************************************************************/
/*!
 *  \brief  Erase data.
 *
 *  \param  id         Erase ID.
 *  \param  compCback  Write callback.
 *
 *  \return TRUE if NVM operation is successful, FALSE otherwise.
 */
/*************************************************************************************************/
bool_t WsfNvmEraseData(uint64_t id, WsfNvmCompEvent_t compCback)
{
    WsfNvmHeader_t header;
    uint32_t headerCrc;
    uint32_t storageAddr = WSF_NVM_START_ADDR;
    bool_t erased = FALSE;

    WSF_ASSERT(!((id == WSF_NVM_RESERVED_FILECODE) || (id == WSF_NVM_UNUSED_FILECODE)));

    /* Read first header. */
    PalFlashRead(&header, sizeof(header), storageAddr);

    do {
        if (header.id == WSF_NVM_UNUSED_FILECODE) {
            /* Found unused entry at end of used storage. */
            break;
        }

        /* Iterate through stored data headers, looking for existing matching stored data header. */
        if (header.id != WSF_NVM_RESERVED_FILECODE) {
            headerCrc = CalcCrc32(WSF_NVM_CRC_INIT_VALUE, sizeof(header.id) + sizeof(header.len),
                                  (uint8_t *)&header);

            if (headerCrc != header.headerCrc) {
                /* Corrupt header. */
                /* TODO: Catastrophic failure? */
                break;
            } else if (header.id == id) {
                header.id = WSF_NVM_RESERVED_FILECODE;
                header.headerCrc = 0;
                header.dataCrc = 0;
                PalFlashWrite(&header, sizeof(header), storageAddr);
                erased = TRUE;
                break;
            }
        }

        /* Move to next stored data block and read header. */
        storageAddr += WSF_NVM_WORD_ALIGN(header.len) + sizeof(header);
        PalFlashRead(&header, sizeof(header), storageAddr);
    } while (1);

    if (compCback) {
        compCback(erased);
    }
    return erased;
}

/*************************************************************************************************/
/*!
 *  \brief  Erase all data located in NVM storage.
 *
 *  \param  compCback          Erase callback.
 *
 *  \note   Security Risk Warning. NVM storage could be shared by multiple Apps.
 */
/*************************************************************************************************/
void WsfNvmEraseDataAll(WsfNvmCompEvent_t compCback)
{
    for (uint32_t eraseAddr = WSF_NVM_START_ADDR; eraseAddr < wsfNvmCb.availAddr;
         eraseAddr += wsfNvmCb.sectorSize) {
        PalFlashEraseSector(1, eraseAddr);
    }
    wsfNvmCb.availAddr = WSF_NVM_START_ADDR;

    if (compCback) {
        compCback(TRUE);
    }
}

/*************************************************************************************************/
/*!
 *  \brief  Defragment NVM
 *  \param  copyBuf       Scratch buffer to temporarily copy NVM stored data.
 *  \param  size          Size of copyBuf in bytes.
 *  \return  TRUE if defragment successful. FALSE otherwise
 * 
 *  \note   Defragment should only be called when the storage is full and a record has been invalidated.
 *          Furthermore, copyBuf must be at least the size of WSF NVM allocated flash.
 */
/*************************************************************************************************/
bool_t WsfNvmDefragment(uint8_t *copyBuf, uint32_t size)
{

    WSF_ASSERT(copyBuf && size >= wsfNvmCb.totalSize);

    if (!copyBuf || size < wsfNvmCb.totalSize) {
        WSF_TRACE_INFO0("Not enough memory given to defragment NVM");
        return FALSE;
    }


    
    WsfCsEnter();


    bool_t defragPossible = FALSE;
    uint32_t currentOffset = WSF_NVM_START_ADDR;
    uint32_t copyOffset = 0;
    WsfNvmHeader_t header = { 0 };

    /* Get the first header*/
    PalFlashRead(&header, WSF_NVM_HEADER_SIZE, currentOffset);
    
    while (currentOffset < wsfNvmCb.availAddr && header.id != WSF_NVM_UNUSED_FILECODE) {
        const uint32_t fileSize = WSF_NVM_FILE_SIZE(header.len);

        /*If the entry is valid copy it into the defragmentation buffer, if its reserved we can defrag it*/
        if (header.id != WSF_NVM_RESERVED_FILECODE) {
            PalFlashRead(&copyBuf[copyOffset], fileSize, currentOffset);
            copyOffset += WSF_NVM_FILE_SIZE(header.len);
        } else {
            defragPossible = TRUE;
        }

        currentOffset += fileSize;

        if(currentOffset + WSF_NVM_HEADER_SIZE <= WSF_NVM_START_ADDR + wsfNvmCb.totalSize)
        {
            PalFlashRead(&header, WSF_NVM_HEADER_SIZE, currentOffset);
        }
    }

    /*
    If we defragged anything clear the sector and rewrite flash with defragmented data
  */
    if (defragPossible) {
        WsfNvmEraseDataAll(NULL);
        PalFlashWrite(copyBuf, copyOffset, wsfNvmCb.availAddr);
        wsfNvmCb.availAddr += copyOffset;
    } else {
        WSF_TRACE_INFO0("No unused memory for defragementation!");
    }

    WsfCsExit();

    return defragPossible;
}
