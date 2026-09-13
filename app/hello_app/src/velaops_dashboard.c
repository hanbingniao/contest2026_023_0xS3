/****************************************************************************
 * VelaOps 240x240 资源看板渲染器。
 ****************************************************************************/

#include "velaops_dashboard.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#define FONT_WIDTH 5
#define FONT_HEIGHT 7

typedef struct
{
  uint16_t *pixels;
} dashboard_canvas_t;

static uint16_t rgb565(unsigned int red, unsigned int green,
                       unsigned int blue)
{
  return (uint16_t)(((red & 0xf8) << 8) | ((green & 0xfc) << 3) |
                    (blue >> 3));
}

/* ESP32-S3-EYE 小尺寸 LCD 的暗部抬升较明显，低亮度蓝色仍会发灰并降低
 * 文字对比度。背景与内容区统一使用纯黑，只用中性深灰绘制分隔和进度槽。
 */

static uint16_t color_background(void) { return rgb565(0, 0, 0); }
static uint16_t color_card(void) { return rgb565(0, 0, 0); }
static uint16_t color_highlight(void) { return rgb565(28, 28, 28); }
static uint16_t color_text(void) { return rgb565(255, 255, 255); }
static uint16_t color_muted(void) { return rgb565(160, 160, 160); }
static uint16_t color_cyan(void) { return rgb565(0, 190, 255); }
static uint16_t color_green(void) { return rgb565(0, 220, 96); }
static uint16_t color_amber(void) { return rgb565(255, 174, 48); }
static uint16_t color_red(void) { return rgb565(255, 56, 72); }

static void fill_rect(dashboard_canvas_t *canvas, int x, int y, int width,
                      int height, uint16_t color)
{
  int px;
  int py;

  for (py = y; py < y + height; py++)
    {
      if (py < 0 || py >= VELAOPS_DISPLAY_HEIGHT)
        {
          continue;
        }

      for (px = x; px < x + width; px++)
        {
          if (px >= 0 && px < VELAOPS_DISPLAY_WIDTH)
            {
              canvas->pixels[py * VELAOPS_DISPLAY_WIDTH + px] = color;
            }
        }
    }
}

static void dot(dashboard_canvas_t *canvas, int center_x, int center_y,
                int radius, uint16_t color)
{
  int x;
  int y;

  for (y = -radius; y <= radius; y++)
    {
      for (x = -radius; x <= radius; x++)
        {
          if (x * x + y * y <= radius * radius)
            {
              fill_rect(canvas, center_x + x, center_y + y, 1, 1, color);
            }
        }
    }
}

