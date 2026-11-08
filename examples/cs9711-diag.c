/*
 * fprint-cs9711-diag: bounded, machine-readable diagnostics for the CS9711 /
 * Acl16 reader using only the public libfprint API.
 *
 * Every event is one JSON object per line on stdout, with "t" in seconds
 * since start (monotonic). Hints for the person at the reader go to stderr.
 * No image is ever written. A template is written only with
 * --save-template, created with mode 0600 and never overwritten.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <glib-unix.h>
#include <glib/gstdio.h>
#include <libfprint/fprint.h>

/* Exit codes */
enum {
  EXIT_OK          = 0,  /* match, enrolled, captured, cycles done */
  EXIT_NO_MATCH    = 1,
  EXIT_ERROR       = 2,
  EXIT_TIMEOUT     = 3,
  EXIT_INTERRUPTED = 4,
  EXIT_RETRY       = 5,  /* scan quality problem reported by the driver */
  EXIT_HANG        = 6,  /* operation ignored cancellation */
  EXIT_USAGE       = 64,
  EXIT_NO_DEVICE   = 69,
};

#define CANCEL_GRACE_SECONDS 10

typedef struct
{
  gint64        t0;
  gboolean      quiet;
  guint         timeout_s;

  FpDevice     *dev;
  GCancellable *cancellable;
  const char   *op;
  guint         attempt;
  gint64        op_t0;
  guint         watchdog_id;
  guint         grace_id;
  gboolean      timed_out;
  gboolean      interrupted;
  guint         signals;

  gboolean      was_waiting;
  gboolean      was_present;
  gulong        status_handler;
} Diag;

static Diag diag;

/************************** Output ********************************************/

static void
json_escape (GString *out, const char *s)
{
  g_string_append_c (out, '"');
  for (; s && *s; s++)
    {
      unsigned char c = *s;

      if (c == '"' || c == '\\')
        g_string_append_printf (out, "\\%c", c);
      else if (c == '\n')
        g_string_append (out, "\\n");
      else if (c < 0x20)
        g_string_append_printf (out, "\\u%04x", c);
      else
        g_string_append_c (out, c);
    }
  g_string_append_c (out, '"');
}

static double
now_s (void)
{
  return (g_get_monotonic_time () - diag.t0) / 1e6;
}

/* emit ("event", "key", 's', "value", "key", 'i', 3, "key", 'b', TRUE,
 *       "key", 'd', 1.5, NULL) */
static void
emit (const char *event, ...)
{
  g_autoptr(GString) line = g_string_new (NULL);
  const char *key;
  va_list ap;

  g_string_append_printf (line, "{\"t\":%.3f,\"event\":", now_s ());
  json_escape (line, event);
  if (diag.op)
    {
      g_string_append (line, ",\"op\":");
      json_escape (line, diag.op);
      if (diag.attempt)
        g_string_append_printf (line, ",\"attempt\":%u", diag.attempt);
    }

  va_start (ap, event);
  while ((key = va_arg (ap, const char *)))
    {
      int type = va_arg (ap, int);

      g_string_append_c (line, ',');
      json_escape (line, key);
      g_string_append_c (line, ':');
      switch (type)
        {
        case 's':
          {
            const char *v = va_arg (ap, const char *);
            if (v)
              json_escape (line, v);
            else
              g_string_append (line, "null");
            break;
          }

        case 'i':
          g_string_append_printf (line, "%d", va_arg (ap, int));
          break;

        case 'b':
          g_string_append (line, va_arg (ap, int) ? "true" : "false");
          break;

        case 'd':
          g_string_append_printf (line, "%.3f", va_arg (ap, double));
          break;

        default:
          g_assert_not_reached ();
        }
    }
  va_end (ap);

  g_string_append_c (line, '}');
  fprintf (stdout, "%s\n", line->str);
  fflush (stdout);
}

G_GNUC_PRINTF (1, 2)
static void
hint (const char *format, ...)
{
  g_autofree char *msg = NULL;
  va_list ap;

  if (diag.quiet)
    return;

  va_start (ap, format);
  msg = g_strdup_vprintf (format, ap);
  va_end (ap);
  fprintf (stderr, "[%8.3fs] %s\n", now_s (), msg);
}

/************************** Watchdog and signals ******************************/

