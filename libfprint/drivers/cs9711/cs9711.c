/*
 * Chipsailing CS9711Fingprint driver
 *
 * Modified based on driver vfs301* so keeping original notice:
 *
 * Copyright (c) 2011-2012 Andrej Krutak <dev@andree.sk>
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

#define FP_COMPONENT "cs9711"

#include "drivers_api.h"
#include "cs9711.h"

G_DEFINE_TYPE (FpDeviceCs9711, fpi_device_cs9711, FP_TYPE_IMAGE_DEVICE)

#define CS9711_SENSOR_WIDTH 34
#define CS9711_SENSOR_HEIGHT 236

#define CS9711_DEFAULT_WAIT_TIMEOUT 300
/* Legacy ids sleep this long before every scan command. Acl16 waits the same
 * time after the init command and, during enrollment, after each frame, so
 * that the device is armed when libfprint reports that a finger is needed.
 * Unit tests shorten it. */
#ifndef CS9711_DEFAULT_RESET_SLEEP
#define CS9711_DEFAULT_RESET_SLEEP  1500
#endif

#define CS9711_SEND_ENDPOINT    0x01
#define CS9711_RECEIVE_ENDPOINT 0x81

#define CS9711_FP_CMD_LEN_1    8
#define CS9711_FP_RECV_LEN_1   8000
#define CS9711_FP_RECV_LEN_2   24
#define CS9711_FP_RECV_LEN_MAX CS9711_FP_RECV_LEN_1

/* 2541:2549 (USB FingPintAcl16) answers the same commands but returns one
 * 8192-byte bulk frame: 112x73 grayscale pixels and 16 bytes of padding.
 * The 68x118 deinterleave used by the other ids scrambles this image.
 */
#define CS9711_ACL16_DRIVER_DATA 1
#define CS9711_ACL16_FRAME       8192
/* How long one armed scan waits for finger contact before the session fails.
 * Callers that need a shorter bound cancel the action. Unit tests shorten it. */
#ifndef CS9711_ACL16_SCAN_TIMEOUT
#define CS9711_ACL16_SCAN_TIMEOUT 180000
#endif
#define CS9711_ACL16_DRAIN_TIMEOUT 80
#define CS9711_ACL16_DRAIN_BUDGET  6
/* Bounds for unexpected short packets while waiting for a frame. */
#define CS9711_ACL16_MAX_REARMS 3
#define CS9711_ACL16_MAX_ECHOES 4

#define CS9711_FP_CMD_TYPE_INIT  1
#define CS9711_FP_CMD_TYPE_RESET 2
#define CS9711_FP_CMD_TYPE_SCAN  4

#define CS9711_FP_CMD_STATE_RESULT_EXPECTED { 0xea, 0x01, 0x62, 0xa0, 0x00, 0x00, 0xc3, 0xea }

/************************** USB I/O *******************************************/

/* All device I/O goes through these four functions. The unit test in
 * tests/test-cs9711-driver.c defines CS9711_TEST_IO, includes this file and
 * supplies scripted implementations, so the state machines can be exercised
 * without hardware. Production builds use libfprint's USB helpers. */
#ifndef CS9711_TEST_IO
static gboolean
cs9711_io_claim (FpDevice *dev, gint iface, GError **error)
{
  return g_usb_device_claim_interface (fpi_device_get_usb_device (dev),
                                       iface, 0, error);
}

static gboolean
cs9711_io_release (FpDevice *dev, gint iface, GError **error)
{
  return g_usb_device_release_interface (fpi_device_get_usb_device (dev),
                                         iface, 0, error);
}

static gboolean
cs9711_io_send_sync (FpiUsbTransfer *transfer, guint timeout_ms, GError **error)
{
  return fpi_usb_transfer_submit_sync (transfer, timeout_ms, error);
}

static void
cs9711_io_submit (FpiUsbTransfer        *transfer,
                  guint                  timeout_ms,
                  GCancellable          *cancellable,
                  FpiUsbTransferCallback callback,
                  gpointer               user_data)
{
  fpi_usb_transfer_submit (transfer, timeout_ms, cancellable, callback, user_data);
}
#else
static gboolean cs9711_io_claim (FpDevice *dev,
                                 gint      iface,
                                 GError  **error);
static gboolean cs9711_io_release (FpDevice *dev,
                                   gint      iface,
                                   GError  **error);
static gboolean cs9711_io_send_sync (FpiUsbTransfer *transfer,
                                     guint           timeout_ms,
                                     GError        **error);
static void cs9711_io_submit (FpiUsbTransfer        *transfer,
                              guint                  timeout_ms,
                              GCancellable          *cancellable,
                              FpiUsbTransferCallback callback,
                              gpointer               user_data);
#endif

/************************** GENERIC STUFF *************************************/

static gboolean
is_acl16 (FpDevice *dev)
{
  return fpi_device_get_driver_data (dev) == CS9711_ACL16_DRIVER_DATA;
}

static gint
cs9711_interface (FpDevice *dev)
{
  /* The Acl16 (2541:2549) puts the fingerprint endpoints on interface 1;
   * the other ids use interface 0. */
  return is_acl16 (dev) ? 1 : 0;
}

/** If error isn't NULL then fail the ssm or move it to the next state */
static void
m_util_fail_if_error_or_next (FpiSsm *ssm, GError *error)
{
  if (error)
    fpi_ssm_mark_failed (ssm, error);
  else
    fpi_ssm_next_state (ssm);
}

