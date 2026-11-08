/*
 * CS9711 / Acl16 driver state machine tests
 *
 * The driver source is included with a scripted USB transport, so capture,
 * enrollment, matching, cancellation, timeouts and USB errors run without
 * hardware. All frames are synthetic noise textures; no biometric data is
 * used or written anywhere.
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

#define CS9711_TEST_IO 1
#define CS9711_DEFAULT_RESET_SLEEP 20
#define CS9711_ACL16_SCAN_TIMEOUT 400

#include "drivers/cs9711/cs9711.c"

#include "fp-print-private.h"
#include "sigfm/sigfm.h"

#define TEST_DOMAIN "libfprint-cs9711"

/************************** Synthetic frames **********************************/

/* Blurred noise: enough SIFT keypoints, no relation to any fingerprint. */
static guint8 *
make_texture (guint32 seed, gint width, gint height)
{
  g_autofree guint8 *noise = g_malloc (width * height);
  guint8 *out = g_malloc (width * height);
  GRand *rand = g_rand_new_with_seed (seed);

  for (gint i = 0; i < width * height; i++)
    noise[i] = g_rand_int_range (rand, 0, 256);

  for (gint y = 0; y < height; y++)
    for (gint x = 0; x < width; x++)
      {
        guint sum = 0, n = 0;

        for (gint dy = -1; dy <= 1; dy++)
          for (gint dx = -1; dx <= 1; dx++)
            {
              gint xx = x + dx, yy = y + dy;

              if (xx < 0 || yy < 0 || xx >= width || yy >= height)
                continue;
              sum += noise[yy * width + xx];
              n++;
            }
        out[y * width + x] = sum / n;
      }

  g_rand_free (rand);
  return out;
}

static const guint8 echo_scan[] = { 0xea, 0x04, 0x00, 0x00, 0x00, 0x00, 0x04, 0xea };
/* What a 2541:2549 answered to an init command (chip id 0x62a2). */
static const guint8 acl16_init_reply[] = { 0xea, 0x01, 0x62, 0xa2, 0x00, 0x00, 0xc1, 0xea };
static const guint8 legacy_init_reply[] = CS9711_FP_CMD_STATE_RESULT_EXPECTED;

/************************** Scripted transport ********************************/

typedef enum {
  FAKE_TIMEOUT,
  FAKE_DATA,
  FAKE_HOLD,
  FAKE_ERROR,
} FakeKind;

typedef struct
{
  FakeKind kind;
  guint8  *data;
  gsize    len;
  GError  *error;
} FakeReply;

typedef struct
{
  FpiUsbTransfer        *transfer;
  FpiUsbTransferCallback callback;
  gpointer               user_data;
  GCancellable          *cancellable;
  gulong                 cancel_id;
  guint                  timeout_id;
  guint                  idle_id;
  guint8                *data;
  gsize                  len;
  GError                *error;
} FakeRead;

typedef void (*FakeHook) (guint count);
typedef void (*FakeSendHook) (guint8 cmd);

typedef struct
{
  gboolean  acl16;
  GQueue    replies;          /* FakeReply *, used before the defaults */
  GArray   *sent;             /* command types, in order */
  FakeRead *held;             /* read waiting for finger contact */
  guint     pending;          /* submitted, not yet completed reads */
  guint     holds;            /* reads that waited for contact */
  guint     drain_reads;      /* Acl16 reads that wait for a quiet pipe */
  guint     tail_reads;       /* Acl16 reads of a frame's last bytes */
  gsize     drained_bytes;
  /* Acl16: bytes the device has queued on the bulk pipe. A read takes what
   * fits, like a bulk transfer; the rest stays for the next read. */
  GByteArray *queued;
  gint      claimed;
  guint     claims;
  guint     releases;
  GError   *release_error;
  guint8    fail_send_cmd;
  GError   *fail_send_error;

  /* What a touch delivers. */
  guint8   *frame;
  gsize     frame_len;
  guint8   *expected;         /* expected image pixels */
  gsize     expected_len;

  FakeHook  on_hold;          /* run from idle after a read starts waiting */
  FakeSendHook on_send;       /* run synchronously for every command */
  FakeHook  on_present;       /* run when finger presence is reported */

  gboolean  was_waiting;
  gboolean  was_present;
  guint     armed_reports;
  guint     present_reports;
} Fake;

static Fake fake;
static GCancellable *test_cancellable;

static guint8
fake_last_sent (void)
{
  if (fake.sent->len == 0)
    return 0;
  return g_array_index (fake.sent, guint8, fake.sent->len - 1);
}

static guint
fake_count_sent (guint8 cmd)
{
  guint n = 0;

  for (guint i = 0; i < fake.sent->len; i++)
    n += g_array_index (fake.sent, guint8, i) == cmd;
  return n;
}

static gboolean
fake_finish_cb (gpointer user_data)
{
  FakeRead *r = user_data;
  FpiUsbTransfer *transfer = r->transfer;

  r->idle_id = 0;
  if (r->timeout_id)
    g_source_remove (r->timeout_id);
  if (r->cancel_id)
    g_cancellable_disconnect (r->cancellable, r->cancel_id);
  if (fake.held == r)
    fake.held = NULL;

  if (r->data)
    {
      g_assert_cmpuint (r->len, <=, transfer->length);
      memcpy (transfer->buffer, r->data, r->len);
      transfer->actual_length = r->len;
    }
  else
    {
      transfer->actual_length = 0;
    }

  g_assert_cmpuint (fake.pending, >, 0);
  fake.pending--;
  r->callback (transfer, transfer->device, r->user_data, g_steal_pointer (&r->error));
  fpi_usb_transfer_unref (transfer);

  g_clear_object (&r->cancellable);
  g_free (r->data);
  g_free (r);

  return G_SOURCE_REMOVE;
}

/* Complete asynchronously, like GUsb does. */
static void
fake_finish_later (FakeRead *r, const guint8 *data, gsize len, GError *error)
{
  g_assert_cmpuint (r->idle_id, ==, 0);
  r->data = data ? g_memdup2 (data, len) : NULL;
  r->len = len;
  r->error = error;
  r->idle_id = g_idle_add (fake_finish_cb, r);
}

static gboolean
fake_held_timeout_cb (gpointer user_data)
{
  FakeRead *r = user_data;

  r->timeout_id = 0;
  if (r->idle_id == 0)
    fake_finish_later (r, NULL, 0,
                       g_error_new_literal (G_USB_DEVICE_ERROR, G_USB_DEVICE_ERROR_TIMED_OUT,
                                            "fake transfer timed out"));
  return G_SOURCE_REMOVE;
}

