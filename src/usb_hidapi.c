/*
 * avrdude - A Downloader/Uploader for AVR device programmers
 * Copyright (C) 2016 Joerg Wunsch
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

/* $Id$ */

/*
 * USB interface via libhidapi for avrdude.
 */

#include "ac_cfg.h"
#if defined(HAVE_LIBHIDAPI)


#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/time.h>
#include <wchar.h>

#include <hidapi/hidapi.h>

#include "avrdude.h"
#include "libavrdude.h"

#include "usbdevs.h"

/*
 * The "baud" parameter is meaningless for USB devices, so we reuse it
 * to pass the desired USB device ID.
 */
static int usbhid_open(const char *port, union pinfo pinfo, union filedescriptor *fd) {
  hid_device *dev;
  char *serno, *cp2;
  size_t x;
  unsigned char usbbuf[USBDEV_MAX_XFER_3 + 1];

  if (fd->usb.max_xfer == 0)
    fd->usb.max_xfer = USBDEV_MAX_XFER_3;

  /*
   * The syntax for usb devices is defined as:
   *
   * -P usb[:serialnumber]
   *
   * See if we've got a serial number passed here.  The serial number
   * might contain colons which we remove below, and we compare it
   * right-to-left, so only the least significant nibbles need to be
   * specified.
   */
  if ((serno = strchr(port, ':')) != NULL) {
    /* First, drop all colons there if any */
    cp2 = ++serno;

    while ((cp2 = strchr(cp2, ':')) != NULL) {
      x = strlen(cp2) - 1;
      memmove(cp2, cp2 + 1, x);
      cp2[x] = '\0';
    }

    if (strlen(serno) > 12) {
      pmsg_error("invalid serial number %s\n", serno);
      return -1;
    }

    wchar_t wserno[15];
    mbstowcs(wserno, serno, 15);
    size_t serlen = strlen(serno);

    /*
     * Now, try finding all devices matching VID:PID, and compare
     * their serial numbers against the requested one.
     */
    struct hid_device_info *list, *walk;
    list = hid_enumerate(pinfo.usbinfo.vid, pinfo.usbinfo.pid);
    if (list == NULL) {
      pmsg_error("No USB HID devices found\n");
      return -1;
    }

    walk = list;
    while (walk) {
      pmsg_notice("usbhid_open(): found %ls, serno: %ls\n", walk->product_string, walk->serial_number);
      size_t slen = wcslen(walk->serial_number);
      if (slen >= serlen && wcscmp(walk->serial_number + slen - serlen, wserno) == 0)
      {
        /* Found matching serial number */
        break;
      }
      pmsg_debug("usbhid_open(): serial number does not match\n");
      walk = walk->next;
    }
    if (walk == NULL) {
      pmsg_error("no matching device found\n");
      hid_free_enumeration(list);
      return -1;
    }
    pmsg_debug("usbhid_open(): opening path %s\n", walk->path);
    dev = hid_open_path(walk->path);
    hid_free_enumeration(list);
    if (dev == NULL) {
      pmsg_error("found device, but hid_open_path() failed\n");
      return -1;
    }
  } else {
    dev = hid_open(pinfo.usbinfo.vid, pinfo.usbinfo.pid, NULL);
    if (dev == NULL)
    {
      pmsg_warning("USB device with VID: 0x%04x and PID: 0x%04x not found\n",
        pinfo.usbinfo.vid, pinfo.usbinfo.pid);
      return -1;
    }
  }

  // Store USB serial number to usbsn string
  wchar_t sn[256];
  if (hid_get_serial_number_string(dev, sn, sizeof(sn)/sizeof(*sn)) == 0) {
    size_t n = wcstombs(NULL, sn, 0) + 1;
    if (n) {
      char *cn = cfg_malloc(__func__, n);
      if (wcstombs(cn, sn, n) != (size_t) -1)
        if(serdev)
          serdev->usbsn = cache_string(cn);
      free(cn);
    }
  }

  fd->usb.handle = dev;

  /*
   * Try finding out the endpoint size.  Alas, libhidapi doesn't
   * provide us with an API function for that, nor for the report
   * descriptor (which also contains that information).
   *
   * Since the Atmel tools a very picky to only respond to incoming
   * packets that have full size, we need to know whether our device
   * handles 512-byte data (JTAGICE3 in CMSIS-DAP mode, or AtmelICE,
   * both on USB 2.0 connections), or 64-byte data only (both these on
   * USB 1.1 connections, or mEDBG devices).
   *
   * In order to find out, we send a CMSIS-DAP DAP_Info command
   * (0x00), with an ID of 0xFF (get maximum packet size).  In theory,
   * this gets us the desired information, but this suffers from a
   * chicken-and-egg problem: the request must be sent with a
   * full-sized packet lest the ICE won't answer.  Thus, we send a
   * 64-byte packet first, and if we don't get a timely reply,
   * complete that request by sending another 448 bytes, and hope it
   * will eventually reply.
   *
   * Note that libhidapi always requires a report ID as the first
   * byte.  If the target doesn't use report IDs (Atmel targets
   * don't), this first byte must be 0x00.  However, the length must
   * be incremented by one, as the report ID will be omitted by the
   * hidapi library.
   */
  if (pinfo.usbinfo.vid == USB_VENDOR_ATMEL) {
    pmsg_debug("usbhid_open(): probing for max packet size\n");
    memset(usbbuf, 0, sizeof usbbuf);
    usbbuf[0] = 0;         /* no HID reports used */
    usbbuf[1] = 0;         /* DAP_Info */
    usbbuf[2] = 0xFF;      /* get max. packet size */

    hid_write(dev, usbbuf, 65);
    fd->usb.max_xfer = 64; /* first guess */

    memset(usbbuf, 0, sizeof usbbuf);
    int res = hid_read_timeout(dev, usbbuf, 10 /* bytes */, 50 /* milliseconds */);
    if (res == 0) {
      /* No timely response, assume 512 byte size */
      hid_write(dev, usbbuf, (512 - 64) + 1);
      fd->usb.max_xfer = 512;
      res = hid_read_timeout(dev, usbbuf, 10, 50);
    }
    if (res <= 0) {
      pmsg_error("no response from device\n");
      hid_close(dev);
      return -1;
    }
    if (usbbuf[0] != 0 || usbbuf[1] != 2) {
      pmsg_error("unexpected reply to DAP_Info: 0x%02x 0x%02x\n",
        usbbuf[0], usbbuf[1]);
    } else {
      fd->usb.max_xfer = usbbuf[2] + (usbbuf[3] << 8);
      pmsg_debug("usbhid_open(): setting max_xfer from DAP_Info response to %d\n",
        fd->usb.max_xfer);
    }
  }
  if (fd->usb.max_xfer > USBDEV_MAX_XFER_3) {
    pmsg_error("unexpected max size %d, reducing to %d\n",
      fd->usb.max_xfer, USBDEV_MAX_XFER_3);
    fd->usb.max_xfer = USBDEV_MAX_XFER_3;
  }

  return 0;
}