/* Format up to 16 bytes for a log line. Only used for command responses and
 * short packets, never for image frames. */
static gchar *
cs9711_hex_prefix (const guint8 *data, gssize len)
{
  GString *str = g_string_new (NULL);

  for (gssize i = 0; i < len && i < 16; i++)
    g_string_append_printf (str, "%s%02x", i ? " " : "", data[i]);
  if (len > 16)
    g_string_append (str, " ...");

  return g_string_free (str, FALSE);
}

/** Synchroneous USB bulk write OUT helper*/
static void
usb_send_out_sync (FpDevice *dev, guint8 type, GError **error)
{
  GError *err = NULL;

  guint8 *data = g_malloc0 (CS9711_FP_CMD_LEN_1);

  data[0] = data[CS9711_FP_CMD_LEN_1 - 1] = 0xEA;
  data[1] = data[CS9711_FP_CMD_LEN_1 - 2] = type;

  g_autoptr(FpiUsbTransfer) transfer = NULL;

  transfer = fpi_usb_transfer_new (FP_DEVICE (dev));
  transfer->short_is_error = FALSE;
  fpi_usb_transfer_fill_bulk_full (transfer, CS9711_SEND_ENDPOINT, (guint8 *) data, CS9711_FP_CMD_LEN_1, g_free);
  cs9711_io_send_sync (transfer, CS9711_DEFAULT_WAIT_TIMEOUT, &err);
  if (err)
    {
      if (g_error_matches (err, G_USB_DEVICE_ERROR, G_USB_DEVICE_ERROR_TIMED_OUT))
        fp_dbg ("Timeout while sending command 0x%X", type);
      else
        fp_warn ("Error while sending command 0x%X: %s", type, err->message);
      g_propagate_error (error, err);
    }
  else
    fp_dbg ("Sent command 0x%X", type);
}

/* Asynchronous bulk IN read. Every read uses the activation's cancellable so
 * that deactivation can reap it. */
static void
cs9711_submit_read (FpDevice              *dev,
                    FpiSsm                *ssm,
                    gsize                  length,
                    guint                  timeout_in_ms,
                    FpiUsbTransferCallback callback,
                    gpointer               user_data)
{
  FpDeviceCs9711 *self = FPI_DEVICE_CS9711 (dev);
  FpiUsbTransfer *transfer = fpi_usb_transfer_new (dev);

  transfer->short_is_error = FALSE;
  transfer->ssm = ssm;
  fpi_usb_transfer_fill_bulk (transfer, CS9711_RECEIVE_ENDPOINT, length);
  cs9711_io_submit (transfer, timeout_in_ms, self->cancellable, callback, user_data);
}

/** Asynchroneous USB bulk write IN helper */
static void
usb_read_in (FpDevice *dev,
             FpiSsm *ssm,
             gsize length,
             guint timeout_in_ms,
             FpiUsbTransferCallback callback,
             gpointer user_data)
{
  fp_dbg ("Reading %lu bytes", (unsigned long) length);
  // If the response is larger than the buffer, then the usb helpers
  // error before. The reader doesn't seem to care about requestend
  // length, and occasionally answers out of sequence with an invalid
  // data size. So just request the max expected, and deal with errors
  // in the callback. For the same reason, cannot use short_is_error
  // facility
  cs9711_submit_read (dev, ssm, CS9711_FP_RECV_LEN_MAX, timeout_in_ms, callback, user_data);
}

/* Same as usb_read_in, but honour the requested length. usb_read_in always
 * asks for CS9711_FP_RECV_LEN_MAX, which is wrong for the 8192-byte frame.
 */
static void
usb_read_exact (FpDevice *dev,
                FpiSsm *ssm,
                gsize length,
                guint timeout_in_ms,
                FpiUsbTransferCallback callback,
                gpointer user_data)
{
  cs9711_submit_read (dev, ssm, length, timeout_in_ms, callback, user_data);
}

/* TRUE once the driver or the caller started to abort the activation. */
static gboolean
cs9711_aborting (FpDeviceCs9711 *self)
{
  return self->deactivating ||
         self->scan_error != NULL ||
         (self->cancellable && g_cancellable_is_cancelled (self->cancellable));
}

static GError *
cs9711_cancelled_error (void)
{
  return g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED,
                              "Operation was cancelled");
}

/* Called first by every read callback and state handler. Whatever the read
 * returned, an aborting activation must not advance to the next command. */
static gboolean
cs9711_fail_if_aborting (FpDeviceCs9711 *self, FpiSsm *ssm, GError *error)
{
  if (!cs9711_aborting (self))
    return FALSE;

  g_clear_error (&error);
  fpi_ssm_mark_failed (ssm, cs9711_cancelled_error ());
  return TRUE;
}

/************************** ACTIVATION LIFETIME *******************************/

static void
cs9711_on_action_cancelled (GCancellable *action_cancellable,
                            GCancellable *cancellable)
{
  g_cancellable_cancel (cancellable);
}

static void
cs9711_end_activation (FpDeviceCs9711 *self)
{
  if (self->action_cancel_id)
    g_cancellable_disconnect (self->action_cancellable, self->action_cancel_id);
  self->action_cancel_id = 0;
  g_clear_object (&self->action_cancellable);
  g_clear_object (&self->cancellable);
  g_clear_error (&self->scan_error);
  self->deactivating = FALSE;
  self->scan_delay_pending = FALSE;
}

