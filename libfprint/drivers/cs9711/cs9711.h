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

#pragma once

#include "fpi-usb-transfer.h"
#include "fpi-image-device.h"

#define CS9711_WIDTH 68
#define CS9711_HEIGHT 118

#define CS9711_FRAME_SIZE (CS9711_WIDTH * CS9711_HEIGHT)

/* 2541:2549 sends a plain 112x73 grayscale frame, then 16 pad bytes. */
#define CS9711_ACL16_WIDTH 112
#define CS9711_ACL16_HEIGHT 73
#define CS9711_ACL16_IMAGE (CS9711_ACL16_WIDTH * CS9711_ACL16_HEIGHT)
#define CS9711_IMAGE_BUFFER_SIZE 8192

struct _FpDeviceCs9711
{
  FpImageDevice parent;

  unsigned char image_buffer[CS9711_IMAGE_BUFFER_SIZE];

  /* One activation (enroll, verify, identify or capture session).
   * cancellable is cancelled when the action is cancelled or when the
   * driver aborts; every asynchronous transfer uses it. */
  GCancellable *cancellable;
  GCancellable *action_cancellable;
  gulong        action_cancel_id;

  /* The running capture state machine, NULL when none is running. */
  FpiSsm  *scan_ssm;
  /* Error to report once the pending read of scan_ssm has been reaped. */
  GError  *scan_error;
  gboolean scan_delay_pending;
  gboolean deactivating;

  /* Acl16 leaves extra bulk data queued after a frame. */
  guint8 acl16_drain_budget;
  guint8 acl16_init_retries;
  guint8 acl16_rearms;
  guint8 acl16_echoes;
  int acl16_after_drain;
  /* Monotonic time (us) before which no command may follow the last
   * post-scan reset. Kept across actions and open/close of this object. */
  gint64 acl16_reset_deadline;
};

G_DECLARE_FINAL_TYPE (FpDeviceCs9711, fpi_device_cs9711, FPI, DEVICE_CS9711, FpImageDevice)
