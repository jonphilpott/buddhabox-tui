/**
 * @file menu.h
 * @brief A scrolling settings menu on the virtual OLED, driven by the
 *        arrow keys and the space bar.
 *
 * Adapted from buddhabox-framework's io/menu.h. The board's menu has one
 * encoder, so it needs an edit mode: press to start changing a value,
 * press again to go back to moving. With four arrow keys there is no
 * need: up and down always move, left and right always change.
 *
 * HOW IT IS PLAYED:
 *   up/down     move the highlight up and down the list
 *   left/right  change the highlighted item's value (shown as <value>)
 *   space       on an action item (such as "Randomize"): do it
 *
 * HOW THE CODE FITS TOGETHER: Menu reads no keys itself. Your loop()
 * hands update() the arrow-key counts and the space bar, and update()
 * says what happened. That keeps Menu usable with any input: keys,
 * MIDI, or a test.
 *
 * Every value is a whole number with a range and a step. Integers keep
 * the maths simple (each key press adds or subtracts one step, so
 * nothing drifts); your code turns them into Hz, seconds or whatever it
 * needs.
 *
 * Example usage:
 *   enum { CUTOFF, WAVE, RANDOMIZE, kItems };   // item numbers
 *   const char *const waves[] = {"Saw", "Square"};
 *   MenuItem items[kItems] = {
 *       {"Cutoff", 50, 0, 100, 2},             // name, value, min, max, step
 *       {"Wave", 0, 0, 1, 1, waves},           // shown as Saw / Square
 *       {"Randomize"},                         // step 0: an action
 *   };
 *   Menu menu(items, kItems);
 *
 *   void loop() {
 *       Menu::Event e = menu.update(tui::menuMove(), tui::menuAdjust(),
 *                                   tui::menuPressed());
 *       if (e == Menu::CHANGED)
 *           applySettings();       // reads items[CUTOFF].value etc.
 *       if (e == Menu::ACTION && menu.selected() == RANDOMIZE)
 *           randomize();
 *       menu.draw(tui::display(), 16, 6);  // six rows from y = 16
 *   }
 */

#pragma once

#include "bbtui/display.h"

namespace bb {

/**
 * One line of a Menu. Fill it in with braces; fields left out are zero,
 * which is what makes `{"Randomize"}` an action.
 *
 * - name:   shown on the left
 * - value:  the current setting; read it in your code
 * - min, max, step: the range, and how much one left/right press
 *           changes it
 * - labels: optional words to show instead of the number, one per value
 *           from min to max (labels[value - min])
 * - suffix: optional text after the number, such as " Hz" or "/16"
 *
 * An item with step 0 is an action: space on it makes update() return
 * Menu::ACTION, and no value shows.
 *
 * Example:
 *   MenuItem tempo = {"Tempo", 120, 60, 200, 1, nullptr, " bpm"};
 */
struct MenuItem {
  const char *name;
  int value, min, max, step;
  const char *const *labels;
  const char *suffix;
};

/**
 * A scrolling list of MenuItems, played with the arrow keys and drawn on
 * the display. Holds a pointer to your array, so your code reads values
 * straight from it.
 *
 * Example:
 *   MenuItem items[] = {{"Level", 70, 0, 100, 2}, {"Reset"}};
 *   Menu menu(items, 2);
 */
class Menu {
public:
  /** What update() reports. */
  enum Event {
    NONE,    ///< nothing to act on (or only the highlight moved)
    CHANGED, ///< the highlighted item's value changed
    ACTION   ///< an action item was pressed
  };

  /**
   * @param items  Your array of items. Menu keeps the pointer, so the
   *               array must outlive it (a global is simplest).
   * @param count  How many items are in it
   *
   * Example:
   *   Menu menu(items, sizeof(items) / sizeof(items[0]));
   */
  Menu(MenuItem *items, int count) : items_(items), count_(count) {}

  /**
   * Apply one round of input. Call it every loop().
   *
   * @param move     Up/down presses since last time (tui::menuMove()):
   *                 positive moves down the list
   * @param adjust   Left/right presses since last time
   *                 (tui::menuAdjust()): positive raises the value
   * @param pressed  true if space was pressed (tui::menuPressed())
   * @return  CHANGED if a value changed, ACTION if an action item was
   *          pressed (selected() says which item), otherwise NONE
   *
   * Example:
   *   if (menu.update(tui::menuMove(), tui::menuAdjust(),
   *                   tui::menuPressed()) == Menu::CHANGED)
   *     applySettings();
   */
  Event update(int move, int adjust, bool pressed) {
    Event event = NONE;

    // 1. Up/down: move the highlight, stopping at either end.
    selected_ = clampInt(selected_ + move, 0, count_ - 1);

    // 2. Left/right: change the highlighted value. Action items have
    //    step 0, so this leaves them alone.
    MenuItem &m = items_[selected_];
    if (adjust != 0 && m.step != 0) {
      int v = clampInt(m.value + adjust * m.step, m.min, m.max);
      if (v != m.value) {
        m.value = v;
        event = CHANGED;
      }
    }

    // 3. Space: fire an action item.
    if (pressed && m.step == 0)
      event = ACTION;
    return event;
  }

  /** @return  The highlighted item's index */
  int selected() const { return selected_; }

  /**
   * Draw the menu: one row of 8 pixels per item, the highlighted row
   * inverted with its value in <brackets>, scrolled so the highlight is
   * always in view. The panel shows it on its next frame.
   *
   * @param oled    The display
   * @param y       Top of the first row, in pixels
   * @param rows    How many rows to draw; (64 - y) / 8 fills the rest
   *                of the screen
   * @param valueX  Where values start, in pixels from the left; names
   *                longer than about valueX / 6 characters run into them
   *
   * Example:
   *   menu.draw(tui::display(), 16, 6); // below a 16-pixel header
   */
  void draw(Oled &oled, int y, int rows, int valueX = 80) const {
    // Scroll only as far as needed to keep the highlight on the last
    // visible row.
    int first = selected_ - rows + 1;
    first = first < 0 ? 0 : first;
    for (int r = 0; r < rows; r++) {
      int item = first + r;
      int top = y + r * 8;
      bool on = item != selected_; // the highlight is drawn inverted
      oled.rect(0, top, 128, 8, true, !on);
      if (item >= count_)
        continue;
      const MenuItem &m = items_[item];
      oled.text(2, top, m.name, on);
      if (m.step == 0)
        continue; // an action: no value

      // The highlighted value gets <brackets>: left/right changes it.
      bool mark = item == selected_;
      int x = mark ? oled.text(valueX - 6, top, "<", on) : valueX;
      if (m.labels)
        x = oled.text(x, top, m.labels[m.value - m.min], on);
      else
        x = oled.number(x, top, (float)m.value, 0, on);
      if (m.suffix)
        x = oled.text(x, top, m.suffix, on);
      if (mark)
        oled.text(x, top, ">", on);
    }
  }

private:
  static int clampInt(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
  }

  MenuItem *items_;
  int count_;
  int selected_ = 0;
};

} // namespace bb
