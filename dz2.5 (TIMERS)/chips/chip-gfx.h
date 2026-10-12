// Маленька графічна бібліотека для дисплеїв кастомних чипів Wokwi.
// Кадровий буфер - RGBA, 4 байти на піксель, розмір задається в chip.json.
// Тут немає libc, тому все на статичних масивах і цілих числах.
#pragma once

#include "wokwi-api.h"

#ifndef GFX_MAX_PIXELS
#define GFX_MAX_PIXELS (128 * 64)
#endif

// RGBA у пам'яті йде як R, G, B, A, тобто в little-endian це 0xAABBGGRR
#define GFX_RGB(r, g, b) ((uint32_t)(0xff000000u | ((uint32_t)(b) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(r)))

typedef struct {
  buffer_t buffer;
  uint32_t width;
  uint32_t height;
  uint32_t pixels[GFX_MAX_PIXELS];
} gfx_t;

static void gfx_init(gfx_t *gfx) {
  gfx->buffer = framebuffer_init(&gfx->width, &gfx->height);
}

static void gfx_clear(gfx_t *gfx, uint32_t color) {
  const uint32_t count = gfx->width * gfx->height;
  for (uint32_t i = 0; i < count; i++) {
    gfx->pixels[i] = color;
  }
}

static void gfx_rect(gfx_t *gfx, int x, int y, int w, int h, uint32_t color) {
  if (w <= 0 || h <= 0) return;

  for (int row = y; row < y + h; row++) {
    if (row < 0 || row >= (int)gfx->height) continue;

    for (int col = x; col < x + w; col++) {
      if (col < 0 || col >= (int)gfx->width) continue;
      gfx->pixels[row * gfx->width + col] = color;
    }
  }
}

// Плавний перехід між двома кольорами, ratio 0...256
static uint32_t gfx_mix(uint32_t from, uint32_t to, int ratio) {
  if (ratio < 0) ratio = 0;
  if (ratio > 256) ratio = 256;

  uint32_t result = 0xff000000u;
  for (int shift = 0; shift <= 16; shift += 8) {
    const int a = (from >> shift) & 0xff;
    const int b = (to >> shift) & 0xff;
    result |= (uint32_t)((a * (256 - ratio) + b * ratio) / 256) << shift;
  }

  return result;
}

// Заокруглений прямокутник: просто зрізає по пікселю в кутах
static void gfx_round_rect(gfx_t *gfx, int x, int y, int w, int h, uint32_t color) {
  gfx_rect(gfx, x + 1, y, w - 2, h, color);
  gfx_rect(gfx, x, y + 1, w, h - 2, color);
}

static void gfx_circle(gfx_t *gfx, int cx, int cy, int radius, uint32_t color) {
  for (int dy = -radius; dy <= radius; dy++) {
    for (int dx = -radius; dx <= radius; dx++) {
      if (dx * dx + dy * dy <= radius * radius + radius) {
        gfx_rect(gfx, cx + dx, cy + dy, 1, 1, color);
      }
    }
  }
}

// Рамка в один піксель по краю дисплея
static void gfx_frame(gfx_t *gfx, uint32_t color) {
  gfx_rect(gfx, 0, 0, (int)gfx->width, 1, color);
  gfx_rect(gfx, 0, (int)gfx->height - 1, (int)gfx->width, 1, color);
  gfx_rect(gfx, 0, 0, 1, (int)gfx->height, color);
  gfx_rect(gfx, (int)gfx->width - 1, 0, 1, (int)gfx->height, color);
}

// Сонечко: кружечок і промені навколо. Яскравість задається кольором
static void gfx_sun(gfx_t *gfx, int cx, int cy, uint32_t color) {
  gfx_circle(gfx, cx, cy, 3, color);

  gfx_rect(gfx, cx - 1, cy - 7, 2, 2, color);
  gfx_rect(gfx, cx - 1, cy + 6, 2, 2, color);
  gfx_rect(gfx, cx - 7, cy - 1, 2, 2, color);
  gfx_rect(gfx, cx + 6, cy - 1, 2, 2, color);

  gfx_rect(gfx, cx - 5, cy - 5, 2, 2, color);
  gfx_rect(gfx, cx + 4, cy - 5, 2, 2, color);
  gfx_rect(gfx, cx - 5, cy + 4, 2, 2, color);
  gfx_rect(gfx, cx + 4, cy + 4, 2, 2, color);
}

// Стрілка вправо: лінія і трикутний наконечник
static void gfx_arrow(gfx_t *gfx, int x, int y, int length, uint32_t color) {
  gfx_rect(gfx, x, y - 1, length - 4, 2, color);
  for (int i = 0; i < 4; i++) {
    gfx_rect(gfx, x + length - 4 + i, y - 3 + i, 1, 6 - i * 2, color);
  }
}

// Цифри 0-9 та '%' у сітці 3x5: кожен рядок - три біти, зверху вниз
static const uint16_t GFX_FONT[11] = {
  0x7B6F,  // 0: 111 101 101 101 111
  0x2C92,  // 1: 010 110 010 010 010
  0x73E7,  // 2: 111 001 111 100 111
  0x73CF,  // 3: 111 001 111 001 111
  0x5BC9,  // 4: 101 101 111 001 001
  0x79CF,  // 5: 111 100 111 001 111
  0x79EF,  // 6: 111 100 111 101 111
  0x7249,  // 7: 111 001 001 001 001
  0x7BEF,  // 8: 111 101 111 101 111
  0x7BCF,  // 9: 111 101 111 001 111
  0x52A5,  // %: 101 001 010 100 101
};

// Малює символ шрифтом 3x5, збільшений у scale разів. index 0-9 = цифра, 10 = '%'
static void gfx_glyph(gfx_t *gfx, int x, int y, int index, int scale, uint32_t color) {
  if (index < 0 || index > 10) return;

  const uint16_t bits = GFX_FONT[index];
  for (int row = 0; row < 5; row++) {
    for (int col = 0; col < 3; col++) {
      const int bit = 14 - (row * 3 + col);
      if ((bits >> bit) & 1) {
        gfx_rect(gfx, x + col * scale, y + row * scale, scale, scale, color);
      }
    }
  }
}

// Ціле число без нулів на початку. Повертає ширину намальованого в пікселях
static int gfx_number(gfx_t *gfx, int x, int y, int value, int scale, uint32_t color) {
  if (value < 0) value = 0;

  int digits[4];
  int count = 0;
  do {
    digits[count++] = value % 10;
    value /= 10;
  } while (value > 0 && count < 4);

  const int step = scale * 4;
  for (int i = 0; i < count; i++) {
    gfx_glyph(gfx, x + (count - 1 - i) * step, y, digits[i], scale, color);
  }

  return count * step;
}

static void gfx_flush(gfx_t *gfx) {
  buffer_write(gfx->buffer, 0, (uint8_t *)gfx->pixels, gfx->width * gfx->height * 4);
}