static void
fake_held_cancelled_cb (GCancellable *cancellable, FakeRead *r)
{
  /* An already completed transfer keeps its result. */
  if (r->idle_id == 0)
    fake_finish_later (r, NULL, 0,
                       g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                            "fake transfer cancelled"));
}

static gboolean
fake_on_hold_cb (gpointer user_data)
{
  if (fake.on_hold)
    fake.on_hold (fake.holds);
  return G_SOURCE_REMOVE;
}

static FakeReply *
fake_reply_new (FakeKind kind, const guint8 *data, gsize len)
{
  FakeReply *reply = g_new0 (FakeReply, 1);

  reply->kind = kind;
  reply->data = data ? g_memdup2 (data, len) : NULL;
  reply->len = len;
  return reply;
}

static void
fake_reply_free (FakeReply *reply)
{
  g_free (reply->data);
  g_clear_error (&reply->error);
  g_free (reply);
}

static FakeReply *
fake_default_reply (gsize length, guint timeout_ms)
{
  if (fake.acl16)
    {
      if (timeout_ms == CS9711_ACL16_DRAIN_TIMEOUT)
        fake.drain_reads++;
      else if (timeout_ms == CS9711_DEFAULT_WAIT_TIMEOUT)
        fake.tail_reads++;

      /* Whatever is queued is delivered at once. */
      if (fake.queued->len > 0)
        {
          gsize n = MIN (length, fake.queued->len);
          FakeReply *reply = fake_reply_new (FAKE_DATA, fake.queued->data, n);

          if (timeout_ms == CS9711_ACL16_DRAIN_TIMEOUT)
            fake.drained_bytes += n;
          g_byte_array_remove_range (fake.queued, 0, n);
          return reply;
        }

      if (timeout_ms == CS9711_ACL16_SCAN_TIMEOUT)
        {
          /* Frames are always read whole. */
          g_assert_cmpuint (length, ==, CS9711_ACL16_WIRE_FRAME);
          return fake_reply_new (FAKE_HOLD, NULL, 0);
        }

      /* A drain or a frame tail with nothing queued. */
      g_assert_true (timeout_ms == CS9711_ACL16_DRAIN_TIMEOUT ||
                     timeout_ms == CS9711_DEFAULT_WAIT_TIMEOUT);
      return fake_reply_new (FAKE_TIMEOUT, NULL, 0);
    }

  if (timeout_ms == 0)
    return fake_reply_new (FAKE_HOLD, NULL, 0);

  g_assert_cmpuint (timeout_ms, ==, CS9711_DEFAULT_WAIT_TIMEOUT);
  if (fake_last_sent () == CS9711_FP_CMD_TYPE_INIT)
    return fake_reply_new (FAKE_DATA, legacy_init_reply, sizeof (legacy_init_reply));

  /* Legacy image tail */
  g_assert_cmpuint (fake_last_sent (), ==, CS9711_FP_CMD_TYPE_SCAN);
  return fake_reply_new (FAKE_DATA, fake.frame + CS9711_FP_RECV_LEN_1, CS9711_FP_RECV_LEN_2);
}

static gboolean
cs9711_io_claim (FpDevice *dev, gint iface, GError **error)
{
  g_assert_cmpint (fake.claimed, ==, -1);
  fake.claimed = iface;
  fake.claims++;
  return TRUE;
}

static gboolean
cs9711_io_release (FpDevice *dev, gint iface, GError **error)
{
  g_assert_cmpint (fake.claimed, ==, iface);
  fake.claimed = -1;
  fake.releases++;
  if (fake.release_error)
    {
      g_propagate_error (error, g_steal_pointer (&fake.release_error));
      return FALSE;
    }
  return TRUE;
}

static gboolean
cs9711_io_send_sync (FpiUsbTransfer *transfer, guint timeout_ms, GError **error)
{
  const guint8 *buf = transfer->buffer;
  guint8 cmd;

  g_assert_cmpint (transfer->endpoint, ==, CS9711_SEND_ENDPOINT);
  g_assert_cmpint (transfer->length, ==, CS9711_FP_CMD_LEN_1);
  g_assert_cmpuint (buf[0], ==, 0xea);
  g_assert_cmpuint (buf[7], ==, 0xea);
  g_assert_cmpuint (buf[1], ==, buf[6]);
  cmd = buf[1];
  g_array_append_val (fake.sent, cmd);

  /* Neither vendor driver sends an init command to this id, and one sent
   * right after a reset stopped the next scan from delivering frames. */
  if (fake.acl16)
    g_assert_cmpuint (cmd, !=, CS9711_FP_CMD_TYPE_INIT);

  if (fake.on_send)
    fake.on_send (cmd);

  if (fake.fail_send_error && fake.fail_send_cmd == cmd)
    {
      g_propagate_error (error, g_steal_pointer (&fake.fail_send_error));
      return FALSE;
    }

  transfer->actual_length = transfer->length;
  return TRUE;
}

static void
cs9711_io_submit (FpiUsbTransfer        *transfer,
                  guint                  timeout_ms,
                  GCancellable          *cancellable,
                  FpiUsbTransferCallback callback,
                  gpointer               user_data)
{
  FakeRead *r = g_new0 (FakeRead, 1);
  FakeReply *reply;

  g_assert_cmpint (transfer->endpoint, ==, CS9711_RECEIVE_ENDPOINT);
  g_assert_nonnull (transfer->ssm);

  r->transfer = transfer;
  r->callback = callback;
  r->user_data = user_data;
  r->cancellable = cancellable ? g_object_ref (cancellable) : NULL;
  fake.pending++;

  reply = g_queue_pop_head (&fake.replies);
  if (!reply)
    reply = fake_default_reply (transfer->length, timeout_ms);

  if (cancellable && g_cancellable_is_cancelled (cancellable))
    {
      fake_finish_later (r, NULL, 0,
                         g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                              "fake transfer cancelled before start"));
      fake_reply_free (reply);
      return;
    }

  switch (reply->kind)
    {
    case FAKE_TIMEOUT:
      fake_finish_later (r, NULL, 0,
                         g_error_new_literal (G_USB_DEVICE_ERROR, G_USB_DEVICE_ERROR_TIMED_OUT,
                                              "fake transfer timed out"));
      break;

    case FAKE_DATA:
      fake_finish_later (r, reply->data, reply->len, NULL);
      break;

    case FAKE_ERROR:
      fake_finish_later (r, NULL, 0, g_steal_pointer (&reply->error));
      break;

    case FAKE_HOLD:
      g_assert_null (fake.held);
      fake.held = r;
      fake.holds++;
      if (timeout_ms)
        r->timeout_id = g_timeout_add (timeout_ms, fake_held_timeout_cb, r);
      if (cancellable)
        r->cancel_id = g_cancellable_connect (cancellable,
                                              G_CALLBACK (fake_held_cancelled_cb),
                                              r, NULL);
      g_idle_add (fake_on_hold_cb, NULL);
      break;
    }

  fake_reply_free (reply);
}