static void glyph_for(char character, uint8_t glyph[FONT_WIDTH])
{
  static const uint8_t letters[][FONT_WIDTH] =
  {
    {0x1e, 0x05, 0x05, 0x1e, 0}, {0x1f, 0x15, 0x15, 0x0a, 0},
    {0x0e, 0x11, 0x11, 0x0a, 0}, {0x1f, 0x11, 0x11, 0x0e, 0},
    {0x1f, 0x15, 0x15, 0x11, 0}, {0x1f, 0x05, 0x05, 0x01, 0},
    {0x0e, 0x11, 0x15, 0x1d, 0}, {0x1f, 0x04, 0x04, 0x1f, 0},
    {0x11, 0x1f, 0x11, 0, 0},    {0x08, 0x10, 0x10, 0x0f, 0},
    {0x1f, 0x04, 0x0a, 0x11, 0}, {0x1f, 0x10, 0x10, 0x10, 0},
    {0x1f, 0x02, 0x04, 0x02, 0x1f}, {0x1f, 0x02, 0x04, 0x1f, 0},
    {0x0e, 0x11, 0x11, 0x0e, 0}, {0x1f, 0x05, 0x05, 0x02, 0},
    {0x0e, 0x11, 0x19, 0x1e, 0}, {0x1f, 0x05, 0x0d, 0x12, 0},
    {0x12, 0x15, 0x15, 0x09, 0}, {0x01, 0x1f, 0x01, 0, 0},
    {0x0f, 0x10, 0x10, 0x0f, 0}, {0x07, 0x08, 0x10, 0x08, 0x07},
    {0x1f, 0x08, 0x04, 0x08, 0x1f}, {0x11, 0x0a, 0x04, 0x0a, 0x11},
    {0x03, 0x04, 0x18, 0x04, 0x03}, {0x19, 0x15, 0x13, 0x11, 0}
  };
  static const uint8_t digits[][FONT_WIDTH] =
  {
    {0x0e, 0x11, 0x11, 0x0e, 0}, {0x12, 0x1f, 0x10, 0, 0},
    {0x19, 0x15, 0x15, 0x12, 0}, {0x11, 0x15, 0x15, 0x0a, 0},
    {0x07, 0x04, 0x1f, 0x04, 0}, {0x17, 0x15, 0x15, 0x09, 0},
    {0x0e, 0x15, 0x15, 0x08, 0}, {0x01, 0x01, 0x1d, 0x03, 0},
    {0x0a, 0x15, 0x15, 0x0a, 0}, {0x02, 0x15, 0x15, 0x0e, 0}
  };

  memset(glyph, 0, FONT_WIDTH);
  if (character >= 'A' && character <= 'Z')
    {
      memcpy(glyph, letters[character - 'A'], FONT_WIDTH);
    }
  else if (character >= '0' && character <= '9')
    {
      memcpy(glyph, digits[character - '0'], FONT_WIDTH);
    }
  else if (character == '%')
    {
      const uint8_t value[] = {0x19, 0x04, 0x02, 0x13, 0};
      memcpy(glyph, value, sizeof(value));
    }
  else if (character == ':')
    {
      glyph[1] = 0x0a;
      glyph[3] = 0x0a;
    }
  else if (character == '.')
    {
      glyph[1] = 0x10;
    }
  else if (character == '-')
    {
      glyph[1] = 0x04;
      glyph[2] = 0x04;
    }
  else if (character == '/')
    {
      glyph[0] = 0x10;
      glyph[1] = 0x08;
      glyph[2] = 0x04;
      glyph[3] = 0x02;
      glyph[4] = 0x01;
    }
}

static void draw_text(dashboard_canvas_t *canvas, int x, int y,
                      const char *text, int scale, uint16_t color)
{
  uint8_t glyph[FONT_WIDTH];
  size_t index;
  int gx;
  int gy;

  for (index = 0; text[index] != '\0'; index++)
    {
      glyph_for(text[index], glyph);
      for (gx = 0; gx < FONT_WIDTH; gx++)
        {
          for (gy = 0; gy < FONT_HEIGHT; gy++)
            {
              if ((glyph[gx] & (1U << gy)) != 0)
                {
                  fill_rect(canvas, x + (int)index * (FONT_WIDTH + 1) * scale +
                            gx * scale, y + gy * scale, scale, scale, color);
                }
            }
        }
    }
}

static uint16_t status_color(const velaops_display_state_t *state)
{
  if (!state->online || state->health == VELAOPS_HEALTH_UNHEALTHY)
    {
      return color_red();
    }

  return color_green();
}

static void draw_header(dashboard_canvas_t *canvas,
                        const velaops_display_state_t *state,
                        unsigned int page)
{
  unsigned int index;

  dot(canvas, 14, 17, 4, status_color(state));
  draw_text(canvas, 25, 10, "VELAOPS", 2, color_text());
  for (index = 0; index < VELAOPS_DISPLAY_PAGE_COUNT; index++)
    {
      dot(canvas, 196 + (int)index * 15, 17, index == page ? 4 : 2,
          index == page ? color_cyan() : color_muted());
    }

  fill_rect(canvas, 10, 34, 220, 1, color_highlight());
}

