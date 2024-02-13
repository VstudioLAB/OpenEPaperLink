#include <Arduino.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lut.h"
#include "settings.h"
#include "hal.h"
#include "wdt.h"

#include "drawing.h"
#include "unissd.h"

#define CMD_DRV_OUTPUT_CTRL 0x01
#define CMD_SOFT_START_CTRL 0x0C
#define CMD_ENTER_SLEEP 0x10
#define CMD_DATA_ENTRY_MODE 0x11
#define CMD_SOFT_RESET 0x12
#define CMD_SOFT_RESET2 0x13
#define CMD_SETUP_VOLT_DETECT 0x15
#define CMD_TEMP_SENSOR_CONTROL 0x18
#define CMD_ACTIVATION 0x20
#define CMD_DISP_UPDATE_CTRL 0x21
#define CMD_DISP_UPDATE_CTRL2 0x22
#define CMD_WRITE_FB_BW 0x24
#define CMD_WRITE_FB_RED 0x26
#define CMD_VCOM_GLITCH_CTRL 0x2B
#define CMD_LOAD_OTP_LUT 0x31
#define CMD_WRITE_LUT 0x32
#define CMD_BORDER_WAVEFORM_CTRL 0x3C
#define CMD_WINDOW_X_SIZE 0x44
#define CMD_WINDOW_Y_SIZE 0x45
#define CMD_WRITE_PATTERN_RED 0x46
#define CMD_WRITE_PATTERN_BW 0x47
#define CMD_XSTART_POS 0x4E
#define CMD_YSTART_POS 0x4F
#define CMD_ANALOG_BLK_CTRL 0x74
#define CMD_DIGITAL_BLK_CTRL 0x7E

#define SCREEN_CMD_CLOCK_ON 0x80
#define SCREEN_CMD_CLOCK_OFF 0x01
#define SCREEN_CMD_ANALOG_ON 0x40
#define SCREEN_CMD_ANALOG_OFF 0x02
#define SCREEN_CMD_LATCH_TEMPERATURE_VAL 0x20
#define SCREEN_CMD_LOAD_LUT 0x10
#define SCREEN_CMD_USE_MODE_2 0x08  // modified commands 0x10 and 0x04
#define SCREEN_CMD_REFRESH 0xC7

/*0x11, 0x00
            0x91, 0x01
            0x44, 0x31, 0x00
            0x45, 0x0F, 0x01, 0x00, 0x00
            0x4E, 0x31
            0x4F, 0x0F, 0x01
            0xC4, 0x00, 0x31
            0xC5, 0x0F, 0x01, 0x00, 0x00
            0xCE, 0x00
            0xCF, 0x0F, 0x01, 0x3C, 0x01*/

void unissd::selectLUT(uint8_t lut) {
    // implement alternative LUTs here. Currently just reset the watchdog to two minutes,
    // to ensure it doesn't reset during the much longer bootup procedure
    lut += 1;  // make the compiler a happy camper
    wdt120s();
    return;
}