/* Deliver data to the read that waits for finger contact. What does not fit
 * into that read stays queued on the Acl16 pipe. */
static void
fake_touch_with (const guint8 *data, gsize len)
{
  gsize n;

  g_assert_nonnull (fake.held);
  n = MIN (len, fake.held->transfer->length);
  if (n < len)
    {
      g_assert_true (fake.acl16);
      g_byte_array_append (fake.queued, data + n, len - n);
    }
  fake_finish_later (fake.held, data, n, NULL);
}

static void
fake_touch (void)
{
  if (fake.acl16)
    fake_touch_with (fake.frame, fake.frame_len);
  else
    fake_touch_with (fake.frame, CS9711_FP_RECV_LEN_1);
}

static void
fake_fail_held (GError *error)
{
  g_assert_nonnull (fake.held);
  fake_finish_later (fake.held, NULL, 0, error);
}

static void
hook_touch (guint count)
{
  fake_touch ();
}

static void
hook_cancel (guint count)
{
  g_cancellable_cancel (test_cancellable);
}

/* A 112x88 wire frame, optionally after an echo of the scan command. Only
 * its top 112x73 rows are expected in the image; the bottom rows hold an
 * unrelated texture so that using them would be noticed. */
static void
fake_set_acl16_frame (guint32 seed, gboolean with_echo)
{
  gsize offset = with_echo ? CS9711_FP_CMD_LEN_1 : 0;
  g_autofree guint8 *wire = make_texture (seed, CS9711_ACL16_WIDTH, CS9711_ACL16_WIRE_HEIGHT);
  g_autofree guint8 *bottom = make_texture (seed + 1000, CS9711_ACL16_WIDTH, CS9711_ACL16_WIRE_HEIGHT);

  g_clear_pointer (&fake.frame, g_free);
  g_clear_pointer (&fake.expected, g_free);

  memcpy (wire + CS9711_ACL16_IMAGE, bottom + CS9711_ACL16_IMAGE,
          CS9711_ACL16_WIRE_FRAME - CS9711_ACL16_IMAGE);
  fake.expected = g_memdup2 (wire, CS9711_ACL16_IMAGE);
  fake.expected_len = CS9711_ACL16_IMAGE;
  fake.frame_len = offset + CS9711_ACL16_WIRE_FRAME;
  fake.frame = g_malloc0 (fake.frame_len);
  if (with_echo)
    memcpy (fake.frame, echo_scan, sizeof (echo_scan));
  memcpy (fake.frame + offset, wire, CS9711_ACL16_WIRE_FRAME);
}

static void
fake_set_legacy_frame (guint32 seed)
{
  g_clear_pointer (&fake.frame, g_free);
  g_clear_pointer (&fake.expected, g_free);

  fake.frame = make_texture (seed, CS9711_SENSOR_WIDTH, CS9711_SENSOR_HEIGHT);
  fake.frame_len = CS9711_FRAME_SIZE;
  fake.expected = g_malloc0 (CS9711_FRAME_SIZE);
  fake.expected_len = CS9711_FRAME_SIZE;
  for (gsize y = 0; y < CS9711_SENSOR_HEIGHT; y++)
    for (gsize x = 0; x < CS9711_SENSOR_WIDTH; x++)
      fake.expected[(y / 2) * CS9711_WIDTH + x * 2 + y % 2] =
        fake.frame[y * CS9711_SENSOR_WIDTH + x];
}

static void
fake_reset (gboolean acl16)
{
  FakeReply *reply;

  g_assert_null (fake.held);
  g_assert_cmpuint (fake.pending, ==, 0);

  while ((reply = g_queue_pop_head (&fake.replies)))
    fake_reply_free (reply);
  g_clear_pointer (&fake.sent, g_array_unref);
  g_clear_pointer (&fake.queued, g_byte_array_unref);
  g_clear_pointer (&fake.frame, g_free);
  g_clear_pointer (&fake.expected, g_free);
  g_clear_error (&fake.release_error);
  g_clear_error (&fake.fail_send_error);
  g_clear_object (&test_cancellable);

  memset (&fake, 0, sizeof (fake));
  g_queue_init (&fake.replies);
  fake.acl16 = acl16;
  fake.sent = g_array_new (FALSE, FALSE, sizeof (guint8));
  fake.queued = g_byte_array_new ();
  fake.claimed = -1;
  test_cancellable = g_cancellable_new ();

  if (acl16)
    fake_set_acl16_frame (1, TRUE);
  else
    fake_set_legacy_frame (1);
}

/************************** Device helpers ************************************/

typedef FpDeviceCs9711Class TestCs9711LegacyClass;
typedef struct
{
  FpDeviceCs9711 parent;
} TestCs9711Legacy;

typedef FpDeviceCs9711Class TestCs9711Acl16Class;
typedef struct
{
  FpDeviceCs9711Acl16 parent;
} TestCs9711Acl16;

static GType test_cs9711_legacy_get_type (void);
static GType test_cs9711_acl16_get_type (void);
G_DEFINE_TYPE (TestCs9711Legacy, test_cs9711_legacy, fpi_device_cs9711_get_type ())
G_DEFINE_TYPE (TestCs9711Acl16, test_cs9711_acl16, fpi_device_cs9711_acl16_get_type ())

static void
test_cs9711_legacy_init (TestCs9711Legacy *self)
{
}

static void
test_cs9711_legacy_class_init (TestCs9711LegacyClass *klass)
{
  /* Skip GUsb; the scripted transport replaces all USB I/O. */
  FP_DEVICE_CLASS (klass)->type = FP_DEVICE_TYPE_VIRTUAL;
}

static void
test_cs9711_acl16_init (TestCs9711Acl16 *self)
{
}

static void
test_cs9711_acl16_class_init (TestCs9711Acl16Class *klass)
{
  FP_DEVICE_CLASS (klass)->type = FP_DEVICE_TYPE_VIRTUAL;
}

