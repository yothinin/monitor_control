#include "oled_i2c.h"
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>

#define OLED_WIDTH 128
#define OLED_HEIGHT 64
#define OLED_ADDR 0x3C

static int i2c_fd = -1;
static uint8_t buffer[OLED_WIDTH*OLED_HEIGHT/8];


// ส่งคำสั่งไป OLED
static void oled_write_cmd(uint8_t cmd){
    uint8_t data[2]={0x00,cmd};
    write(i2c_fd,data,2);
}

// ส่ง data buffer ไป OLED
static void oled_write_data(uint8_t *data,size_t len){
    uint8_t tmp[len+1];
    tmp[0]=0x40;
    memcpy(tmp+1,data,len);
    write(i2c_fd,tmp,len+1);
}

void oled_init(){
    if((i2c_fd=open("/dev/i2c-0",O_RDWR))<0){ perror("i2c open"); exit(1); }
    if(ioctl(i2c_fd,I2C_SLAVE,OLED_ADDR)<0){ perror("i2c ioctl"); exit(1); }

    memset(buffer,0,sizeof(buffer));

    // Init sequence SSD1306
    oled_write_cmd(0xAE); oled_write_cmd(0x20); oled_write_cmd(0x00);
    oled_write_cmd(0xB0); oled_write_cmd(0xC8); oled_write_cmd(0x00);
    oled_write_cmd(0x10); oled_write_cmd(0x40); oled_write_cmd(0x81);
    oled_write_cmd(0xFF); oled_write_cmd(0xA1); oled_write_cmd(0xA6);
    oled_write_cmd(0xA8); oled_write_cmd(0x3F); oled_write_cmd(0xA4);
    oled_write_cmd(0xD3); oled_write_cmd(0x00); oled_write_cmd(0xD5);
    oled_write_cmd(0xF0); oled_write_cmd(0xD9); oled_write_cmd(0x22);
    oled_write_cmd(0xDA); oled_write_cmd(0x12); oled_write_cmd(0xDB);
    oled_write_cmd(0x20); oled_write_cmd(0x8D); oled_write_cmd(0x14);
    oled_write_cmd(0xAF);
}

void oled_clear(){ memset(buffer,0,sizeof(buffer)); }

void oled_display(){
    for(int page=0;page<8;page++){
        oled_write_cmd(0xB0+page);
        oled_write_cmd(0x00); oled_write_cmd(0x10);
        oled_write_data(&buffer[OLED_WIDTH*page],OLED_WIDTH);
    }
}

void oled_draw_pixel(int x,int y,uint8_t color){
    if(x<0||x>=OLED_WIDTH||y<0||y>=OLED_HEIGHT) return;
    int page=y/8; int bit=y%8;
    if(color) buffer[page*OLED_WIDTH+x]|=(1<<bit);
    else buffer[page*OLED_WIDTH+x]&=~(1<<bit);
}

void oled_clear_line(int y,int height){
    for(int row=y;row<y+height && row<OLED_HEIGHT;row++){
        for(int col=0;col<OLED_WIDTH;col++){
            oled_draw_pixel(col,row,0);
        }
    }
}

// Render ข้อความด้วย FreeType
void render_text(const char *text,int x_offset,int y_offset,FT_Face face){
    while(*text){
        uint32_t codepoint=0;
        unsigned char c=text[0];
        int len=1;
        if(c<0x80) codepoint=c;
        else if((c&0xE0)==0xC0){ codepoint=((c&0x1F)<<6)|(text[1]&0x3F); len=2; }
        else if((c&0xF0)==0xE0){ codepoint=((c&0x0F)<<12)|((text[1]&0x3F)<<6)|(text[2]&0x3F); len=3; }
        else if((c&0xF8)==0xF0){ codepoint=((c&0x07)<<18)|((text[1]&0x3F)<<12)|((text[2]&0x3F)<<6)|(text[3]&0x3F); len=4; }
        else { text++; continue; }

        if(FT_Load_Char(face,codepoint,FT_LOAD_RENDER)){ text+=len; continue; }

        FT_GlyphSlot g=face->glyph;
        for(int row=0;row<g->bitmap.rows;row++){
            for(int col=0;col<g->bitmap.width;col++){
                uint8_t pixel=g->bitmap.buffer[row*g->bitmap.pitch+col];
                if(pixel>128){
                    int px=x_offset+g->bitmap_left+col;
                    int py=y_offset-g->bitmap_top+row;
                    oled_draw_pixel(px,py,1);
                }
            }
        }
        x_offset+=g->advance.x>>6;
        text+=len;
    }
}