static void usbhid_close(union filedescriptor *fd) {
  hid_device *udev = (hid_device *)fd->usb.handle;

  if (udev == NULL)
    return;

  hid_close(udev);
}


static int usbhid_send(const union filedescriptor *fd, const unsigned char *bp, size_t mlen) {
  hid_device *udev = (hid_device *)fd->usb.handle;
  int rv;
  int i = mlen;
  const unsigned char * p = bp;
  unsigned char usbbuf[USBDEV_MAX_XFER_3 + 1];

  int tx_size;

  if (udev == NULL)
    return -1;

  tx_size = (mlen < USBDEV_MAX_XFER_3)? mlen: USBDEV_MAX_XFER_3;
  usbbuf[0] = 0; /* No report ID used */
  memcpy(usbbuf + 1, bp, tx_size);
  rv = hid_write(udev, usbbuf, tx_size + 1);
  if (rv < 0) {
    pmsg_error("unable to write %d bytes to USB\n", tx_size);
    return -1;
  }
  if (rv != tx_size + 1)
    pmsg_error("short write to USB: %d bytes out of %d written\n", rv, tx_size + 1);

  if (verbose > 4) {
    pmsg_trace2("sent: ");

    while (i) {
      unsigned char c = *p;
      if (isprint(c))
        msg_trace2("%c ", c);
      else
        msg_trace2(". ");
      msg_trace2("[%02x] ", c);

      p++;
      i--;
    }
    msg_trace2("\n");
  }
  return 0;
}

static int usbhid_recv(const union filedescriptor *fd, unsigned char *buf, size_t nbytes) {
  hid_device *udev = (hid_device *)fd->usb.handle;
  int i, rv;
  unsigned char * p = buf;

  if (udev == NULL)
    return -1;

  rv = i = hid_read_timeout(udev, buf, nbytes, 10000);
  if (i < 0)
    pmsg_error("hid_read_timeout(usb, %lu, 10000) failed\n", (unsigned long) nbytes);
  else if ((size_t) i != nbytes)
    pmsg_error("short read, read only %d out of %lu bytes\n", i, (unsigned long) nbytes);

  if (verbose > 4) {
    pmsg_trace2("recv: ");

    while (i) {
      unsigned char c = *p;
      if (isprint(c))
        msg_trace2("%c ", c);
      else
        msg_trace2(". ");
      msg_trace2("[%02x] ", c);

      p++;
      i--;
    }
    msg_trace2("\n");
  }

  return rv;
}

static int usbhid_drain(const union filedescriptor *fd, int display) {
  /*
   * There is not much point in trying to flush any data
   * on an USB endpoint, as the endpoint is supposed to
   * start afresh after being configured from the host.
   *
   * As trying to flush the data here caused strange effects
   * in some situations (see
   * https://savannah.nongnu.org/bugs/index.php?43268 )
   * better avoid it.
   */

  return 0;
}

/*
 * Device descriptor.
 */
struct serial_device usbhid_serdev = {
  .open = usbhid_open,
  .close = usbhid_close,
  .send = usbhid_send,
  .recv = usbhid_recv,
  .drain = usbhid_drain,
  .flags = SERDEV_FL_NONE,
};

#endif  /* HAVE_LIBHIDAPI */
