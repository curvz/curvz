#pragma once
//
// ImageInfo.hpp — payload struct for the Image-Info dialog.
//
// s210 m1. Tiny shared header: defined here so both Canvas.hpp (which
// emits signal_request_image_info with this payload) and
// ImageInfoDialog.hpp (which renders it) can name the type without
// either header having to include the other. Mirrors the lightweight
// forward-decl discipline at the top of Canvas.hpp — small structs and
// names live in tiny headers so the heavy Canvas/Dialog headers stay
// independent.
//
// Pre-baked strings, not a SceneNode pointer. The dialog is a pure
// presenter; Canvas does the file-system reads (read_image_meta,
// last_write_time, format_file_size, the canvas-units printf) and
// hands over a fully-prepared payload. Same separation StyleEditorDialog
// uses — caller owns the data, dialog owns the widgets.
//

#include <string>

namespace Curvz {

struct ImageInfo {
  std::string filename;        // last path component
  std::string full_path;       // absolute path
  std::string pixels;          // "1920 × 1080" or "unknown"
  std::string format;          // "PNG", "JPEG", … — empty if unknown
  std::string depth;           // "8-bit RGBA", "16-bit RGB", … — empty if unknown
  std::string file_size;       // human-readable, e.g. "1.4 MB"
  std::string modified;        // "2026-05-13 10:42" or "unknown"
  std::string placed_size;     // "12.0 × 8.0" — doc units, no suffix
  std::string linkage;         // "External file (not embedded)"
};

} // namespace Curvz