static gboolean
grace_cb (gpointer user_data)
{
  diag.grace_id = 0;
  emit ("cancel-unresponsive", "seconds", 'i', CANCEL_GRACE_SECONDS, NULL);
  hint ("The operation did not stop %d s after cancellation; exiting.", CANCEL_GRACE_SECONDS);
  _exit (EXIT_HANG);
  return G_SOURCE_REMOVE;
}

static void
cancel_current (const char *reason)
{
  if (!diag.cancellable || g_cancellable_is_cancelled (diag.cancellable))
    return;

  emit ("cancel", "reason", 's', reason, NULL);
  g_cancellable_cancel (diag.cancellable);
  if (!diag.grace_id)
    diag.grace_id = g_timeout_add (CANCEL_GRACE_SECONDS * 1000, grace_cb, NULL);
}

static gboolean
watchdog_cb (gpointer user_data)
{
  diag.watchdog_id = 0;
  diag.timed_out = TRUE;
  hint ("No result within %u s, cancelling.", diag.timeout_s);
  cancel_current ("timeout");
  return G_SOURCE_REMOVE;
}

static void
watchdog_rearm (void)
{
  g_clear_handle_id (&diag.watchdog_id, g_source_remove);
  if (diag.timeout_s)
    diag.watchdog_id = g_timeout_add (diag.timeout_s * 1000, watchdog_cb, NULL);
}

static gboolean
signal_cb (gpointer user_data)
{
  diag.signals++;
  if (diag.signals > 1)
    _exit (EXIT_INTERRUPTED);

  diag.interrupted = TRUE;
  hint ("Interrupted, cancelling (signal again to exit at once).");
  cancel_current ("signal");
  return G_SOURCE_CONTINUE;
}

/************************** Device state **************************************/

static void
on_finger_status (FpDevice *dev, GParamSpec *pspec, gpointer user_data)
{
  FpFingerStatusFlags status = fp_device_get_finger_status (dev);
  gboolean needed = (status & FP_FINGER_STATUS_NEEDED) != 0;
  gboolean present = (status & FP_FINGER_STATUS_PRESENT) != 0;
  gboolean waiting = needed && !present;

  emit ("finger-status", "needed", 'b', needed, "present", 'b', present, NULL);

  if (waiting && !diag.was_waiting)
    {
      g_autofree char *attempt = diag.attempt ? g_strdup_printf (" #%u", diag.attempt) : NULL;

      /* For 2541:2549 this is reported only after the scan command has been
       * sent, i.e. the sensor is armed now. */
      emit ("waiting-for-contact", NULL);
      hint ("%s%s: sensor armed - place your finger now.",
            diag.op ? diag.op : "reader", attempt ? attempt : "");
    }
  if (present && !diag.was_present)
    {
      emit ("contact", NULL);
      hint ("Frame received - lift your finger.");
    }

  diag.was_waiting = waiting;
  diag.was_present = present;
}

static void
op_begin (const char *op, guint attempt)
{
  diag.op = op;
  diag.attempt = attempt;
  diag.op_t0 = g_get_monotonic_time ();
  diag.timed_out = FALSE;
  g_clear_object (&diag.cancellable);
  diag.cancellable = g_cancellable_new ();
  emit ("op-start", "timeout_s", 'i', (int) diag.timeout_s, NULL);
  watchdog_rearm ();
}

/* Map the result of one operation to an outcome and an exit code. */
static int
op_end (const char *success_outcome, GError *error)
{
  const char *outcome = success_outcome;
  int code = EXIT_OK;
  double duration = (g_get_monotonic_time () - diag.op_t0) / 1e6;

  g_clear_handle_id (&diag.watchdog_id, g_source_remove);
  g_clear_handle_id (&diag.grace_id, g_source_remove);

  if (error)
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED) && diag.timed_out)
        outcome = "timeout", code = EXIT_TIMEOUT;
      else if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED) && diag.interrupted)
        outcome = "interrupted", code = EXIT_INTERRUPTED;
      else if (error->domain == FP_DEVICE_RETRY)
        outcome = "retry", code = EXIT_RETRY;
      else
        outcome = "error", code = EXIT_ERROR;

      emit ("result", "outcome", 's', outcome, "duration_s", 'd', duration,
            "error_domain", 's', g_quark_to_string (error->domain),
            "error_code", 'i', error->code, "message", 's', error->message, NULL);
    }
  else
    {
      if (g_strcmp0 (outcome, "no-match") == 0)
        code = EXIT_NO_MATCH;
      emit ("result", "outcome", 's', outcome, "duration_s", 'd', duration, NULL);
    }

  hint ("%s: %s%s%s", diag.op, outcome, error ? " - " : "", error ? error->message : "");
  diag.op = NULL;
  diag.attempt = 0;

  return code;
}