static void
cs9711_begin_activation (FpDeviceCs9711 *self)
{
  GCancellable *action_cancellable;

  cs9711_end_activation (self);
  self->cancellable = g_cancellable_new ();

  /* Abort pending transfers as soon as the caller cancels, also while the
   * device is still activating (libfprint only deactivates active devices). */
  action_cancellable = fpi_device_get_cancellable (FP_DEVICE (self));
  if (action_cancellable)
    {
      self->action_cancellable = g_object_ref (action_cancellable);
      self->action_cancel_id =
        g_cancellable_connect (action_cancellable,
                               G_CALLBACK (cs9711_on_action_cancelled),
                               g_object_ref (self->cancellable),
                               g_object_unref);
    }
}

static void
cs9711_finish_deactivation (FpDeviceCs9711 *self)
{
  cs9711_end_activation (self);
  fpi_image_device_deactivate_complete (FP_IMAGE_DEVICE (self), NULL);
}

/* If the quiet period after the last Acl16 reset has not passed, re-enter
 * @state of @ssm once it should have and return TRUE. GLib timeouts are
 * measured from the main loop's cached time and may fire slightly early,
 * so the state checks the clock again when it is re-entered. */
static gboolean
cs9711_wait_for_reset_deadline (FpDeviceCs9711 *self, FpiSsm *ssm, int state)
{
  gint64 remaining = self->acl16_reset_deadline - g_get_monotonic_time ();
  guint ms;

  if (remaining <= 0)
    return FALSE;

  ms = (guint) ((remaining + 999) / 1000);
  fp_dbg ("Acl16: waiting %u ms after the last reset", ms);
  fpi_ssm_jump_to_state_delayed (ssm, state, ms);
  return TRUE;
}

/************************** ACL16 FRAMES **************************************/

typedef enum {
  CS9711_ACL16_PACKET_IMAGE,
  CS9711_ACL16_PACKET_ECHO,
  CS9711_ACL16_PACKET_SHORT,
} Cs9711Acl16Packet;

static gboolean
cs9711_is_command_echo (const guint8 *data, gsize len)
{
  /* Echo of a command: ea <type> 00 00 00 00 <type> ea. The middle bytes
   * are not checked, as in the version that was tested on hardware. */
  return len >= CS9711_FP_CMD_LEN_1 &&
         data[0] == 0xea && data[7] == 0xea && data[1] == data[6];
}

/* Classify one bulk packet received while waiting for a scan. For an image,
 * *pixel_offset is where the 112x73 pixels start (an echo of the scan
 * command may precede them). */
static Cs9711Acl16Packet
cs9711_acl16_classify (const guint8 *data, gsize len, gsize *pixel_offset)
{
  gsize offset = 0;

  if (cs9711_is_command_echo (data, len))
    offset = CS9711_FP_CMD_LEN_1;

  if (len - offset >= CS9711_ACL16_IMAGE)
    {
      *pixel_offset = offset;
      return CS9711_ACL16_PACKET_IMAGE;
    }

  *pixel_offset = 0;
  if (len == CS9711_FP_CMD_LEN_1 && offset == CS9711_FP_CMD_LEN_1 &&
      data[1] == CS9711_FP_CMD_TYPE_SCAN)
    return CS9711_ACL16_PACKET_ECHO;

  return CS9711_ACL16_PACKET_SHORT;
}

/* Read until the bulk pipe is quiet, then jump to acl16_after_drain.
 * A timeout means there is nothing left queued.
 */
