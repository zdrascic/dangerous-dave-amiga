#pragma once
#include "sade.h"
#include "sade_ilbm.h"
#include "sade_bob.h"

// Sprite bank — AMOS-style: cut N fixed-size sprites out of an IFF ILBM
// spritesheet and store them as ready-to-blit BobImages, indexed 0..count-1.
//
//   EMBED sheet[] = { #embed "sprites.iff" };
//   SpriteBank *bank = CreateSpriteBankFromFile(sheet, sizeof(sheet),
//                                               0, 0,    // sheet start x,y
//                                               32, 32,  // sprite w,h
//                                               4);      // sprite count
//   SpriteBankApplyPalette(bank, scr);
//   Bob *b = BobCreate(SpriteBankImage(bank, 2));
//
// Sprites are cut left-to-right from (startX, startY); when the next sprite
// would pass the sheet's right edge the cutter wraps to startX on the next
// row (startY + sprH, then + 2*sprH, ...), so both strips and grids work.
//
// Color 0 is transparent (AMOS convention): the Bob mask is set only where
// a pixel has a non-zero color index.

typedef struct {
    BobImage **images;                   // count entries, indexed 0..count-1
    UWORD      count;
    ULONG      palette[ILBM_MAX_COLORS]; // sheet CMAP, $00RRGGBB
    UWORD      numColors;
} SpriteBank;

// -------------------------------------------------------------------------
// Internal: read one pixel's color index from an IlbmImage.
// -------------------------------------------------------------------------
static inline UBYTE _BankGetIlbmPixel(const IlbmImage *img, UWORD x, UWORD y) {
    UWORD byteOff = (UWORD)(y * img->rowBytes) + (x >> 3);
    UBYTE bit     = 0x80 >> (x & 7);
    UBYTE col     = 0;
    for (UBYTE p = 0; p < img->depth; p++)
        if (img->planes[p][byteOff] & bit) col |= (1 << p);
    return col;
}

// -------------------------------------------------------------------------
// Internal: copy one sprW×sprH cell from the sheet into a fresh BobImage.
// Color 0 stays transparent: plane bits and the mask are set only for
// non-zero pixels (BobImageSetPixel is bypassed — it marks every pixel
// opaque, which is wrong for cut-out sprites).
// -------------------------------------------------------------------------
static BobImage *_BankCutSprite(const IlbmImage *sheet,
                                UWORD srcX, UWORD srcY,
                                UWORD sprW, UWORD sprH)
{
    BobImage *img = BobImageCreate(sprW, sprH, sheet->depth);
    if (!img) return 0;

    for (UWORD y = 0; y < sprH; y++) {
        UWORD dstRow = y * img->rowBytes;
        for (UWORD x = 0; x < sprW; x++) {
            UBYTE col = _BankGetIlbmPixel(sheet, srcX + x, srcY + y);
            if (!col) continue;
            UWORD off = dstRow + (x >> 3);
            UBYTE bit = 0x80 >> (x & 7);
            for (UBYTE p = 0; p < img->depth; p++)
                if (col & (1 << p))
                    img->planeData[p][off] |= bit;
            img->maskData[off] |= bit;
            img->planeMask |= col;
        }
    }
    return img;
}

// -------------------------------------------------------------------------
// SpriteBankFromIlbm — cut a bank from an already-loaded IlbmImage.
// sprW must be a multiple of 16 (blitter word constraint on BobImage).
// Returns NULL if the parameters don't fit inside the sheet or on
// allocation failure.
// -------------------------------------------------------------------------
static void SpriteBankFree(SpriteBank *bank);

// Optional: called once per sprite cut — cutting is slow (per-pixel), so a
// loading screen can advance a progress bar from here. Shared by all .c files.
__attribute__((weak)) void (*SpriteBankProgress)(void) = NULL;