/************************** Templates *****************************************/

static gboolean
save_template (FpPrint *print, const char *path, GError **error)
{
  g_autofree guchar *data = NULL;
  gsize len = 0, done = 0;
  int fd, sync_errno;

  if (!fp_print_serialize (print, &data, &len, error))
    return FALSE;

  /* Never overwrite, never follow a symlink, owner-only. */
  fd = open (path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0)
    {
      int errsv = errno;
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errsv),
                   "Cannot create %s: %s", path, g_strerror (errsv));
      return FALSE;
    }

  while (done < len)
    {
      ssize_t n = write (fd, data + done, len - done);

      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0)
        {
          int errsv = errno;
          close (fd);
          g_unlink (path);
          g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errsv),
                       "Cannot write %s: %s", path, g_strerror (errsv));
          return FALSE;
        }
      done += n;
    }

  /* Always close the descriptor; report the first failure. */
  sync_errno = fsync (fd) != 0 ? errno : 0;
  if (close (fd) != 0 && sync_errno == 0)
    sync_errno = errno;
  if (sync_errno != 0)
    {
      g_unlink (path);
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (sync_errno),
                   "Cannot finish %s: %s", path, g_strerror (sync_errno));
      return FALSE;
    }

  emit ("template-saved", "path", 's', path, "bytes", 'i', (int) len, NULL);
  hint ("Template written to %s (mode 0600). It is biometric data: delete it with 'rm %s' when done.",
        path, path);
  return TRUE;
}

static FpPrint *
load_template (FpDevice *dev, const char *path, GError **error)
{
  g_autofree gchar *data = NULL;
  g_autoptr(FpPrint) print = NULL;
  GStatBuf st;
  gsize len = 0;

  if (g_lstat (path, &st) != 0 || !S_ISREG (st.st_mode))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_REGULAR_FILE,
                   "%s is not a regular file", path);
      return NULL;
    }
  if (st.st_mode & 0077)
    hint ("Warning: %s is readable by other users; chmod 600 it.", path);

  if (!g_file_get_contents (path, &data, &len, error))
    return NULL;

  print = fp_print_deserialize ((guchar *) data, len, error);
  if (!print)
    return NULL;

  if (!fp_print_compatible (print, dev))
    {
      g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_DATA_INVALID,
                   "Template %s was not enrolled on this reader/driver", path);
      return NULL;
    }

  emit ("template-loaded", "path", 's', path, "bytes", 'i', (int) len, NULL);
  return g_steal_pointer (&print);
}

/************************** Operations ****************************************/

static void
on_enroll_progress (FpDevice *dev, gint completed, FpPrint *print,
                    gpointer user_data, GError *error)
{
  gint total = fp_device_get_nr_enroll_stages (dev);

  if (error)
    {
      emit ("enroll-progress", "completed", 'i', completed, "total", 'i', total,
            "retry", 's', error->message, NULL);
      hint ("Stage not accepted (%s), try again.", error->message);
    }
  else
    {
      emit ("enroll-progress", "completed", 'i', completed, "total", 'i', total, NULL);
      hint ("Stage %d/%d accepted.", completed, total);
    }

  /* The timeout bounds each stage, not the whole enrollment. */
  watchdog_rearm ();
}

static int
do_enroll (FpPrint **out_print, const char *save_path)
{
  g_autoptr(GError) error = NULL;
  /* Floating: fp_device_enroll() takes it over and returns it enrolled. */
  FpPrint *template = fp_print_new (diag.dev);
  g_autoptr(FpPrint) print = NULL;
  int code;

  fp_print_set_username (template, g_get_user_name ());
  fp_print_set_description (template, "fprint-cs9711-diag");

  op_begin ("enroll", 0);
  print = fp_device_enroll_sync (diag.dev, template, diag.cancellable,
                                 on_enroll_progress, NULL, &error);
  code = op_end ("enrolled", error);
  if (code != EXIT_OK)
    return code;

  if (save_path)
    {
      g_autoptr(GError) save_error = NULL;

      if (!save_template (print, save_path, &save_error))
        {
          emit ("template-save-failed", "message", 's', save_error->message, NULL);
          hint ("Saving the template failed: %s", save_error->message);
          return EXIT_ERROR;
        }
    }

  if (out_print)
    *out_print = g_steal_pointer (&print);
  return EXIT_OK;
}

