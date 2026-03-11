#include "pipeline_dump.h"

#include <gst/gst.h>
#include <glib.h>

GST_DEBUG_CATEGORY_EXTERN(deepstream_debug_category);
#define GST_CAT_DEFAULT deepstream_debug_category

// =============================================================================
// Internal helpers
// =============================================================================

/**
 * Escape a plain string so it is safe to embed inside a JSON double-quoted
 * value.  Only the characters mandated by RFC 8259 are escaped.
 * Returns a newly-allocated string; caller must g_free().
 */
static gchar *
json_escape_string(const gchar *src)
{
  if (!src) return g_strdup("null");  /* bare null — no quotes around it */

  GString *out = g_string_new(NULL);
  for (const gchar *p = src; *p; p++) {
    guchar c = (guchar)*p;
    switch (c) {
      case '"':  g_string_append(out, "\\\""); break;
      case '\\': g_string_append(out, "\\\\"); break;
      case '\b': g_string_append(out, "\\b");  break;
      case '\f': g_string_append(out, "\\f");  break;
      case '\n': g_string_append(out, "\\n");  break;
      case '\r': g_string_append(out, "\\r");  break;
      case '\t': g_string_append(out, "\\t");  break;
      default:
        if (c < 0x20) {
          g_string_append_printf(out, "\\u%04x", c);
        } else {
          g_string_append_c(out, (gchar)c);
        }
        break;
    }
  }
  return g_string_free(out, FALSE);
}

/**
 * Serialize a single GObject property value to a JSON value fragment
 * (string, number, boolean, or "null").
 * Returns a newly-allocated string; caller must g_free().
 */
static gchar *
property_value_to_json(GObject *obj, GParamSpec *pspec)
{
  /* Skip write-only properties */
  if (!(pspec->flags & G_PARAM_READABLE))
    return g_strdup("\"<not-readable>\"");

  GValue val = G_VALUE_INIT;
  g_value_init(&val, pspec->value_type);

  g_object_get_property(obj, pspec->name, &val);

  gchar *result = NULL;
  GType t = G_VALUE_TYPE(&val);

  if (t == G_TYPE_BOOLEAN) {
    result = g_strdup(g_value_get_boolean(&val) ? "true" : "false");
  } else if (t == G_TYPE_INT) {
    result = g_strdup_printf("%d", g_value_get_int(&val));
  } else if (t == G_TYPE_UINT) {
    result = g_strdup_printf("%u", g_value_get_uint(&val));
  } else if (t == G_TYPE_INT64) {
    result = g_strdup_printf("%" G_GINT64_FORMAT, g_value_get_int64(&val));
  } else if (t == G_TYPE_UINT64) {
    result = g_strdup_printf("%" G_GUINT64_FORMAT, g_value_get_uint64(&val));
  } else if (t == G_TYPE_LONG) {
    result = g_strdup_printf("%ld", g_value_get_long(&val));
  } else if (t == G_TYPE_ULONG) {
    result = g_strdup_printf("%lu", g_value_get_ulong(&val));
  } else if (t == G_TYPE_FLOAT) {
    result = g_strdup_printf("%g", (gdouble)g_value_get_float(&val));
  } else if (t == G_TYPE_DOUBLE) {
    result = g_strdup_printf("%g", g_value_get_double(&val));
  } else if (t == G_TYPE_STRING) {
    const gchar *s = g_value_get_string(&val);
    if (!s) {
      result = g_strdup("null");
    } else {
      gchar *escaped = json_escape_string(s);
      result = g_strdup_printf("\"%s\"", escaped);
      g_free(escaped);
    }
  } else if (G_TYPE_IS_ENUM(t)) {
    GEnumClass *ec = (GEnumClass *)g_type_class_peek(t);
    GEnumValue *ev = ec ? g_enum_get_value(ec, g_value_get_enum(&val)) : NULL;
    gchar *escaped = json_escape_string(ev ? ev->value_nick : "unknown");
    result = g_strdup_printf("\"%s\"", escaped);
    g_free(escaped);
  } else if (G_TYPE_IS_FLAGS(t)) {
    result = g_strdup_printf("%u", g_value_get_flags(&val));
  } else {
    /* Fallback: use GValue's string representation */
    gchar *contents = g_strdup_value_contents(&val);
    gchar *escaped  = json_escape_string(contents);
    result = g_strdup_printf("\"%s\"", escaped);
    g_free(escaped);
    g_free(contents);
  }

  g_value_unset(&val);
  return result;
}

