#define __packed
#include "syncedproto.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "adc.h"
#include "asmUtil.h"
#include "board.h"
#include "comms.h"
#include "cpu.h"
#include "drawing.h"
#include "eeprom.h"
#include "i2c.h"
#include "printf.h"
#include "proto.h"
#include "radio.h"

#include "epd.h"
#include "sleep.h"
#include "timer.h"
#include "wdt.h"

struct MacFrameFromMaster {
    struct MacFcs fcs;
    uint8_t seq;
    uint16_t pan;
    uint8_t dst[8];
    uint16_t from;
} __packed;

struct MacFrameNormal {
    struct MacFcs fcs;
    uint8_t seq;
    uint16_t pan;
    uint8_t dst[8];
    uint8_t src[8];
} __packed;

struct MacFrameBcast {
    struct MacFcs fcs;
    uint8_t seq;
    uint16_t dstPan;
    uint16_t dstAddr;
    uint16_t srcPan;
    uint8_t src[8];
} __packed;

#define PKT_AVAIL_DATA_REQ 0xE5
#define PKT_AVAIL_DATA_INFO 0xE6
#define PKT_BLOCK_PARTIAL_REQUEST 0xE7
#define PKT_BLOCK_REQUEST_ACK 0xE9
#define PKT_BLOCK_REQUEST 0xE4
#define PKT_BLOCK_PART 0xE8
#define PKT_XFER_COMPLETE 0xEA
#define PKT_XFER_COMPLETE_ACK 0xEB
#define PKT_CANCEL_XFER 0xEC

struct AvailDataReq {
    uint8_t checksum;
    uint8_t lastPacketLQI;  // zero if not reported/not supported to be reported
    int8_t lastPacketRSSI;  // zero if not reported/not supported to be reported
    uint8_t temperature;    // zero if not reported/not supported to be reported. else, this minus CHECKIN_TEMP_OFFSET is temp in degrees C
    uint16_t batteryMv;
    uint8_t softVer;
    uint8_t hwType;
    uint8_t protoVer;
    uint8_t buttonState;
} __packed;

#define DATATYPE_NOUPDATE 0
#define DATATYPE_IMG 1
#define DATATYPE_IMGRAW 2
#define DATATYPE_UPDATE 3

struct AvailDataInfo {
    uint8_t checksum;
    uint64_t dataVer;
    uint32_t dataSize;
    uint8_t dataType;
    uint16_t nextCheckIn;
} __packed;

struct blockPart {
    uint8_t checksum;
    uint8_t blockId;
    uint8_t blockPart;
    uint8_t data[];
} __packed;

struct blockData {
    uint16_t size;
    uint16_t checksum;
    uint8_t data[];
} __packed;

struct burstMacData {
    uint16_t offset;
    uint8_t targetMac[8];
} __packed;

#define BLOCK_PART_DATA_SIZE 99
#define BLOCK_MAX_PARTS 42
#define BLOCK_DATA_SIZE 4096
#define BLOCK_XFER_BUFFER_SIZE BLOCK_DATA_SIZE + sizeof(struct blockData)
#define BLOCK_REQ_PARTS_BYTES 6

struct blockRequest {
    uint8_t checksum;
    uint64_t ver;
    uint8_t blockId;
    uint8_t type;
    uint8_t requestedParts[BLOCK_REQ_PARTS_BYTES];
} __packed;

struct blockRequestAck {
    uint8_t checksum;
    uint16_t pleaseWaitMs;
} __packed;

#define TIMER_TICKS_PER_MS 1333UL
// #define DEBUGBLOCKS

// download-stuff
bool __xdata dataPending = true;
uint8_t __xdata blockXferBuffer[BLOCK_XFER_BUFFER_SIZE] = {0};
struct blockRequest __xdata curBlock = {0};
struct AvailDataInfo __xdata curDataInfo = {0};
uint16_t __xdata dataRemaining = 0;
bool __xdata curXferComplete = false;
bool __xdata requestPartialBlock = false;

//uint8_t __xdata *tempBuffer = blockXferBuffer;
uint8_t __xdata curImgSlot = 0;
uint32_t __xdata curHighSlotId = 0;
uint8_t __xdata nextImgSlot = 0;
uint8_t __xdata imgSlots = 0;

// doDownload persistent variables
bool __xdata lastBlock = false;
uint8_t __xdata partsThisBlock = 0;
uint8_t __xdata blockRequestAttempt = 0;
uint8_t __xdata blockValidateAttempt = 0;

// stuff we need to keep track of related to the network/AP
uint8_t __xdata APmac[8] = {0};
uint16_t __xdata APsrcPan = 0;
uint8_t __xdata mSelfMac[8] = {0};
uint8_t __xdata seq = 0;