static void draw_progress(dashboard_canvas_t *canvas, int x, int y, int width,
                          double percent, uint16_t color)
{
  int filled;

  if (percent < 0.0)
    {
      percent = 0.0;
    }
  else if (percent > 100.0)
    {
      percent = 100.0;
    }

  fill_rect(canvas, x, y, width, 8, color_highlight());
  filled = (int)((double)(width - 4) * percent / 100.0);
  fill_rect(canvas, x + 2, y + 2, filled, 4, color);
}

static void format_megabytes(char *buffer, size_t capacity, uint64_t bytes)
{
  snprintf(buffer, capacity, "%llu MB",
           (unsigned long long)(bytes / 1048576));
}

static void draw_overview(dashboard_canvas_t *canvas,
                          const velaops_display_state_t *state)
{
  char value[24];
  uint16_t accent = status_color(state);

  draw_text(canvas, 12, 44, "SYSTEM HEALTH", 1, color_muted());
  draw_text(canvas, 12, 57,
            !state->online ? "OFFLINE" :
            state->health == VELAOPS_HEALTH_HEALTHY ? "HEALTHY" : "WARNING",
            2, accent);

  fill_rect(canvas, 10, 82, 220, 78, color_card());
  draw_text(canvas, 20, 92, "MEMORY LOAD", 1, color_muted());
  if (state->has_resources)
    {
      snprintf(value, sizeof(value), "%.1f%%", state->memory.used_percent);
    }
  else
    {
      snprintf(value, sizeof(value), "--");
    }
  draw_text(canvas, 20, 108, value, 4, color_text());
  draw_progress(canvas, 20, 146, 200,
                state->has_resources ? state->memory.used_percent : 0.0,
                state->memory.used_percent >= 85.0 ? color_red() : color_cyan());

  fill_rect(canvas, 10, 170, 105, 47, color_card());
  draw_text(canvas, 18, 179, "DISK", 1, color_muted());
  if (state->has_resources)
    {
      snprintf(value, sizeof(value), "%.1f%%",
               state->resources.disk_percent);
    }
  else
    {
      snprintf(value, sizeof(value), "--");
    }
  draw_text(canvas, 18, 194, value, 2,
            state->resources.disk_percent >= 85.0 ? color_amber() :
                                                    color_text());

  fill_rect(canvas, 125, 170, 105, 47, color_card());
  draw_text(canvas, 133, 179, "PROXY PORT", 1, color_muted());
  draw_text(canvas, 133, 194,
            state->has_resources && state->resources.port_reachable ?
            "OPEN" : "CLOSED", 2,
            state->has_resources && state->resources.port_reachable ?
            color_green() : color_red());
}

static void draw_service(dashboard_canvas_t *canvas,
                         const velaops_display_state_t *state)
{
  char value[24];
  int active = state->has_resources && state->resources.service_active;
  int reachable = state->has_resources && state->resources.port_reachable;

  draw_text(canvas, 12, 44, "SERVICE STATUS", 1, color_muted());
  fill_rect(canvas, 10, 61, 220, 55, color_card());
  dot(canvas, 28, 88, 7, active ? color_green() : color_red());
  draw_text(canvas, 46, 75, "PROXY SERVICE", 1, color_muted());
  draw_text(canvas, 46, 91, active ? "ACTIVE" : "DOWN", 2,
            active ? color_green() : color_red());

  fill_rect(canvas, 10, 126, 105, 71, color_card());
  draw_text(canvas, 18, 137, "PORT", 1, color_muted());
  draw_text(canvas, 18, 157, reachable ? "OPEN" : "CLOSED", 2,
            reachable ? color_green() : color_red());

  fill_rect(canvas, 125, 126, 105, 71, color_card());
  draw_text(canvas, 133, 137, "LATENCY", 1, color_muted());
  if (state->has_resources)
    {
      snprintf(value, sizeof(value), "%d MS",
               state->resources.port_latency_ms);
    }
  else
    {
      snprintf(value, sizeof(value), "-- MS");
    }
  draw_text(canvas, 133, 157, value, 2,
            state->resources.port_latency_ms > 500 ? color_amber() :
                                                    color_cyan());

  draw_text(canvas, 12, 222, "BOOT NEXT PAGE", 1, color_muted());
}