static void
m_acl16_drain_cb (FpiUsbTransfer *transfer,
                  FpDevice       *dev,
                  gpointer        user_data,
                  GError         *error)
{
  FpDeviceCs9711 *self = FPI_DEVICE_CS9711 (dev);

  (void) user_data;

  if (cs9711_fail_if_aborting (self, transfer->ssm, error))
    return;

  if (error)
    {
      if (g_error_matches (error, G_USB_DEVICE_ERROR, G_USB_DEVICE_ERROR_TIMED_OUT))
        {
          g_error_free (error);
          fpi_ssm_jump_to_state (transfer->ssm, self->acl16_after_drain);
          return;
        }
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  fp_dbg ("Acl16 drained %ld leftover bytes", (long) transfer->actual_length);
  if (self->acl16_drain_budget == 0)
    {
      fpi_ssm_jump_to_state (transfer->ssm, self->acl16_after_drain);
      return;
    }
  self->acl16_drain_budget--;
  usb_read_exact (dev, transfer->ssm, CS9711_ACL16_FRAME, CS9711_ACL16_DRAIN_TIMEOUT, m_acl16_drain_cb, NULL);
}

/************************** INIT SSM *************************************/

enum {
  M_INIT_STATE_ACL16_POST_RESET_WAIT = 0,
  M_INIT_STATE_DRAIN,
  M_INIT_STATE_SEND_INI_QUERY,
  M_INIT_STATE_RECOVER_READ_IGNORED,
  M_INIT_STATE_RECOVER_SEND_RESET,
  M_INIT_STATE_RECOVER_READ_IGNORED_RESET,
  M_INIT_STATE_RECOVER_SEND_INIT,
  M_INIT_STATE_RECEIVE_STATUS,
  M_INIT_STATE_ACL16_SETTLE,
  M_INIT_STATE_COUNT,
};

static void
m_init_read_cb_check_expected (FpiUsbTransfer *transfer,
                               FpDevice       *dev,
                               gpointer        user_data_is_ignore_mismatch_if_non_null,
                               GError         *error)
{
  FpDeviceCs9711 *self = FPI_DEVICE_CS9711 (dev);
  const guint8 expected[] = CS9711_FP_CMD_STATE_RESULT_EXPECTED;

  g_assert (CS9711_FP_CMD_LEN_1 == sizeof (expected));
  g_assert (transfer->ssm != NULL);

  if (cs9711_fail_if_aborting (self, transfer->ssm, error))
    return;

  if (error)
    {
      fp_warn ("Read failed: %s, aborting", error->message);
      fpi_ssm_mark_failed (transfer->ssm, error);
    }
  else
    {
      fp_dbg ("Read %ld of requested %ld", (long) transfer->actual_length, (long) transfer->length);
      if (transfer->actual_length != CS9711_FP_CMD_LEN_1)
        {
          /* Acl16 often still has the tail of the previous image queued. */
          if (is_acl16 (dev) && self->acl16_init_retries < 2)
            {
              self->acl16_init_retries++;
              fp_warn ("Acl16 init got %ld bytes, draining and retrying", (long) transfer->actual_length);
              fpi_ssm_jump_to_state (transfer->ssm, M_INIT_STATE_DRAIN);
            }
          else
            {
              fp_warn ("Error; expected %lu bytes but got %ld, failing", (gulong) CS9711_FP_CMD_LEN_1, (long) transfer->actual_length);
              error = g_error_new (FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                                   "Init: expected %lu bytes but got %ld",
                                   (gulong) CS9711_FP_CMD_LEN_1, (long) transfer->actual_length);
              fpi_ssm_mark_failed (transfer->ssm, error);
            }
        }
      else if (memcmp (transfer->buffer, expected, CS9711_FP_CMD_LEN_1))
        {
          self->acl16_init_retries = 0;
          if (is_acl16 (dev))
            {
              /* Every Acl16 seen so far answers differently from the other
               * ids; log the bytes so that the answer can be studied. */
              g_autofree gchar *hex = cs9711_hex_prefix (transfer->buffer, transfer->actual_length);
              fp_info ("Acl16 init response: %s", hex);
            }
          else if (!user_data_is_ignore_mismatch_if_non_null)
            {
              fp_warn ("Error; got different state response than expected, but don't understand it anyway, continuing");
            }
          fpi_ssm_next_state (transfer->ssm);
        }
      else
        {
          self->acl16_init_retries = 0;
          fp_dbg ("Init response valid");
          fpi_ssm_next_state (transfer->ssm);
        }
    }
}

/* Exec init sequential state machine */
static void
m_init_state (FpiSsm *ssm, FpDevice *_dev)
{
  FpDeviceCs9711 *self = FPI_DEVICE_CS9711 (_dev);
  GError *error = NULL;

  /* No read is pending when an init state is entered. */
  if (cs9711_fail_if_aborting (self, ssm, NULL))
    return;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case M_INIT_STATE_ACL16_POST_RESET_WAIT:
      /* On real hardware every scan that worked was armed at least
       * CS9711_DEFAULT_RESET_SLEEP after the previous post-scan reset with
       * no command in between. An init sent right after that reset (one
       * action ending, the next starting, e.g. fprintd's duplicate check
       * followed by enrollment) was answered, but the next scan never
       * delivered a frame. Keep the same quiet period before init. */
      if (is_acl16 (_dev) &&
          cs9711_wait_for_reset_deadline (self, ssm, M_INIT_STATE_ACL16_POST_RESET_WAIT))
        break;
      fpi_ssm_next_state (ssm);
      break;

    case M_INIT_STATE_DRAIN:
      if (!is_acl16 (_dev))
        {
          fpi_ssm_next_state (ssm);
          break;
        }
      self->acl16_drain_budget = CS9711_ACL16_DRAIN_BUDGET;
      self->acl16_after_drain = M_INIT_STATE_SEND_INI_QUERY;
      usb_read_exact (_dev, ssm, CS9711_ACL16_FRAME, CS9711_ACL16_DRAIN_TIMEOUT, m_acl16_drain_cb, NULL);
      break;

    case M_INIT_STATE_SEND_INI_QUERY:
      usb_send_out_sync (_dev, CS9711_FP_CMD_TYPE_INIT, &error);
      if (error)
        {
          fp_dbg ("Error details: '%s', quark: %u code: %d", error->message, error->domain, error->code);
          if (g_error_matches (error, G_USB_DEVICE_ERROR, G_USB_DEVICE_ERROR_TIMED_OUT))
            {
              g_error_free (error);
              fpi_ssm_next_state (ssm);
            }
          else
            {
              fpi_ssm_mark_failed (ssm, error);
            }
        }
      else
        fpi_ssm_jump_to_state (ssm, M_INIT_STATE_RECEIVE_STATUS);
      break;

    case M_INIT_STATE_RECOVER_READ_IGNORED:
      fp_warn ("Send operation had a timeout. Switching to reset procedure. Ignore next message about the result not matching the expected data.");
      usb_read_in (_dev, ssm, CS9711_FP_CMD_LEN_1, CS9711_DEFAULT_WAIT_TIMEOUT, m_init_read_cb_check_expected, NULL);
      break;

    case M_INIT_STATE_RECOVER_SEND_RESET:
      usb_send_out_sync (_dev, CS9711_FP_CMD_TYPE_RESET, &error);
      m_util_fail_if_error_or_next (ssm, error);
      break;

    case M_INIT_STATE_RECOVER_READ_IGNORED_RESET:
      fp_warn ("Send operation had a timeout. Switching to reset procedure.");
      usb_read_in (_dev, ssm, CS9711_FP_CMD_LEN_1, CS9711_DEFAULT_WAIT_TIMEOUT, m_init_read_cb_check_expected, self);
      break;

    case M_INIT_STATE_RECOVER_SEND_INIT:
      usb_send_out_sync (_dev, CS9711_FP_CMD_TYPE_INIT, &error); // Do not reuse state to only try reset once
      m_util_fail_if_error_or_next (ssm, error);
      break;

    case M_INIT_STATE_RECEIVE_STATUS:
      usb_read_in (_dev, ssm, CS9711_FP_CMD_LEN_1, CS9711_DEFAULT_WAIT_TIMEOUT, m_init_read_cb_check_expected, NULL);
      break;

    case M_INIT_STATE_ACL16_SETTLE:
      /* Wait here rather than before the scan command, so that the device
       * is armed by the time libfprint reports that a finger is needed. */
      if (is_acl16 (_dev))
        fpi_ssm_next_state_delayed (ssm, CS9711_DEFAULT_RESET_SLEEP);
      else
        fpi_ssm_next_state (ssm);
      break;

    default:
      g_assert_not_reached ();
    }
}