static SpriteBank *SpriteBankFromIlbm(const IlbmImage *sheet,
                                      UWORD startX, UWORD startY,
                                      UWORD sprW, UWORD sprH,
                                      UWORD count)
{
    if (!sheet || count == 0) return 0;
    if (sprW == 0 || (sprW & 15)) return 0;   // must be multiple of 16
    if (sprH == 0) return 0;
    if (startX + sprW > sheet->width)  return 0;
    if (startY + sprH > sheet->height) return 0;

    SpriteBank *bank = (SpriteBank*)SadeAlloc(sizeof(SpriteBank),
                                             MEMF_PUBLIC | MEMF_CLEAR);
    if (!bank) return 0;

    bank->images = (BobImage**)SadeAlloc((ULONG)count * sizeof(BobImage*),
                                        MEMF_PUBLIC | MEMF_CLEAR);
    if (!bank->images) { SadeFree(bank, sizeof(SpriteBank)); return 0; }
    bank->count = count;

    bank->numColors = sheet->numColors;
    for (UWORD i = 0; i < sheet->numColors; i++)
        bank->palette[i] = sheet->palette[i];

    UWORD x = startX, y = startY;
    for (UWORD n = 0; n < count; n++) {
        if (x + sprW > sheet->width) {        // wrap to next grid row
            x = startX;
            y += sprH;
        }
        if (y + sprH > sheet->height) goto fail;

        bank->images[n] = _BankCutSprite(sheet, x, y, sprW, sprH);
        if (!bank->images[n]) goto fail;
        if (SpriteBankProgress) SpriteBankProgress();
        x += sprW;
    }
    return bank;

fail:
    SpriteBankFree(bank);
    return 0;
}

// -------------------------------------------------------------------------
// CreateSpriteBankFromFile — load an IFF ILBM spritesheet from raw file
// bytes (e.g. #embed), cut count sprites of sprW×sprH starting at
// (startX, startY), free the sheet, return the bank.
// -------------------------------------------------------------------------
static SpriteBank *CreateSpriteBankFromFile(const UBYTE *iffData, ULONG size,
                                            UWORD startX, UWORD startY,
                                            UWORD sprW, UWORD sprH,
                                            UWORD count)
{
    IlbmImage *sheet = IlbmLoad(iffData, size);
    if (!sheet) return 0;
    SpriteBank *bank = SpriteBankFromIlbm(sheet, startX, startY,
                                          sprW, sprH, count);
    IlbmFree(sheet);
    return bank;
}

// -------------------------------------------------------------------------
// SpriteBankImage — fetch sprite n (0-based). NULL if out of range.
// -------------------------------------------------------------------------
static inline BobImage *SpriteBankImage(const SpriteBank *bank, UWORD n) {
    return (bank && n < bank->count) ? bank->images[n] : 0;
}

// -------------------------------------------------------------------------
// SpriteBankTrim — BobImageTrim every sprite: each draws only its art, not
// the whole cell (cheaper blits when the art is smaller than the cell).
// -------------------------------------------------------------------------
static void SpriteBankTrim(SpriteBank *bank) {
    if (!bank) return;
    for (UWORD n = 0; n < bank->count; n++) BobImageTrim(bank->images[n]);
}

// -------------------------------------------------------------------------
// SpriteBankApplyPalette — load the sheet's CMAP into the screen palette,
// like IlbmApplyPalette but from the bank's stored copy.
// -------------------------------------------------------------------------
static void SpriteBankApplyPalette(const SpriteBank *bank, Screen *scr) {
    UWORD n = bank->numColors < scr->numColors ? bank->numColors
                                               : scr->numColors;
    for (UWORD i = 0; i < n; i++)
        ScreenSetRGB(scr, i, bank->palette[i]);
}

// -------------------------------------------------------------------------
// SpriteBankFree — free every BobImage in the bank, then the bank itself.
// Free any Bobs still bound to the bank's images first.
// -------------------------------------------------------------------------
static void SpriteBankFree(SpriteBank *bank) {
    if (!bank) return;
    if (bank->images) {
        for (UWORD n = 0; n < bank->count; n++)
            BobImageFree(bank->images[n]);
        SadeFree(bank->images, (ULONG)bank->count * sizeof(BobImage*));
    }
    SadeFree(bank, sizeof(SpriteBank));
}
