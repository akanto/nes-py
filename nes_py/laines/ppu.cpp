#include <cstring>
#include "cpu.hpp"
#include "ppu.hpp"

namespace PPU {
#include "palette.inc"
    /// Mirroring mode
    Mirroring mirroring;
    /// VRAM for name-tables
    u8 ciRam[0x800];
    /// VRAM for palettes
    u8 cgRam[0x20];
    /// VRAM for sprite properties
    u8 oamMem[0x100];
    /// Sprite buffers
    Sprite oam[8], secOam[8];
    /// Video buffer
    u32 pixels[256 * 240];

    /// Loopy V, T
    Addr vAddr, tAddr;
    /// Fine X
    u8 fX;
    /// OAM address
    u8 oamAddr;

    /// PPUCTRL   ($2000) register
    Ctrl ctrl;
    /// PPUMASK   ($2001) register
    Mask mask;
    /// PPUSTATUS ($2002) register
    Status status;

    /// Background latches:
    u8 nt, at, bgL, bgH;
    /// Background shift registers:
    u8 atShiftL, atShiftH; u16 bgShiftL, bgShiftH;
    bool atLatchL, atLatchH;

    /// Rendering counters:
    int scanline, dot;
    bool frameOdd;

    inline bool rendering() { return mask.bg || mask.spr; }
    inline int spr_height() { return ctrl.sprSz ? 16 : 8; }

    /// the GUI this PPU has access to
    GUI* gui;
    void set_gui(GUI* new_gui) { gui = new_gui; }
    GUI* get_gui() { return gui; }

    /// the cartridge this PPU uses for game data
    Cartridge* cartridge;
    void set_cartridge(Cartridge* new_cartridge) { cartridge = new_cartridge; }
    Cartridge* get_cartridge() { return cartridge; }

    /// Get CIRAM address according to mirroring.
    u16 nt_mirror(u16 addr) {
        switch (mirroring) {
            case VERTICAL:    return addr % 0x800;
            case HORIZONTAL:  return ((addr / 2) & 0x400) + (addr % 0x400);
            default:          return addr - 0x2000;
        }
    }
    /// Set the PPU to the given mirroring mode.
    void set_mirroring(Mirroring mode) { mirroring = mode; }

    /// Read an address from PPU memory.
    u8 rd(u16 addr) {
        switch (addr) {
            // CHR-ROM/RAM.
            case 0x0000 ... 0x1FFF:  return cartridge->chr_access<0>(addr);
            // Nametables.
            case 0x2000 ... 0x3EFF:  return ciRam[nt_mirror(addr)];
            // Palettes:
            case 0x3F00 ... 0x3FFF:
                if ((addr & 0x13) == 0x10) addr &= ~0x10;
                return cgRam[addr & 0x1F] & (mask.gray ? 0x30 : 0xFF);
            default: return 0;
        }
    }
    /// Write a byte to PPU memory.
    void wr(u16 addr, u8 v) {
        switch (addr) {
            // CHR-ROM/RAM.
            case 0x0000 ... 0x1FFF:  cartridge->chr_access<1>(addr, v); break;
            // Nametables.
            case 0x2000 ... 0x3EFF:  ciRam[nt_mirror(addr)] = v; break;
            // Palettes:
            case 0x3F00 ... 0x3FFF:
                if ((addr & 0x13) == 0x10) addr &= ~0x10;
                cgRam[addr & 0x1F] = v; break;
        }
    }

    /// Access PPU through registers.
    template <bool write> u8 access(u16 index, u8 v) {
        static u8 res;      // Result of the operation.
        static u8 buffer;   // VRAM read buffer.
        static bool latch;  // Detect second reading.

        /* Write into register */
        if (write) {
            res = v;

            switch (index) {
                // PPUCTRL   ($2000).
                case 0:  ctrl.r = v; tAddr.nt = ctrl.nt; break;
                // PPUMASK   ($2001).
                case 1:  mask.r = v; break;
                // OAMADDR   ($2003).
                case 3:  oamAddr = v; break;
                // OAMDATA   ($2004).
                case 4:  oamMem[oamAddr++] = v; break;
                // PPUSCROLL ($2005).
                case 5:
                    // First write.
                    if (!latch) { fX = v & 7; tAddr.cX = v >> 3; }
                    // Second write.
                    else  { tAddr.fY = v & 7; tAddr.cY = v >> 3; }
                    latch = !latch; break;
                // PPUADDR   ($2006).
                case 6:
                    // First write.
                    if (!latch) { tAddr.h = v & 0x3F; }
                    // Second write.
                    else        { tAddr.l = v; vAddr.r = tAddr.r; }
                    latch = !latch; break;
                // PPUDATA ($2007).
                case 7:  wr(vAddr.addr, v); vAddr.addr += ctrl.incr ? 32 : 1;
            }
        }
        /* Read from register */
        else
            switch (index) {
                // PPUSTATUS ($2002):
                case 2:  res = (res & 0x1F) | status.r; status.vBlank = 0; latch = 0; break;
                // OAMDATA ($2004).
                case 4:  res = oamMem[oamAddr]; break;
                // PPUDATA ($2007).
                case 7:
                    if (vAddr.addr <= 0x3EFF) {
                        res = buffer;
                        buffer = rd(vAddr.addr);
                    }
                    else
                        res = buffer = rd(vAddr.addr);
                    vAddr.addr += ctrl.incr ? 32 : 1;
            }
        return res;
    }
    template u8 access<0>(u16, u8); template u8 access<1>(u16, u8);