static void
on_match (FpDevice *dev, FpPrint *match, FpPrint *print, gpointer user_data, GError *error)
{
  if (error)
    emit ("match-report", "match", 's', NULL, "retry", 's', error->message, NULL);
  else
    emit ("match-report", "match", 'b', match != NULL, NULL);
}

static int
do_verify (FpPrint *template, guint attempt)
{
  g_autoptr(GError) error = NULL;
  gboolean match = FALSE;

  op_begin ("verify", attempt);
  fp_device_verify_sync (diag.dev, template, diag.cancellable, on_match, NULL,
                         &match, NULL, &error);
  return op_end (match ? "match" : "no-match", error);
}

static int
do_capture (guint attempt)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(FpImage) image = NULL;
  int code;

  op_begin ("capture", attempt);
  image = fp_device_capture_sync (diag.dev, TRUE, diag.cancellable, &error);
  if (image)
    {
      /* Summary statistics only; the pixels are never stored. */
      gsize len = 0;
      const guchar *px = fp_image_get_data (image, &len);
      double sum = 0, sq = 0;
      int lo = 255, hi = 0;

      for (gsize i = 0; i < len; i++)
        {
          sum += px[i];
          sq += (double) px[i] * px[i];
          lo = MIN (lo, px[i]);
          hi = MAX (hi, px[i]);
        }
      if (len)
        emit ("image", "width", 'i', (int) fp_image_get_width (image),
              "height", 'i', (int) fp_image_get_height (image),
              "mean", 'd', sum / len, "stddev", 'd', sqrt (MAX (0.0, sq / len - (sum / len) * (sum / len))),
              "min", 'i', lo, "max", 'i', hi, NULL);
    }
  code = op_end ("captured", error);
  return code;
}

static gboolean
open_device (GError **error)
{
  gboolean ok;

  diag.op = "open";
  diag.op_t0 = g_get_monotonic_time ();
  ok = fp_device_open_sync (diag.dev, NULL, error);
  if (ok)
    emit ("opened", "duration_s", 'd', (g_get_monotonic_time () - diag.op_t0) / 1e6, NULL);
  else
    emit ("open-failed", "message", 's', (*error)->message, NULL);
  diag.op = NULL;
  return ok;
}

/* Returns FALSE if closing failed. */
static gboolean
close_device (void)
{
  g_autoptr(GError) error = NULL;
  gboolean ok;

  if (!fp_device_is_open (diag.dev))
    return TRUE;

  diag.op = "close";
  diag.op_t0 = g_get_monotonic_time ();
  ok = fp_device_close_sync (diag.dev, NULL, &error);
  if (ok)
    emit ("closed", "duration_s", 'd', (g_get_monotonic_time () - diag.op_t0) / 1e6, NULL);
  else
    emit ("close-failed", "message", 's', error->message, NULL);
  diag.op = NULL;
  return ok;
}

/* The first failure decides the exit code; a close failure turns a
 * successful or definite (no-match) run into an error. */
static int
finish_code (int code, gboolean closed)
{
  if (diag.interrupted && code == EXIT_OK)
    code = EXIT_INTERRUPTED;
  if (!closed && (code == EXIT_OK || code == EXIT_NO_MATCH))
    code = EXIT_ERROR;
  return code;
}

static void
emit_device (FpDevice *dev, guint index, const char *event)
{
  emit (event,
        "index", 'i', (int) index,
        "driver", 's', fp_device_get_driver (dev),
        "device_id", 's', fp_device_get_device_id (dev),
        "name", 's', fp_device_get_name (dev),
        "scan_type", 's', fp_device_get_scan_type (dev) == FP_SCAN_TYPE_PRESS ? "press" : "swipe",
        "enroll_stages", 'i', fp_device_get_nr_enroll_stages (dev),
        "capture", 'b', fp_device_has_feature (dev, FP_DEVICE_FEATURE_CAPTURE),
        "storage", 'b', fp_device_has_feature (dev, FP_DEVICE_FEATURE_STORAGE),
        NULL);
}