static void
on_finger_status (FpDevice *dev, GParamSpec *pspec, gpointer user_data)
{
  FpFingerStatusFlags status = fp_device_get_finger_status (dev);
  gboolean waiting = (status & FP_FINGER_STATUS_NEEDED) &&
                     !(status & FP_FINGER_STATUS_PRESENT);
  gboolean present = (status & FP_FINGER_STATUS_PRESENT) != 0;

  /* Acl16: "finger needed" must mean the scan command has been sent and a
   * read is waiting for the frame. */
  if (fake.acl16 && waiting && !fake.was_waiting)
    {
      g_assert_nonnull (fake.held);
      g_assert_cmpuint (fake_last_sent (), ==, CS9711_FP_CMD_TYPE_SCAN);
      fake.armed_reports++;
    }
  if (present && !fake.was_present)
    {
      fake.present_reports++;
      if (fake.on_present)
        fake.on_present (fake.present_reports);
    }

  fake.was_waiting = waiting;
  fake.was_present = present;
}

static gboolean
watchdog_cb (gpointer user_data)
{
  g_error ("Test did not finish in time; an operation hangs");
  return G_SOURCE_REMOVE;
}

static FpDevice *
open_device (void)
{
  g_autoptr(GError) error = NULL;
  FpDevice *dev;

  if (fake.acl16)
    dev = g_object_new (test_cs9711_acl16_get_type (),
                        "fpi-driver-data", (guint64) CS9711_ACL16_DRIVER_DATA, NULL);
  else
    dev = g_object_new (test_cs9711_legacy_get_type (), NULL);

  g_signal_connect (dev, "notify::finger-status", G_CALLBACK (on_finger_status), NULL);

  g_assert_true (fp_device_open_sync (dev, NULL, &error));
  g_assert_no_error (error);
  g_assert_cmpint (fake.claimed, ==, fake.acl16 ? 1 : 0);

  return dev;
}

static void
close_device (FpDevice *dev)
{
  g_autoptr(GError) error = NULL;

  g_assert_true (fp_device_close_sync (dev, NULL, &error));
  g_assert_no_error (error);
  g_assert_cmpint (fake.claimed, ==, -1);
  g_assert_cmpuint (fake.pending, ==, 0);
  g_assert_null (FPI_DEVICE_CS9711 (dev)->cancellable);
  g_object_unref (dev);
}

/* Drain idle callbacks left behind by a completed action. */
static void
flush_main_context (void)
{
  while (g_main_context_iteration (NULL, FALSE))
    ;
}

static FpImage *
capture (FpDevice *dev, GError **error)
{
  FpImage *image;

  g_clear_object (&test_cancellable);
  test_cancellable = g_cancellable_new ();
  image = fp_device_capture_sync (dev, TRUE, test_cancellable, error);
  flush_main_context ();
  g_assert_cmpuint (fake.pending, ==, 0);
  g_assert_null (fake.held);

  return image;
}

/* Commands sent since @from, as a string such as "24" for SCAN, RESET. */
static gchar *
sent_since (guint from)
{
  GString *s = g_string_new (NULL);

  for (guint i = from; i < fake.sent->len; i++)
    g_string_append_printf (s, "%x", g_array_index (fake.sent, guint8, i));
  return g_string_free (s, FALSE);
}

static void
assert_capture_matches_frame (FpDevice *dev)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(FpImage) image = NULL;
  const guchar *data;
  gsize len;

  fake.on_hold = hook_touch;
  image = capture (dev, &error);
  g_assert_no_error (error);
  g_assert_nonnull (image);

  data = fp_image_get_data (image, &len);
  g_assert_cmpuint (len, ==, fake.expected_len);
  g_assert_cmpmem (data, len, fake.expected, fake.expected_len);
}

static FpPrint *
enroll (FpDevice *dev, guint32 seed, GError **error)
{
  FpPrint *template = fp_print_new (dev);
  FpPrint *print;

  fp_print_set_finger (template, FP_FINGER_RIGHT_INDEX);
  fp_print_set_username (template, "cs9711-test");
  fake_set_acl16_frame (seed, TRUE);
  fake.on_hold = hook_touch;

  g_clear_object (&test_cancellable);
  test_cancellable = g_cancellable_new ();
  print = fp_device_enroll_sync (dev, template, test_cancellable, NULL, NULL, error);
  flush_main_context ();
  g_assert_cmpuint (fake.pending, ==, 0);

  return print;
}

static gboolean
verify (FpDevice *dev, FpPrint *template, guint32 seed, GError **error)
{
  gboolean match = FALSE;

  fake_set_acl16_frame (seed, FALSE);
  g_clear_object (&test_cancellable);
  test_cancellable = g_cancellable_new ();
  fp_device_verify_sync (dev, template, test_cancellable, NULL, NULL, &match, NULL, error);
  flush_main_context ();
  g_assert_cmpuint (fake.pending, ==, 0);
  g_assert_null (fake.held);

  return match;
}

/************************** Tests *********************************************/

static void
test_classify (void)
{
  g_autofree guint8 *buf = g_malloc0 (CS9711_ACL16_WIRE_FRAME);
  gsize offset = 99;

  /* A whole wire frame */
  memset (buf, 0x55, CS9711_FP_CMD_LEN_1);
  g_assert_cmpint (cs9711_acl16_classify (buf, CS9711_ACL16_WIRE_FRAME, &offset), ==, CS9711_ACL16_PACKET_IMAGE);
  g_assert_cmpuint (offset, ==, 0);

  /* Echo followed by the start of a frame; its last 8 bytes are queued. */
  memcpy (buf, echo_scan, sizeof (echo_scan));
  g_assert_cmpint (cs9711_acl16_classify (buf, CS9711_ACL16_WIRE_FRAME, &offset), ==, CS9711_ACL16_PACKET_IMAGE);
  g_assert_cmpuint (offset, ==, CS9711_FP_CMD_LEN_1);

  /* Pixels that only resemble an echo (ea x . . . . x ea) are pixels. */
  memcpy (buf, (const guint8[]) { 0xea, 0xea, 0xe9, 0xeb, 0xea, 0xe8, 0xea, 0xea }, 8);
  g_assert_cmpint (cs9711_acl16_classify (buf, CS9711_ACL16_WIRE_FRAME, &offset), ==, CS9711_ACL16_PACKET_IMAGE);
  g_assert_cmpuint (offset, ==, 0);

  /* Frames that ended early are rejected, also those that would hold the
   * 112x73 crop: the 8192 bytes earlier releases read, the bare crop, and
   * one byte short of a frame. */
  memset (buf, 0x55, CS9711_FP_CMD_LEN_1);
  g_assert_cmpint (cs9711_acl16_classify (buf, 8192, &offset), ==, CS9711_ACL16_PACKET_SHORT);
  g_assert_cmpint (cs9711_acl16_classify (buf, CS9711_ACL16_IMAGE, &offset), ==, CS9711_ACL16_PACKET_SHORT);
  g_assert_cmpint (cs9711_acl16_classify (buf, CS9711_ACL16_WIRE_FRAME - 1, &offset), ==, CS9711_ACL16_PACKET_SHORT);
  g_assert_cmpuint (offset, ==, 0);

  /* Echo of the scan command alone */
  g_assert_cmpint (cs9711_acl16_classify (echo_scan, sizeof (echo_scan), &offset), ==, CS9711_ACL16_PACKET_ECHO);

  /* Echo of another command, or random short data */
  g_assert_cmpint (cs9711_acl16_classify (acl16_init_reply, sizeof (acl16_init_reply), &offset), ==, CS9711_ACL16_PACKET_SHORT);
  g_assert_cmpint (cs9711_acl16_classify (buf + 100, 0, &offset), ==, CS9711_ACL16_PACKET_SHORT);
  g_assert_cmpint (cs9711_acl16_classify (buf + 100, 512, &offset), ==, CS9711_ACL16_PACKET_SHORT);
}

