/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2014 David Quintana [gigaherz]
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with PCSX2.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include "CDVD.h"
#include <atomic>
#include <condition_variable>
#include <thread>

const s32 prefetch_max_blocks = 16;
s32 prefetch_mode = 0;
s32 prefetch_last_lba = 0;
s32 prefetch_last_mode = 0;
s32 prefetch_left = 0;

static std::thread s_thread;

static std::mutex s_notify_lock;
static std::condition_variable s_notify_cv;
static std::mutex s_request_lock;
static std::condition_variable s_request_cv;
static std::mutex s_cache_lock;

static std::atomic<bool> cdvd_is_open;

typedef struct
{
    int lsn;
    int mode;
    char data[2352 * 16]; //we will read in blocks of 16 sectors
} SectorInfo;

//bits: 12 would use 1<<12 entries, or 4096*16 sectors ~ 128MB
#define CACHE_SIZE 12

const s32 CacheSize = (1 << CACHE_SIZE);
SectorInfo Cache[CacheSize];

bool threadRequestPending;
SectorInfo threadRequestInfo;

u32 cdvdSectorHash(int lsn, int mode)
{
    u32 t = 0;

    int i = 32;
    int m = CacheSize - 1;

    while (i >= 0) {
        t ^= lsn & m;
        lsn >>= CACHE_SIZE;
        i -= CACHE_SIZE;
    }

    return (t ^ mode) & m;
}

void cdvdCacheUpdate(int lsn, int mode, char *data)
{
    std::lock_guard<std::mutex> guard(s_cache_lock);
    u32 entry = cdvdSectorHash(lsn, mode);

    memcpy(Cache[entry].data, data, 2352 * 16);
    Cache[entry].lsn = lsn;
    Cache[entry].mode = mode;
}

bool cdvdCacheFetch(int lsn, int mode, char *data)
{
    std::lock_guard<std::mutex> guard(s_cache_lock);
    u32 entry = cdvdSectorHash(lsn, mode);

    if ((Cache[entry].lsn == lsn) &&
        (Cache[entry].mode == mode)) {
        memcpy(data, Cache[entry].data, 2352 * 16);
        return true;
    }
    //printf("NOT IN CACHE\n");
    return false;
}

void cdvdCacheReset()
{
    std::lock_guard<std::mutex> guard(s_cache_lock);
    for (int i = 0; i < CacheSize; i++) {
        Cache[i].lsn = -1;
        Cache[i].mode = -1;
    }
}

void cdvdCallNewDiscCB()
{
    weAreInNewDiskCB = true;
    newDiscCB();
    weAreInNewDiskCB = false;
}

bool cdvdUpdateDiscStatus()
{
    bool ready = src->DiscReady();

    if (!ready) {
        if (!disc_has_changed) {
            disc_has_changed = true;
            curDiskType = CDVD_TYPE_NODISC;
            curTrayStatus = CDVD_TRAY_OPEN;
            cdvdCallNewDiscCB();
        }
    } else {
        if (disc_has_changed) {
            curDiskType = CDVD_TYPE_NODISC;
            curTrayStatus = CDVD_TRAY_CLOSE;

            disc_has_changed = false;
            cdvdRefreshData();
            cdvdCallNewDiscCB();
        }
    }
    return !ready;
}