/************************** main **********************************************/

static const char usage_text[] =
  "Commands:\n"
  "  list            List readers seen by this libfprint and exit\n"
  "  capture         Wait for --count presses and report frame statistics\n"
  "  enroll          Enroll one finger (template kept in memory unless\n"
  "                  --save-template is given)\n"
  "  verify          Verify --count times against --template FILE\n"
  "  enroll-verify   Enroll, then verify --count times in the same process\n"
  "  cycle           Open and close the reader --count times\n"
  "\n"
  "Output: one JSON object per line on stdout; hints on stderr.\n"
  "Exit status: 0 success (or every verify gave a definite match/no-match\n"
  "in enroll-verify), 1 no match (verify), 2 error, 3 timeout,\n"
  "4 interrupted, 5 retry, 6 cancellation ignored, 64 usage, 69 no reader.";

int
main (int argc, char **argv)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(FpContext) ctx = NULL;
  g_autoptr(FpPrint) template = NULL;
  g_autofree char *save_path = NULL;
  g_autofree char *template_path = NULL;
  gint timeout_s = 30;
  gint device_index = -1;
  gint count = 1;
  gboolean any_driver = FALSE;
  gboolean debug = FALSE;
  GPtrArray *devices;
  guint selected = 0;
  const char *command;
  int code = EXIT_OK;
  guint sigint_id, sigterm_id;

  GOptionEntry entries[] = {
    { "timeout", 't', 0, G_OPTION_ARG_INT, &timeout_s,
      "Seconds to wait for each result or enroll stage (default 30, 0 = unbounded, max 86400)", "S" },
    { "device", 'd', 0, G_OPTION_ARG_INT, &device_index,
      "Reader index from 'list' (default: first cs9711 reader)", "N" },
    { "count", 'n', 0, G_OPTION_ARG_INT, &count,
      "Repetitions for capture, verify, enroll-verify and cycle (default 1)", "N" },
    { "save-template", 0, 0, G_OPTION_ARG_FILENAME, &save_path,
      "After enroll, write the template to FILE (0600, must not exist)", "FILE" },
    { "template", 0, 0, G_OPTION_ARG_FILENAME, &template_path,
      "Template for verify, written earlier by --save-template", "FILE" },
    { "any-driver", 0, 0, G_OPTION_ARG_NONE, &any_driver,
      "Allow readers of other drivers", NULL },
    { "debug", 0, 0, G_OPTION_ARG_NONE, &debug,
      "Print libfprint driver and state machine debug messages on stderr", NULL },
    { "quiet", 'q', 0, G_OPTION_ARG_NONE, &diag.quiet,
      "No hints on stderr", NULL },
    { NULL }
  };

  diag.t0 = g_get_monotonic_time ();

  context = g_option_context_new ("COMMAND");
  g_option_context_set_summary (context, usage_text);
  g_option_context_add_main_entries (context, entries, NULL);
  if (!g_option_context_parse (context, &argc, &argv, &error))
    {
      fprintf (stderr, "%s\n", error->message);
      return EXIT_USAGE;
    }
  if (argc != 2 || timeout_s < 0 || timeout_s > 86400 || count < 1)
    {
      g_autofree char *help = g_option_context_get_help (context, TRUE, NULL);
      fprintf (stderr, "%s", help);
      return EXIT_USAGE;
    }
  command = argv[1];
  if (!g_strv_contains ((const char *[]) { "list", "capture", "enroll", "verify",
                                           "enroll-verify", "cycle", NULL }, command))
    {
      fprintf (stderr, "Unknown command '%s'\n", command);
      return EXIT_USAGE;
    }
  if (g_str_equal (command, "verify") && !template_path)
    {
      fprintf (stderr, "verify needs --template FILE (see --save-template)\n");
      return EXIT_USAGE;
    }
  if (save_path && !g_str_equal (command, "enroll") && !g_str_equal (command, "enroll-verify"))
    {
      fprintf (stderr, "--save-template only applies to enroll and enroll-verify\n");
      return EXIT_USAGE;
    }
  diag.timeout_s = timeout_s;

  /* Transfer dumps would print raw fingerprint frames. */
  if (g_getenv ("FP_DEBUG_TRANSFER"))
    {
      g_unsetenv ("FP_DEBUG_TRANSFER");
      hint ("FP_DEBUG_TRANSFER ignored: it would log raw fingerprint frames.");
    }
  if (debug)
    g_setenv ("G_MESSAGES_DEBUG",
              "libfprint-cs9711 libfprint-image_device libfprint-device libfprint-SSM", TRUE);

  sigint_id = g_unix_signal_add (SIGINT, signal_cb, NULL);
  sigterm_id = g_unix_signal_add (SIGTERM, signal_cb, NULL);

  emit ("start", "command", 's', command, "pid", 'i', (int) getpid (),
        "uid", 'i', (int) getuid (), "timeout_s", 'i', timeout_s, NULL);

  ctx = fp_context_new ();
  devices = fp_context_get_devices (ctx);
  for (guint i = 0; devices && i < devices->len; i++)
    {
      FpDevice *dev = g_ptr_array_index (devices, i);

      emit_device (dev, i, "device");
      if (diag.dev)
        continue;
      if (device_index >= 0 ? (guint) device_index == i
                            : (any_driver || g_strcmp0 (fp_device_get_driver (dev), "cs9711") == 0))
        {
          diag.dev = dev;
          selected = i;
        }
    }

  if (g_str_equal (command, "list"))
    goto out;

  if (!diag.dev ||
      (!any_driver && g_strcmp0 (fp_device_get_driver (diag.dev), "cs9711") != 0))
    {
      emit ("no-device", NULL);
      hint ("No matching reader. Check 'list', USB access rights, and that no other "
            "process (e.g. fprintd) holds the reader.");
      code = EXIT_NO_DEVICE;
      goto out;
    }
  emit_device (diag.dev, selected, "selected");
  diag.status_handler = g_signal_connect (diag.dev, "notify::finger-status",
                                          G_CALLBACK (on_finger_status), NULL);

  if (g_str_equal (command, "cycle"))
    {
      for (gint i = 1; i <= count && !diag.interrupted; i++)
        {
          g_clear_error (&error);
          diag.attempt = i;
          if (!open_device (&error))
            {
              code = EXIT_ERROR;
              break;
            }
          if (!close_device ())
            {
              code = EXIT_ERROR;
              break;
            }
          diag.attempt = 0;
        }
      code = finish_code (code, TRUE);
      goto out;
    }

  if (!open_device (&error))
    {
      code = EXIT_ERROR;
      goto out;
    }

  if (g_str_equal (command, "capture"))
    {
      for (gint i = 1; i <= count && code == EXIT_OK && !diag.interrupted; i++)
        code = do_capture (count > 1 ? i : 0);
    }
  else if (g_str_equal (command, "enroll"))
    {
      code = do_enroll (NULL, save_path);
    }
  else if (g_str_equal (command, "verify"))
    {
      template = load_template (diag.dev, template_path, &error);
      if (!template)
        {
          emit ("template-load-failed", "message", 's', error->message, NULL);
          hint ("Cannot use template: %s", error->message);
          code = EXIT_ERROR;
        }
      /* 0 if every attempt matched, 1 if any did not, else the failure. */
      for (gint i = 1; template && i <= count && !diag.interrupted; i++)
        {
          int vcode = do_verify (template, count > 1 ? i : 0);

          if (vcode != EXIT_OK && vcode != EXIT_NO_MATCH)
            {
              code = vcode;
              break;
            }
          if (vcode == EXIT_NO_MATCH)
            code = EXIT_NO_MATCH;
        }
    }
  else if (g_str_equal (command, "enroll-verify"))
    {
      code = do_enroll (&template, save_path);
      for (gint i = 1; code == EXIT_OK && i <= count && !diag.interrupted; i++)
        {
          int vcode = do_verify (template, i);

          /* A definite match or no-match is a successful diagnostic run. */
          if (vcode != EXIT_OK && vcode != EXIT_NO_MATCH)
            code = vcode;
        }
    }

  code = finish_code (code, close_device ());

out:
  emit ("exit", "code", 'i', code, NULL);
  g_source_remove (sigint_id);
  g_source_remove (sigterm_id);
  g_clear_object (&diag.cancellable);
  return code;
}