// power saving algorithm
#define INTERVAL_BASE 40              // interval (in seconds) (when 1 packet is sent/received) for target current (7.2µA)
#define INTERVAL_AT_MAX_ATTEMPTS 600  // interval (in seconds) (at max attempts) for target average current
#define INTERVAL_NO_SIGNAL 1800       // interval (in seconds) when no answer for POWER_SAVING_SMOOTHING attempts,
                                      // (INTERVAL_AT_MAX_ATTEMPTS * POWER_SAVING_SMOOTHING) seconds
#define DATA_REQ_RX_WINDOW_SIZE 5UL   // How many milliseconds we should wait for a packet during the data_request.
                                      // If the AP holds a long list of data for tags, it may need a little more time to lookup the mac address
#define DATA_REQ_MAX_ATTEMPTS 14      // How many attempts (at most) we should do to get something back from the AP
#define POWER_SAVING_SMOOTHING 8      // How many samples we should use to smooth the data request interval
#define MINIMUM_INTERVAL 45           // IMPORTANT: Minimum interval for check-in; this determines overal battery life!

#define HAS_BUTTON  // uncomment to enable reading a push button (connect between 'TEST' en 'GND' on the tag, along with a 100nF capacitor in parallel).

uint16_t __xdata dataReqAttemptArr[POWER_SAVING_SMOOTHING] = {0};  // Holds the amount of attempts required per data_req/check-in
uint8_t __xdata dataReqAttemptArrayIndex = 0;
uint8_t __xdata dataReqLastAttempt = 0;
uint16_t __xdata nextCheckInFromAP = 0;

// buffer we use to prepare/read packets
// static uint8_t __xdata mRxBuf[130];
static uint8_t __xdata inBuffer[128] = {0};
static uint8_t __xdata outBuffer[128] = {0};

// tools
uint8_t __xdata getPacketType(void *__xdata buffer) {
    struct MacFcs *__xdata fcs = buffer;
    if ((fcs->frameType == 1) && (fcs->destAddrType == 2) && (fcs->srcAddrType == 3) && (fcs->panIdCompressed == 0)) {
        // broadcast frame
        uint8_t __xdata type = ((uint8_t *)buffer)[sizeof(struct MacFrameBcast)];
        return type;
    } else if ((fcs->frameType == 1) && (fcs->destAddrType == 3) && (fcs->srcAddrType == 3) && (fcs->panIdCompressed == 1)) {
        // normal frame
        uint8_t __xdata type = ((uint8_t *)buffer)[sizeof(struct MacFrameNormal)];
        return type;
    }
    return 0;
}
void dump(uint8_t *__xdata a, uint16_t __xdata l) {
    pr("\n        ");
#define ROWS 16
    for (uint8_t c = 0; c < ROWS; c++) {
        pr(" %02X", c);
    }
    pr("\n--------");
    for (uint8_t c = 0; c < ROWS; c++) {
        pr("---");
    }
    for (uint16_t c = 0; c < l; c++) {
        if ((c % ROWS) == 0) {
            pr("\n0x%04X | ", c);
        }
        pr("%02X ", a[c]);
    }
    pr("\n--------");
    for (uint8_t c = 0; c < ROWS; c++) {
        pr("---");
    }
    pr("\n");
}
bool checkCRC(void *p, uint8_t len) {
    uint8_t total = 0;
    for (uint8_t c = 1; c < len; c++) {
        total += ((uint8_t *)p)[c];
    }
    // pr("CRC: rx %d, calc %d\n", ((uint8_t *)p)[0], total);
    return ((uint8_t *)p)[0] == total;
}
void addCRC(void *p, uint8_t len) {
    uint8_t total = 0;
    for (uint8_t c = 1; c < len; c++) {
        total += ((uint8_t *)p)[c];
    }
    ((uint8_t *)p)[0] = total;
}

