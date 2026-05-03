/*
 * NABU G2 screen driver and CP437 font -- VDP init, cell write, and the
 * waitvsync delay loop.
 * Ported from the CP437 terminal program, with battleship tile colours added.
 */

#include "../misc.h"
#define BIN_TYPE BIN_NABU
#define FONT_CP437
#define DISABLE_KEYBOARD_INT
#include "cp437_patterns.h"
#include "battleship_patterns.h"
#include "../../../../NABULIB/NABU-LIB.h"

extern void nabu_tick(void);
void nabu_vdp_clear(void);

static bool cp437_ready;
static const uint8_t ui_vline_glyph[8] = {
    0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18
};

/* Live cell writes pick ASCII, CP437, or Battleship glyph bytes here.
   Battleship tiles override 0x80-0xB7, while 0xFE is a custom line.
   Not the best approach but working so the icons can be kept in the
   origonal file */
static uint8_t *cp437_glyph(uint8_t ch)
{
    if (ch == 0xFEu)
        return (uint8_t *)ui_vline_glyph;
    if (ch >= 0x80u && ch <= 0xB7u)
        return (uint8_t *)FAT + (uint16_t)ch * 8u;
    if (ch >= 0x80u)
        return (uint8_t *)CP437_EXT + (uint16_t)(ch - 0x80u) * 8u;
    if (ch >= 0x20u)
        return (uint8_t *)ASCII + (uint16_t)(ch - 0x20u) * 8u;
    return (uint8_t *)ASCII;
}

static void nabu_g2_write_cell(uint8_t col, uint8_t row, uint8_t ch, uint8_t fg, uint8_t bg)
{
    uint8_t slot;
    uint16_t bank_off;
    uint8_t *glyph;
    uint8_t colour;

    if (col >= 32u || row >= 24u)
        return;

    slot = (uint8_t)(col + (uint8_t)((row & 7u) * 32u));
    bank_off = (uint16_t)(row >> 3u) * 2048u;
    glyph = cp437_glyph(ch);
    colour = (uint8_t)((fg << 4) | bg);

    __critical {
        vdp_setWriteAddress(_vdpPatternGeneratorTableAddr + bank_off + (uint16_t)slot * 8u);
        IO_VDPDATA = glyph[0]; IO_VDPDATA = glyph[1]; IO_VDPDATA = glyph[2]; IO_VDPDATA = glyph[3];
        IO_VDPDATA = glyph[4]; IO_VDPDATA = glyph[5]; IO_VDPDATA = glyph[6]; IO_VDPDATA = glyph[7];

        vdp_setWriteAddress(_vdpColorTableAddr + bank_off + (uint16_t)slot * 8u);
        IO_VDPDATA = colour; IO_VDPDATA = colour; IO_VDPDATA = colour; IO_VDPDATA = colour;
        IO_VDPDATA = colour; IO_VDPDATA = colour; IO_VDPDATA = colour; IO_VDPDATA = colour;
    }
}

static void nabu_g2_colours_for_char(uint8_t ch, uint8_t *fg, uint8_t *bg)
{
    *fg = VDP_WHITE;
    *bg = VDP_BLACK;

    if (ch == 0x80u) {
        *fg = VDP_DARK_BLUE;
        return;
    }

    if (ch == 0x81u) {
        *fg = VDP_LIGHT_RED;
        return;
    }

    if (ch == 0x88u) {
        *fg = VDP_WHITE;
        return;
    }

    if (ch >= 0x90u && ch <= 0x95u) {
        *fg = VDP_LIGHT_GREEN;
        return;
    }

    if (ch >= 0x98u && ch <= 0x9Du) {
        *fg = VDP_LIGHT_RED;
        return;
    }

    if (ch >= 0xA0u && ch <= 0xA5u) {
        *fg = VDP_LIGHT_YELLOW;
        return;
    }

    if (ch >= 0xA8u && ch <= 0xADu) {
        *fg = VDP_CYAN;
        return;
    }

    if (ch >= 0xB0u && ch <= 0xB3u) {
        *fg = VDP_DARK_YELLOW;
        return;
    }
}

static void nabu_g2_init_nametable(void)
{
    uint8_t row;
    uint8_t col;

    for (row = 0u; row < 24u; ++row) {
        vdp_setWriteAddress(_vdpPatternNameTableAddr + (uint16_t)row * 32u);
        for (col = 0u; col < 32u; ++col)
            IO_VDPDATA = (uint8_t)((row & 7u) * 32u + col);
    }
}

static void cp437_copy_ascii_to_all_bands(void)
{
    const uint8_t *src;
    const uint8_t *end;

    src = (const uint8_t *)ASCII;
    end = src + 768u;

    vdp_setWriteAddress(_vdpPatternGeneratorTableAddr + 2048u + 0x100u);
    do {
        IO_VDPDATA = *src;
        src++;
    } while (src != end);

    src = (const uint8_t *)ASCII;
    vdp_setWriteAddress(_vdpPatternGeneratorTableAddr + 4096u + 0x100u);
    do {
        IO_VDPDATA = *src;
        src++;
    } while (src != end);
}

/* Preload the normal CP437 extension range.
   Live cell writes still overlay Battleship tiles through cp437_glyph().
   as above */
static void cp437_load_extended_chars(void)
{
    uint8_t i;

    for (i = 0; i < 128; ++i) {
        vdp_loadPatternToId((uint8_t)(0x80 + i), (uint8_t *)&CP437_EXT[(uint16_t)i * 8]);
        vdp_colorizePattern((uint8_t)(0x80 + i), VDP_WHITE, VDP_BLACK);
    }
}

void nabu_vdp_init_cp437(void)
{
    if (!cp437_ready) {
        vdp_initG2Mode(VDP_BLACK, false, false, false, true);
        vdp_loadASCIIFont((uint8_t *)ASCII);
        cp437_copy_ascii_to_all_bands();
        cp437_load_extended_chars();
        nabu_g2_init_nametable();
        cp437_ready = true;
    }

    nabu_vdp_clear();
}

void nabu_vdp_clear(void)
{
    uint8_t row;
    uint8_t col;

    for (row = 0u; row < 24u; ++row) {
        for (col = 0u; col < 32u; ++col)
            nabu_g2_write_cell(col, row, ' ', VDP_WHITE, VDP_BLACK);
    }
}

void nabu_vdp_text_at(uint8_t x, uint8_t y, const char *s)
{
    uint8_t col = x;

    while (*s && col < 32u) {
        nabu_g2_write_cell(col, y, (uint8_t)*s, VDP_WHITE, VDP_BLACK);
        ++col;
        ++s;
    }
}

void nabu_vdp_text_colour_at(uint8_t x, uint8_t y, const char *s, uint8_t fg, uint8_t bg)
{
    uint8_t col = x;

    while (*s && col < 32u) {
        nabu_g2_write_cell(col, y, (uint8_t)*s, fg, bg);
        ++col;
        ++s;
    }
}

void nabu_vdp_char_at(uint8_t x, uint8_t y, uint8_t c)
{
    uint8_t fg;
    uint8_t bg;

    nabu_g2_colours_for_char(c, &fg, &bg);
    nabu_g2_write_cell(x, y, c, fg, bg);
}

void waitvsync(void)
{
    vdpIsReady = false;
    vdp_waitVDPReadyInt();
    nabu_tick();
    soundTick();
}