void unissd::epdEnterSleep() {
    /*digitalWrite(EPD_RST, LOW);
    delay(10);
    digitalWrite(EPD_RST, HIGH);
    delay(50);
    //epdWrite(CMD_SOFT_RESET2, 0);
    epdWrite(CMD_SOFT_RESET, 0);
    epdBusyWaitFalling(15);
    //epdWrite(CMD_ENTER_SLEEP, 1, 0x03);*/
    epdWrite(CMD_ENTER_SLEEP, 1, 0x01);

}
void unissd::epdSetup() {
    
    printf("init epd...\n");
    epdReset();
    //epdWrite(CMD_SOFT_RESET, 0);
    delay(10);
    switch (this->controllerType) {
        case 0x0F:
        case 0x12:
        case 0x15:
            // stock init 1.6"
            /*epdWrite(CMD_DRV_OUTPUT_CTRL, 3, this->effectiveYRes & 0xFF, this->effectiveYRes >> 8, 0x00);
            epdWrite(CMD_DATA_ENTRY_MODE, 1, 0x01);
            epdWrite(CMD_WINDOW_X_SIZE, 2, this->XOffset / 8, ((this->XOffset + this->effectiveXRes) / 8) - 1);
            epdWrite(CMD_WINDOW_Y_SIZE, 4, (this->YOffset + this->effectiveYRes) & 0xFF, (this->YOffset + this->effectiveYRes) >> 8, this->YOffset & 0xFF, this->YOffset >> 8);
            
            epdWrite(CMD_BORDER_WAVEFORM_CTRL, 1, 0x05);
            epdWrite(CMD_TEMP_SENSOR_CONTROL, 1, 0x80);
            */
            // end stock init
            // added
            //epdWrite(CMD_DISP_UPDATE_CTRL, 2, 0x08, 0x00);  // fix reversed image with stock setup


            /* 5.85 INIT SEQUENCE
            0x11, 0x00
            0x91, 0x01
            0x44, 0x31, 0x00
            0x45, 0x0F, 0x01, 0x00, 0x00
            0x4E, 0x31
            0x4F, 0x0F, 0x01
            0xC4, 0x00, 0x31
            0xC5, 0x0F, 0x01, 0x00, 0x00
            0xCE, 0x00
            0xCF, 0x0F, 0x01, 0x3C, 0x01
            
            */
           /* 5.85 BW INIT SEQUENCE
            0x11, 0x00
            0x91, 0x01
            0x44, 0x31, 0x00
            0x45, 0x0F, 0x01, 0x00, 0x00
            0x4E, 0x31
            0x4F, 0x0F, 0x01
            0xC4, 0x00, 0x31
            0xC5, 0x0F, 0x01, 0x00, 0x00
            0xCE, 0x00
            0xCF, 0x0F, 0x01, 0x3C, 0x01
            
            */


            epdWrite(0x11, 1, 0x00);
            epdWrite(0x91, 1, 0x01);
            epdWrite(0x21, 2, 0x08, 0x10);
            //epdWrite(CMD_WINDOW_X_SIZE, 2, this->XOffset / 8, ((this->XOffset + this->effectiveXRes) / 8) - 1);
            //epdWrite(CMD_WINDOW_Y_SIZE, 4, (this->YOffset + this->effectiveYRes) & 0xFF, (this->YOffset + this->effectiveYRes) >> 8, this->YOffset & 0xFF, this->YOffset >> 8);
            epdWrite(0x44, 2, 0x31, 0x00);
            epdWrite(0x45, 4, 0x0F, 0x01, 0x00, 0x00);
            //epdWrite(0x4E, 2, this->XOffset / 8, ((this->XOffset + this->effectiveXRes) / 8) - 1);
            //epdWrite(0x4F, 4, (this->YOffset + this->effectiveYRes) & 0xFF, (this->YOffset + this->effectiveYRes) >> 8, this->YOffset & 0xFF, this->YOffset >> 8);
            epdWrite(0x4E, 1, 0x31);
            epdWrite(0x4F, 2, 0x0F, 0x01);
            epdWrite(0xC4, 2, 0x00, 0x31);
            epdWrite(0xC5, 4, 0x0F, 0x01, 0x00, 0x00);
            epdWrite(0xCE, 1, 0x00);
            epdWrite(0xCF, 4, 0x0F, 0x01, 0x3C, 0x01);
            
            //epdWrite(CMD_DISP_UPDATE_CTRL, 2, 0x08, 0x00);  // fix reversed image with stock setup


            break;
        case 0x19:
            // stock init 9.7"
            epdWrite(0x46, 1, 0xF7);
            delay(15);
            epdWrite(0x47, 1, 0xF7);
            delay(15);
            epdWrite(0x0C, 5, 0xAE, 0xC7, 0xC3, 0xC0, 0x80);
            epdWrite(0x01, 3, 0x9F, 0x02, 0x00);
            epdWrite(0x11, 1, 0x02);
            epdWrite(0x44, 4, 0xBF, 0x03, 0x00, 0x00);
            epdWrite(0x45, 4, 0x00, 0x00, 0x9F, 0x02);
            epdWrite(0x3C, 1, 0x01);
            epdWrite(0x18, 1, 0x80);
            epdWrite(0x22, 1, 0xF7);
            // end stock init
            // added
            //epdWrite(CMD_DISP_UPDATE_CTRL, 2, 0x08, 0x00);  // fix reversed image with stock setup
            break;
    }
}

