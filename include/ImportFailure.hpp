#pragma once
//
// ImportFailure.hpp — payload struct for the import-failure dialog.
//
// s360. Tiny shared header, same discipline as ImageInfo.hpp: defined
// here so both Canvas.hpp (which emits signal_request_import_failure
// with this payload) and ImportFailureDialog.hpp (which renders it) can
// name the type without either heavy header including the other.
//
// Pre-baked strings, not an error code or a doc pointer. The dialog is a
// pure presenter; Canvas (in import_svg_to_canvas) decides why the
// import failed and hands over a fully-prepared payload. Same caller-
// owns-the-data separation ImageInfoDialog uses.
//

#include <string>

namespace Curvz {

struct ImportFailure {
  std::string filename;   // last path component, e.g. "folio-brand.svg"
  std::string full_path;  // absolute path the user tried to import
  std::string reason;     // one-line human summary of what went wrong
  std::string detail;     // optional elaboration (parser text / hint) — empty hides
};

} // namespace Curvz
