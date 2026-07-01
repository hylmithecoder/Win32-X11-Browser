#ifndef ELEMENTS_INPUT_HPP
#define ELEMENTS_INPUT_HPP

#include "Font.hpp" // -> Paint.hpp -> Layout.hpp (Canvas, Color, Rect)
#include "Wrapper.hpp"

#include <string>

namespace DesktopWebview {
namespace Elements {

// The kind of form control a DOM element represents. This is the single place
// that maps a tag/type to a control category; the sizing, painting and
// interaction helpers below all key off it, so the Browser and Layout engines
// no longer each carry their own copy of the "is this a form control?" logic.
enum class ControlKind {
  NoControl, // not a form control (named to avoid X11's `None` macro)
  Text,     // <input> text/search/email/url/tel/number/date/... and the default
  Password, // <input type=password> (rendered masked)
  Checkbox, // <input type=checkbox>
  Radio,    // <input type=radio>
  Button,   // <button>, <input type=submit|button|reset>
  Textarea, // <textarea>
  Select,   // <select>
  Hidden,   // <input type=hidden> (generates no box)
};

// Classify a DOM node. Returns None for non-elements and non-controls.
ControlKind classify(const Wrapper::Node &node);

// True for any recognised form control, including hidden inputs.
bool isFormControl(const Wrapper::Node &node);

// True for controls that accept typed text (Text, Password, Textarea).
bool isTextEntry(const Wrapper::Node &node);

// The intrinsic (default) content-box size of a control in CSS pixels, used
// when width/height are not otherwise specified. `fontSize` is the control's
// resolved font size. Returns {0,0} for None/Hidden.
struct Size {
  int width = 0;
  int height = 0;
};
Size intrinsicSize(const Wrapper::Node &node, int fontSize);

// The text a control shows and whether it is placeholder text (drawn greyed).
// For checkboxes/radios the text is empty.
struct DisplayText {
  std::string text;
  bool placeholder = false;
};
DisplayText displayText(const Wrapper::Node &node);

// Paint the native widget for `node` into content rect `rect`. `focused` draws
// a focus ring (and a caret for text entry). No-op for None/Hidden controls.
void paint(Paint::Canvas &canvas, const Wrapper::Node &node,
           const Layout::Rect &rect, int fontSize, bool focused);

// ---- Interaction (mutate the DOM; caller re-styles/relayouts afterwards) ----

// Toggle a checkbox's `checked` attribute; returns the resulting state.
bool toggleCheckbox(Wrapper::Node &node);

// Check `node` and clear every other radio sharing its `name` in `doc`.
void selectRadio(Wrapper::Node &node, Wrapper::HtmlDocument &doc);

// Advance a <select> to its next <option> (wrapping). Returns true if it has
// options to cycle.
bool cycleSelect(Wrapper::Node &node);

// Append a typed character to a text-entry control's value/content.
void insertChar(Wrapper::Node &node, char ch);

// Delete the last character from a text-entry control. Returns true if a
// character was removed.
bool backspace(Wrapper::Node &node);

} // namespace Elements
} // namespace DesktopWebview

#endif // ELEMENTS_INPUT_HPP