// =============================================================================
// Public API
// =============================================================================

void
dump_pipeline_elements_to_json(GstElement *pipeline, const gchar *output_dir)
{
  const gchar *dir  = output_dir ? output_dir : "/tmp";
  gchar       *path = g_strdup_printf("%s/pipeline_elements.json", dir);

  GString *js = g_string_new("{\n");

  /* Timestamp */
  GDateTime *now = g_date_time_new_now_local();
  gchar     *ts  = g_date_time_format(now, "%Y-%m-%dT%H:%M:%S%z");
  g_date_time_unref(now);
  gchar *ts_escaped = json_escape_string(ts);
  g_string_append_printf(js, "  \"pipeline_dump_at\": \"%s\",\n", ts_escaped);
  g_free(ts_escaped);
  g_free(ts);

  g_string_append(js, "  \"elements\": [\n");

  /* Iterate all elements in the bin recursively */
  GstIterator      *iter       = gst_bin_iterate_recurse(GST_BIN(pipeline));
  GValue            item       = G_VALUE_INIT;
  GstIteratorResult res;
  gboolean          first_elem = TRUE;

  do {
    res = gst_iterator_next(iter, &item);
    if (res == GST_ITERATOR_RESYNC) {
      gst_iterator_resync(iter);
      continue;
    }
    if (res != GST_ITERATOR_OK) break;

    GstElement *elem = GST_ELEMENT(g_value_get_object(&item));
    if (!elem) { g_value_reset(&item); continue; }

    gchar       *elem_name = gst_element_get_name(elem);
    const gchar *type_name = G_OBJECT_TYPE_NAME(elem);

    if (!first_elem) g_string_append(js, ",\n");
    first_elem = FALSE;

    gchar *en_escaped = json_escape_string(elem_name);
    gchar *tn_escaped = json_escape_string(type_name);
    g_string_append_printf(js,
        "    {\n"
        "      \"name\": \"%s\",\n"
        "      \"type\": \"%s\",\n"
        "      \"properties\": {\n",
        en_escaped, tn_escaped);
    g_free(en_escaped);
    g_free(tn_escaped);
    g_free(elem_name);

    /* Enumerate all GObject properties */
    GObjectClass *klass   = G_OBJECT_GET_CLASS(elem);
    guint         n_props = 0;
    GParamSpec  **props   = g_object_class_list_properties(klass, &n_props);

    for (guint i = 0; i < n_props; i++) {
      GParamSpec *pspec = props[i];

      gchar *val_json   = property_value_to_json(G_OBJECT(elem), pspec);
      gchar *pn_escaped = json_escape_string(pspec->name);

      g_string_append_printf(js, "        \"%s\": %s", pn_escaped, val_json);
      if (i + 1 < n_props) g_string_append_c(js, ',');
      g_string_append_c(js, '\n');

      g_free(pn_escaped);
      g_free(val_json);
    }
    g_free(props);

    g_string_append(js, "      }\n    }");

    g_value_reset(&item);
  } while (res == GST_ITERATOR_OK);

  g_value_unset(&item);
  gst_iterator_free(iter);

  g_string_append(js, "\n  ]\n}\n");

  /* Write file */
  GError *err = NULL;
  if (!g_file_set_contents(path, js->str, (gssize)js->len, &err)) {
    GST_WARNING("[pipeline-dump] Failed to write %s: %s", path,
                err ? err->message : "unknown");
    if (err) g_error_free(err);
  } else {
    GST_INFO("[pipeline-dump] Saved pipeline element dump → %s (%zu bytes)",
             path, js->len);
    g_print("[pipeline-dump] Saved pipeline element dump → %s\n", path);
  }

  g_string_free(js, TRUE);
  g_free(path);
}
