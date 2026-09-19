#ifndef CMARK_EXPORT_H
#define CMARK_EXPORT_H

/* Replaces cmake GenerateExportHeader output.
 * No shared-library visibility annotations needed for static linking. */
#ifndef CMARK_EXPORT
#  define CMARK_EXPORT
#endif

#ifndef CMARK_DEPRECATED
#  define CMARK_DEPRECATED
#endif

#endif /* CMARK_EXPORT_H */