/* Complete init sequential state machine */
static void
m_init_complete (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpDeviceCs9711 *self = FPI_DEVICE_CS9711 (dev);

  /* libfprint ignores cancellation while a device is activating, so a
   * cancellation that arrived during the settle delay is reported here. */
  if (!error && g_cancellable_is_cancelled (self->cancellable))
    error = cs9711_cancelled_error ();

  if (error)
    cs9711_end_activation (self);

  fpi_image_device_activate_complete (FP_IMAGE_DEVICE (dev), error);
}

/************************** SCAN SSM *************************************/

enum {
  M_SCAN_INIT_SLEEP = 0,
  M_SCAN_INIT_READ,
  M_SCAN_WAIT_FOR_READ_TO_COMPLETE,
  M_SCAN_GET_IMAGE_TAIL,
  M_SCAN_ACL16_DRAIN,
  M_SCAN_SEND_POST_SCAN,
  M_SCAN_ACL16_SETTLE,
  M_SCAN_IMAGE_COMPLETE,
  M_SCAN_STATE_COUNT,
};

static const gpointer M_SCAN_READ_CB_BULK_UD_FIRST_BLOCK = (gpointer)1;
static const gpointer M_SCAN_READ_CB_BULK_UD_SECOND_BLOCK = (gpointer)2;

static void
m_scan_read_cb_acl16 (FpiUsbTransfer *transfer,
                      FpDevice       *dev,
                      gpointer        user_data,
                      GError         *error)
{
  FpDeviceCs9711 *self = FPI_DEVICE_CS9711 (dev);
  Cs9711Acl16Packet packet;
  gsize offset;

  (void) user_data;
  g_assert (transfer->ssm != NULL);

  if (cs9711_fail_if_aborting (self, transfer->ssm, error))
    return;

  if (error)
    {
      if (g_error_matches (error, G_USB_DEVICE_ERROR, G_USB_DEVICE_ERROR_TIMED_OUT))
        {
          g_error_free (error);
          error = fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                            "No finger contact detected within %u seconds",
                                            (guint) (CS9711_ACL16_SCAN_TIMEOUT + 999) / 1000);
          fp_info ("Acl16 scan timed out: %s", error->message);
        }
      else
        {
          fp_warn ("Acl16 scan read failed: %s", error->message);
        }
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  packet = cs9711_acl16_classify (transfer->buffer, transfer->actual_length, &offset);
  switch (packet)
    {
    case CS9711_ACL16_PACKET_IMAGE:
      fp_dbg ("Acl16 frame received (%ld bytes, %s echo)",
              (long) transfer->actual_length, offset ? "with" : "without");
      memcpy (self->image_buffer, transfer->buffer + offset, CS9711_ACL16_IMAGE);
      /* Acl16 only answers once a finger touches the sensor. */
      fpi_image_device_report_finger_status (FP_IMAGE_DEVICE (dev), TRUE);
      self->acl16_after_drain = M_SCAN_SEND_POST_SCAN;
      fpi_ssm_jump_to_state (transfer->ssm, M_SCAN_ACL16_DRAIN);
      return;

    case CS9711_ACL16_PACKET_ECHO:
      if (++self->acl16_echoes > CS9711_ACL16_MAX_ECHOES)
        break;
      /* The device acknowledged the scan command; keep waiting. */
      fp_dbg ("Acl16 scan command echo, still waiting for finger contact");
      usb_read_exact (dev, transfer->ssm, CS9711_ACL16_FRAME, CS9711_ACL16_SCAN_TIMEOUT, m_scan_read_cb_acl16, NULL);
      return;

    case CS9711_ACL16_PACKET_SHORT:
      {
        g_autofree gchar *hex = cs9711_hex_prefix (transfer->buffer, transfer->actual_length);

        if (++self->acl16_rearms > CS9711_ACL16_MAX_REARMS)
          break;
        fp_dbg ("Acl16 short packet (%ld bytes: %s), draining and re-arming",
                (long) transfer->actual_length, hex);
        self->acl16_after_drain = M_SCAN_INIT_READ;
        fpi_ssm_jump_to_state (transfer->ssm, M_SCAN_ACL16_DRAIN);
        return;
      }
    }

  fpi_ssm_mark_failed (transfer->ssm,
                       fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                 "Acl16 kept sending %ld-byte packets instead of a frame",
                                                 (long) transfer->actual_length));
}