// init/sleep
void initRadio() {
    radioInit();
    radioRxFilterCfg(mSelfMac, 0x10000, PROTO_PAN_ID);
    radioSetChannel(RADIO_FIRST_CHANNEL);
    radioSetTxPower(10);
}
void killRadio() {
    radioRxEnable(false, true);
    RADIO_IRQ4_pending = 0;
    UNK_C1 &= ~0x81;
    TCON &= ~0x20;
    uint8_t __xdata cfgPg = CFGPAGE;
    CFGPAGE = 4;
    RADIO_command = 0xCA;
    RADIO_command = 0xC5;
    CFGPAGE = cfgPg;
}
void initAfterWake() {
    clockingAndIntsInit();
    timerInit();
    // partialInit();
    boardInit();
    epdEnterSleep();
    irqsOn();
    boardInitStage2();
    initRadio();
}
void doSleep(uint32_t __xdata t) {
    if (t > 1000) pr("s=%lu\n ", t / 1000);
    powerPortsDownForSleep();

#ifdef HAS_BUTTON
    // Button setup on TEST pin 1.0 (input pullup)
    P1FUNC &= ~(1 << 0);
    P1DIR |= (1 << 0);
    P1PULL |= (1 << 0);
    P1LVLSEL |= (1 << 0);
    P1INTEN = (1 << 0);
    P1CHSTA &= ~(1 << 0);
#endif

    // sleepy
    sleepForMsec(t);

#ifdef HAS_BUTTON
    P1INTEN = 0;
#endif

    initAfterWake();
}
uint16_t getNextSleep() {
    uint16_t __xdata curval = INTERVAL_AT_MAX_ATTEMPTS - INTERVAL_BASE;
    curval *= dataReqLastAttempt;
    curval /= DATA_REQ_MAX_ATTEMPTS;
    curval += INTERVAL_BASE;
    dataReqAttemptArr[dataReqAttemptArrayIndex % POWER_SAVING_SMOOTHING] = curval;
    dataReqAttemptArrayIndex++;

    uint16_t avg = 0;
    bool noNetwork = true;
    for (uint8_t c = 0; c < POWER_SAVING_SMOOTHING; c++) {
        avg += dataReqAttemptArr[c];
        if (dataReqAttemptArr[c] != INTERVAL_AT_MAX_ATTEMPTS) {
            noNetwork = false;
        }
    }
    if (noNetwork == true) return INTERVAL_NO_SIGNAL;
    avg /= POWER_SAVING_SMOOTHING;
    return avg;
}

