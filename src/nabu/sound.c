/*
 * NABU sound -- not yet implemented.
 * AY sound was attempted in 1.00.48/1.00.49, caused a CP/M startup crash,
 * rolled back in 1.00.50. Do not revisit until gameplay is stable.
 * Updated Sound - Based on Dave's amazing help!
 * Added digit audio file playback - GTAMP
 */

#include "../misc.h"
#include "../../../../NABULIB/RetroNET-FileStore.h"

#define AY_CHA_TONE_L 0
#define AY_CHA_TONE_H 1
#define AY_NOISE_GEN  6
#define AY_ENABLES    7
#define AY_CHA_AMPL   8
#define AY_CHC_AMPL   10
#define AY_ENV_PERIOD_L 11
#define AY_ENV_PERIOD_H 12
#define AY_ENV_SHAPE  13
#define AY_ENV_SHAPE_FADE_OUT 0x09

static uint8_t pew_frames;
static uint8_t pew_sweep;
static uint8_t pew_active;

uint8_t g_audio_buffer[4096]; 
uint16_t g_current_size = 0;

void play(const char* filename) {
    uint8_t fh = rn_fileOpen((uint8_t)strlen(filename), (uint8_t*)filename, 1, 0);
    
    if (fh == 0xff) {
        // file not found
        return;
    }

    // download
    g_current_size = rn_fileHandleRead(fh, g_audio_buffer, 0, 0, 4096);
    rn_fileHandleClose(fh);

    if (g_current_size == 0) return;
	
	// init mixer
    IO_AYLATCH = 7; IO_AYDATA = 0x7F; 
	// play
    __asm
        ld hl, #_g_audio_buffer
        ld de, (_g_current_size)
    _sb_play:
        ld a, (hl)
        inc hl

        ; --- MAX VOLUME LOGIC ---
        add a, a
        add a, a
        add a, a
        add a, a
        rrca
        rrca
        rrca
        rrca
        and #0x0F

        ld c, a
        ld a, #8         
        out (#0x41), a
        ld a, c
        out (#0x40), a

        ld b, #19         ; Timing delay for 11kHz
    _sb_wait:
        djnz _sb_wait

        dec de
        ld a, d
        or e
        jr nz, _sb_play
    __endasm;

    // Silence
    IO_AYLATCH = 8; IO_AYDATA = 0;
}

static void enable_tone_a_only(void)
{
    uint8_t mixer;

    mixer = ayRead(AY_ENABLES);
    mixer = (uint8_t)((mixer & 0xC0u) | 0x3Eu);
    ayWrite(AY_ENABLES, mixer);
}

static void enable_noise_c_only(void)
{
    uint8_t mixer;

    mixer = ayRead(AY_ENABLES);
    mixer = (uint8_t)((mixer & 0xC0u) | 0x1Fu);
    ayWrite(AY_ENABLES, mixer);
}

void initSound() {}
void disableKeySounds() {}
void enableKeySounds() {}
void soundCursor() {}
void soundSelect() {}
void soundStop()
{
    pew_frames = 0;
    pew_active = 0;
    ayWrite(AY_CHA_AMPL, 0);
}
void soundJoinGame() {}
void soundMyTurn() {}
void soundGameDone() {}
void soundTick()
{
    if (pew_frames > 0) {
        pew_frames--;
        pew_sweep = (uint8_t)(pew_sweep + 16);
        ayWrite(AY_CHA_TONE_L, pew_sweep);
    } else if (pew_active) {
        pew_active = 0;
        ayWrite(AY_CHA_AMPL, 0);
    }
}
void soundPlaceShip() {}
void soundAttack()
{
    pew_frames = 8;
    pew_sweep = 0x50;
    pew_active = 1;
    enable_tone_a_only();
    ayWrite(AY_CHA_TONE_L, 0);
    ayWrite(AY_CHA_TONE_H, pew_sweep);
    ayWrite(AY_CHA_AMPL, 0x0A);
}
void soundInvalid() {}
void soundHit()
{
    enable_noise_c_only();
    ayWrite(AY_CHC_AMPL, 0x10);
    ayWrite(AY_NOISE_GEN, 0x1F);
    ayWrite(AY_ENV_PERIOD_L, 0x00);
    ayWrite(AY_ENV_PERIOD_H, 0x0F);
    ayWrite(AY_ENV_SHAPE, AY_ENV_SHAPE_FADE_OUT);
}
void soundSink() {}
void soundMiss()
{
    enable_noise_c_only();
    ayWrite(AY_CHC_AMPL, 0x10);
    ayWrite(AY_NOISE_GEN, 0x08);
    ayWrite(AY_ENV_PERIOD_L, 0x80);
    ayWrite(AY_ENV_PERIOD_H, 0x03);
    ayWrite(AY_ENV_SHAPE, AY_ENV_SHAPE_FADE_OUT);
}

void soundYouHit() {
	play("https://gtamp.com/bship/hit-bill.pcm");
}

void soundTheyHit() {
    play("https://gtamp.com/bship/hit-death.pcm");
}

void soundYouSunk() {
    play("https://gtamp.com/bship/sunk-jasper.pcm");
}

void soundTheySunk() {
    play("https://gtamp.com/bship/sunk-death.pcm");
}

void soundYouMiss() {
	play("https://gtamp.com/bship/miss-bill.pcm");
}

void soundTheyMiss() {
    play("https://gtamp.com/bship/miss-death.pcm");
}
