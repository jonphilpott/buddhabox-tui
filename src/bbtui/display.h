/**
 * @file display.h
 * @brief The virtual OLED: a 128 x 64, one-bit frame buffer with the
 *        same drawing calls as buddhabox-framework's Oled.
 *
 * On the board, Oled keeps a 1 KB frame buffer and sends it over I2C.
 * Here there is no I2C: the panel (panel.cpp) reads the same buffer and
 * draws it in the terminal with Unicode braille characters. Each braille
 * character is a 2 x 4 grid of dots, so the 128 x 64 screen takes 64
 * columns by 16 rows of text.
 *
 * You never construct one: the panel owns it, and tui::display()
 * returns it. Draw in loop(), as often as you like; the panel shows
 * whatever is in the buffer about 30 times a second. Nothing appears
 * until something is drawn, so patches without a screen get a smaller
 * panel.
 *
 * Copied from the framework's io/oled.h with the I2C parts removed: no
 * begin() and no display(), since there is nothing to send.
 *
 * Example:
 *   Oled &oled = tui::display();
 *   oled.clear();
 *   oled.text(0, 0, "HELLO");
 *   oled.rect(0, 56, (int)(level * 128), 8, true); // a level bar
 */

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace bb {

/**
 * The 5 x 7 font: 95 printable ASCII characters from ' ' to '~'.
 *
 * Each character is 5 bytes, one per column, left to right. In each
 * byte bit 0 is the top row and bit 6 the bottom; bit 7 is unused. This
 * is the classic "glcdfont" layout used by most small displays.
 */
static const uint8_t OLED_FONT[95][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, // ' '
    {0x00, 0x00, 0x5F, 0x00, 0x00}, // !
    {0x00, 0x07, 0x00, 0x07, 0x00}, // "
    {0x14, 0x7F, 0x14, 0x7F, 0x14}, // #
    {0x24, 0x2A, 0x7F, 0x2A, 0x12}, // $
    {0x23, 0x13, 0x08, 0x64, 0x62}, // %
    {0x36, 0x49, 0x55, 0x22, 0x50}, // &
    {0x00, 0x05, 0x03, 0x00, 0x00}, // '
    {0x00, 0x1C, 0x22, 0x41, 0x00}, // (
    {0x00, 0x41, 0x22, 0x1C, 0x00}, // )
    {0x14, 0x08, 0x3E, 0x08, 0x14}, // *
    {0x08, 0x08, 0x3E, 0x08, 0x08}, // +
    {0x00, 0x50, 0x30, 0x00, 0x00}, // ,
    {0x08, 0x08, 0x08, 0x08, 0x08}, // -
    {0x00, 0x60, 0x60, 0x00, 0x00}, // .
    {0x20, 0x10, 0x08, 0x04, 0x02}, // /
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, // 0
    {0x00, 0x42, 0x7F, 0x40, 0x00}, // 1
    {0x42, 0x61, 0x51, 0x49, 0x46}, // 2
    {0x21, 0x41, 0x45, 0x4B, 0x31}, // 3
    {0x18, 0x14, 0x12, 0x7F, 0x10}, // 4
    {0x27, 0x45, 0x45, 0x45, 0x39}, // 5
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, // 6
    {0x01, 0x71, 0x09, 0x05, 0x03}, // 7
    {0x36, 0x49, 0x49, 0x49, 0x36}, // 8
    {0x06, 0x49, 0x49, 0x29, 0x1E}, // 9
    {0x00, 0x36, 0x36, 0x00, 0x00}, // :
    {0x00, 0x56, 0x36, 0x00, 0x00}, // ;
    {0x08, 0x14, 0x22, 0x41, 0x00}, // <
    {0x14, 0x14, 0x14, 0x14, 0x14}, // =
    {0x00, 0x41, 0x22, 0x14, 0x08}, // >
    {0x02, 0x01, 0x51, 0x09, 0x06}, // ?
    {0x32, 0x49, 0x79, 0x41, 0x3E}, // @
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, // A
    {0x7F, 0x49, 0x49, 0x49, 0x36}, // B
    {0x3E, 0x41, 0x41, 0x41, 0x22}, // C
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, // D
    {0x7F, 0x49, 0x49, 0x49, 0x41}, // E
    {0x7F, 0x09, 0x09, 0x09, 0x01}, // F
    {0x3E, 0x41, 0x49, 0x49, 0x7A}, // G
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, // H
    {0x00, 0x41, 0x7F, 0x41, 0x00}, // I
    {0x20, 0x40, 0x41, 0x3F, 0x01}, // J
    {0x7F, 0x08, 0x14, 0x22, 0x41}, // K
    {0x7F, 0x40, 0x40, 0x40, 0x40}, // L
    {0x7F, 0x02, 0x0C, 0x02, 0x7F}, // M
    {0x7F, 0x04, 0x08, 0x10, 0x7F}, // N
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, // O
    {0x7F, 0x09, 0x09, 0x09, 0x06}, // P
    {0x3E, 0x41, 0x51, 0x21, 0x5E}, // Q
    {0x7F, 0x09, 0x19, 0x29, 0x46}, // R
    {0x46, 0x49, 0x49, 0x49, 0x31}, // S
    {0x01, 0x01, 0x7F, 0x01, 0x01}, // T
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, // U
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, // V
    {0x3F, 0x40, 0x38, 0x40, 0x3F}, // W
    {0x63, 0x14, 0x08, 0x14, 0x63}, // X
    {0x07, 0x08, 0x70, 0x08, 0x07}, // Y
    {0x61, 0x51, 0x49, 0x45, 0x43}, // Z
    {0x00, 0x7F, 0x41, 0x41, 0x00}, // [
    {0x02, 0x04, 0x08, 0x10, 0x20}, // backslash
    {0x00, 0x41, 0x41, 0x7F, 0x00}, // ]
    {0x04, 0x02, 0x01, 0x02, 0x04}, // ^
    {0x40, 0x40, 0x40, 0x40, 0x40}, // _
    {0x00, 0x01, 0x02, 0x04, 0x00}, // `
    {0x20, 0x54, 0x54, 0x54, 0x78}, // a
    {0x7F, 0x48, 0x44, 0x44, 0x38}, // b
    {0x38, 0x44, 0x44, 0x44, 0x20}, // c
    {0x38, 0x44, 0x44, 0x48, 0x7F}, // d
    {0x38, 0x54, 0x54, 0x54, 0x18}, // e
    {0x08, 0x7E, 0x09, 0x01, 0x02}, // f
    {0x0C, 0x52, 0x52, 0x52, 0x3E}, // g
    {0x7F, 0x08, 0x04, 0x04, 0x78}, // h
    {0x00, 0x44, 0x7D, 0x40, 0x00}, // i
    {0x20, 0x40, 0x44, 0x3D, 0x00}, // j
    {0x7F, 0x10, 0x28, 0x44, 0x00}, // k
    {0x00, 0x41, 0x7F, 0x40, 0x00}, // l
    {0x7C, 0x04, 0x18, 0x04, 0x78}, // m
    {0x7C, 0x08, 0x04, 0x04, 0x78}, // n
    {0x38, 0x44, 0x44, 0x44, 0x38}, // o
    {0x7C, 0x14, 0x14, 0x14, 0x08}, // p
    {0x08, 0x14, 0x14, 0x18, 0x7C}, // q
    {0x7C, 0x08, 0x04, 0x04, 0x08}, // r
    {0x48, 0x54, 0x54, 0x54, 0x20}, // s
    {0x04, 0x3F, 0x44, 0x40, 0x20}, // t
    {0x3C, 0x40, 0x40, 0x20, 0x7C}, // u
    {0x1C, 0x20, 0x40, 0x20, 0x1C}, // v
    {0x3C, 0x40, 0x30, 0x40, 0x3C}, // w
    {0x44, 0x28, 0x10, 0x28, 0x44}, // x
    {0x0C, 0x50, 0x50, 0x50, 0x3C}, // y
    {0x44, 0x64, 0x54, 0x4C, 0x44}, // z
    {0x00, 0x08, 0x36, 0x41, 0x00}, // {
    {0x00, 0x00, 0x7F, 0x00, 0x00}, // |
    {0x00, 0x41, 0x36, 0x08, 0x00}, // }
    {0x08, 0x04, 0x08, 0x10, 0x08}, // ~
};

/**
 * A 128 x 64 one-bit frame buffer with simple drawing: pixels, lines,
 * rectangles, text and numbers.
 *
 * The buffer is laid out like the SSD1306's memory: 8 "pages" of 8 rows,
 * each page 128 bytes, one byte per column, bit 0 at the top. Keeping
 * the board's layout means drawing code moves between the two projects
 * unchanged.
 *
 * Example:
 *   Oled &oled = tui::display();
 *   oled.clear();
 *   int x = oled.text(0, 0, "Freq ");
 *   oled.number(x, 0, 440.0f, 1);  // "Freq 440.0"
 */
class Oled {
public:
  static constexpr int WIDTH = 128;
  static constexpr int HEIGHT = 64;
  static constexpr int PAGES = HEIGHT / 8;
  // Text cells: 5-pixel glyph + 1 blank column, 7 rows + 1 blank row.
  static constexpr int CHAR_W = 6;
  static constexpr int CHAR_H = 8;

  /** An empty (dark) screen. */
  Oled() { memset(buf_, 0, sizeof(buf_)); }

  /**
   * Blank the frame buffer.
   *
   * Example:
   *   oled.clear();   // start each frame from black
   */
  void clear() {
    memset(buf_, 0, sizeof(buf_));
    used_ = true;
  }

  /**
   * Set or clear one pixel. Off-screen coordinates are ignored.
   *
   * @param x   Column, 0 (left) to 127.
   * @param y   Row, 0 (top) to 63.
   * @param on  true lights the pixel, false darkens it.
   *
   * Example:
   *   oled.pixel(64, 32);   // dot in the middle
   */
  void pixel(int x, int y, bool on = true) {
    // One unsigned comparison checks both "< 0" and ">= WIDTH": a
    // negative int converted to unsigned becomes a huge number.
    if ((unsigned)x >= WIDTH || (unsigned)y >= HEIGHT)
      return;
    uint8_t bit = 1 << (y & 7); // y % 8: which bit in the column byte
    if (on)
      buf_[y >> 3][x] |= bit; // y / 8: which page
    else
      buf_[y >> 3][x] &= ~bit;
    used_ = true;
  }

  /**
   * Read one pixel back (the panel uses this to draw the screen).
   *
   * @param x, y  As pixel(); off-screen reads as dark.
   * @return      true if the pixel is lit.
   *
   * Example:
   *   bool lit = oled.get(0, 0);
   */
  bool get(int x, int y) const {
    if ((unsigned)x >= WIDTH || (unsigned)y >= HEIGHT)
      return false;
    return (buf_[y >> 3][x] >> (y & 7)) & 1;
  }

  /**
   * @return  true once anything has been drawn. The panel leaves the
   *          screen out until then.
   */
  bool used() const { return used_; }

  /**
   * Draw a straight line between two points, ends included.
   *
   * @param x0, y0  Start point.
   * @param x1, y1  End point. Either end may be off-screen; the visible
   *                part is drawn.
   * @param on      true draws lit pixels, false dark ones.
   *
   * Example:
   *   oled.line(0, 63, 127, 0);   // diagonal, bottom-left to top-right
   */
  void line(int x0, int y0, int x1, int y1, bool on = true) {
    // Bresenham's algorithm: step one pixel at a time along the line,
    // keeping a running error term that says when the other coordinate
    // should also step. Integer-only, so no float maths per pixel.
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
      pixel(x0, y0, on);
      if (x0 == x1 && y0 == y1)
        break;
      int e2 = 2 * err;
      if (e2 >= dy) { // time to step in x
        err += dy;
        x0 += sx;
      }
      if (e2 <= dx) { // time to step in y
        err += dx;
        y0 += sy;
      }
    }
  }

  /**
   * Draw a rectangle, outlined or filled.
   *
   * @param x, y  Top-left corner.
   * @param w, h  Width and height in pixels. Zero or negative draws
   *              nothing.
   * @param fill  false (default) draws a 1-pixel outline; true fills it.
   * @param on    true draws lit pixels, false dark ones. A filled dark
   *              rectangle erases an area.
   *
   * Example:
   *   oled.rect(0, 56, 128, 8);                       // empty bar
   *   oled.rect(0, 56, (int)(level * 128), 8, true);  // filled part
   */
  void rect(int x, int y, int w, int h, bool fill = false, bool on = true) {
    if (w <= 0 || h <= 0)
      return;
    if (fill) {
      for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++)
          pixel(i, j, on);
      return;
    }
    int x1 = x + w - 1, y1 = y + h - 1;
    line(x, y, x1, y, on);   // top
    line(x, y1, x1, y1, on); // bottom
    line(x, y, x, y1, on);   // left
    line(x1, y, x1, y1, on); // right
  }

  /**
   * Draw a string in the built-in 5 x 7 font.
   *
   * Each character fills a 6 x 8 cell, background included, so new text
   * fully replaces old text drawn at the same place. 21 characters fit
   * across, 8 lines down. Characters outside printable ASCII show as '?'.
   *
   * @param x, y  Top-left of the first character.
   * @param s     The text (a null-terminated C string).
   * @param on    true for lit text on black, false for black text on a
   *              lit cell (a highlight).
   * @return      The x just after the last character, to continue on
   *              the same line.
   *
   * Example:
   *   int x = oled.text(0, 0, "Freq ");
   *   oled.text(0, 8, "MENU", false); // inverted
   */
  int text(int x, int y, const char *s, bool on = true) {
    for (; *s; s++) {
      char c = *s;
      if (c < ' ' || c > '~')
        c = '?';
      const uint8_t *glyph = OLED_FONT[c - ' '];
      // Column by column; column 5 is the blank gap (0), and bit 7 of
      // every column is the blank row under the glyph.
      for (int col = 0; col < CHAR_W; col++) {
        uint8_t bits = col < 5 ? glyph[col] : 0;
        for (int row = 0; row < CHAR_H; row++)
          pixel(x + col, y + row, ((bits >> row) & 1) ? on : !on);
      }
      x += CHAR_W;
    }
    return x;
  }

  /**
   * Draw a number with a fixed count of decimal places.
   *
   * @param x, y      Top-left, as text().
   * @param value     The number.
   * @param decimals  Digits after the point, 0 to 4.
   * @param on        As text().
   * @return          The x just after the last character.
   *
   * Example:
   *   oled.number(0, 0, 3.14159f, 2);   // "3.14"
   */
  int number(int x, int y, float value, int decimals = 2, bool on = true) {
    // On a desktop, printf's %f works (the board's toolchain leaves it
    // out), so this is one call rather than the board's digit loop.
    char buf[24];
    decimals = decimals < 0 ? 0 : (decimals > 4 ? 4 : decimals);
    snprintf(buf, sizeof(buf), "%.*f", decimals, value);
    return text(x, y, buf, on);
  }

private:
  uint8_t buf_[PAGES][WIDTH]; // the frame buffer, 1 KB
  bool used_ = false;         // drawn on yet?
};

} // namespace bb