/* Read into FpDeviceCs9711->image_buffer if one of the two expected chunk sizes */
static void
m_scan_read_cb_bulk (FpiUsbTransfer *transfer,
                     FpDevice       *dev,
                     gpointer        user_data,
                     GError         *error)
{
  FpDeviceCs9711 *self = FPI_DEVICE_CS9711 (dev);

  g_assert (transfer->ssm != NULL);

  gsize expected_size = 0;
  gpointer offset = NULL;

  g_assert (FALSE
    || user_data == M_SCAN_READ_CB_BULK_UD_FIRST_BLOCK
    || user_data == M_SCAN_READ_CB_BULK_UD_SECOND_BLOCK
  );

  if (user_data == M_SCAN_READ_CB_BULK_UD_FIRST_BLOCK)
    {
      expected_size = CS9711_FP_RECV_LEN_1;
      offset = self->image_buffer;
    }
  else if (user_data == M_SCAN_READ_CB_BULK_UD_SECOND_BLOCK)
    {
      expected_size = CS9711_FP_RECV_LEN_2;
      offset = self->image_buffer + CS9711_FP_RECV_LEN_1;
    }
  else
    g_assert_not_reached ();

  if (cs9711_fail_if_aborting (self, transfer->ssm, error))
    return;

  if (error)
    {
      fp_warn ("Read failed: %s, aborting", error->message);
      fpi_ssm_mark_failed (transfer->ssm, error);
    }
  else
    {
      if (transfer->actual_length != expected_size)
        {
          fp_dbg ("\tSkipping buffer print - got %ld bytes, expected %lu", (long) transfer->actual_length, (gulong) expected_size);
          error = g_error_new (FP_DEVICE_ERROR, FP_DEVICE_ERROR_DATA_INVALID, "expected %lu bytes but got %ld, can't continue", (gulong) expected_size, (long) transfer->actual_length);
          fpi_ssm_mark_failed (transfer->ssm, error);
        }
      else
        {
          memcpy (offset, transfer->buffer, expected_size);
          if (user_data == M_SCAN_READ_CB_BULK_UD_SECOND_BLOCK)
            /* The Acl16 drain does not apply to these ids. */
            fpi_ssm_jump_to_state (transfer->ssm, M_SCAN_SEND_POST_SCAN);
          else
            fpi_ssm_next_state (transfer->ssm);
        }
    }
}

static int
m_scan_submit_image (FpiSsm        *ssm,
                     FpImageDevice *dev)
{
  FpDeviceCs9711 *self = FPI_DEVICE_CS9711 (dev);
  FpImage *img;

  if (is_acl16 (FP_DEVICE (dev)))
    {
      img = fp_image_new (CS9711_ACL16_WIDTH, CS9711_ACL16_HEIGHT);
      if (img == NULL)
        return 1;
      memcpy (img->data, self->image_buffer, CS9711_ACL16_IMAGE);
    }
  else
    {
      img = fp_image_new (CS9711_WIDTH, CS9711_HEIGHT);
      if (img == NULL)
        return 1;

      for (gsize y = 0; y < CS9711_SENSOR_HEIGHT; y++)
        for (gsize x = 0; x < CS9711_SENSOR_WIDTH; x++) {
          gsize dy = y / 2;
          gsize dx = x * 2 + y % 2;
          img->data[dy * CS9711_WIDTH + dx] = self->image_buffer[y * CS9711_SENSOR_WIDTH + x];
        }
    }

  img->flags = FPI_IMAGE_PARTIAL;

  fpi_image_device_image_captured (dev, img);

  return 0;
}

