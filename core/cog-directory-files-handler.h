/*
 * cog-directory-files-handler.h
 * Copyright (C) 2017-2018 Adrian Perez <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#ifndef COG_DIRECTORY_FILES_HANDLER_H
#define COG_DIRECTORY_FILES_HANDLER_H

#if !(defined(COG_INSIDE_COG__) && COG_INSIDE_COG__)
# error "Do not include this header directly, use <cog.h> instead"
#endif

#include "cog-request-handler.h"

G_BEGIN_DECLS

typedef struct _GFile  GFile;
typedef struct _GError GError;

#define COG_TYPE_DIRECTORY_FILES_HANDLER  (cog_directory_files_handler_get_type ())

G_DECLARE_FINAL_TYPE (CogDirectoryFilesHandler,
                      cog_directory_files_handler,
                      COG, DIRECTORY_FILES_HANDLER,
                      GObject)

struct _CogDirectoryFilesHandlerClass {
    GObjectClass parent_class;
};


enum {
    COG_DIRECTORY_FILES_HANDLER_ERROR_PATH_NOT_NATIVE,
    COG_DIRECTORY_FILES_HANDLER_ERROR_PATH_NOT_DIRECTORY,
    COG_DIRECTORY_FILES_HANDLER_ERROR_CANNOT_RESOLVE,
};


GQuark             cog_directory_files_handler_error_quark      (void);
CogRequestHandler* cog_directory_files_handler_new              (GFile   *base_path);
gboolean           cog_directory_files_handler_is_suitable_path (GFile   *file,
                                                                 GError **error);
gboolean           cog_directory_files_handler_get_use_host     (CogDirectoryFilesHandler *self);
void               cog_directory_files_handler_set_use_host     (CogDirectoryFilesHandler *self,
                                                                 gboolean                  use_host);

unsigned           cog_directory_files_handler_get_strip_components
                                                                (CogDirectoryFilesHandler *self);
void               cog_directory_files_handler_set_strip_components
                                                                (CogDirectoryFilesHandler *self,
                                                                 unsigned                  count);

G_END_DECLS

#endif /* !COG_DIRECTORY_FILES_HANDLER_H */