static void
test_acl16_capture (void)
{
  g_autofree gchar *cmds = NULL;
  FpDevice *dev;

  fake_reset (TRUE);
  dev = open_device ();

  /* The first activation after open stops whatever the sensor was doing
   * and empties the pipe. Then: frame preceded by the scan command echo,
   * then a plain frame. No init command. */
  fake_set_acl16_frame (7, TRUE);
  assert_capture_matches_frame (dev);
  g_assert_cmpuint (fake.tail_reads, ==, 1);
  g_assert_cmpuint (fake.queued->len, ==, 0);
  fake_set_acl16_frame (8, FALSE);
  assert_capture_matches_frame (dev);

  /* RESET, then SCAN and RESET per frame. The second action reuses the
   * clean pipe: no reset and no drain before its scan. */
  cmds = sent_since (0);
  g_assert_cmpstr (cmds, ==, "24242");
  g_assert_cmpuint (fake.drain_reads, ==, 1);
  g_assert_cmpuint (fake.tail_reads, ==, 1);
  g_assert_cmpuint (fake.armed_reports, ==, 2);
  g_assert_cmpuint (fake.present_reports, ==, 2);

  close_device (dev);
}

static void
test_acl16_enroll_verify (void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(FpPrint) enrolled = NULL;
  g_autoptr(FpPrint) restored = NULL;
  g_autofree guchar *data = NULL;
  gsize len = 0;
  FpDevice *dev;

  fake_reset (TRUE);
  dev = open_device ();

  enrolled = enroll (dev, 21, &error);
  g_assert_no_error (error);
  g_assert_nonnull (enrolled);
  g_assert_cmpuint (enrolled->prints->len, ==, 15);
  /* One reset after open, then one armed scan and one reset per stage;
   * the drain runs once. */
  g_assert_cmpuint (fake_count_sent (CS9711_FP_CMD_TYPE_SCAN), ==, 15);
  g_assert_cmpuint (fake_count_sent (CS9711_FP_CMD_TYPE_RESET), ==, 16);
  g_assert_cmpuint (fake.drain_reads, ==, 1);
  g_assert_cmpuint (fake.armed_reports, ==, 15);

  /* Same texture matches, a different one does not. */
  g_assert_true (verify (dev, enrolled, 21, &error));
  g_assert_no_error (error);
  g_assert_false (verify (dev, enrolled, 22, &error));
  g_assert_no_error (error);

  /* A serialized template behaves the same. */
  g_assert_true (fp_print_serialize (enrolled, &data, &len, &error));
  g_assert_no_error (error);
  restored = fp_print_deserialize (data, len, &error);
  g_assert_no_error (error);
  g_assert_true (verify (dev, restored, 21, &error));
  g_assert_no_error (error);
  g_assert_false (verify (dev, restored, 23, &error));
  g_assert_no_error (error);

  close_device (dev);
}

static void
test_acl16_cancel_waiting (void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(FpImage) image = NULL;
  g_autofree gchar *cmds = NULL;
  FpDevice *dev;
  guint sent;

  fake_reset (TRUE);
  dev = open_device ();

  fake.on_hold = hook_cancel;
  image = capture (dev, &error);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_assert_null (image);
  g_assert_null (FPI_DEVICE_CS9711 (dev)->cancellable);
  g_assert_cmpuint (fp_device_get_finger_status (dev), ==, FP_FINGER_STATUS_NONE);

  /* The device stays usable. The cancelled scan may still be armed, so
   * the next activation stops it and drains the pipe first. */
  sent = fake.sent->len;
  assert_capture_matches_frame (dev);
  cmds = sent_since (sent);
  g_assert_cmpstr (cmds, ==, "242");
  g_assert_cmpuint (fake.drain_reads, ==, 2);
  close_device (dev);
}

static void
test_acl16_stale_frame_after_cancel (void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(FpImage) image = NULL;
  g_autofree guint8 *stale = NULL;
  FpDevice *dev;

  fake_reset (TRUE);
  dev = open_device ();

  fake.on_hold = hook_cancel;
  image = capture (dev, &error);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_clear_error (&error);

  /* Someone touched the sensor that was still armed: a whole frame waits
   * on the pipe. It must be discarded, not returned for the next action. */
  fake_set_acl16_frame (71, FALSE);
  stale = g_memdup2 (fake.frame, fake.frame_len);
  g_byte_array_append (fake.queued, stale, CS9711_ACL16_WIRE_FRAME);

  fake_set_acl16_frame (72, FALSE);
  assert_capture_matches_frame (dev);
  g_assert_cmpuint (fake.drained_bytes, ==, CS9711_ACL16_WIRE_FRAME);
  g_assert_cmpuint (fake.queued->len, ==, 0);

  close_device (dev);
}

static void
hook_cancel_on_first_command (guint8 cmd)
{
  if (fake.sent->len == 1)
    g_cancellable_cancel (test_cancellable);
}

static void
test_acl16_cancel_activation (void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(FpImage) image = NULL;
  FpDevice *dev;

  fake_reset (TRUE);
  dev = open_device ();

  /* Cancelled right after the activation's reset. */
  fake.on_send = hook_cancel_on_first_command;
  image = capture (dev, &error);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_assert_cmpuint (fake_count_sent (CS9711_FP_CMD_TYPE_SCAN), ==, 0);

  fake.on_send = NULL;
  assert_capture_matches_frame (dev);
  close_device (dev);
}

static void
test_acl16_cancel_drain (void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(FpImage) image = NULL;
  FpDevice *dev;

  fake_reset (TRUE);
  dev = open_device ();

  /* The activation's drain read is still pending when the caller cancels;
   * it is reaped and no scan is armed. */
  g_queue_push_tail (&fake.replies, fake_reply_new (FAKE_HOLD, NULL, 0));
  fake.on_hold = hook_cancel;
  image = capture (dev, &error);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_assert_null (image);
  g_assert_cmpuint (fake_count_sent (CS9711_FP_CMD_TYPE_SCAN), ==, 0);

  assert_capture_matches_frame (dev);
  close_device (dev);
}