void cdvdThread()
{
    printf(" * CDVD: IO thread started...\n");
    std::unique_lock<std::mutex> guard(s_notify_lock);

    while (cdvd_is_open) {
        if (cdvdUpdateDiscStatus()) {
            // Need to sleep some to avoid an aggressive spin that sucks the cpu dry.
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        s_notify_cv.wait_for(guard, std::chrono::milliseconds(prefetch_left ? 1 : 250));

        // check again to make sure we're not done here...
        if (!cdvd_is_open)
            break;

        static SectorInfo info;

        bool handlingRequest = false;

        if (threadRequestPending) {
            info = threadRequestInfo;
            handlingRequest = true;
        } else {
            info.lsn = prefetch_last_lba;
            info.mode = prefetch_last_mode;
        }

        if (threadRequestPending || prefetch_left) {
            s32 count = 16;
            s32 left = src->GetSectorCount() - info.lsn;

            if (left < count)
                count = left;

            for (int tries = 0; tries < 4; ++tries) {
                if (info.mode == CDVD_MODE_2048) {
                    if (src->ReadSectors2048(info.lsn, count, info.data))
                        break;
                } else {
                    if (src->ReadSectors2352(info.lsn, count, info.data))
                        break;
                }
            }

            cdvdCacheUpdate(info.lsn, info.mode, info.data);

            if (handlingRequest) {
                threadRequestInfo = info;

                handlingRequest = false;
                threadRequestPending = false;
                s_request_cv.notify_one();

                prefetch_last_lba = info.lsn;
                prefetch_last_mode = info.mode;

                prefetch_left = prefetch_max_blocks;
            } else {
                prefetch_last_lba += 16;
                prefetch_left--;
            }
        }
    }
    printf(" * CDVD: IO thread finished.\n");
}

bool cdvdStartThread()
{
    cdvd_is_open = true;
    try {
        s_thread = std::thread(cdvdThread);
    } catch (std::system_error &) {
        cdvd_is_open = false;
        return false;
    }

    cdvdCacheReset();

    return true;
}

void cdvdStopThread()
{
    cdvd_is_open = false;
    s_notify_cv.notify_one();
    s_thread.join();
}

s32 cdvdRequestSector(u32 sector, s32 mode)
{
    if (sector >= src->GetSectorCount())
        return -1;

    sector &= ~15; //align to 16-sector block

    threadRequestInfo.lsn = sector;
    threadRequestInfo.mode = mode;
    threadRequestPending = false;
    if (cdvdCacheFetch(sector, mode, threadRequestInfo.data)) {
        return 0;
    }

    threadRequestPending = true;
    s_notify_cv.notify_one();

    return 0;
}

s32 cdvdRequestComplete()
{
    return !threadRequestPending;
}

s8 *cdvdGetSector(s32 sector, s32 mode)
{
    {
        std::unique_lock<std::mutex> guard(s_request_lock);
        while (threadRequestPending)
            s_request_cv.wait_for(guard, std::chrono::milliseconds(10));
    }

    s32 offset;

    if (mode == CDVD_MODE_2048) {
        offset = 2048 * (sector - threadRequestInfo.lsn);
        return threadRequestInfo.data + offset;
    }

    offset = 2352 * (sector - threadRequestInfo.lsn);
    s8 *data = threadRequestInfo.data + offset;

    switch (mode) {
        case CDVD_MODE_2328:
            return data + 24;
        case CDVD_MODE_2340:
            return data + 12;
    }
    return data;
}

s32 cdvdDirectReadSector(s32 first, s32 mode, char *buffer)
{
    static char data[16 * 2352];

    if ((u32)first >= src->GetSectorCount())
        return -1;

    s32 sector = first & (~15); //align to 16-sector block

    if (!cdvdCacheFetch(sector, mode, data)) {
        s32 count = 16;
        s32 left = src->GetSectorCount() - sector;

        if (left < count)
            count = left;

        for (int tries = 0; tries < 4; ++tries) {
            if (mode == CDVD_MODE_2048) {
                if (src->ReadSectors2048(sector, count, data))
                    break;
            } else {
                if (src->ReadSectors2352(sector, count, data))
                    break;
            }
        }

        cdvdCacheUpdate(sector, mode, data);
    }

    s32 offset;

    if (mode == CDVD_MODE_2048) {
        offset = 2048 * (first - sector);
        memcpy(buffer, data + offset, 2048);
        return 0;
    }

    offset = 2352 * (first - sector);
    s8 *bfr = data + offset;

    switch (mode) {
        case CDVD_MODE_2328:
            memcpy(buffer, bfr + 24, 2328);
            return 0;
        case CDVD_MODE_2340:
            memcpy(buffer, bfr + 12, 2340);
            return 0;
        default:
            memcpy(buffer, bfr + 12, 2352);
            return 0;
    }
}

s32 cdvdGetMediaType()
{
    return src->GetMediaType();
}

s32 cdvdRefreshData()
{
    const char *diskTypeName = "Unknown";

    //read TOC from device
    cdvdParseTOC();

    if ((etrack == 0) || (strack > etrack)) {
        curDiskType = CDVD_TYPE_NODISC;
    } else {
        s32 mt = cdvdGetMediaType();

        if (mt < 0)
            curDiskType = CDVD_TYPE_DETCTCD;
        else if (mt == 0)
            curDiskType = CDVD_TYPE_DETCTDVDS;
        else
            curDiskType = CDVD_TYPE_DETCTDVDD;
    }

    curTrayStatus = CDVD_TRAY_CLOSE;

    switch (curDiskType) {
        case CDVD_TYPE_DETCTDVDD:
            diskTypeName = "Double-Layer DVD";
            break;
        case CDVD_TYPE_DETCTDVDS:
            diskTypeName = "Single-Layer DVD";
            break;
        case CDVD_TYPE_DETCTCD:
            diskTypeName = "CD-ROM";
            break;
        case CDVD_TYPE_NODISC:
            diskTypeName = "No Disc";
            break;
    }

    printf(" * CDVD: Disk Type: %s\n", diskTypeName);

    cdvdCacheReset();

    return 0;
}