static void draw_memory_details(dashboard_canvas_t *canvas,
                                const velaops_display_state_t *state)
{
  char value[24];

  draw_text(canvas, 12, 44, "MEMORY DETAILS", 1, color_muted());
  fill_rect(canvas, 10, 61, 220, 128, color_card());

  draw_text(canvas, 20, 76, "TOTAL", 1, color_muted());
  if (state->has_resources)
    {
      format_megabytes(value, sizeof(value), state->memory.total_bytes);
    }
  else
    {
      snprintf(value, sizeof(value), "-- MB");
    }
  draw_text(canvas, 112, 71, value, 2, color_text());
  fill_rect(canvas, 20, 101, 200, 1, color_highlight());

  draw_text(canvas, 20, 116, "USED", 1, color_muted());
  if (state->has_resources)
    {
      format_megabytes(value, sizeof(value), state->memory.used_bytes);
    }
  draw_text(canvas, 112, 111, value, 2, color_cyan());
  fill_rect(canvas, 20, 141, 200, 1, color_highlight());

  draw_text(canvas, 20, 156, "AVAILABLE", 1, color_muted());
  if (state->has_resources)
    {
      format_megabytes(value, sizeof(value), state->memory.available_bytes);
    }
  draw_text(canvas, 112, 151, value, 2, color_green());

  fill_rect(canvas, 10, 199, 220, 20, color_highlight());
  draw_text(canvas, 20, 205, "AUTO REFRESH 5 SEC", 1, color_cyan());
  draw_text(canvas, 12, 225, "BOOT NEXT PAGE", 1, color_muted());
}

int velaops_dashboard_render(uint16_t *pixels, size_t pixel_count,
                             const velaops_display_state_t *state,
                             unsigned int page)
{
  dashboard_canvas_t canvas;

  if (pixels == NULL || state == NULL ||
      pixel_count < VELAOPS_DISPLAY_WIDTH * VELAOPS_DISPLAY_HEIGHT ||
      page >= VELAOPS_DISPLAY_PAGE_COUNT)
    {
      return -EINVAL;
    }

  canvas.pixels = pixels;
  fill_rect(&canvas, 0, 0, VELAOPS_DISPLAY_WIDTH, VELAOPS_DISPLAY_HEIGHT,
            color_background());
  draw_header(&canvas, state, page);

  if (page == 0)
    {
      draw_overview(&canvas, state);
    }
  else if (page == 1)
    {
      draw_service(&canvas, state);
    }
  else
    {
      draw_memory_details(&canvas, state);
    }

  return 0;
}