static void
hook_cancel_on_second_frame (guint count)
{
  /* The second stage's frame arrived and was reported, but the reset and
   * the image submission are still to come. */
  if (count == 2)
    g_cancellable_cancel (test_cancellable);
}

static void
test_acl16_cancel_enroll_stage (void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(FpPrint) print = NULL;
  g_autofree gchar *cmds = NULL;
  FpDevice *dev;
  guint sent;

  fake_reset (TRUE);
  dev = open_device ();

  fake.on_present = hook_cancel_on_second_frame;
  print = enroll (dev, 31, &error);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_assert_null (print);
  /* Reset after open, stage 1, stage 2 stopped before its reset. */
  cmds = sent_since (0);
  g_assert_cmpstr (cmds, ==, "2424");
  g_assert_null (FPI_DEVICE_CS9711 (dev)->scan_ssm);
  g_clear_pointer (&cmds, g_free);

  /* The second scan was not stopped by its reset, so the next activation
   * resets and drains first. */
  fake.on_present = NULL;
  sent = fake.sent->len;
  assert_capture_matches_frame (dev);
  cmds = sent_since (sent);
  g_assert_cmpstr (cmds, ==, "242");
  close_device (dev);
}

static void
hook_touch_and_cancel (guint count)
{
  /* The frame completes but the caller cancels before it is processed. */
  fake_touch ();
  g_cancellable_cancel (test_cancellable);
}

static void
test_acl16_cancel_races_frame (void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(FpImage) image = NULL;
  g_autofree gchar *cmds = NULL;
  FpDevice *dev;

  fake_reset (TRUE);
  dev = open_device ();

  fake.on_hold = hook_touch_and_cancel;
  image = capture (dev, &error);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_assert_null (image);
  /* The aborted scan must not send the post-scan command or submit; the
   * only reset is the one of the activation. */
  cmds = sent_since (0);
  g_assert_cmpstr (cmds, ==, "24");
  g_assert_cmpuint (fake.present_reports, ==, 0);
  g_clear_pointer (&cmds, g_free);

  /* That scan was not stopped: reset and drain before the next one. */
  assert_capture_matches_frame (dev);
  cmds = sent_since (0);
  g_assert_cmpstr (cmds, ==, "24242");
  close_device (dev);
}

static void
test_acl16_scan_timeout (void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(FpImage) image = NULL;
  FpDevice *dev;
  gint64 start;

  fake_reset (TRUE);
  dev = open_device ();

  /* Nobody touches the sensor. */
  fake.on_hold = NULL;
  start = g_get_monotonic_time ();
  image = capture (dev, &error);
  g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_GENERAL);
  g_assert_nonnull (strstr (error->message, "No finger contact"));
  g_assert_cmpint (g_get_monotonic_time () - start, >=, CS9711_ACL16_SCAN_TIMEOUT * 1000);

  assert_capture_matches_frame (dev);
  close_device (dev);
}

static void
hook_unplug (guint count)
{
  fake_fail_held (g_error_new_literal (G_USB_DEVICE_ERROR, G_USB_DEVICE_ERROR_NO_DEVICE,
                                       "fake device gone"));
}

static void
test_acl16_unplug (void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(FpImage) image = NULL;
  FpDevice *dev;

  fake_reset (TRUE);
  dev = open_device ();

  g_test_expect_message (TEST_DOMAIN, G_LOG_LEVEL_WARNING, "*Acl16 scan read failed*");
  fake.on_hold = hook_unplug;
  image = capture (dev, &error);
  g_test_assert_expected_messages ();
  g_assert_error (error, G_USB_DEVICE_ERROR, G_USB_DEVICE_ERROR_NO_DEVICE);

  /* Releasing a removed device's interface fails; close still succeeds. */
  fake.release_error = g_error_new_literal (G_USB_DEVICE_ERROR, G_USB_DEVICE_ERROR_NO_DEVICE,
                                            "fake device gone");
  close_device (dev);
}

static void
test_acl16_send_failure (void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(FpImage) image = NULL;
  FpDevice *dev;

  fake_reset (TRUE);
  dev = open_device ();

  /* The scan command fails after the frame read was submitted. */
  fake.fail_send_cmd = CS9711_FP_CMD_TYPE_SCAN;
  fake.fail_send_error = g_error_new_literal (G_USB_DEVICE_ERROR, G_USB_DEVICE_ERROR_IO,
                                              "fake write error");
  g_test_expect_message (TEST_DOMAIN, G_LOG_LEVEL_WARNING, "*Error while sending command 0x4*");
  fake.on_hold = NULL;
  image = capture (dev, &error);
  g_test_assert_expected_messages ();
  g_assert_error (error, G_USB_DEVICE_ERROR, G_USB_DEVICE_ERROR_IO);

  assert_capture_matches_frame (dev);
  close_device (dev);
}

static void
hook_echo_then_frame (guint count)
{
  if (count == 1)
    fake_touch_with (echo_scan, sizeof (echo_scan));
  else
    fake_touch ();
}

static void
test_acl16_echo_then_frame (void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(FpImage) image = NULL;
  FpDevice *dev;

  fake_reset (TRUE);
  dev = open_device ();

  fake.on_hold = hook_echo_then_frame;
  image = capture (dev, &error);
  g_assert_no_error (error);
  g_assert_nonnull (image);
  g_assert_cmpmem (fp_image_get_data (image, NULL), CS9711_ACL16_IMAGE, fake.expected, CS9711_ACL16_IMAGE);
  /* An acknowledgement keeps the scan armed; it is not sent again. */
  g_assert_cmpuint (fake.holds, ==, 2);
  g_assert_cmpuint (fake_count_sent (CS9711_FP_CMD_TYPE_SCAN), ==, 1);

  close_device (dev);
}

static const guint8 junk[100] = { 0x11, 0x22, 0x33 };

static void
hook_short_then_frame (guint count)
{
  if (count == 1)
    fake_touch_with (junk, sizeof (junk));
  else
    fake_touch ();
}

static void
hook_always_short (guint count)
{
  fake_touch_with (junk, sizeof (junk));
}

