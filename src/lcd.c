/*
 * lcd.c - Display handling for T-962 reflow controller
 *
 * Copyright (C) 2010,2012,2013,2014 Werner Johansson, wj@unifiedengineering.se
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "LPC214x.h"
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "lcd.h"
#include "smallfont.h"
#include "timer.h"

// Frame buffer storage (each "page" is 8 pixels high)
uint8_t FB[FB_HEIGHT/8][FB_WIDTH];

typedef struct __attribute__ ((packed)) {
	uint8_t bfType[2]; // 'BM' only in this case
	uint32_t bfSize; // Total size
	uint16_t bfReserved[2];
	uint32_t bfOffBits; // Pixel start byte
	uint32_t biSize; // 40 bytes for BITMAPINFOHEADER
	int32_t biWidth; // Image width in pixels
	int32_t biHeight; // Image height in pixels (if negative image is right-side-up)
	uint16_t biPlanes; // Must be 1
	uint16_t biBitCount; // Must be 1
	uint32_t biCompression; // Only 0 (uncompressed) supported at the moment
	uint32_t biSizeImage; // Pixel data size in bytes
	int32_t biXPelsPerMeter;
	int32_t biYPelsPerMeter;
	uint32_t biClrUsed;
	uint32_t biClrImportant;
	uint32_t aColors[2]; // Palette data, first color is used if pixel bit is 0, second if pixel bit is 1
} BMhdr_t;

void charoutsmall(uint8_t theChar, uint8_t X, uint8_t Y) {
	// First of all, make lowercase into uppercase
	// (as there are no lowercase letters in the font)
	if((theChar&0x7f)>=0x61 && (theChar&0x7f)<=0x7a) theChar-=0x20;
	uint16_t fontoffset=((theChar&0x7f)-0x20) * 6;
	uint8_t yoffset=Y&0x7;
	Y>>=3;
#ifndef MINIMALISTIC
	uint8_t width=(theChar&0x80)?7:6;
#else
	uint8_t width=6;
#endif
	for(uint8_t x=0; x<width; x++) {
		uint16_t temp=smallfont[fontoffset++];
#ifndef MINIMALISTIC
		if(theChar&0x80) temp^=0x7f;
#endif
		temp=temp<<yoffset; // Shift pixel data to the correct lines
		uint16_t old=(FB[Y][X] | (FB[Y+1][X]<<8));
#ifndef MINIMALISTIC
		old&=~(0x7f<<yoffset); //Clean out old data
#endif
		temp|=old; // Merge old data in FB with new char
		if(X>=(FB_WIDTH)) return; // make sure we don't overshoot
		if(Y<((FB_HEIGHT/8)-0)) FB[Y][X]=temp&0xff;
		if(Y<((FB_HEIGHT/8)-1)) FB[Y+1][X]=temp>>8;
		X++;
	}
}

void LCD_disp_str(uint8_t* theStr, uint8_t theLen, uint8_t startx, uint8_t y, uint8_t theFormat) {
#ifdef MINIMALISTIC
	for(uint8_t q=0;q<theLen;q++) {
		charoutsmall( theStr[q], startx, y );
		startx+=6;
	}
#else
	uint8_t invmask = theFormat & 0x80;
	switch(theFormat & 0x7f) {
		case FONT6X6:
			for(uint8_t q=0;q<theLen;q++) {
				charoutsmall( theStr[q] | invmask, startx, y );
				startx+=6;
			}
			break;
#if 0
		case FONT12X16:
			for(uint8_t q=0;q<theLen;q++) {
				charoutbig( theStr[q] | invmask, startx, y );
				startx+=12;
			}
			break;
		case FONT4X6:
			for(uint8_t q=0;q<theLen;q++) {
				charout4x6( theStr[q] | invmask, startx, y );
				startx+=4;
			}
			break;
		case FONT6X8:
			for(uint8_t q=0;q<theLen;q++) {
				charout6x8( theStr[q] | invmask, startx, y );
				startx+=6;
			}
			break;
		case FONTBIGNUMBERS:
			for(uint8_t q=0;q<theLen;q++) {
				charoutbignum( theStr[q] | invmask, startx, y );
				startx+=13;
			}
			break;
#endif
	}
#endif
}

void LCD_MultiLineH(uint8_t startx, uint8_t endx, uint64_t ymask) {
	for(uint8_t x=startx;x<=endx;x++) {
		FB[0][x]|=ymask&0xff;
		FB[1][x]|=ymask>>8;
		FB[2][x]|=ymask>>16;
		FB[3][x]|=ymask>>24;
#if FB_HEIGHT==64
		FB[4][x]|=ymask>>32;
		FB[5][x]|=ymask>>40;
		FB[6][x]|=ymask>>48;
		FB[7][x]|=ymask>>56;
#endif
	}
}

// At the moment this is a very basic BMP file reader with the following limitations:
// The bitmap must be 1-bit, uncompressed with a BITMAPINFOHEADER.
uint8_t LCD_BMPDisplay(uint8_t* thebmp,uint8_t xoffset,uint8_t yoffset) {
	BMhdr_t* bmhdr;
	uint8_t upsidedown=1;
	uint8_t inverted=0;
	uint16_t pixeloffset;
	uint8_t numpadbytes=0;

	// The following code grabs the header portion of the bitmap and caches it locally on the stack
	BMhdr_t temp;
	uint8_t* xxx=(uint8_t*)&temp;
	for(uint16_t xx=0;xx<sizeof(BMhdr_t);xx++) {
		xxx[xx]=*(thebmp+xx);
	}
	bmhdr=&temp;

//	printf("\n%s: bfSize=%x biSize=%x", __FUNCTION__, (uint16_t)bmhdr->bfSize, (uint16_t)bmhdr->biSize);
//	printf("\n%s: Image size is %d x %d", __FUNCTION__, (int16_t)bmhdr->biWidth, (int16_t)bmhdr->biHeight);
	if(bmhdr->biPlanes!=1 || bmhdr->biBitCount!=1 || bmhdr->biCompression!=0) {
		printf("\n%s: Incompatible bitmap format!", __FUNCTION__);
		return 1;
	}
	pixeloffset=bmhdr->bfOffBits;
	if(bmhdr->aColors[0]==0) {
		inverted=1;
	}
	if(bmhdr->biHeight<0) {
		bmhdr->biHeight=-bmhdr->biHeight;
		upsidedown=0;
	}
	if((bmhdr->biWidth+xoffset > FB_WIDTH) || (bmhdr->biHeight+yoffset > FB_HEIGHT)) {
		printf("\n%s: Image won't fit on display!", __FUNCTION__);
		return 1;
	}

	// Figure out how many dummy bytes that is present at the end of each line
	// If the image is 132 pixels wide then the pixel lines will be 20 bytes (160 pixels)
	// 132&31 is 4 which means that there are 3 bytes of padding
	numpadbytes=(4-((((bmhdr->biWidth)&0x1f)+7)>>3))&0x03;
//	printf("\n%s: Skipping %d padding bytes after each line", __FUNCTION__, numpadbytes);

	for(int8_t y=bmhdr->biHeight-1; y>=0; y--) {
		uint8_t realY=upsidedown?(uint8_t)y:(uint8_t)(bmhdr->biHeight)-y;
		realY+=yoffset;
		uint8_t pagenum=realY>>3;
		uint8_t pixelval=1<<(realY&0x07);
		for(uint8_t x=0; x<bmhdr->biWidth; x+=8) {
			uint8_t pixel=*(thebmp+(pixeloffset++));
			if(inverted) pixel^=0xff;
			uint8_t max_b = bmhdr->biWidth - x;
			if(max_b>8) max_b = 8;
			for(uint8_t b=0; b<max_b; b++) {
				if(pixel&0x80) {
					FB[pagenum][x+b+xoffset]|=pixelval;
				}
				pixel=pixel<<1;
			}
		}
		pixeloffset+=numpadbytes;
	}
	return 0;
}

void LCD_SetPixel(uint8_t x,uint8_t y) {
	if(x>=FB_WIDTH || y>=FB_HEIGHT) return; // No random memory overwrites thank you
	FB[y>>3][x] |= 1<<(y&0x07);
}

void LCD_SetBacklight(uint8_t backlight) {
	if(backlight) {
		FIO0SET = (1<<11);
	} else {
		FIO0CLR = (1<<11);
	}
}

static void LCD_WriteCmd(uint32_t cmdbyte) {
	FIO0SET = (1<<12) | (1<<13); // Both CS active
	FIO1PIN = cmdbyte << 16; // Cmd on pins
	FIO0CLR = (1<<22); // RS low
	FIO0SET = (1<<18); // E high
	BusyWait8(4);
	FIO0CLR = (1<<18); // E low
	BusyWait8(4);
	FIO0SET = (1<<22); // RS high
	FIO0CLR = (1<<12) | (1<<13); // Both CS inactive
}

static void LCD_WriteData(uint32_t databyte, uint8_t chipnum) {
	uint32_t csmask=chipnum?(1<<12):(1<<13);
	FIO0SET = csmask; // CS active
	FIO1PIN = databyte << 16; // Data on pins
	FIO0SET = (1<<18); // E high
	BusyWait8(4);
	FIO0CLR = (1<<18); // E low
	BusyWait8(4);
	FIO0CLR = csmask; // CS inactive
}

void LCD_Init(void) {
	FIO1DIR = 0xff0000; // Data pins output
	LCD_WriteCmd(0x3f); // Display on
	LCD_WriteCmd(0x40); // Set Y address to 0
	LCD_WriteCmd(0xb8); // Set X page 0
	LCD_WriteCmd(0xc0); // Set display start line to 0
	LCD_FB_Clear();
	LCD_FB_Update();
	LCD_SetBacklight(1);
}

void LCD_FB_Clear(void) {
	// Init FB storage
	for(uint8_t j=0;j<(FB_HEIGHT/8);j++) {
		memset(FB[j],0,FB_WIDTH);
	}
}

void LCD_FB_Update() {
	for(uint32_t page=0; page<(FB_HEIGHT>>3); page++) {
		LCD_WriteCmd(0xb8 + page);
		LCD_WriteCmd(0x40);
		for(uint32_t foo=0; foo<64; foo++) {
			LCD_WriteData(FB[page][foo],0);
			LCD_WriteData(FB[page][foo+64],1);
		}
	}
}