// data xfer stuff
void sendAvailDataReq() {
    struct MacFrameBcast __xdata *txframe = (struct MacFrameBcast *)(outBuffer + 1);
    memset(outBuffer, 0, sizeof(struct MacFrameBcast) + sizeof(struct AvailDataReq) + 2 + 4);
    struct AvailDataReq *__xdata availreq = (struct AvailDataReq *)(outBuffer + 2 + sizeof(struct MacFrameBcast));
    outBuffer[0] = sizeof(struct MacFrameBcast) + sizeof(struct AvailDataReq) + 2 + 2;
    outBuffer[sizeof(struct MacFrameBcast) + 1] = PKT_AVAIL_DATA_REQ;
    memcpy(txframe->src, mSelfMac, 8);
    txframe->fcs.frameType = 1;
    txframe->fcs.ackReqd = 1;
    txframe->fcs.destAddrType = 2;
    txframe->fcs.srcAddrType = 3;
    txframe->seq = seq++;
    txframe->dstPan = 0xFFFF;
    txframe->dstAddr = 0xFFFF;
    txframe->srcPan = 0x4447;
    // TODO: send some meaningful data
    availreq->softVer = 1;
    if (P1CHSTA && (1 << 0)) {
        availreq->buttonState = 1;
        pr("button pressed\n");
        P1CHSTA &= ~(1 << 0);
    }
    addCRC(availreq, sizeof(struct AvailDataReq));
    commsTxNoCpy(outBuffer);
}
struct AvailDataInfo *__xdata getAvailDataInfo() {
    uint32_t __xdata t;
    for (uint8_t c = 0; c < DATA_REQ_MAX_ATTEMPTS; c++) {
        sendAvailDataReq();
        t = timerGet() + (TIMER_TICKS_PER_MS * DATA_REQ_RX_WINDOW_SIZE);
        while (timerGet() < t) {
            int8_t __xdata ret = commsRxUnencrypted(inBuffer);
            if (ret > 1) {
                if (getPacketType(inBuffer) == PKT_AVAIL_DATA_INFO) {
                    if (checkCRC(inBuffer + sizeof(struct MacFrameNormal) + 1, sizeof(struct AvailDataInfo))) {
                        struct MacFrameNormal *__xdata f = (struct MacFrameNormal *)inBuffer;
                        memcpy(APmac, f->src, 8);
                        APsrcPan = f->pan;
                        // pr("RSSI: %d\n", commsGetLastPacketRSSI());
                        // pr("LQI: %d\n", commsGetLastPacketLQI());
                        dataReqLastAttempt = c;
                        return (struct AvailDataInfo *)(inBuffer + sizeof(struct MacFrameNormal) + 1);
                    }
                }
            }
        }
    }
    dataReqLastAttempt = DATA_REQ_MAX_ATTEMPTS;
    return NULL;
}
bool processBlockPart(struct blockPart *bp) {
    uint16_t __xdata start = bp->blockPart * BLOCK_PART_DATA_SIZE;
    uint16_t __xdata size = BLOCK_PART_DATA_SIZE;
    // validate if it's okay to copy data
    if (bp->blockId != curBlock.blockId) {
        // pr("got a packet for block %02X\n", bp->blockId);
        return false;
    }
    if (start >= (sizeof(blockXferBuffer) - 1)) return false;
    if (bp->blockPart > BLOCK_MAX_PARTS) return false;
    if ((start + size) > sizeof(blockXferBuffer)) {
        size = sizeof(blockXferBuffer) - start;
    }
    if (checkCRC(bp, sizeof(struct blockPart) + BLOCK_PART_DATA_SIZE)) {
        //  copy block data to buffer
        xMemCopy((void *)(blockXferBuffer + start), (const void *)bp->data, size);
        // we don't need this block anymore, set bit to 0 so we don't request it again
        curBlock.requestedParts[bp->blockPart / 8] &= ~(1 << (bp->blockPart % 8));
        return true;
    } else {
        return false;
    }
}
bool blockRxLoop(uint32_t timeout) {
    uint32_t __xdata t;
    bool success = false;
    radioRxEnable(true, true);
    t = timerGet() + (TIMER_TICKS_PER_MS * (timeout + 20));
    while (timerGet() < t) {
        int8_t __xdata ret = commsRxUnencrypted(inBuffer);
        if (ret > 1) {
            if (getPacketType(inBuffer) == PKT_BLOCK_PART) {
                struct blockPart *bp = (struct blockPart *)(inBuffer + sizeof(struct MacFrameNormal) + 1);
                success = processBlockPart(bp);
            }
        }
    }
    radioRxEnable(false, true);
    radioRxFlush();
    return success;
}
struct blockRequestAck *__xdata continueToRX() {
    struct blockRequestAck *ack = (struct blockRequestAck *)(inBuffer + sizeof(struct MacFrameNormal) + 1);
    ack->pleaseWaitMs = 0;
    return ack;
}
void sendBlockRequest() {
    memset(outBuffer, 0, sizeof(struct MacFrameNormal) + sizeof(struct blockRequest) + 2 + 2);
    struct MacFrameNormal *__xdata f = (struct MacFrameNormal *)(outBuffer + 1);
    struct blockRequest *__xdata blockreq = (struct blockRequest *)(outBuffer + 2 + sizeof(struct MacFrameNormal));
    outBuffer[0] = sizeof(struct MacFrameNormal) + sizeof(struct blockRequest) + 2 + 2;
    if (requestPartialBlock) {
        ;
        outBuffer[sizeof(struct MacFrameNormal) + 1] = PKT_BLOCK_PARTIAL_REQUEST;
    } else {
        outBuffer[sizeof(struct MacFrameNormal) + 1] = PKT_BLOCK_REQUEST;
    }
    memcpy(f->src, mSelfMac, 8);
    memcpy(f->dst, APmac, 8);
    f->fcs.frameType = 1;
    f->fcs.secure = 0;
    f->fcs.framePending = 0;
    f->fcs.ackReqd = 0;
    f->fcs.panIdCompressed = 1;
    f->fcs.destAddrType = 3;
    f->fcs.frameVer = 0;
    f->fcs.srcAddrType = 3;
    f->seq = seq++;
    f->pan = APsrcPan;
    memcpy(blockreq, &curBlock, sizeof(struct blockRequest));
    // pr("req ver: %02X%02X%02X%02X%02X%02X%02X%02X\n", ((uint8_t*)&blockreq->ver)[0],((uint8_t*)&blockreq->ver)[1],((uint8_t*)&blockreq->ver)[2],((uint8_t*)&blockreq->ver)[3],((uint8_t*)&blockreq->ver)[4],((uint8_t*)&blockreq->ver)[5],((uint8_t*)&blockreq->ver)[6],((uint8_t*)&blockreq->ver)[7]);
    addCRC(blockreq, sizeof(struct blockRequest));
    commsTxNoCpy(outBuffer);
}
struct blockRequestAck *__xdata performBlockRequest() {
    uint32_t __xdata t;
    radioRxEnable(true, true);
    radioRxFlush();
    for (uint8_t c = 0; c < 30; c++) {
        sendBlockRequest();
        t = timerGet() + (TIMER_TICKS_PER_MS * (7UL + c / 10));
        do {
            int8_t __xdata ret = commsRxUnencrypted(inBuffer);
            if (ret > 1) {
                switch (getPacketType(inBuffer)) {
                    case PKT_BLOCK_REQUEST_ACK:
                        if (checkCRC((inBuffer + sizeof(struct MacFrameNormal) + 1), sizeof(struct blockRequestAck)))
                            return (struct blockRequestAck *)(inBuffer + sizeof(struct MacFrameNormal) + 1);
                        break;
                    case PKT_BLOCK_PART:
                        // block already started while we were waiting for a get block reply
                        // pr("!");
                        // processBlockPart((struct blockPart *)(inBuffer + sizeof(struct MacFrameNormal) + 1));
                        return continueToRX();
                        break;
                    case PKT_CANCEL_XFER:
                        return NULL;
                    default:
                        pr("pkt w/type %02X\n", getPacketType(inBuffer));
                        break;
                }
            }

        } while (timerGet() < t);
    }
    return continueToRX();
    // return NULL;
}
void sendXferCompletePacket() {
    memset(outBuffer, 0, sizeof(struct MacFrameNormal) + 2 + 4);
    struct MacFrameNormal *__xdata f = (struct MacFrameNormal *)(outBuffer + 1);
    outBuffer[0] = sizeof(struct MacFrameNormal) + 2 + 2;
    outBuffer[sizeof(struct MacFrameNormal) + 1] = PKT_XFER_COMPLETE;
    memcpy(f->src, mSelfMac, 8);
    memcpy(f->dst, APmac, 8);
    f->fcs.frameType = 1;
    f->fcs.secure = 0;
    f->fcs.framePending = 0;
    f->fcs.ackReqd = 0;
    f->fcs.panIdCompressed = 1;
    f->fcs.destAddrType = 3;
    f->fcs.frameVer = 0;
    f->fcs.srcAddrType = 3;
    f->pan = APsrcPan;
    f->seq = seq++;
    commsTxNoCpy(outBuffer);
}
void sendXferComplete() {
    radioRxEnable(true, true);

    for (uint8_t c = 0; c < 8; c++) {
        sendXferCompletePacket();
        uint32_t __xdata start = timerGet();
        while ((timerGet() - start) < (TIMER_TICKS_PER_MS * 6UL)) {
            int8_t __xdata ret = commsRxUnencrypted(inBuffer);
            if (ret > 1) {
                if (getPacketType(inBuffer) == PKT_XFER_COMPLETE_ACK) {
                    pr("XFC ACK\n");
                    return;
                }
            }
        }
    }
    pr("XFC NACK!\n");
    return;
}
bool validateBlockData() {
    struct blockData *bd = (struct blockData *)blockXferBuffer;
    // pr("expected len = %04X, checksum=%04X\n", bd->size, bd->checksum);
    uint16_t t = 0;
    for (uint16_t c = 0; c < bd->size; c++) {
        t += bd->data[c];
    }
    return bd->checksum == t;
}