static void
test_acl16_short_packets (void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(FpImage) image = NULL;
  g_autoptr(FpImage) none = NULL;
  g_autofree gchar *cmds = NULL;
  FpDevice *dev;

  fake_reset (TRUE);
  dev = open_device ();

  /* Unexpected short data: stop the scan, drain and arm again. */
  fake.on_hold = hook_short_then_frame;
  image = capture (dev, &error);
  g_assert_no_error (error);
  g_assert_nonnull (image);
  cmds = sent_since (0);
  g_assert_cmpstr (cmds, ==, "24242");
  g_assert_cmpuint (fake.drain_reads, ==, 2);
  g_assert_cmpuint (fake.present_reports, ==, 1);

  /* Re-arming is bounded. */
  fake.on_hold = hook_always_short;
  none = capture (dev, &error);
  g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO);
  g_assert_null (none);
  g_assert_cmpuint (fake_count_sent (CS9711_FP_CMD_TYPE_SCAN), ==, 2 + 1 + CS9711_ACL16_MAX_REARMS);
  g_assert_cmpuint (fake.present_reports, ==, 1);

  close_device (dev);
}

static void
hook_truncated_then_frame (guint count)
{
  /* First a frame cut where earlier releases assumed it ended. */
  if (count == 1)
    fake_touch_with (fake.frame, 8192);
  else
    fake_touch ();
}

static void
test_acl16_truncated_frame (void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(FpImage) image = NULL;
  FpDevice *dev;

  fake_reset (TRUE);
  dev = open_device ();

  /* The short frame is discarded, never submitted; the sensor is armed
   * again and the next whole frame is used. */
  fake_set_acl16_frame (81, FALSE);
  fake.on_hold = hook_truncated_then_frame;
  image = capture (dev, &error);
  g_assert_no_error (error);
  g_assert_nonnull (image);
  g_assert_cmpmem (fp_image_get_data (image, NULL), CS9711_ACL16_IMAGE, fake.expected, CS9711_ACL16_IMAGE);
  g_assert_cmpuint (fake_count_sent (CS9711_FP_CMD_TYPE_SCAN), ==, 2);
  g_assert_cmpuint (fake.present_reports, ==, 1);

  close_device (dev);
}

static void
hook_echo_frame_without_tail (guint count)
{
  /* The read fills up, but the frame's last bytes never arrive. */
  fake_touch_with (fake.frame, CS9711_ACL16_WIRE_FRAME);
}

static void
test_acl16_frame_tail_missing (void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(FpImage) image = NULL;
  g_autofree gchar *cmds = NULL;
  FpDevice *dev;
  guint sent;

  fake_reset (TRUE);
  dev = open_device ();

  fake_set_acl16_frame (82, TRUE);
  fake.on_hold = hook_echo_frame_without_tail;
  g_test_expect_message (TEST_DOMAIN, G_LOG_LEVEL_WARNING, "*Acl16 frame incomplete*");
  image = capture (dev, &error);
  g_test_assert_expected_messages ();
  g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO);
  g_assert_null (image);
  g_assert_cmpuint (fake.tail_reads, ==, 1);
  /* An incomplete frame is not reported as finger contact. */
  g_assert_cmpuint (fake.present_reports, ==, 0);
  g_clear_error (&error);

  /* The pipe state is unknown now: reset and drain before the next scan. */
  sent = fake.sent->len;
  fake_set_acl16_frame (83, FALSE);
  assert_capture_matches_frame (dev);
  cmds = sent_since (sent);
  g_assert_cmpstr (cmds, ==, "242");

  close_device (dev);
}

static void
test_acl16_echo_like_pixels (void)
{
  static const guint8 pattern[] = { 0xea, 0x04, 0x11, 0x22, 0x33, 0x44, 0x04, 0xea };
  FpDevice *dev;

  fake_reset (TRUE);
  dev = open_device ();

  /* A frame whose first pixels look like a command echo, but not like the
   * exact scan echo: they are pixels, no tail is expected. */
  fake_set_acl16_frame (84, FALSE);
  memcpy (fake.frame, pattern, sizeof (pattern));
  memcpy (fake.expected, pattern, sizeof (pattern));
  assert_capture_matches_frame (dev);
  g_assert_cmpuint (fake.tail_reads, ==, 0);
  g_assert_cmpuint (fake.present_reports, ==, 1);

  close_device (dev);
}

static void
test_acl16_drain_never_quiet (void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(FpImage) image = NULL;
  g_autofree gchar *cmds = NULL;
  FpDevice *dev;

  fake_reset (TRUE);
  dev = open_device ();

  /* After the reset the pipe keeps delivering data: the activation fails
   * instead of trusting the pipe, and nothing is armed. */
  for (guint i = 0; i < CS9711_ACL16_DRAIN_BUDGET + 1; i++)
    g_byte_array_append (fake.queued, fake.frame, CS9711_ACL16_WIRE_FRAME);
  image = capture (dev, &error);
  g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO);
  g_assert_null (image);
  g_assert_cmpuint (fake_count_sent (CS9711_FP_CMD_TYPE_SCAN), ==, 0);
  g_assert_false (FPI_DEVICE_CS9711 (dev)->acl16_pipe_clean);

  /* Once it is quiet, the next activation resets and drains again. */
  assert_capture_matches_frame (dev);
  cmds = sent_since (0);
  g_assert_cmpstr (cmds, ==, "2242");

  close_device (dev);
}

static void
test_acl16_open_close_cycles (void)
{
  fake_reset (TRUE);

  for (guint i = 0; i < 3; i++)
    {
      FpDevice *dev = open_device ();

      fake_set_acl16_frame (40 + i, i % 2);
      assert_capture_matches_frame (dev);
      close_device (dev);
    }

  g_assert_cmpuint (fake.claims, ==, 3);
  g_assert_cmpuint (fake.releases, ==, 3);
}

static void
test_acl16_chained_actions (void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(FpImage) image = NULL;
  g_autoptr(FpPrint) enrolled = NULL;
  g_autofree gchar *cmds = NULL;
  FpDevice *dev;

  fake_reset (TRUE);
  dev = open_device ();

  /* What fprintd does: a non-enroll scan (its duplicate check), then
   * enrollment started as soon as the first action completes. */
  fake.on_hold = hook_touch;
  image = capture (dev, &error);
  g_assert_no_error (error);
  g_assert_nonnull (image);
  cmds = sent_since (0);
  g_assert_cmpstr (cmds, ==, "242");
  g_clear_pointer (&cmds, g_free);

  enrolled = enroll (dev, 61, &error);
  g_assert_no_error (error);
  g_assert_nonnull (enrolled);
  g_assert_cmpuint (enrolled->prints->len, ==, 15);
  /* Enrollment reuses the clean pipe: it arms at once, with no init,
   * reset or drain in between, and stops each stage's scan. */
  cmds = sent_since (3);
  g_assert_cmpstr (cmds, ==, "424242424242424242424242424242");
  g_assert_cmpuint (fake.drain_reads, ==, 1);
  g_assert_cmpuint (fake.armed_reports, ==, 16);

  close_device (dev);
}

