// Scene player for the GBX PPU test ROMs.
//
// Each PPU test ROM is this player linked with one scene blob generated from
// tests/gba/roms/ppu/scenes.py. The player loads VRAM, palette RAM and OAM
// under forced blank, writes the scene's I/O registers, and then shows a
// picture that is the same in every frame.
//
// Raster effects change registers once per line. A table entry for line L is
// written during the H-Blank of line L-1, by H-Blank DMA or by the H-Blank
// IRQ handler, so it takes effect from line L on; entry 0 is written in the
// V-Blank IRQ handler. No register is written while a line is being drawn
// (GBATEK "LCD Dimensions and Timings": 960 drawing cycles, H-Blank flag set
// at cycle 1006 of 1232).

#include "../common/gba.h"

struct SceneHeader {
  u32 magic;
  u32 vram_offset;
  u32 vram_size;
  u32 pram_offset;
  u32 oam_offset;
  u32 io_count;
  u32 io_offset;
  u32 group_count;
  u32 group_offset;
  u32 dispcnt;
};

struct IoWrite {
  u32 addr;
  u32 value;
  u32 width;
};

enum { BY_DMA = 0, BY_IRQ = 1 };

struct LineGroup {
  u32 addr;
  u32 width;
  u32 units;
  u32 mode;
  u32 channel;
  u32 table_offset;
};

#define LINES 161
#define MAX_GROUPS 4
#define TABLE_BYTES (LINES * 16)
#define MAX_IRQ_WRITES 16

extern const u8 scene_blob[];

// DMA0 can only read internal memory (GBATEK "GBA DMA Transfers"); the DMA
// tables are copied to IWRAM, which every channel can read.
static u8 tables[MAX_GROUPS][TABLE_BYTES] __attribute__((aligned(4)));
static struct LineGroup groups[MAX_GROUPS];
static u32 group_count;
static u32 dispcnt;

// Writes of the IRQ groups, flattened per line into halfword writes
// (offset from 4000000h << 16 | value), each list ended by 0, so the H-Blank
// handler stays short: it has to finish within the 226 cycles of H-Blank.
static u32 irq_writes[LINES][MAX_IRQ_WRITES + 1];

static void copy32(volatile u32* dst, const u32* src, u32 bytes) {
  for (u32 i = 0; i < bytes / 4; i++) dst[i] = src[i];
}

static void write_reg(u32 addr, const u8* value, u32 width) {
  if (width == 4) {
    REG32(addr) = *(const u32*)value;
  } else {
    REG16(addr) = *(const u16*)value;
  }
}

static void write_line(const struct LineGroup* g, const u8* table, u32 line) {
  const u8* entry = table + line * g->units * g->width;
  for (u32 i = 0; i < g->units; i++) {
    u32 addr = g->addr + i * g->width;
    if (g->width == 4) {
      REG32(addr) = ((const u32*)entry)[i];
    } else {
      REG16(addr) = ((const u16*)entry)[i];
    }
  }
}

static IWRAM_CODE void irq_handler(void) {
  u16 flags = REG_IF;
  if (flags & IRQ_HBLANK) {
    u32 line = REG_VCOUNT;
    if (line < 160) {
      const u32* w = irq_writes[line + 1];
      for (u32 e; (e = *w++) != 0;) REG16(0x04000000 + (e >> 16)) = (u16)e;
    }
  }
  if (flags & IRQ_VBLANK) {
    REG_DISPCNT = dispcnt;
    for (u32 i = 0; i < group_count; i++) {
      const struct LineGroup* g = &groups[i];
      write_line(g, tables[i], 0);
      if (g->mode == BY_DMA) {
        u32 n = g->channel;
        REG_DMACNT_H(n) = 0;
        REG_DMASAD(n) = (u32)(tables[i] + g->units * g->width);
        REG_DMADAD(n) = g->addr;
        REG_DMACNT_L(n) = g->units;
        REG_DMACNT_H(n) = DMA_ENABLE | DMA_HBLANK | DMA_REPEAT | DMA_SRC_INC |
                          (g->width == 4 ? DMA_32 : 0) |
                          (g->units == 1 ? DMA_DST_FIXED : DMA_DST_RELOAD);
      }
    }
  }
  REG_IF = flags;
}

static void flatten_irq_writes(void) {
  for (u32 line = 0; line < LINES; line++) {
    u32 n = 0;
    for (u32 i = 0; i < group_count; i++) {
      const struct LineGroup* g = &groups[i];
      if (g->mode != BY_IRQ) continue;
      const u16* entry = (const u16*)(tables[i] + line * g->units * g->width);
      for (u32 k = 0; k < g->units * g->width / 2; k++) {
        if (n < MAX_IRQ_WRITES) irq_writes[line][n++] = ((g->addr - 0x04000000 + 2 * k) << 16) | entry[k];
      }
    }
    irq_writes[line][n] = 0;
  }
}

int main(void) {
  const struct SceneHeader* h = (const struct SceneHeader*)scene_blob;
  REG_IME = 0;
  REG_DISPCNT = 0x0080;  // forced blank while loading

  copy32((volatile u32*)0x06000000, (const u32*)(scene_blob + h->vram_offset), h->vram_size);
  copy32((volatile u32*)0x05000000, (const u32*)(scene_blob + h->pram_offset), 0x400);
  copy32((volatile u32*)0x07000000, (const u32*)(scene_blob + h->oam_offset), 0x400);

  const struct IoWrite* io = (const struct IoWrite*)(scene_blob + h->io_offset);
  for (u32 i = 0; i < h->io_count; i++) {
    write_reg(io[i].addr, (const u8*)&io[i].value, io[i].width);
  }

  const struct LineGroup* src = (const struct LineGroup*)(scene_blob + h->group_offset);
  bool any_irq = false;
  group_count = h->group_count;
  for (u32 i = 0; i < group_count; i++) {
    groups[i] = src[i];
    const u32* table = (const u32*)(scene_blob + src[i].table_offset);
    u32 bytes = (LINES * src[i].units * src[i].width + 3) & ~3u;
    for (u32 k = 0; k < bytes / 4; k++) ((u32*)tables[i])[k] = table[k];
    if (src[i].mode == BY_IRQ) any_irq = true;
  }
  flatten_irq_writes();

  // The first V-Blank IRQ ends forced blank, so the first shown frame
  // already has every setting.
  dispcnt = h->dispcnt;
  IRQ_HANDLER = (u32)irq_handler;
  REG_DISPSTAT = (1 << 3) | (any_irq ? (1 << 4) : 0);
  REG_IF = 0xFFFF;
  REG_IE = IRQ_VBLANK | (any_irq ? IRQ_HBLANK : 0);
  // The handler reads what main wrote; keep the stores before the loop.
  __asm__ volatile("" ::: "memory");
  REG_IME = 1;
  // Wait in BIOS Halt (SWI 02h) so the IRQ starts at the same point of the
  // wait loop every time.
  for (;;) __asm__ volatile("swi 0x020000" ::: "memory");
}
