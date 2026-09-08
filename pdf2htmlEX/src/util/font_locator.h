/*
 * font_locator.h
 *
 * Locate a real font file on the system for a PDF font name,
 * used when poppler's locateFont fails (or when we deliberately
 * prefer a full system font over an embedded subset).
 *
 * The matching logic mirrors WebFontExpander (doc-proc-ms, MISSCUT-3159):
 * name cleanup -> alias expansion -> fc-match with family consistency
 * check -> directory scan -> configured last-resort font.
 */

#ifndef FONT_LOCATOR_H__
#define FONT_LOCATOR_H__

#include <string>
#include <vector>

namespace pdf2htmlEX {

struct LocatedFontFile {
    std::string path;      // font file path
    int face_index = -1;   // TTC face index; -1 = not a TTC / unknown
};

/*
 * Try to locate a real, full font file for the given PDF font name.
 * The name may carry a subset prefix ("ABCDEF+SimSun") or style suffixes.
 *
 * bold/italic are style hints from the PDF font descriptor.
 * extra_dirs (may be empty) are scanned only when fontconfig matching fails.
 * default_font (may be empty) is the last resort, used without name checking.
 *
 * Returns true and fills `out` on success.
 */
bool locate_full_font(const std::string & pdf_font_name,
                      bool bold, bool italic,
                      const std::vector<std::string> & extra_dirs,
                      const std::string & default_font,
                      LocatedFontFile & out);

/*
 * Consistency check between a requested font name and the family of a
 * matched font file: after normalization (lowercase, strip [\s-_]) one
 * must contain the other. Prevents silent wrong substitutes (e.g. DejaVu).
 */
bool font_family_matches(const std::string & requested, const std::string & matched_family);

/*
 * Check whether the font file located by poppler/fontconfig is consistent
 * with the requested font name. The family of the file (at the given TTC
 * face) is queried via fc-query and compared with font_family_matches.
 * Returns true when consistent (or when the check cannot be performed).
 */
bool font_file_consistent(const LocatedFontFile & located, const std::string & requested);

} // namespace pdf2htmlEX

#endif // FONT_LOCATOR_H__
