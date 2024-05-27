#ifndef OLED_I2C_H
#define OLED_I2C_H

#include <stdint.h>
#include <ft2build.h>
#include FT_FREETYPE_H

void oled_init(void);
void oled_clear(void);
void oled_display(void);
void oled_draw_pixel(int x,int y,uint8_t color);
void render_text(const char *text,int x_offset,int y_offset,FT_Face face);
void oled_clear_line(int y,int height);

#endif