    /* Calculate graphics addresses */
    inline u16 nt_addr() {
        return 0x2000 | (vAddr.r & 0xFFF);
    }
    inline u16 at_addr() {
        return 0x23C0 | (vAddr.nt << 10) | ((vAddr.cY / 4) << 3) | (vAddr.cX / 4);
    }
    inline u16 bg_addr() {
        return (ctrl.bgTbl * 0x1000) + (nt * 16) + vAddr.fY;
    }
    /* Increment the scroll by one pixel */
    inline void h_scroll() {
        if (!rendering()) return;
        if (vAddr.cX == 31) vAddr.r ^= 0x41F;
        else vAddr.cX++;
    }
    inline void v_scroll() {
        if (!rendering()) return;
        if (vAddr.fY < 7) vAddr.fY++;
        else {
            vAddr.fY = 0;
            if      (vAddr.cY == 31)   vAddr.cY = 0;
            else if (vAddr.cY == 29) { vAddr.cY = 0; vAddr.nt ^= 0b10; }
            else                       vAddr.cY++;
        }
    }
    /* Copy scrolling data from loopy T to loopy V */
    inline void h_update() {
        if (!rendering()) return;
        vAddr.r = (vAddr.r & ~0x041F) | (tAddr.r & 0x041F);
    }
    inline void v_update() {
        if (!rendering()) return;
        vAddr.r = (vAddr.r & ~0x7BE0) | (tAddr.r & 0x7BE0);
    }
    /* Put new data into the shift registers */
    inline void reload_shift() {
        bgShiftL = (bgShiftL & 0xFF00) | bgL;
        bgShiftH = (bgShiftH & 0xFF00) | bgH;

        atLatchL = (at & 1);
        atLatchH = (at & 2);
    }

    /* Clear secondary OAM */
    void clear_oam() {
        for (int i = 0; i < 8; i++) {
            secOam[i].id    = 64;
            secOam[i].y     = 0xFF;
            secOam[i].tile  = 0xFF;
            secOam[i].attr  = 0xFF;
            secOam[i].x     = 0xFF;
            secOam[i].dataL = 0;
            secOam[i].dataH = 0;
        }
    }

    /* Fill secondary OAM with the sprite infos for the next scanline */
    void eval_sprites() {
        int n = 0;
        for (int i = 0; i < 64; i++) {
            int line = (scanline == 261 ? -1 : scanline) - oamMem[i*4 + 0];
            // If the sprite is in the scanline, copy its properties
            // into secondary OAM:
            if (line >= 0 and line < spr_height()) {
                secOam[n].id   = i;
                secOam[n].y    = oamMem[i*4 + 0];
                secOam[n].tile = oamMem[i*4 + 1];
                secOam[n].attr = oamMem[i*4 + 2];
                secOam[n].x    = oamMem[i*4 + 3];

                if (++n > 8) {
                    status.sprOvf = true;
                    break;
                }
            }
        }
    }

    /* Load the sprite info into primary OAM and fetch their tile data. */
    void load_sprites() {
        u16 addr;
        for (int i = 0; i < 8; i++) {
            // Copy secondary OAM into primary.
            oam[i] = secOam[i];

            // Different address modes depending on the sprite height:
            if (spr_height() == 16)
                addr = ((oam[i].tile & 1) * 0x1000) + ((oam[i].tile & ~1) * 16);
            else
                addr = ( ctrl.sprTbl      * 0x1000) + ( oam[i].tile       * 16);

            // Line inside the sprite.
            unsigned sprY = (scanline - oam[i].y) % spr_height();
            // Vertical flip.
            if (oam[i].attr & 0x80) sprY ^= spr_height() - 1;
            // Select the second tile if on 8x16.
            addr += sprY + (sprY & 8);

            oam[i].dataL = rd(addr + 0);
            oam[i].dataH = rd(addr + 8);
        }
    }