static void
test_legacy_capture (void)
{
  FpDevice *dev;
  const guint8 expected_cmds[] = {
    CS9711_FP_CMD_TYPE_INIT, CS9711_FP_CMD_TYPE_SCAN, CS9711_FP_CMD_TYPE_RESET,
  };

  fake_reset (FALSE);
  dev = open_device ();

  /* 8000 + 24 bytes, deinterleaved to 68x118. No Acl16 drain reads. */
  assert_capture_matches_frame (dev);
  g_assert_cmpmem (fake.sent->data, fake.sent->len, expected_cmds, sizeof (expected_cmds));
  g_assert_cmpuint (fake.drain_reads, ==, 0);

  close_device (dev);
}

static void
test_legacy_cancel_waiting (void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(FpImage) image = NULL;
  FpDevice *dev;

  fake_reset (FALSE);
  dev = open_device ();

  /* The legacy frame read has no timeout; only cancellation ends it. */
  fake.on_hold = hook_cancel;
  image = capture (dev, &error);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);

  assert_capture_matches_frame (dev);
  close_device (dev);
}

static FpPrint *
sigfm_print (FpDevice *dev, guint32 seed, gboolean with_info)
{
  FpPrint *print = g_object_ref_sink (fp_print_new (dev));

  fpi_print_set_type (print, FPI_PRINT_SIGFM);
  if (with_info)
    {
      g_autofree guint8 *pixels = make_texture (seed, CS9711_ACL16_WIDTH, CS9711_ACL16_HEIGHT);
      SigfmImgInfo *info = sigfm_extract (pixels, CS9711_ACL16_WIDTH, CS9711_ACL16_HEIGHT);

      g_assert_nonnull (info);
      g_ptr_array_add (print->prints, info);
    }
  return print;
}

static void
test_sigfm_fail_closed (void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(FpPrint) template = NULL;
  g_autoptr(FpPrint) same = NULL;
  g_autoptr(FpPrint) other = NULL;
  g_autoptr(FpPrint) empty = NULL;
  g_autoptr(FpPrint) nbis = NULL;
  g_autoptr(FpDevice) dev = NULL;

  fake_reset (TRUE);
  dev = g_object_new (test_cs9711_acl16_get_type (),
                      "fpi-driver-data", (guint64) CS9711_ACL16_DRIVER_DATA, NULL);

  template = sigfm_print (dev, 50, TRUE);
  same = sigfm_print (dev, 50, TRUE);
  other = sigfm_print (dev, 51, TRUE);
  empty = sigfm_print (dev, 0, FALSE);
  nbis = g_object_ref_sink (fp_print_new (dev));
  fpi_print_set_type (nbis, FPI_PRINT_NBIS);

  g_assert_cmpint (fpi_print_sigfm_match (template, same, 40, &error), ==, FPI_MATCH_SUCCESS);
  g_assert_no_error (error);
  g_assert_cmpint (fpi_print_sigfm_match (template, other, 40, &error), ==, FPI_MATCH_FAIL);
  g_assert_no_error (error);

  /* A probe without data is an error, never a match. */
  g_assert_cmpint (fpi_print_sigfm_match (template, empty, 40, &error), ==, FPI_MATCH_ERROR);
  g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_GENERAL);
  g_clear_error (&error);
  g_assert_cmpint (fpi_print_sigfm_match (template, nbis, 40, &error), ==, FPI_MATCH_ERROR);
  g_assert_nonnull (error);
  g_clear_error (&error);
  g_assert_cmpint (fpi_print_sigfm_match (nbis, same, 40, &error), ==, FPI_MATCH_ERROR);
  g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_NOT_SUPPORTED);
  g_clear_error (&error);

  /* An empty template never matches. */
  g_assert_cmpint (fpi_print_sigfm_match (empty, same, 40, &error), ==, FPI_MATCH_FAIL);
  g_assert_no_error (error);
}

typedef void (*TestFunc) (void);

static void
run_with_watchdog (gconstpointer data)
{
  TestFunc func = (TestFunc) data;
  guint id = g_timeout_add_seconds (20, watchdog_cb, NULL);

  func ();
  g_source_remove (id);
  flush_main_context ();
}

static void
add_test (const char *path, TestFunc func)
{
  g_test_add_data_func (path, (gconstpointer) func, run_with_watchdog);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  /* Never dump transfer buffers in tests either. */
  g_unsetenv ("FP_DEBUG_TRANSFER");

  add_test ("/cs9711/acl16/classify", test_classify);
  add_test ("/cs9711/acl16/capture", test_acl16_capture);
  add_test ("/cs9711/acl16/enroll-verify", test_acl16_enroll_verify);
  add_test ("/cs9711/acl16/cancel-waiting", test_acl16_cancel_waiting);
  add_test ("/cs9711/acl16/stale-frame-after-cancel", test_acl16_stale_frame_after_cancel);
  add_test ("/cs9711/acl16/cancel-activation", test_acl16_cancel_activation);
  add_test ("/cs9711/acl16/cancel-drain", test_acl16_cancel_drain);
  add_test ("/cs9711/acl16/cancel-enroll-stage", test_acl16_cancel_enroll_stage);
  add_test ("/cs9711/acl16/cancel-races-frame", test_acl16_cancel_races_frame);
  add_test ("/cs9711/acl16/scan-timeout", test_acl16_scan_timeout);
  add_test ("/cs9711/acl16/unplug", test_acl16_unplug);
  add_test ("/cs9711/acl16/send-failure", test_acl16_send_failure);
  add_test ("/cs9711/acl16/echo-then-frame", test_acl16_echo_then_frame);
  add_test ("/cs9711/acl16/short-packets", test_acl16_short_packets);
  add_test ("/cs9711/acl16/truncated-frame", test_acl16_truncated_frame);
  add_test ("/cs9711/acl16/frame-tail-missing", test_acl16_frame_tail_missing);
  add_test ("/cs9711/acl16/echo-like-pixels", test_acl16_echo_like_pixels);
  add_test ("/cs9711/acl16/drain-never-quiet", test_acl16_drain_never_quiet);
  add_test ("/cs9711/acl16/open-close-cycles", test_acl16_open_close_cycles);
  add_test ("/cs9711/acl16/chained-actions", test_acl16_chained_actions);
  add_test ("/cs9711/legacy/capture", test_legacy_capture);
  add_test ("/cs9711/legacy/cancel-waiting", test_legacy_cancel_waiting);
  add_test ("/cs9711/sigfm/fail-closed", test_sigfm_fail_closed);

  return g_test_run ();
}
