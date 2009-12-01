/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-*/
/*
 * Copyright (C) 2009 David Zeuthen <david@fubar.dk>
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
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <time.h>

#include <scsi/sg_lib.h>
#include <scsi/sg_cmds.h>

#define LIBUDEV_I_KNOW_THE_API_IS_SUBJECT_TO_CHANGE
#include <libudev.h>

#include <glib.h>

static void
usage (void)
{
  g_printerr ("incorrect usage\n");
}

int
main (int argc,
      char *argv[])
{
  int ret;
  int sg_fd;
  const gchar *device;
  const gchar *sysfs_path;
  struct udev *udev;
  struct udev_device *udevice;
  struct udev_device *udevice_usb_interface;
  struct udev_device *udevice_usb_device;
  gchar *unbind_path;
  gchar *power_level_path;
  gchar *usb_interface_name;
  size_t usb_interface_name_len;
  FILE *f;

  udev = NULL;
  udevice = NULL;
  udevice_usb_interface = NULL;
  udevice_usb_device = NULL;
  usb_interface_name = NULL;
  unbind_path = NULL;
  power_level_path = NULL;

  ret = 1;
  sg_fd = -1;

  if (argc != 3)
    {
      usage ();
      goto out;
    }

  device = argv[1];
  sysfs_path = argv[2];

  sg_fd = sg_cmds_open_device (device, 1 /* read_only */, 1);
  if (sg_fd < 0)
    {
      g_printerr ("Cannot open %s: %m\n", device);
      goto out;
    }

  if (sg_ll_sync_cache_10 (sg_fd, 0, /* sync_nv */
                           0, /* immed */
                           0, /* group */
                           0, /* lba */
                           0, /* count */
                           1, /* noisy */
                           0 /* verbose */
                           ) != 0)
    {
      g_printerr ("Error SYNCHRONIZE CACHE for %s: %m\n", device);
      /* this is not a catastrophe, carry on */
    }

  if (sg_ll_start_stop_unit (sg_fd, 0, /* immed */
                             0, /* pc_mod__fl_num */
                             0, /* power_cond */
                             0, /* noflush__fl */
                             0, /* loej */
                             0, /* start */
                             1, /* noisy */
                             0 /* verbose */
                             ) != 0)
    {
      g_printerr ("Error STOP UNIT for %s: %m\n", device);
      goto out;
    }

  /* OK, close the device */
  sg_cmds_close_device (sg_fd);
  sg_fd = -1;

  /* Now unbind the usb-storage driver from the usb interface */
  udev = udev_new ();
  if (udev == NULL)
    {
      g_printerr ("Error initializing libudev: %m\n");
      goto out;
    }

  udevice = udev_device_new_from_syspath (udev, sysfs_path);
  if (udevice == NULL)
    {
      g_printerr ("No udev device for %s: %m\n", sysfs_path);
      goto out;
    }

  /* unbind the mass storage driver (e.g. usb-storage) */
  udevice_usb_interface = udev_device_get_parent_with_subsystem_devtype (udevice, "usb", "usb_interface");
  if (udevice_usb_interface == NULL)
    {
      g_printerr ("No usb parent interface for %s: %m\n", sysfs_path);
      goto out;
    }

  usb_interface_name = g_path_get_basename (udev_device_get_devpath (udevice_usb_interface));
  usb_interface_name_len = strlen (usb_interface_name);

  unbind_path = g_strdup_printf ("%s/driver/unbind", udev_device_get_syspath (udevice_usb_interface));
  f = fopen (unbind_path, "w");
  if (f == NULL)
    {
      g_printerr ("Cannot open %s for writing: %m\n", unbind_path);
      goto out;
    }
  if (fwrite (usb_interface_name, sizeof(char), usb_interface_name_len, f) < usb_interface_name_len)
    {
      g_printerr ("Error writing %s to %s: %m\n", unbind_path, usb_interface_name);
      fclose (f);
      goto out;
    }
  fclose (f);

  /* If this is the only USB interface on the device, also suspend the
   * USB device to e.g.  make the lights on the device power off.
   *
   * Failing to do so is not an error, the user may not have
   * CONFIG_USB_SUSPEND enabled in their kernel.
   */
  ret = 0;

  udevice_usb_device = udev_device_get_parent_with_subsystem_devtype (udevice, "usb", "usb_device");
  if (udevice_usb_device != NULL)
    {
      const char *bNumInterfaces;
      char *endp;
      int num_interfaces;
      gchar * power_level_path;
      gchar suspend_str[] = "suspend";

      bNumInterfaces = udev_device_get_sysattr_value (udevice_usb_device, "bNumInterfaces");
      num_interfaces = strtol (bNumInterfaces, &endp, 0);
      if (endp != NULL && num_interfaces == 1)
        {
          power_level_path = g_strdup_printf ("%s/power/level", udev_device_get_syspath (udevice_usb_device));
          f = fopen (power_level_path, "w");
          if (f == NULL)
            {
              g_printerr ("Cannot open %s for writing: %m\n", unbind_path);
            }
          else
            {
              if (fwrite (suspend_str, sizeof(char), strlen (suspend_str), f) < strlen (suspend_str))
                {
                  g_printerr ("Error writing %s to %s: %m\n", power_level_path, suspend_str);
                }
              fclose (f);
            }
        }
    }

 out:
  g_free (usb_interface_name);
  g_free (unbind_path);
  g_free (power_level_path);
  if (sg_fd > 0)
    sg_cmds_close_device (sg_fd);
  if (udevice != NULL)
    udev_device_unref (udevice);
  if (udev != NULL)
    udev_unref (udev);
  return ret;
}