/* Exec scan sequential state machine */
static void
m_scan_state (FpiSsm *ssm, FpDevice *_dev)
{
  FpDeviceCs9711 *self = FPI_DEVICE_CS9711 (_dev);
  FpImageDevice *image_device = FP_IMAGE_DEVICE (_dev);
  GError *error = NULL;

  self->scan_delay_pending = FALSE;

  /* Only the wait state is entered with a read in flight; every other state
   * may stop here if the activation is being aborted. */
  if (fpi_ssm_get_cur_state (ssm) != M_SCAN_WAIT_FOR_READ_TO_COMPLETE &&
      cs9711_fail_if_aborting (self, ssm, NULL))
    return;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case M_SCAN_INIT_SLEEP:
      /* Acl16 already waited after init or after the previous frame. */
      if (is_acl16 (_dev))
        {
          fpi_ssm_next_state (ssm);
          break;
        }
      self->scan_delay_pending = TRUE;
      fpi_ssm_next_state_delayed (ssm, CS9711_DEFAULT_RESET_SLEEP);
      break;

    case M_SCAN_INIT_READ:
      if (is_acl16 (_dev))
        usb_read_exact (_dev, ssm, CS9711_ACL16_FRAME, CS9711_ACL16_SCAN_TIMEOUT, m_scan_read_cb_acl16, NULL);
      else
        usb_read_in (_dev, ssm, CS9711_FP_RECV_LEN_1, 0, m_scan_read_cb_bulk, M_SCAN_READ_CB_BULK_UD_FIRST_BLOCK);
      usb_send_out_sync (_dev, CS9711_FP_CMD_TYPE_SCAN, &error);
      if (error)
        {
          /* The read is in flight and holds the SSM, so the SSM may only
           * finish once the read has been reaped. Cancel it and stay in this
           * state; m_scan_complete reports the send error. */
          self->scan_error = error;
          g_cancellable_cancel (self->cancellable);
          break;
        }
      if (is_acl16 (_dev))
        fp_info ("Acl16 armed, waiting up to %u s for finger contact",
                 (guint) (CS9711_ACL16_SCAN_TIMEOUT + 999) / 1000);
      else
        fpi_image_device_report_finger_status (image_device, TRUE);
      fpi_ssm_next_state (ssm);
      break;

    case M_SCAN_WAIT_FOR_READ_TO_COMPLETE:
      /* Wait for the read callback to advance the state */
      break;

    case M_SCAN_GET_IMAGE_TAIL:
      usb_read_in (_dev, ssm, CS9711_FP_RECV_LEN_2, CS9711_DEFAULT_WAIT_TIMEOUT, m_scan_read_cb_bulk, M_SCAN_READ_CB_BULK_UD_SECOND_BLOCK);
      break;

    case M_SCAN_ACL16_DRAIN:
      g_assert (is_acl16 (_dev));
      self->acl16_drain_budget = CS9711_ACL16_DRAIN_BUDGET;
      usb_read_exact (_dev, ssm, CS9711_ACL16_FRAME, CS9711_ACL16_DRAIN_TIMEOUT, m_acl16_drain_cb, NULL);
      break;

    case M_SCAN_SEND_POST_SCAN:
      usb_send_out_sync (_dev, CS9711_FP_CMD_TYPE_RESET, &error);
      if (is_acl16 (_dev))
        self->acl16_reset_deadline = g_get_monotonic_time () +
                                     (gint64) CS9711_DEFAULT_RESET_SLEEP * 1000;
      m_util_fail_if_error_or_next (ssm, error);
      break;

    case M_SCAN_ACL16_SETTLE:
      /* During enrollment the next stage arms the sensor right after this
       * image, so wait out the post-reset quiet period here; it also gives
       * the user time to lift the finger. The image is submitted afterwards,
       * so the last stage never leaves the device waiting for a finger-off
       * report. Other actions end here; the next action's init waits. */
      if (is_acl16 (_dev) &&
          fpi_device_get_current_action (_dev) == FPI_DEVICE_ACTION_ENROLL &&
          cs9711_wait_for_reset_deadline (self, ssm, M_SCAN_ACL16_SETTLE))
        {
          self->scan_delay_pending = TRUE;
          break;
        }
      fpi_ssm_next_state (ssm);
      break;

    case M_SCAN_IMAGE_COMPLETE:
      m_scan_submit_image (ssm, image_device);
      fpi_ssm_mark_completed (ssm);
      break;

    default:
      g_assert_not_reached ();
    }
}

static void
m_scan_complete (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpDeviceCs9711 *self = FPI_DEVICE_CS9711 (dev);
  FpImageDevice *image_device = FP_IMAGE_DEVICE (dev);

  self->scan_ssm = NULL;
  self->scan_delay_pending = FALSE;

  if (self->scan_error)
    {
      g_clear_error (&error);
      error = g_steal_pointer (&self->scan_error);
    }

  if (self->deactivating)
    {
      g_clear_error (&error);
      cs9711_finish_deactivation (self);
      return;
    }

  if (!error)
    {
      /* The image has been submitted. Neither id reports finger removal. */
      fpi_image_device_report_finger_status (image_device, FALSE);
      return;
    }

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      /* The caller cancelled; libfprint deactivates the device next. */
      fp_dbg ("Scan cancelled");
      g_error_free (error);
      return;
    }

  fpi_image_device_session_error (image_device, error);
}

/************************** ImageDevice impl. *************************************/

/* Activate device */
static void
dev_activate (FpImageDevice *dev)
{
  FpDeviceCs9711 *self = FPI_DEVICE_CS9711 (dev);
  FpiSsm *ssm;

  cs9711_begin_activation (self);
  self->acl16_init_retries = 0;

  /* Start init ssm */
  ssm = fpi_ssm_new (FP_DEVICE (dev), m_init_state, M_INIT_STATE_COUNT);
  fpi_ssm_start (ssm, m_init_complete);
}

/* Deactivate device */
static void
dev_deactivate (FpImageDevice *dev)
{
  FpDeviceCs9711 *self = FPI_DEVICE_CS9711 (dev);

  if (!self->scan_ssm)
    {
      cs9711_finish_deactivation (self);
      return;
    }

  /* Finish only once the capture state machine has stopped: reap the
   * pending read (its callback fails the SSM) or stop a pending delay. */
  self->deactivating = TRUE;
  g_cancellable_cancel (self->cancellable);

  if (self->scan_delay_pending)
    {
      self->scan_delay_pending = FALSE;
      fpi_ssm_cancel_delayed_state_change (self->scan_ssm);
      fpi_ssm_mark_failed (self->scan_ssm, cs9711_cancelled_error ());
    }
}