int velaops_dashboard_render_test(uint16_t *pixels, size_t pixel_count)
{
  static const char *labels[] =
  {
    "RED", "GREEN", "BLUE", "CYAN", "MAGENTA", "YELLOW", "WHITE"
  };
  uint16_t colors[] =
  {
    rgb565(255, 0, 0), rgb565(0, 255, 0), rgb565(0, 0, 255),
    rgb565(0, 255, 255), rgb565(255, 0, 255), rgb565(255, 255, 0),
    rgb565(255, 255, 255)
  };
  dashboard_canvas_t canvas;
  unsigned int index;

  if (pixels == NULL ||
      pixel_count < VELAOPS_DISPLAY_WIDTH * VELAOPS_DISPLAY_HEIGHT)
    {
      return -EINVAL;
    }

  canvas.pixels = pixels;
  fill_rect(&canvas, 0, 0, VELAOPS_DISPLAY_WIDTH, VELAOPS_DISPLAY_HEIGHT,
            rgb565(0, 0, 0));
  draw_text(&canvas, 12, 10, "RGB565 COLOR TEST", 2,
            rgb565(255, 255, 255));
  fill_rect(&canvas, 10, 31, 220, 1, rgb565(255, 255, 255));

  for (index = 0; index < sizeof(colors) / sizeof(colors[0]); index++)
    {
      int y = 42 + (int)index * 23;

      draw_text(&canvas, 12, y + 4, labels[index], 1,
                rgb565(255, 255, 255));
      fill_rect(&canvas, 76, y, 154, 15, colors[index]);
    }

  draw_text(&canvas, 12, 207, "GRAY", 1, rgb565(255, 255, 255));
  fill_rect(&canvas, 76, 205, 30, 20, rgb565(0, 0, 0));
  fill_rect(&canvas, 106, 205, 31, 20, rgb565(64, 64, 64));
  fill_rect(&canvas, 137, 205, 31, 20, rgb565(128, 128, 128));
  fill_rect(&canvas, 168, 205, 31, 20, rgb565(192, 192, 192));
  fill_rect(&canvas, 199, 205, 31, 20, rgb565(255, 255, 255));
  fill_rect(&canvas, 10, 233, 220, 1, rgb565(255, 255, 255));
  return 0;
}

static void draw_text_centered(dashboard_canvas_t *canvas, int y,
                               const char *text, int scale, uint16_t color)
{
  int text_width = (int)strlen(text) * (FONT_WIDTH + 1) * scale;
  int x = (VELAOPS_DISPLAY_WIDTH - text_width) / 2;

  if (x < 0)
    {
      x = 0;
    }
  draw_text(canvas, x, y, text, scale, color);
}

int velaops_dashboard_render_message(uint16_t *pixels, size_t pixel_count,
                                     const char *title, const char *text,
                                     int blink_phase)
{
  dashboard_canvas_t canvas;
  char line[2][13];
  uint16_t border;
  size_t length;
  size_t first;
  int lines;

  if (pixels == NULL || title == NULL || text == NULL ||
      pixel_count < VELAOPS_DISPLAY_WIDTH * VELAOPS_DISPLAY_HEIGHT)
    {
      return -EINVAL;
    }

  /* 边框在两种高对比颜色间交替，由调用方按固定节奏重绘实现闪烁。 */
  border = blink_phase != 0 ? color_amber() : color_red();
  canvas.pixels = pixels;
  fill_rect(&canvas, 0, 0, VELAOPS_DISPLAY_WIDTH, VELAOPS_DISPLAY_HEIGHT,
            color_background());

  /* 消息框边框：四条 3 像素色带。 */
  fill_rect(&canvas, 10, 36, 220, 3, border);
  fill_rect(&canvas, 10, 201, 220, 3, border);
  fill_rect(&canvas, 10, 36, 3, 168, border);
  fill_rect(&canvas, 227, 36, 3, 168, border);

  draw_text_centered(&canvas, 52, title, 2, border);
  fill_rect(&canvas, 22, 74, 196, 1, color_highlight());

  /* 正文最多两行，每行最多 12 个字符；字库无中文，非 ASCII 在屏显层之前
   * 已被净化。 */
  length = strlen(text);
  if (length > 24)
    {
      length = 24;
    }
  first = length <= 12 ? length : 12;
  memcpy(line[0], text, first);
  line[0][first] = '\0';
  lines = 1;
  if (length > 12)
    {
      memcpy(line[1], text + 12, length - 12);
      line[1][length - 12] = '\0';
      lines = 2;
    }

  if (lines == 1)
    {
      draw_text_centered(&canvas, 108, line[0], 3, color_text());
    }
  else
    {
      draw_text_centered(&canvas, 92, line[0], 3, color_text());
      draw_text_centered(&canvas, 120, line[1], 3, color_text());
    }

  draw_text_centered(&canvas, 168, "PRESS BOOT TO CLOSE", 1, color_muted());
  return 0;
}