// EEprom related stuff
uint32_t getAddressForSlot(uint8_t s) {
    return EEPROM_IMG_START + (EEPROM_IMG_EACH * s);
}
void getNumSlots() {
    uint32_t eeSize = eepromGetSize();
    uint16_t nSlots = mathPrvDiv32x16(eeSize - EEPROM_IMG_START, EEPROM_IMG_EACH >> 8) >> 8;
    if (eeSize < EEPROM_IMG_START || !nSlots) {
        pr("eeprom is too small\n");
        while (1)
            ;
    } else if (nSlots >> 8) {
        pr("eeprom is too big, some will be unused\n");
        imgSlots = 254;
    } else
        imgSlots = nSlots;
}
uint8_t findSlot(uint8_t *__xdata ver) {
    // return 0xFF;  // remove me! This forces the tag to re-download each and every upload without checking if it's already in the eeprom somewhere
    uint32_t __xdata markerValid = EEPROM_IMG_VALID;
    for (uint8_t __xdata c = 0; c < imgSlots; c++) {
        struct EepromImageHeader __xdata *eih = (struct EepromImageHeader __xdata *)blockXferBuffer;
        eepromRead(getAddressForSlot(c), eih, sizeof(struct EepromImageHeader));
        if (xMemEqual4(&eih->validMarker, &markerValid)) {
            if (xMemEqual(&eih->version, (void *)ver, 8)) {
                return c;
            }
        }
    }
    return 0xFF;
}
void eraseUpdateBlock() {
    eepromErase(EEPROM_UPDATA_AREA_START, EEPROM_UPDATE_AREA_LEN / EEPROM_ERZ_SECTOR_SZ);
}
void saveUpdateBlockData(uint8_t blockId) {
    if (!eepromWrite(EEPROM_UPDATA_AREA_START + (blockId * BLOCK_DATA_SIZE), blockXferBuffer + sizeof(struct blockData), BLOCK_DATA_SIZE))
        pr("EEPROM write failed\n");
}
void saveImgBlockData(uint8_t blockId) {
    uint16_t length = EEPROM_IMG_EACH - (sizeof(struct EepromImageHeader) + (blockId * BLOCK_DATA_SIZE));
    if (length > 4096) length = 4096;

    if (!eepromWrite(getAddressForSlot(curImgSlot) + sizeof(struct EepromImageHeader) + (blockId * BLOCK_DATA_SIZE), blockXferBuffer + sizeof(struct blockData), length))
        pr("EEPROM write failed\n");
}
void drawImageFromEeprom() {
    // enable WDT, to make sure de tag resets if it's for some reason unable to draw the image
    wdtSetResetVal(0xFFFFFFFF - 0x38C340);
    wdtOn();
    drawImageAtAddress(getAddressForSlot(curImgSlot));
    //   adcSampleBattery();
    initRadio();
}
uint32_t getHighSlotId() {
    uint32_t temp = 0;
    uint32_t __xdata markerValid = EEPROM_IMG_VALID;
    for (uint8_t __xdata c = 0; c < imgSlots; c++) {
        struct EepromImageHeader __xdata *eih = (struct EepromImageHeader __xdata *)blockXferBuffer;
        eepromRead(getAddressForSlot(c), eih, sizeof(struct EepromImageHeader));
        if (xMemEqual4(&eih->validMarker, &markerValid)) {
            if (temp < eih->id) {
                temp = eih->id;
                nextImgSlot = c;
            }
        }
    }
    pr("found high id=%lu in slot %d\n", temp, nextImgSlot);
    return temp;
}