static void
dev_change_state (FpImageDevice *dev, FpiImageDeviceState state)
{
  FpDeviceCs9711 *self = FPI_DEVICE_CS9711 (dev);
  FpiSsm *ssm;

  if (state != FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_ON)
    return;

  g_return_if_fail (self->scan_ssm == NULL);

  self->acl16_rearms = 0;
  self->acl16_echoes = 0;

  /* Start a capture operation. */
  ssm = fpi_ssm_new (FP_DEVICE (dev), m_scan_state, M_SCAN_STATE_COUNT);
  self->scan_ssm = ssm;
  fpi_ssm_start (ssm, m_scan_complete);
}

static void
dev_open (FpImageDevice *dev)
{
  FpDeviceCs9711 *self = FPI_DEVICE_CS9711 (dev);
  GError *error = NULL;

  /* Initialize private structure */
  memset (self->image_buffer, 0, sizeof (self->image_buffer));
  cs9711_end_activation (self);
  self->scan_ssm = NULL;
  self->acl16_init_retries = 0;

  /* Claim usb interface. */
  cs9711_io_claim (FP_DEVICE (dev), cs9711_interface (FP_DEVICE (dev)), &error);

  /* Notify open complete */
  fpi_image_device_open_complete (dev, error);
}

static void
dev_close (FpImageDevice *dev)
{
  FpDeviceCs9711 *self = FPI_DEVICE_CS9711 (dev);
  GError *error = NULL;

  g_warn_if_fail (self->scan_ssm == NULL);
  cs9711_end_activation (self);

  /* Release usb interface */
  if (!cs9711_io_release (FP_DEVICE (dev), cs9711_interface (FP_DEVICE (dev)), &error) &&
      g_error_matches (error, G_USB_DEVICE_ERROR, G_USB_DEVICE_ERROR_NO_DEVICE))
    {
      /* The reader was unplugged; there is nothing left to release. */
      fp_dbg ("Not releasing interface of removed device: %s", error->message);
      g_clear_error (&error);
    }

  /* Notify close complete */
  fpi_image_device_close_complete (dev, error);
}

static void
fpi_device_cs9711_finalize (GObject *object)
{
  FpDeviceCs9711 *self = FPI_DEVICE_CS9711 (object);

  cs9711_end_activation (self);

  G_OBJECT_CLASS (fpi_device_cs9711_parent_class)->finalize (object);
}

/* Usb id table of device */
static const FpIdEntry id_table[] = {
  { .vid = 0x2541,  .pid = 0x0236, },
  { .vid = 0x2541,  .pid = 0x9711, },
  { .vid = 0, .pid = 0, .driver_data = 0 },
};

static void
fpi_device_cs9711_init (FpDeviceCs9711 *self)
{
}

static void
fpi_device_cs9711_class_init (FpDeviceCs9711Class *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);
  FpImageDeviceClass *img_class = FP_IMAGE_DEVICE_CLASS (klass);

  g_assert ((CS9711_FRAME_SIZE) == (CS9711_FP_RECV_LEN_1 + CS9711_FP_RECV_LEN_2));

  object_class->finalize = fpi_device_cs9711_finalize;

  dev_class->id = "cs9711";
  dev_class->full_name = "Chipsailing CS9711Fingprint";
  dev_class->type = FP_DEVICE_TYPE_USB;
  dev_class->id_table = id_table;
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;
  dev_class->nr_enroll_stages = 15;

  img_class->algorithm = FPI_PRINT_SIGFM;
  img_class->img_open = dev_open;
  img_class->img_close = dev_close;
  img_class->activate = dev_activate;
  img_class->deactivate = dev_deactivate;
  img_class->change_state = dev_change_state;

  //TODO: Makes very marginal improvement, stick with default in case
  //      it changes with a better implementation in the future
  // img_class->bz3_threshold = 24;

  img_class->img_width = CS9711_WIDTH;
  img_class->img_height = CS9711_HEIGHT;
}

/* Temperature settings are copied from the class during construction. Keep
 * Acl16's exception in its own class so the original readers retain their
 * default temperature model. The driver id stays the same for stored prints.
 */
typedef struct
{
  FpDeviceCs9711 parent;
} FpDeviceCs9711Acl16;

typedef FpDeviceCs9711Class FpDeviceCs9711Acl16Class;

GType fpi_device_cs9711_acl16_get_type (void);
G_DEFINE_TYPE (FpDeviceCs9711Acl16, fpi_device_cs9711_acl16, fpi_device_cs9711_get_type ())

static const FpIdEntry acl16_id_table[] = {
  { .vid = 0x2541, .pid = 0x2549, .driver_data = CS9711_ACL16_DRIVER_DATA },
  { .vid = 0, .pid = 0, .driver_data = 0 },
};

static void
fpi_device_cs9711_acl16_init (FpDeviceCs9711Acl16 *self)
{
}

static void
fpi_device_cs9711_acl16_class_init (FpDeviceCs9711Acl16Class *klass)
{
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);

  dev_class->id_table = acl16_id_table;
  /* The default 3-minute estimate aborts a 15-stage Acl16 enroll. */
  dev_class->temp_hot_seconds = -1;
  fpi_device_class_auto_initialize_features (dev_class);
}
