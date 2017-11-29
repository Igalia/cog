/*
 * dy-directory-files-handler.h
 * Copyright (C) 2017 Adrian Perez <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#ifndef DY_DIRECTORY_FILES_HANDLER_H
#define DY_DIRECTORY_FILES_HANDLER_H

#if !(defined(DY_INSIDE_DINGHY__) && DY_INSIDE_DINGHY__)
# error "Do not include this header directly, use <dinghy.h> instead"
#endif

#include "dy-request-handler.h"

G_BEGIN_DECLS

typedef struct _GFile  GFile;
typedef struct _GError GError;

#define DY_TYPE_DIRECTORY_FILES_HANDLER  (dy_directory_files_handler_get_type ())

G_DECLARE_FINAL_TYPE (DyDirectoryFilesHandler,
                      dy_directory_files_handler,
                      DY, DIRECTORY_FILES_HANDLER,
                      GObject)

struct _DyDirectoryFilesHandlerClass {
    GObjectClass parent_class;
};


enum {
    DY_DIRECTORY_FILES_HANDLER_ERROR_PATH_NOT_NATIVE,
    DY_DIRECTORY_FILES_HANDLER_ERROR_PATH_NOT_DIRECTORY,
    DY_DIRECTORY_FILES_HANDLER_ERROR_CANNOT_RESOLVE,
};


GQuark            dy_directory_files_handler_error_quark      (void);
DyRequestHandler* dy_directory_files_handler_new              (GFile   *file);
gboolean          dy_directory_files_handler_is_suitable_path (GFile   *file,
                                                               GError **error);

G_END_DECLS

#endif /* !DY_DIRECTORY_FILES_HANDLER_H */
