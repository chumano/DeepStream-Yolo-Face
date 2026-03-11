#ifndef __FILE_CLEANUP_H__
#define __FILE_CLEANUP_H__

#include <glib.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the periodic file cleanup timer.
 *
 * Scans configured output directories (frame_save.dir, smart_record.dir,
 * json_save.dir) and deletes files older than max_age_sec.  The timer fires
 * every interval_sec seconds as specified in app_config.file_cleanup.
 *
 * If file_cleanup.enabled is FALSE, this function does nothing and
 * returns immediately.
 *
 * @return TRUE if the timer was started successfully (or cleanup is disabled),
 *         FALSE on error.
 */
gboolean file_cleanup_start(void);

/**
 * @brief Stop the periodic file cleanup timer.
 *
 * Call this before application shutdown to cleanly remove the GSource timer.
 * Safe to call even if file_cleanup_start() was never called or if cleanup
 * is disabled.
 */
void file_cleanup_stop(void);

/**
 * @brief Run cleanup immediately (one-shot).
 *
 * Scans configured output directories and deletes files older than max_age_sec.
 * Useful for manual cleanup or testing.
 *
 * @param dirs        NULL-terminated array of directory paths to scan.
 * @param max_age_sec Maximum file age in seconds; files older than this are deleted.
 * @return The number of files deleted.
 */
guint file_cleanup_run_once(const gchar **dirs, guint max_age_sec);

#ifdef __cplusplus
}
#endif

#endif /* __FILE_CLEANUP_H__ */
