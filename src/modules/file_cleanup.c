/**
 * file_cleanup.c
 * --------------
 * Periodic cleanup of old frames and video files.
 *
 * Scans configured output directories (frame_save.dir, smart_record.dir,
 * json_save.dir) and deletes files older than max_age_sec.
 */

#include "file_cleanup.h"
#include "config.h"

#include <glib.h>
#include <gst/gst.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <time.h>

GST_DEBUG_CATEGORY_EXTERN(deepstream_debug_category);
#define GST_CAT_DEFAULT deepstream_debug_category

// =============================================================================
// Internal state
// =============================================================================

static guint cleanup_timer_id = 0;

// =============================================================================
// File deletion helper
// =============================================================================

/**
 * Recursively scan a directory and delete files older than max_age_sec.
 * Returns the number of files deleted.
 */
static guint
cleanup_directory(const gchar *dir_path, guint max_age_sec, time_t now)
{
  if (!dir_path || !g_file_test(dir_path, G_FILE_TEST_IS_DIR)) {
    return 0;
  }

  guint deleted_count = 0;
  GError *error = NULL;
  GDir *dir = g_dir_open(dir_path, 0, &error);

  if (!dir) {
    GST_WARNING("Failed to open directory for cleanup: %s (%s)",
                dir_path, error ? error->message : "unknown error");
    g_clear_error(&error);
    return 0;
  }

  const gchar *entry_name;
  while ((entry_name = g_dir_read_name(dir)) != NULL) {
    gchar *full_path = g_build_filename(dir_path, entry_name, NULL);
    struct stat st;

    if (g_stat(full_path, &st) == 0) {
      if (S_ISDIR(st.st_mode)) {
        /* Recurse into subdirectories (e.g., source_00, source_01) */
        deleted_count += cleanup_directory(full_path, max_age_sec, now);

        /* Try to remove empty directories */
        g_rmdir(full_path);  /* Silently fails if not empty */
      } else if (S_ISREG(st.st_mode)) {
        /* Check file age */
        time_t file_age = now - st.st_mtime;
        if (file_age > (time_t)max_age_sec) {
          if (g_unlink(full_path) == 0) {
            deleted_count++;
            GST_DEBUG("Deleted old file: %s (age: %ld sec)", full_path, (long)file_age);
          } else {
            GST_WARNING("Failed to delete file: %s", full_path);
          }
        }
      }
    }

    g_free(full_path);
  }

  g_dir_close(dir);
  return deleted_count;
}

// =============================================================================
// Public API
// =============================================================================

guint
file_cleanup_run_once(const gchar **dirs, guint max_age_sec)
{
  if (!dirs || max_age_sec == 0) {
    return 0;
  }

  time_t now = time(NULL);
  guint total_deleted = 0;

  for (guint i = 0; dirs[i] != NULL; i++) {
    guint deleted = cleanup_directory(dirs[i], max_age_sec, now);
    if (deleted > 0) {
      GST_INFO("Cleaned up %u files from: %s", deleted, dirs[i]);
    }
    total_deleted += deleted;
  }

  return total_deleted;
}

// =============================================================================
// Timer callback
// =============================================================================

static gboolean
cleanup_timer_callback(gpointer user_data G_GNUC_UNUSED)
{
  if (!app_config.file_cleanup.enabled) {
    GST_DEBUG("File cleanup disabled, skipping");
    return TRUE;  /* Keep timer running in case config changes */
  }

  guint max_age = app_config.file_cleanup.max_age_sec;

  /* Build list of directories to clean */
  GPtrArray *dirs_array = g_ptr_array_new();

  if (app_config.frame_save.enabled && app_config.frame_save.dir) {
    g_ptr_array_add(dirs_array, (gpointer)app_config.frame_save.dir);
  }

  if (app_config.smart_record.enabled && app_config.smart_record.dir) {
    g_ptr_array_add(dirs_array, (gpointer)app_config.smart_record.dir);
  }

  if (app_config.json_save.enabled && app_config.json_save.dir) {
    g_ptr_array_add(dirs_array, (gpointer)app_config.json_save.dir);
  }

  g_ptr_array_add(dirs_array, NULL);  /* Null-terminate */

  const gchar **dirs = (const gchar **)dirs_array->pdata;
  guint deleted = file_cleanup_run_once(dirs, max_age);

  if (deleted > 0) {
    GST_INFO("File cleanup complete: deleted %u files (max_age=%u sec)", deleted, max_age);
  } else {
    GST_DEBUG("File cleanup: no old files found (max_age=%u sec)", max_age);
  }

  g_ptr_array_free(dirs_array, TRUE);

  return TRUE;  /* Keep timer running */
}

// =============================================================================
// Start / Stop
// =============================================================================

gboolean
file_cleanup_start(void)
{
  if (!app_config.file_cleanup.enabled) {
    GST_INFO("File cleanup is disabled");
    return TRUE;
  }

  if (cleanup_timer_id != 0) {
    GST_WARNING("File cleanup timer already running");
    return TRUE;
  }

  guint interval = app_config.file_cleanup.interval_sec;
  if (interval == 0) {
    GST_WARNING("File cleanup interval is 0, disabling cleanup");
    return TRUE;
  }

  GST_INFO("Starting file cleanup timer (interval=%u sec, max_age=%u sec)",
           interval, app_config.file_cleanup.max_age_sec);

  /* Run cleanup immediately on startup, then schedule periodic */
  cleanup_timer_callback(NULL);

  cleanup_timer_id = g_timeout_add_seconds(interval, cleanup_timer_callback, NULL);

  return (cleanup_timer_id != 0);
}

void
file_cleanup_stop(void)
{
  if (cleanup_timer_id != 0) {
    g_source_remove(cleanup_timer_id);
    cleanup_timer_id = 0;
    GST_INFO("File cleanup timer stopped");
  }
}