// #define DEBUGBLOCKS
//  Main download function
bool doDataDownload(struct AvailDataInfo *__xdata avail) {
    // this is the main function for the download process

    if (!eepromInit()) {  // we'll need the eeprom here, init it.
        pr("failed to init eeprom\n");
        return false;
    }

    // GET AVAIL DATA INFO - enable the radio and get data
    if (avail == NULL) {  // didn't receive a reply to get info about the data, we'll resync and try again later
#ifdef DEBUGBLOCKS
        pr("didn't receive getavaildatainfo");
#endif
        return false;
    }

    // did receive available data info (avail struct)
    switch (avail->dataType) {
        case DATATYPE_IMG:
        case DATATYPE_IMGRAW:
            // check if this download is currently displayed or active
            if (curXferComplete && xMemEqual((const void *__xdata) & avail->dataVer, (const void *__xdata) & curDataInfo.dataVer, 8)) {
                // we've downloaded this already, we're guessing it's already displayed
                pr("old ver, already downloaded!\n");
                sendXferComplete();
                return true;
            } else {
                // check if we've seen this version before
                curImgSlot = findSlot(&(avail->dataVer));
                if (curImgSlot != 0xFF) {
                    // found a (complete)valid image slot for this version
                    sendXferComplete();
                    killRadio();

                    pr("already seen, drawing from eeprom slot %d\n", curImgSlot);

                    // mark as completed and draw from EEPROM
                    curXferComplete = true;
                    xMemCopyShort(&curDataInfo, (void *)avail, sizeof(struct AvailDataInfo));

                    drawImageFromEeprom();
                    return true;
                } else {
                    // not found in cache, prepare to download
                    // go to the next image slot
                    nextImgSlot++;
                    if (nextImgSlot >= imgSlots) nextImgSlot = 0;
                    curImgSlot = nextImgSlot;

                    eepromErase(getAddressForSlot(curImgSlot), EEPROM_IMG_EACH / EEPROM_ERZ_SECTOR_SZ);
                    pr("new download, writing to slot %d\n", curImgSlot);
                    // continue!
                }
            }
            break;
        case DATATYPE_UPDATE:
            pr("received firmware!\n");
            eepromErase(EEPROM_UPDATA_AREA_START, EEPROM_UPDATE_AREA_LEN / EEPROM_ERZ_SECTOR_SZ);
            break;
    }

    // prepare for download
    curXferComplete = false;
    curBlock.blockId = 0;
    xMemCopy8(&(curBlock.ver), &(avail->dataVer));
    curBlock.type = avail->dataType;
    xMemCopyShort(&curDataInfo, (void *)avail, sizeof(struct AvailDataInfo));
    dataRemaining = curDataInfo.dataSize;  // this was + 2, and I can't remember why. It works fine without it, so I don't know....

    // set requested parts - check if the transfer is contained in this block
    if (dataRemaining > BLOCK_DATA_SIZE) {
        // full block, not last
        lastBlock = false;
        partsThisBlock = BLOCK_MAX_PARTS;
        memset(curBlock.requestedParts, 0xFF, BLOCK_REQ_PARTS_BYTES);
    } else {
        // final block, probably partial
        lastBlock = true;
        partsThisBlock = dataRemaining / BLOCK_PART_DATA_SIZE;
        if (dataRemaining % BLOCK_PART_DATA_SIZE) partsThisBlock++;
        memset(curBlock.requestedParts, 0x00, BLOCK_REQ_PARTS_BYTES);
        for (uint8_t c = 0; c < partsThisBlock; c++) {
            curBlock.requestedParts[c / 8] |= (1 << (c % 8));
        }
    }

    // do transfer!
    blockRequestAttempt = 0;
    blockValidateAttempt = 0;
    while (!curXferComplete) {
        // this while loop loops until the transfer has been completed, or we get tired for other reasons
    startdownload:;
#ifndef DEBUGBLOCKS
        pr("REQ %d ", curBlock.blockId);
#endif
#ifdef DEBUGBLOCKS
        pr("REQ %d[", curBlock.blockId);

        for (uint8_t c = 0; c < BLOCK_MAX_PARTS; c++) {
            if ((c != 0) && (c % 8 == 0)) pr("][");
            if (curBlock.requestedParts[c / 8] & (1 << (c % 8))) {
                pr("R");
            } else {
                pr("_");
            }
        }
        pr("]\n");
#endif

        // timerDelay(TIMER_TICKS_PER_MS*100);

        // DO BLOCK REQUEST - request a block, get an ack with timing info (hopefully)
        struct blockRequestAck *__xdata ack = performBlockRequest();
        if (ack == NULL) {
            pr("Cancelled request\n");
            return false;
        } else {
            // got an ack!
        }
        // SLEEP - until the AP is ready with the data
        if (ack->pleaseWaitMs) {
            if (ack->pleaseWaitMs < 35) {
                timerDelay(ack->pleaseWaitMs * TIMER_TICKS_PER_MS);
            } else {
                doSleep(ack->pleaseWaitMs - 10);
                radioRxEnable(true, true);
            }
        } else {
            // immediately start with the reception of the block data
        }
        // BLOCK RX LOOP - receive a block, until the timeout has passed
        if (!blockRxLoop(270)) {  // was 300
            // didn't receive packets
            blockRequestAttempt++;
            if (blockRequestAttempt > 5) {
                pr("bailing on download, 0 blockparts rx'd\n");
                return false;
            }
        } else {
            // successfull block RX loop
            blockRequestAttempt = 0;
        }

#ifdef DEBUGBLOCKS
        pr("RX  %d[", curBlock.blockId);
        for (uint8_t c = 0; c < BLOCK_MAX_PARTS; c++) {
            if ((c != 0) && (c % 8 == 0)) pr("][");
            if (curBlock.requestedParts[c / 8] & (1 << (c % 8))) {
                pr(".");
            } else {
                pr("R");
            }
        }
        pr("]\n");
#endif

        // check if we got all the parts we needed, e.g: has the block been completed?
        bool blockComplete = true;
        for (uint8_t c = 0; c < partsThisBlock; c++) {
            if (curBlock.requestedParts[c / 8] & (1 << (c % 8))) blockComplete = false;
        }

        if (blockComplete) {
#ifndef DEBUGBLOCKS
            pr("- COMPLETE\n");
#endif
            if (validateBlockData()) {
                // checked and found okay
                requestPartialBlock = false;  // next block is going to be requested from the ESP32 by the AP
                blockValidateAttempt = 0;
                switch (curBlock.type) {
                    case DATATYPE_IMG:
                    case DATATYPE_IMGRAW:
                        saveImgBlockData(curBlock.blockId);
                        break;
                    case DATATYPE_UPDATE:
                        saveUpdateBlockData(curBlock.blockId);
                        break;
                }
            } else {
                // block checked, but failed validation. Mark all parts for this block as 'request'
                blockValidateAttempt++;
                if (blockValidateAttempt > 5) {
                    pr("bailing on download, 0 blockparts rx'd\n");
                    return false;
                }
                for (uint8_t c = 0; c < partsThisBlock; c++) {
                    curBlock.requestedParts[c / 8] |= (1 << (c % 8));
                }
                blockComplete = false;
                requestPartialBlock = false;
                pr("block failed validation!\n");
            }
        } else {
#ifndef DEBUGBLOCKS
            pr("- INCOMPLETE\n");
#endif
            // block incomplete, re-request a partial block
            requestPartialBlock = true;
        }

        if (blockComplete) {
            if (!lastBlock) {
                // Not the last block! check what the next block is going to be
                curBlock.blockId++;
                dataRemaining -= BLOCK_DATA_SIZE;
                if (dataRemaining > BLOCK_DATA_SIZE) {
                    // full block-size
                    partsThisBlock = BLOCK_MAX_PARTS;
                    memset(curBlock.requestedParts, 0xFF, BLOCK_REQ_PARTS_BYTES);
                    lastBlock = false;
                } else {
                    // final block, probably partial
                    partsThisBlock = dataRemaining / BLOCK_PART_DATA_SIZE;
                    if (dataRemaining % BLOCK_PART_DATA_SIZE) partsThisBlock++;
                    memset(curBlock.requestedParts, 0x00, BLOCK_REQ_PARTS_BYTES);
                    for (uint8_t c = 0; c < partsThisBlock; c++) {
                        curBlock.requestedParts[c / 8] |= (1 << (c % 8));
                    }
                    lastBlock = true;
                }

            } else {
                // this was the last block. What should we do next?
                switch (curBlock.type) {
                    case DATATYPE_IMG:
                    case DATATYPE_IMGRAW:;
                        // transfer complete. Save data info and mark data in image slot as 'valid'
                        struct EepromImageHeader __xdata *eih = (struct EepromImageHeader __xdata *)blockXferBuffer;
                        xMemCopy8(&eih->version, &curDataInfo.dataVer);
                        eih->size = curDataInfo.dataSize;
                        eih->validMarker = EEPROM_IMG_VALID;
                        eih->id = ++curHighSlotId;
                        eepromWrite(getAddressForSlot(curImgSlot), eih, sizeof(struct EepromImageHeader));
                        // pr("transfer complete!");
                        curXferComplete = true;
                        sendXferComplete();
                        killRadio();
                        drawImageFromEeprom();
                        curDataInfo.dataVer = 0xAA;
                        break;
                    case DATATYPE_UPDATE:
                        pr("firmware download complete, doing update.\n");
                        curXferComplete = true;
                        sendXferComplete();
                        killRadio();
                        eepromReadStart(EEPROM_UPDATA_AREA_START);
                        // wdtDeviceReset();
                        selfUpdate();
                        break;
                }
            }
        } else {
            // incomplete block, wrap around and get the rest of the block...
        }
    }  // end download while loop
    return true;
}

