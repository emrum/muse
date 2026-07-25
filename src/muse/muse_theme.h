//=========================================================
//  MusE
//  Linux Music Editor
//
//  muse_theme.h
//  Copyright (C) 2026 by the MusE development team
//
//  This program is free software; you can redistribute it and/or
//  modify it under the terms of the GNU General Public License
//  as published by the Free Software Foundation; version 2 of
//  the License, or (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program; if not, write to the Free Software
//  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
//
//=========================================================

#ifndef __MUSE_THEME_H__
#define __MUSE_THEME_H__

#include <optional>

#include <oclero/qlementine/style/Theme.hpp>
#include <oclero/qlementine/style/QlementineStyle.hpp>

#include <QString>
#include <QJsonDocument>

namespace MusEGui {

//---------------------------------------------------------
//   MuseTheme
//    Extends oclero::qlementine::Theme (the ~120-field palette Qlementine
//    uses to style *standard* Qt widgets) with loading of MusE's own
//    custom-widget colors (the ones in MusEGlobal::config / gconfig.h -
//    canvases, knobs, meters, track labels, etc. - that Qlementine's
//    QStyle has no knowledge of, since those widgets paint themselves).
//
//    Both color sets live in the SAME JSON file: Qlementine's fields at
//    the document root (parsed by the base class as usual, via
//    Theme::fromJsonDoc()), and MusE's own fields nested under a
//    "museColors" object, parsed by loadMuseColors() below directly into
//    MusEGlobal::config - the same struct every custom-painted widget
//    already reads from. This does NOT introduce a second, parallel color
//    store: it's a new *file format* (JSON) for the data that used to
//    live in a theme's .cfc file, not a new runtime *model*.
//---------------------------------------------------------

class MuseTheme : public oclero::qlementine::Theme {
public:
  using oclero::qlementine::Theme::Theme;

  // Parses both the Qlementine base fields AND the "museColors" object
  //  (loading the latter straight into MusEGlobal::config as a side
  //  effect). Returns std::nullopt if the file doesn't exist or isn't
  //  valid JSON/doesn't parse as a Theme.
  // NOTE: for a combined one-file theme only. Prefer loadColorPalette*()
  //  below for the split theme/palette setup (themes/ vs themes/muse_custom/).
  static std::optional<MuseTheme> fromJsonPath(const QString& jsonPath);
  static std::optional<MuseTheme> fromJsonDoc(const QJsonDocument& jsonDoc);

  // Loads a standalone MusE color-palette JSON file directly into
  //  MusEGlobal::config - e.g. themes/muse_custom/<name>.json. Unlike
  //  fromJsonPath() above, these files have MusE's color fields directly
  //  at the document root (no "museColors" wrapper, no Qlementine chrome
  //  fields) - they're a palette, not a Theme. Returns false if the file
  //  doesn't exist or isn't valid JSON.
  static bool loadColorPaletteFromJsonPath(const QString& jsonPath);
  static bool loadColorPaletteFromJsonDoc(const QJsonDocument& jsonDoc);

  // Write-side counterpart: serializes MusEGlobal::config's current color
  //  values (the same 94 fields/partColors loadColorPaletteFromJson*()
  //  reads) into a standalone color-palette JSON file - mirrors
  //  Qlementine's own Theme::toJson() pattern for chrome themes, just for
  //  MusE's own museColors extension instead. paletteName (if non-empty)
  //  is written into "meta"/"name" for display purposes; it's not used to
  //  derive the file name - the caller decides where to write via jsonPath.
  static bool saveColorPaletteToJsonPath(const QString& jsonPath, const QString& paletteName = QString());
  static QJsonDocument colorPaletteToJsonDoc(const QString& paletteName = QString());

private:
  // Reads museColorsObj's keys into MusEGlobal::config's QColor fields
  //  (see gconfig.h) by name, plus the "partColors" array. Keys not
  //  present in museColorsObj leave the corresponding MusEGlobal::config
  //  field untouched (whatever it already was - typically the compiled-in
  //  default, unless a previous theme load already changed it).
  static void loadMuseColors(const QJsonObject& museColorsObj);
};

//---------------------------------------------------------
//   MuseStyle
//    Thin subclass of QlementineStyle (the QStyle Qlementine uses to paint
//    *standard* Qt widgets - MuseTheme above is its color/data source, this
//    is the class that does the actual painting). Installed app-wide in
//    main.cpp instead of the stock oclero::qlementine::QlementineStyle.
//
//    Exists to decouple colors Theme ties together only by accident of
//    shared field reuse, without patching Qlementine itself - every
//    override here goes through a documented, public "virtual" extension
//    point QlementineStyle already provides for exactly this purpose (see
//    its header's "Theme-related methods" section).
//
//    Concretely: Theme::secondaryColor is Qlementine's secondary
//    action/foreground color (button fills, QGroupBox label text, etc.),
//    NOT a background/surface color - but MusE toolbar code (ArrangerToolbar
//    and friends in components/*toolbar*.cpp) legitimately uses it for
//    label text, which is its correct semantic role. A popup background
//    that was (indirectly) also tracking secondaryColor caused popup bg
//    and toolbar label text to move together, sometimes colliding (dark
//    text on a bright popup). QlementineStyle's real popup background is
//    menuBackgroundColor(), which normally returns theme.backgroundColorMain1
//    - already independent of secondaryColor. Overriding it here makes
//    that independence explicit and gives MusE its own place to tune the
//    popup surface further, permanently separate from any foreground-role
//    color like secondaryColor.
//---------------------------------------------------------

class MuseStyle : public oclero::qlementine::QlementineStyle {
public:
  explicit MuseStyle(QObject* parent = nullptr) : oclero::qlementine::QlementineStyle(parent) {}

  QColor const& menuBackgroundColor() const override;
};

} // namespace MusEGui

#endif // __MUSE_THEME_H__