void unissd::epdWriteDisplayData() {
    printf("getting ready to draw\n");

    //epd_cmd(0x24);

    // this display expects two entire framebuffers worth of data to be written, one for b/w and one for red
    if(epd->epdMirrorV){
        printf("unissd: uses epdMirorV\n");
    }else{
        printf("uniss: epdMirorV is false\n");
    }
    if(epd->epdMirrorH){
        printf("unissd: uses epdMirorH\n");
    }else{
        printf("uniss: epdMirorH is false\n");
    }
    
    uint8_t *buf[2] = {0, 0};  // this will hold pointers to odd/even data lines
    uint8_t *buf_b[2] = {0, 0};  // this will hold pointers to odd/even data lines
    for (uint8_t c = 0; c < 4; c++) {
        printf("boucle for iteration %d\n", c);
        switch (this->controllerType) {
            case 0x0F:
            case 0x12:
            case 0x15:
                //epdWrite(CMD_XSTART_POS, 1, (this->XOffset / 8));
                //epdWrite(CMD_YSTART_POS, 2, (this->YOffset + this->effectiveYRes) & 0xFF, (this->YOffset + this->effectiveYRes) >> 8);
                break;
            case 0x19:
                epdWrite(CMD_XSTART_POS, 2, 0xBF, 0x03);
                epdWrite(CMD_YSTART_POS, 2, 0x00, 0x00);
                break;
        }
        if (c == 0) epd_cmd(0x24);//RED
        if (c == 1) epd_cmd(0x26);//BW
        if (c == 2) epd_cmd(0xA4);//Red 2
        if (c == 3) epd_cmd(0xA6);//BW 2
        delay(10);
        markData();
        epdSelect();
        for (uint16_t curY = 0; curY < epd->effectiveYRes; curY += 2) {
            // Get 'even' screen line
            buf[0] = (uint8_t *)calloc(epd->effectiveXRes / 8, 1);
            //buf_b[0] = (uint8_t *)calloc(epd->effectiveXRes / 16, 1);

            if (epd->epdMirrorV) {
                drawItem::renderDrawLine(buf[0], (epd->effectiveYRes - 1) - curY, c%2);
            } else {
                drawItem::renderDrawLine(buf[0], curY, c%2);
            }
            if (epd->epdMirrorH) drawItem::reverseBytes(buf[0], epd->effectiveXRes / 8);
            // on the first pass, the second (buf[1]) buffer is unused, so we don't have to wait for it to flush to the display / free it
            if (buf[1]) {
                // wait for 'odd' display line to finish writing to the screen
                epdSPIWait();
                free(buf[1]);
            }

            // start transfer of even data line to the screen
            if(c<2){
                epdSPIAsyncWrite(buf[0]+49, 50);//(epd->effectiveXRes / 8));
            }else{
                //cpyHalf(buf_b[0], buf[0], (epd->effectiveXRes / 16), (epd->effectiveXRes / 8));
                epdSPIAsyncWrite(buf[0], 50);//(epd->effectiveXRes / 8));
            }
            // Get 'odd' screen display line
            buf[1] = (uint8_t *)calloc(epd->effectiveXRes / 8, 1);
            //buf_b[1] = (uint8_t *)calloc(epd->effectiveXRes / 16, 1);
            if (epd->epdMirrorV) {
                drawItem::renderDrawLine(buf[1], (epd->effectiveYRes - 1) - (curY + 1), c%2);
            } else {
                drawItem::renderDrawLine(buf[1], curY + 1, c%2);
            }
            if (epd->epdMirrorH) drawItem::reverseBytes(buf[1], epd->effectiveXRes / 8);

            // wait until the 'even' data has finished writing
            epdSPIWait();
            free(buf[0]);
            free(buf_b[0]);

            // start transfer of the 'odd' data line
            //epdSPIAsyncWrite(buf[1], (epd->effectiveXRes / 8));
            if(c<2){
                epdSPIAsyncWrite(buf[1]+49, 50);//(epd->effectiveXRes / 8));
            }else{
                //cpyHalf(buf_b[1], buf[1], (epd->effectiveXRes / 16), (epd->effectiveXRes / 8));
                epdSPIAsyncWrite(buf[1], 50);//(epd->effectiveXRes / 8));
            }
        }
        // check if this was the first pass. If it was, we'll need to wait until the last display line finished writing
        if (c == 0 || c==2) {
            epdSPIWait();
            epdDeselect();
            free(buf[1]);
            free(buf_b[1]);
            buf[1] = nullptr;
            buf_b[1] = nullptr;
        }
    }
    // flush the draw list, make sure items don't appear on subsequent screens
    drawItem::flushDrawItems();

    // wait until the last line of display has finished writing and clean our stuff up
    epdSPIWait();
    epdDeselect();
    if (buf[1]) free(buf[1]);
}

void unissd::draw() {
    drawNoWait();
    getVoltage();
    epdBusyWaitFalling(120000);
}
void unissd::drawNoWait() {
    epdWriteDisplayData();
    epdWrite(CMD_DISP_UPDATE_CTRL2, 1, 0xF7);
    epdWrite(CMD_ACTIVATION, 0);
}
void unissd::epdWaitRdy() {
    epdBusyWaitFalling(120000);
}



void unissd::cpyHalf(uint8_t* target, uint8_t* source, u_int targetLen, u_int sourceLen){
    for(int i = targetLen; i<=0; i--){
        //target[i] = source[i+(sourceLen-targetLen)];
    }
}