// main loop;
void mainProtocolLoop(void) {
    clockingAndIntsInit();
    timerInit();
    boardInit();

    if (!boardGetOwnMac(mSelfMac)) {
        pr("failed to get MAC. Aborting\n");
        while (1)
            ;
    } else {
        /*
        for (uint8_t c = 0; c < 8; c++) {
            mSelfMac[c] = c + 5;
        }
        */
        // really... if I do the call below, it'll cost me 8 bytes IRAM. Not the kind of 'optimization' I ever dreamed of doing
        // pr("MAC>%02X%02X%02X%02X%02X%02X%02X%02X\n", mSelfMac[0], mSelfMac[1], mSelfMac[2], mSelfMac[3], mSelfMac[4], mSelfMac[5], mSelfMac[6], mSelfMac[7]);
        pr("MAC>%02X%02X", mSelfMac[0], mSelfMac[1]);
        pr("%02X%02X", mSelfMac[2], mSelfMac[3]);
        pr("%02X%02X", mSelfMac[4], mSelfMac[5]);
        pr("%02X%02X\n", mSelfMac[6], mSelfMac[7]);
    }

    irqsOn();
    boardInitStage2();
    // i2ctest();

    pr("BOOTED> (new epd driver!!!)\n\n");

    if (!eepromInit()) {
        pr("failed to init eeprom\n");
        while (1)
            ;
    } else {
        getNumSlots();
        curHighSlotId = getHighSlotId();
    }

    // initialize attempt-array with the default value;
    for (uint8_t c = 0; c < POWER_SAVING_SMOOTHING; c++) {
        dataReqAttemptArr[c] = INTERVAL_BASE;
    }

    epdEnterSleep();
    eepromDeepPowerDown();
    initRadio();

    P1CHSTA &= ~(1 << 0);

    while (1) {
        radioRxEnable(true, true);

        struct AvailDataInfo *__xdata avail = getAvailDataInfo();
        if (avail == NULL) {
            // no data :(
            nextCheckInFromAP = 0;  // let the power-saving algorithm determine the next sleep period
        } else {
            nextCheckInFromAP = avail->nextCheckIn;
            // got some data from the AP!
            if (avail->dataType != DATATYPE_NOUPDATE) {
                // data transfer
                if (doDataDownload(avail)) {
                    // succesful transfer, next wake time is determined by the NextCheckin;
                } else {
                    // failed transfer, let the algorithm determine next sleep interval (not the AP)
                    nextCheckInFromAP = 0;
                }
            } else {
                // no data transfer, just sleep.
            }
        }

        // if the AP told us to sleep for a specific period, do so.
        if (nextCheckInFromAP) {
            doSleep(nextCheckInFromAP * 60000UL);
        } else {
            doSleep(getNextSleep() * 1000UL);
        }
    }
}