    /* Process a pixel, draw it if it's on screen */
    void pixel() {
        u8 palette = 0, objPalette = 0;
        bool objPriority = 0;
        int x = dot - 2;

        if (scanline < 240 and x >= 0 and x < 256) {
            if (mask.bg and not (!mask.bgLeft && x < 8)) {
                // Background:
                palette = (NTH_BIT(bgShiftH, 15 - fX) << 1) |
                           NTH_BIT(bgShiftL, 15 - fX);
                if (palette)
                    palette |= ((NTH_BIT(atShiftH,  7 - fX) << 1) |
                                 NTH_BIT(atShiftL,  7 - fX))      << 2;
            }
            // Sprites:
            if (mask.spr and not (!mask.sprLeft && x < 8))
                for (int i = 7; i >= 0; i--) {
                    // Void entry.
                    if (oam[i].id == 64) continue;
                    unsigned sprX = x - oam[i].x;
                    // Not in range.
                    if (sprX >= 8) continue;
                    // Horizontal flip.
                    if (oam[i].attr & 0x40) sprX ^= 7;

                    u8 sprPalette = (NTH_BIT(oam[i].dataH, 7 - sprX) << 1) |
                                     NTH_BIT(oam[i].dataL, 7 - sprX);
                    // Transparent pixel.
                    if (sprPalette == 0) continue;

                    if (oam[i].id == 0 && palette && x != 255)
                        status.sprHit = true;
                    sprPalette |= (oam[i].attr & 3) << 2;
                    objPalette  = sprPalette + 16;
                    objPriority = oam[i].attr & 0x20;
                }
            // Evaluate priority:
            if (objPalette && (palette == 0 || objPriority == 0))
                palette = objPalette;

            pixels[scanline*256 + x] = nesRgb[rd(0x3F00 + (rendering() ? palette : 0))];
        }
        // Perform background shifts:
        bgShiftL <<= 1; bgShiftH <<= 1;
        atShiftL = (atShiftL << 1) | atLatchL;
        atShiftH = (atShiftH << 1) | atLatchH;
    }

    /* Execute a cycle of a scanline */
    template<Scanline s> void scanline_cycle() {
        static u16 addr;

        if (s == NMI and dot == 1) { status.vBlank = true; if (ctrl.nmi) CPU::set_nmi(); }
        else if (s == POST and dot == 0) gui->new_frame(pixels);
        else if (s == VISIBLE or s == PRE) {
            // Sprites:
            switch (dot) {
                case   1: clear_oam(); if (s == PRE) { status.sprOvf = status.sprHit = false; } break;
                case 257: eval_sprites(); break;
                case 321: load_sprites(); break;
            }
            // Background:
            switch (dot) {
                case 2 ... 255: case 322 ... 337:
                    pixel();
                    switch (dot % 8) {
                        // Nametable:
                        case 1:  addr  = nt_addr(); reload_shift(); break;
                        case 2:  nt    = rd(addr);  break;
                        // Attribute:
                        case 3:  addr  = at_addr(); break;
                        case 4:  at    = rd(addr);  if (vAddr.cY & 2) at >>= 4;
                                                    if (vAddr.cX & 2) at >>= 2; break;
                        // Background (low bits):
                        case 5:  addr  = bg_addr(); break;
                        case 6:  bgL   = rd(addr);  break;
                        // Background (high bits):
                        case 7:  addr += 8;         break;
                        case 0:  bgH   = rd(addr); h_scroll(); break;
                    } break;
                // Vertical bump.
                case         256:  pixel(); bgH = rd(addr); v_scroll(); break;
                // Update horizontal position.
                case         257:  pixel(); reload_shift(); h_update(); break;
                // Update vertical position.
                case 280 ... 304:  if (s == PRE)            v_update(); break;

                // No shift reloading:
                case             1:  addr = nt_addr(); if (s == PRE) status.vBlank = false; break;
                case 321: case 339:  addr = nt_addr(); break;
                // Nametable fetch instead of attribute:
                case           338:  nt = rd(addr); break;
                case           340:  nt = rd(addr); if (s == PRE && rendering() && frameOdd) dot++;
            }
            // Signal scanline to mapper:
            if (dot == 260 && rendering()) cartridge->signal_scanline();
        }
    }

    void step() {
        switch (scanline) {
            case 0 ... 239:  scanline_cycle<VISIBLE>(); break;
            case       240:  scanline_cycle<POST>();    break;
            case       241:  scanline_cycle<NMI>();     break;
            case       261:  scanline_cycle<PRE>();     break;
        }
        // Update dot and scanline counters:
        if (++dot > 340) {
            dot %= 341;
            if (++scanline > 261) {
                scanline = 0;
                frameOdd ^= 1;
            }
        }
    }

    void reset() {
        frameOdd = false;
        scanline = dot = 0;
        ctrl.r = mask.r = status.r = 0;

        memset(pixels, 0x00, sizeof(pixels));
        memset(ciRam,  0xFF, sizeof(ciRam));
        memset(oamMem, 0x00, sizeof(oamMem));
    }

    PPUState* get_state() {
        PPUState* state = new PPUState();
        // TODO: fill state
        return state;
    }

    void set_state(PPUState* state) {
        // TODO: set with state variables
    }
}
