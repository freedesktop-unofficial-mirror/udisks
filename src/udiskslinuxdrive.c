/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2007-2010 David Zeuthen <zeuthen@gmail.com>
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

#include "config.h"
#include <glib/gi18n-lib.h>

#include <sys/types.h>
#include <pwd.h>
#include <grp.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <mntent.h>

#include <glib/gstdio.h>

#include "udiskslogging.h"
#include "udiskslinuxprovider.h"
#include "udiskslinuxdriveobject.h"
#include "udiskslinuxdrive.h"
#include "udiskslinuxblockobject.h"
#include "udisksdaemon.h"
#include "udiskscleanup.h"
#include "udisksdaemonutil.h"

/**
 * SECTION:udiskslinuxdrive
 * @title: UDisksLinuxDrive
 * @short_description: Linux implementation of #UDisksDrive
 *
 * This type provides an implementation of the #UDisksDrive interface
 * on Linux.
 */

typedef struct _UDisksLinuxDriveClass   UDisksLinuxDriveClass;

/**
 * UDisksLinuxDrive:
 *
 * The #UDisksLinuxDrive structure contains only private data and should
 * only be accessed using the provided API.
 */
struct _UDisksLinuxDrive
{
  UDisksDriveSkeleton parent_instance;

  gint64 time_detected;
  gint64 time_media_detected;
  gchar *sort_key;
};

struct _UDisksLinuxDriveClass
{
  UDisksDriveSkeletonClass parent_class;
};

static void drive_iface_init (UDisksDriveIface *iface);

G_DEFINE_TYPE_WITH_CODE (UDisksLinuxDrive, udisks_linux_drive, UDISKS_TYPE_DRIVE_SKELETON,
                         G_IMPLEMENT_INTERFACE (UDISKS_TYPE_DRIVE, drive_iface_init));

/* ---------------------------------------------------------------------------------------------------- */

static void
udisks_linux_drive_finalize (GObject *object)
{
  UDisksLinuxDrive *drive = UDISKS_LINUX_DRIVE (object);

  g_free (drive->sort_key);

  if (G_OBJECT_CLASS (udisks_linux_drive_parent_class)->finalize != NULL)
    G_OBJECT_CLASS (udisks_linux_drive_parent_class)->finalize (object);
}

static void
udisks_linux_drive_init (UDisksLinuxDrive *drive)
{
  g_dbus_interface_skeleton_set_flags (G_DBUS_INTERFACE_SKELETON (drive),
                                       G_DBUS_INTERFACE_SKELETON_FLAGS_HANDLE_METHOD_INVOCATIONS_IN_THREAD);
}

static void
udisks_linux_drive_class_init (UDisksLinuxDriveClass *klass)
{
  GObjectClass *gobject_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->finalize     = udisks_linux_drive_finalize;
}

/**
 * udisks_linux_drive_new:
 *
 * Creates a new #UDisksLinuxDrive instance.
 *
 * Returns: A new #UDisksLinuxDrive. Free with g_object_unref().
 */
UDisksDrive *
udisks_linux_drive_new (void)
{
  return UDISKS_DRIVE (g_object_new (UDISKS_TYPE_LINUX_DRIVE,
                                          NULL));
}

/* ---------------------------------------------------------------------------------------------------- */

static const gchar *
find_iscsi_target (GDBusObjectManagerServer *object_manager,
                   const gchar              *target_name)
{
  const gchar *ret;
  GList *objects;
  GList *l;

  ret = NULL;

  objects = g_dbus_object_manager_get_objects (G_DBUS_OBJECT_MANAGER (object_manager));
  for (l = objects; l != NULL; l = l->next)
    {
      UDisksObjectSkeleton *object = UDISKS_OBJECT_SKELETON (l->data);
      UDisksiSCSITarget *target;

      target = udisks_object_peek_iscsi_target (UDISKS_OBJECT (object));
      if (target == NULL)
        continue;

      if (g_strcmp0 (udisks_iscsi_target_get_name (target), target_name) == 0)
        {
          ret = g_dbus_object_get_object_path (G_DBUS_OBJECT (object));
          goto out;
        }
    }

 out:
  g_list_foreach (objects, (GFunc) g_object_unref, NULL);
  g_list_free (objects);
  return ret;
}

static gboolean
find_iscsi_devices_for_block (GUdevClient  *udev_client,
                              GUdevDevice  *block_device,
                              GUdevDevice **out_session_device,
                              GUdevDevice **out_connection_device)
{
  gchar *s;
  gchar *session_sysfs_path;
  gchar *connection_sysfs_path;
  GDir *session_dir;
  GDir *connection_dir;
  const gchar *name;
  gboolean ret;
  GUdevDevice *session_device;
  GUdevDevice *connection_device;

  ret = FALSE;
  session_device = NULL;
  connection_device = NULL;

  session_dir = NULL;
  connection_dir = NULL;
  s = NULL;
  session_sysfs_path = NULL;
  connection_sysfs_path = NULL;

  /* This is a bit sketchy and includes assumptions about what sysfs
   * currently looks like...
   */

  if (out_session_device != NULL)
    {
      s = g_strdup_printf ("%s/device/../../iscsi_session", g_udev_device_get_sysfs_path (block_device));
      if (!g_file_test (s, G_FILE_TEST_IS_DIR))
        goto out;
      session_dir = g_dir_open (s, 0, NULL);
      if (session_dir == NULL)
        goto out;
      while ((name = g_dir_read_name (session_dir)) != NULL)
        {
          gint session_num;
          if (sscanf (name, "session%d", &session_num) == 1)
            {
              session_sysfs_path = g_strdup_printf ("%s/%s", s, name);
              break;
            }
        }
      if (session_sysfs_path == NULL)
        goto out;
      session_device = g_udev_client_query_by_sysfs_path (udev_client, session_sysfs_path);
      if (session_device == NULL)
        goto out;
    }

  if (out_connection_device != NULL)
    {
      /* here we assume there is only one connection per session... this could end up not being true */
      g_free (s);
      s = g_strdup_printf ("%s/device/../..", g_udev_device_get_sysfs_path (block_device));
      if (!g_file_test (s, G_FILE_TEST_IS_DIR))
        goto out;
      connection_dir = g_dir_open (s, 0, NULL);
      if (connection_dir == NULL)
        goto out;
      while ((name = g_dir_read_name (connection_dir)) != NULL)
        {
          gint connection_num;
          if (sscanf (name, "connection%d", &connection_num) == 1)
            {
              connection_sysfs_path = g_strdup_printf ("%s/%s/iscsi_connection/%s", s, name, name);
              break;
            }
        }
      if (connection_sysfs_path == NULL)
        goto out;
      connection_device = g_udev_client_query_by_sysfs_path (udev_client, connection_sysfs_path);
      if (connection_device == NULL)
        goto out;
    }

  ret = TRUE;

 out:
  g_free (s);
  g_free (session_sysfs_path);
  if (session_dir != NULL)
    g_dir_close (session_dir);
  g_free (connection_sysfs_path);
  if (connection_dir != NULL)
    g_dir_close (connection_dir);

  if (ret)
    {
      if (out_session_device != NULL)
        *out_session_device = session_device;
      else
        g_object_unref (session_device);

      if (out_connection_device != NULL)
        *out_connection_device = connection_device;
      else
        g_object_unref (connection_device);
    }
  else
    {
      if (session_device != NULL)
        g_object_unref (session_device);
      if (connection_device != NULL)
        g_object_unref (connection_device);
    }

  return ret;
}

static void
set_iscsi_target (UDisksLinuxDrive *drive,
                  UDisksDrive      *iface,
                  GUdevDevice      *device,
                  UDisksDaemon     *daemon)
{
  GUdevClient *udev_client;
  GUdevDevice *session_device;
  GUdevDevice *connection_device;

  /* note: @device may vary - it can be any path for drive */
  session_device = NULL;
  connection_device = NULL;

  udisks_drive_set_iscsi_target (iface, "/");

  udev_client = udisks_linux_provider_get_udev_client (udisks_daemon_get_linux_provider (daemon));
  if (find_iscsi_devices_for_block (udev_client,
                                    device,
                                    &session_device,
                                    &connection_device))
    {
      GDBusObjectManagerServer *object_manager;
      const gchar *target_name;
      const gchar *target_object_path;

      target_name = g_udev_device_get_sysfs_attr (session_device, "targetname");
      if (target_name == NULL)
        {
          udisks_warning ("Cannot find iSCSI target name for sysfs path %s",
                          g_udev_device_get_sysfs_path (session_device));
          goto out;
        }

      object_manager = udisks_daemon_get_object_manager (daemon);
      target_object_path = find_iscsi_target (object_manager, target_name);
      if (target_object_path == NULL)
        {
          udisks_warning ("Cannot find iSCSI target object for name `%s'",
                          target_name);
          goto out;
        }
      udisks_drive_set_iscsi_target (iface, target_object_path);

    }
 out:
  if (connection_device != NULL)
    g_object_unref (connection_device);
  if (session_device != NULL)
    g_object_unref (session_device);
}

/* ---------------------------------------------------------------------------------------------------- */

static const struct
{
  const gchar *udev_property;
  const gchar *media_name;
  gboolean force_non_removable;
  gboolean force_removable;
} drive_media_mapping[] =
{
  { "ID_DRIVE_THUMB", "thumb", TRUE, FALSE },
  { "ID_DRIVE_FLASH", "flash", FALSE, TRUE },
  { "ID_DRIVE_FLASH_CF", "flash_cf", FALSE, TRUE },
  { "ID_DRIVE_FLASH_MS", "flash_ms", FALSE, TRUE },
  { "ID_DRIVE_FLASH_SM", "flash_sm", FALSE, TRUE },
  { "ID_DRIVE_FLASH_SD", "flash_sd", FALSE, TRUE },
  { "ID_DRIVE_FLASH_SDHC", "flash_sdhc", FALSE, TRUE },
  { "ID_DRIVE_FLASH_SDXC", "flash_sdxc", FALSE, TRUE },
  { "ID_DRIVE_FLASH_MMC", "flash_mmc", FALSE, TRUE },
  { "ID_DRIVE_FLOPPY", "floppy", FALSE, TRUE },
  { "ID_DRIVE_FLOPPY_ZIP", "floppy_zip", FALSE, TRUE },
  { "ID_DRIVE_FLOPPY_JAZ", "floppy_jaz", FALSE, TRUE },
  { "ID_CDROM", "optical_cd", FALSE, TRUE },
  { "ID_CDROM_CD_R", "optical_cd_r", FALSE, TRUE },
  { "ID_CDROM_CD_RW", "optical_cd_rw", FALSE, TRUE },
  { "ID_CDROM_DVD", "optical_dvd", FALSE, TRUE },
  { "ID_CDROM_DVD_R", "optical_dvd_r", FALSE, TRUE },
  { "ID_CDROM_DVD_RW", "optical_dvd_rw", FALSE, TRUE },
  { "ID_CDROM_DVD_RAM", "optical_dvd_ram", FALSE, TRUE },
  { "ID_CDROM_DVD_PLUS_R", "optical_dvd_plus_r", FALSE, TRUE },
  { "ID_CDROM_DVD_PLUS_RW", "optical_dvd_plus_rw", FALSE, TRUE },
  { "ID_CDROM_DVD_PLUS_R_DL", "optical_dvd_plus_r_dl", FALSE, TRUE },
  { "ID_CDROM_DVD_PLUS_RW_DL", "optical_dvd_plus_rw_dl", FALSE, TRUE },
  { "ID_CDROM_BD", "optical_bd", FALSE, TRUE },
  { "ID_CDROM_BD_R", "optical_bd_r", FALSE, TRUE },
  { "ID_CDROM_BD_RE", "optical_bd_re", FALSE, TRUE },
  { "ID_CDROM_HDDVD", "optical_hddvd", FALSE, TRUE },
  { "ID_CDROM_HDDVD_R", "optical_hddvd_r", FALSE, TRUE },
  { "ID_CDROM_HDDVD_RW", "optical_hddvd_rw", FALSE, TRUE },
  { "ID_CDROM_MO", "optical_mo", FALSE, TRUE },
  { "ID_CDROM_MRW", "optical_mrw", FALSE, TRUE },
  { "ID_CDROM_MRW_W", "optical_mrw_w", FALSE, TRUE },
  { NULL, NULL, FALSE, FALSE }
};

static const struct
{
  const gchar *udev_property;
  const gchar *media_name;
} media_mapping[] =
{
  { "ID_DRIVE_MEDIA_FLASH", "flash" },
  { "ID_DRIVE_MEDIA_FLASH_CF", "flash_cf" },
  { "ID_DRIVE_MEDIA_FLASH_MS", "flash_ms" },
  { "ID_DRIVE_MEDIA_FLASH_SM", "flash_sm" },
  { "ID_DRIVE_MEDIA_FLASH_SD", "flash_sd" },
  { "ID_DRIVE_MEDIA_FLASH_SDHC", "flash_sdhc" },
  { "ID_DRIVE_MEDIA_FLASH_SDXC", "flash_sdxc" },
  { "ID_DRIVE_MEDIA_FLASH_MMC", "flash_mmc" },
  { "ID_DRIVE_MEDIA_FLOPPY", "floppy" },
  { "ID_DRIVE_MEDIA_FLOPPY_ZIP", "floppy_zip" },
  { "ID_DRIVE_MEDIA_FLOPPY_JAZ", "floppy_jaz" },
  { "ID_CDROM_MEDIA_CD", "optical_cd" },
  { "ID_CDROM_MEDIA_CD_R", "optical_cd_r" },
  { "ID_CDROM_MEDIA_CD_RW", "optical_cd_rw" },
  { "ID_CDROM_MEDIA_DVD", "optical_dvd" },
  { "ID_CDROM_MEDIA_DVD_R", "optical_dvd_r" },
  { "ID_CDROM_MEDIA_DVD_RW", "optical_dvd_rw" },
  { "ID_CDROM_MEDIA_DVD_RAM", "optical_dvd_ram" },
  { "ID_CDROM_MEDIA_DVD_PLUS_R", "optical_dvd_plus_r" },
  { "ID_CDROM_MEDIA_DVD_PLUS_RW", "optical_dvd_plus_rw" },
  { "ID_CDROM_MEDIA_DVD_PLUS_R_DL", "optical_dvd_plus_r_dl" },
  { "ID_CDROM_MEDIA_DVD_PLUS_RW_DL", "optical_dvd_plus_rw_dl" },
  { "ID_CDROM_MEDIA_BD", "optical_bd" },
  { "ID_CDROM_MEDIA_BD_R", "optical_bd_r" },
  { "ID_CDROM_MEDIA_BD_RE", "optical_bd_re" },
  { "ID_CDROM_MEDIA_HDDVD", "optical_hddvd" },
  { "ID_CDROM_MEDIA_HDDVD_R", "optical_hddvd_r" },
  { "ID_CDROM_MEDIA_HDDVD_RW", "optical_hddvd_rw" },
  { "ID_CDROM_MEDIA_MO", "optical_mo" },
  { "ID_CDROM_MEDIA_MRW", "optical_mrw" },
  { "ID_CDROM_MEDIA_MRW_W", "optical_mrw_w" },
  { NULL, NULL }
};

static gint
ptr_str_array_compare (const gchar **a,
                       const gchar **b)
{
  return g_strcmp0 (*a, *b);
}

static void
set_media (UDisksDrive      *iface,
           GUdevDevice      *device,
           gboolean          is_pc_floppy_drive)
{
  guint n;
  GPtrArray *media_compat_array;
  const gchar *media_in_drive;
  gboolean is_disc = FALSE;
  gboolean disc_is_blank = FALSE;
  guint disc_session_count = 0;
  guint disc_track_count = 0;
  guint disc_track_count_audio = 0;
  guint disc_track_count_data = 0;
  gboolean force_non_removable = FALSE;
  gboolean force_removable = FALSE;
  gboolean ejectable;
  gboolean removable;

  media_compat_array = g_ptr_array_new ();
  for (n = 0; drive_media_mapping[n].udev_property != NULL; n++)
    {
      if (g_udev_device_get_property_as_boolean (device, drive_media_mapping[n].udev_property))
        {
          g_ptr_array_add (media_compat_array, (gpointer) drive_media_mapping[n].media_name);
          if (drive_media_mapping[n].force_non_removable)
            force_non_removable = TRUE;
          if (drive_media_mapping[n].force_removable)
            force_removable = TRUE;
        }
    }
  g_ptr_array_sort (media_compat_array, (GCompareFunc) ptr_str_array_compare);
  g_ptr_array_add (media_compat_array, NULL);

  removable = ejectable = g_udev_device_get_sysfs_attr_as_boolean (device, "removable");
  if (force_non_removable)
    removable = FALSE;
  if (force_removable)
    removable = TRUE;
  udisks_drive_set_media_removable (iface, removable);
  if (is_pc_floppy_drive)
    ejectable = FALSE;
  udisks_drive_set_ejectable (iface, ejectable);

  media_in_drive = NULL;
  if (udisks_drive_get_media_available (iface))
    {
      for (n = 0; media_mapping[n].udev_property != NULL; n++)
        {
          if (g_udev_device_get_property_as_boolean (device, media_mapping[n].udev_property))
            {
              media_in_drive = media_mapping[n].media_name;
              break;
            }
        }
      /* If the media isn't set (from e.g. udev rules), just pick the first one in media_compat - note
       * that this may be NULL (if we don't know what media is compatible with the drive) which is OK.
       */
      if (media_in_drive == NULL)
        media_in_drive = ((const gchar **) media_compat_array->pdata)[0];
    }
  udisks_drive_set_media_compatibility (iface, (const gchar* const *) media_compat_array->pdata);
  udisks_drive_set_media (iface, media_in_drive);
  g_ptr_array_free (media_compat_array, TRUE);

  if (g_udev_device_get_property_as_boolean (device, "ID_CDROM_MEDIA"))
    {
      const gchar *state;
      is_disc = TRUE;
      state = g_udev_device_get_property (device, "ID_CDROM_MEDIA_STATE");
      if (g_strcmp0 (state, "blank") == 0)
        disc_is_blank = TRUE;
      disc_session_count = g_udev_device_get_property_as_int (device, "ID_CDROM_MEDIA_SESSION_COUNT");
      disc_track_count = g_udev_device_get_property_as_int (device, "ID_CDROM_MEDIA_TRACK_COUNT");
      disc_track_count_audio = g_udev_device_get_property_as_int (device, "ID_CDROM_MEDIA_TRACK_COUNT_AUDIO");
      disc_track_count_data = g_udev_device_get_property_as_int (device, "ID_CDROM_MEDIA_TRACK_COUNT_DATA");
    }
  udisks_drive_set_optical (iface, is_disc);
  udisks_drive_set_optical_blank (iface, disc_is_blank);
  udisks_drive_set_optical_num_sessions (iface, disc_session_count);
  udisks_drive_set_optical_num_tracks (iface, disc_track_count);
  udisks_drive_set_optical_num_audio_tracks (iface, disc_track_count_audio);
  udisks_drive_set_optical_num_data_tracks (iface, disc_track_count_data);
}

static void
set_rotation_rate (UDisksDrive      *iface,
                   GUdevDevice      *device)
{
  gint rate;

  if (!g_udev_device_get_sysfs_attr_as_boolean (device, "queue/rotational"))
    {
      rate = 0;
    }
  else
    {
      rate = -1;
      if (g_udev_device_has_property (device, "ID_ATA_ROTATION_RATE_RPM"))
        rate = g_udev_device_get_property_as_int (device, "ID_ATA_ROTATION_RATE_RPM");
    }
  udisks_drive_set_rotation_rate (iface, rate);
}

static void
set_connection_bus (UDisksDrive      *iface,
                    GUdevDevice      *device)
{
  GUdevDevice *parent;

  /* note: @device may vary - it can be any path for drive */

  udisks_drive_set_connection_bus (iface, "");
  parent = g_udev_device_get_parent_with_subsystem (device, "usb", "usb_interface");
  if (parent != NULL)
    {
      /* TODO: should probably check that it's a storage interface */
      udisks_drive_set_connection_bus (iface, "usb");
      g_object_unref (parent);
      goto out;
    }

  parent = g_udev_device_get_parent_with_subsystem (device, "firewire", NULL);
  if (parent != NULL)
    {
      /* TODO: should probably check that it's a storage interface */
      udisks_drive_set_connection_bus (iface, "ieee1394");
      g_object_unref (parent);
      goto out;
    }

  if (g_str_has_prefix (g_udev_device_get_name (device), "mmcblk"))
    {
      udisks_drive_set_connection_bus (iface, "sdio");
      goto out;
    }

 out:
  ;
}

static void
set_media_time_detected (UDisksLinuxDrive *drive,
                         GUdevDevice      *device,
                         gboolean          is_pc_floppy_drive,
                         gboolean          coldplug)
{
  UDisksDrive *iface = UDISKS_DRIVE (drive);
  gint64 now;

  now = g_get_real_time ();

  /* First, initialize time_detected */
  if (drive->time_detected == 0)
    {
      if (coldplug)
        {
          drive->time_detected = now - g_udev_device_get_usec_since_initialized (device);
        }
      else
        {
          drive->time_detected = now;
        }
    }

  if (!g_udev_device_get_sysfs_attr_as_boolean (device, "removable") || is_pc_floppy_drive)
    {
      drive->time_media_detected = drive->time_detected;
    }
  else
    {
      if (!udisks_drive_get_media_available (iface))
        {
          /* no media currently available */
          drive->time_media_detected = 0;
        }
      else
        {
          /* media currently available */
          if (drive->time_media_detected == 0)
            {
              if (coldplug)
                {
                  drive->time_media_detected = drive->time_detected;
                }
              else
                {
                  drive->time_media_detected = now;
                }
            }
        }
    }

  udisks_drive_set_time_detected (iface, drive->time_detected);
  udisks_drive_set_time_media_detected (iface, drive->time_media_detected);
}

static gchar *
append_fixedup_sd (const gchar *prefix,
                   const gchar *device_name)
{
  guint num_alphas, n;
  GString *str;

  g_return_val_if_fail (g_str_has_prefix (device_name, "sd"), NULL);

  /* make sure sdaa comes after e.g. sdz by inserting up to 5 '_' characters
   * between sd and a in sda...
   */
  for (num_alphas = 0; g_ascii_isalpha (device_name[num_alphas + 2]); num_alphas++)
    ;
  str = g_string_new (prefix);
  g_string_append (str, "sd");
  for (n = 0; n < 5 - num_alphas; n++)
    g_string_append_c (str, '_');

  g_string_append (str, device_name + 2);

  return g_string_free (str, FALSE);
}

/**
 * udisks_linux_drive_update:
 * @drive: A #UDisksLinuxDrive.
 * @object: The enclosing #UDisksLinuxDriveObject instance.
 *
 * Updates the interface.
 */
void
udisks_linux_drive_update (UDisksLinuxDrive       *drive,
                           UDisksLinuxDriveObject *object)
{
  UDisksDrive *iface = UDISKS_DRIVE (drive);
  GUdevDevice *device;
  guint64 size;
  gboolean media_available;
  gboolean media_change_detected;
  gboolean is_pc_floppy_drive = FALSE;
  gboolean removable_hint = FALSE;
  UDisksDaemon *daemon;
  UDisksLinuxProvider *provider;
  gboolean coldplug = FALSE;

  device = udisks_linux_drive_object_get_device (object, TRUE /* get_hw */);
  if (device == NULL)
    goto out;

  if (object != NULL)
    {
      daemon = udisks_linux_drive_object_get_daemon (object);
      provider = udisks_daemon_get_linux_provider (daemon);
      coldplug = udisks_linux_provider_get_coldplug (provider);
    }

  if (g_udev_device_get_property_as_boolean (device, "ID_DRIVE_FLOPPY") ||
      g_str_has_prefix (g_udev_device_get_name (device), "fd"))
    is_pc_floppy_drive = TRUE;

  /* this is the _almost_ the same for both ATA and SCSI devices (cf. udev's ata_id and scsi_id)
   * but we special case since there are subtle differences...
   */
  if (g_udev_device_get_property_as_boolean (device, "ID_ATA"))
    {
      const gchar *model;

      model = g_udev_device_get_property (device, "ID_MODEL_ENC");
      if (model != NULL)
        {
          gchar *s;
          s = udisks_decode_udev_string (model);
          g_strstrip (s);
          udisks_drive_set_model (iface, s);
          g_free (s);
        }

      udisks_drive_set_vendor (iface, g_udev_device_get_property (device, ""));
      udisks_drive_set_revision (iface, g_udev_device_get_property (device, "ID_REVISION"));
      udisks_drive_set_serial (iface, g_udev_device_get_property (device, "ID_SERIAL_SHORT"));
      udisks_drive_set_wwn (iface, g_udev_device_get_property (device, "ID_WWN_WITH_EXTENSION"));
    }
  else if (g_udev_device_get_property_as_boolean (device, "ID_SCSI"))
    {
      const gchar *vendor;
      const gchar *model;

      vendor = g_udev_device_get_property (device, "ID_VENDOR_ENC");
      if (vendor != NULL)
        {
          gchar *s;
          s = udisks_decode_udev_string (vendor);
          g_strstrip (s);
          udisks_drive_set_vendor (iface, s);
          g_free (s);
        }

      model = g_udev_device_get_property (device, "ID_MODEL_ENC");
      if (model != NULL)
        {
          gchar *s;
          s = udisks_decode_udev_string (model);
          g_strstrip (s);
          udisks_drive_set_model (iface, s);
          g_free (s);
        }

      udisks_drive_set_revision (iface, g_udev_device_get_property (device, "ID_REVISION"));
      udisks_drive_set_serial (iface, g_udev_device_get_property (device, "ID_SCSI_SERIAL"));
      udisks_drive_set_wwn (iface, g_udev_device_get_property (device, "ID_WWN_WITH_EXTENSION"));

      set_iscsi_target (drive, iface, device, daemon);
    }
  else if (g_str_has_prefix (g_udev_device_get_name (device), "mmcblk"))
    {
      /* sigh, mmc is non-standard and using ID_NAME instead of ID_MODEL.. */
      udisks_drive_set_model (iface, g_udev_device_get_property (device, "ID_NAME"));
      udisks_drive_set_serial (iface, g_udev_device_get_property (device, "ID_SERIAL"));
      /* TODO:
       *  - lookup Vendor from manfid and oemid in sysfs
       *  - lookup Revision from fwrev and hwrev in sysfs
       */
    }
  else
    {
      const gchar *vendor;
      const gchar *model;
      const gchar *name;

      name = g_udev_device_get_name (device);

      /* generic fallback... */
      vendor = g_udev_device_get_property (device, "ID_VENDOR_ENC");
      if (vendor != NULL)
        {
          gchar *s;
          s = udisks_decode_udev_string (vendor);
          g_strstrip (s);
          udisks_drive_set_vendor (iface, s);
          g_free (s);
        }
      else
        {
          vendor = g_udev_device_get_property (device, "ID_VENDOR");
          if (vendor != NULL)
            {
              udisks_drive_set_vendor (iface, vendor);
            }
          /* workaround for missing ID_VENDOR for floppy drives */
          else if (is_pc_floppy_drive)
            {
              udisks_drive_set_vendor (iface, "");
            }
          /* workaround for missing ID_VENDOR on virtio-blk */
          else if (g_str_has_prefix (name, "vd"))
            {
              /* TODO: could lookup the vendor sysfs attr on the virtio object */
              udisks_drive_set_vendor (iface, "");
            }
        }

      model = g_udev_device_get_property (device, "ID_MODEL_ENC");
      if (model != NULL)
        {
          gchar *s;
          s = udisks_decode_udev_string (model);
          g_strstrip (s);
          udisks_drive_set_model (iface, s);
          g_free (s);
        }
      else
        {
          model = g_udev_device_get_property (device, "ID_MODEL");
          if (model != NULL)
            {
              udisks_drive_set_model (iface, model);
            }
          /* workaround for missing ID_MODEL for floppy drives */
          else if (g_str_has_prefix (name, "fd"))
            {
              udisks_drive_set_model (iface, "Floppy Drive");
            }
          /* workaround for missing ID_MODEL on virtio-blk */
          else if (g_str_has_prefix (name, "vd"))
            {
              udisks_drive_set_model (iface, "VirtIO Disk");
            }
        }

      udisks_drive_set_revision (iface, g_udev_device_get_property (device, "ID_REVISION"));
      udisks_drive_set_serial (iface, g_udev_device_get_property (device, "ID_SERIAL_SHORT"));
      if (g_udev_device_has_property (device, "ID_WWN_WITH_EXTENSION"))
        udisks_drive_set_wwn (iface, g_udev_device_get_property (device, "ID_WWN_WITH_EXTENSION"));
      else
        udisks_drive_set_wwn (iface, g_udev_device_get_property (device, "ID_WWN"));
    }

  /* common bits go here */
  size = udisks_daemon_util_block_get_size (device,
                                            &media_available,
                                            &media_change_detected);
  udisks_drive_set_size (iface, size);
  udisks_drive_set_media_available (iface, media_available);
  udisks_drive_set_media_change_detected (iface, media_change_detected);
  set_media (iface, device, is_pc_floppy_drive);
  set_rotation_rate (iface, device);
  set_connection_bus (iface, device);

  if (udisks_drive_get_media_removable (iface) ||
      g_strcmp0 (udisks_drive_get_connection_bus (iface), "usb") == 0 ||
      g_strcmp0 (udisks_drive_get_connection_bus (iface), "sdio") == 0 ||
      g_strcmp0 (udisks_drive_get_connection_bus (iface), "ieee1394") == 0)
    removable_hint = TRUE;
  udisks_drive_set_removable (iface, removable_hint);

  set_media_time_detected (drive, device, is_pc_floppy_drive, coldplug);

  /* calculate sort-key  */
  if (drive->sort_key == NULL)
    {
      if (coldplug)
        {
          const gchar *device_name;
          /* TODO: adjust device_name for better sort order (so e.g. sdaa comes after sdz) */
          device_name = g_udev_device_get_name (device);
          if (udisks_drive_get_removable (iface))
            {
              /* make sure fd* BEFORE sr* BEFORE sd* */
              if (g_str_has_prefix (device_name, "fd"))
                {
                  drive->sort_key = g_strdup_printf ("00coldplug/10removable/%s", device_name);
                }
              else if (g_str_has_prefix (device_name, "sr"))
                {
                  drive->sort_key = g_strdup_printf ("00coldplug/11removable/%s", device_name);
                }
              else if (g_str_has_prefix (device_name, "sd"))
                {
                  drive->sort_key = append_fixedup_sd ("00coldplug/12removable/", device_name);
                }
              else
                {
                  drive->sort_key = g_strdup_printf ("00coldplug/12removable/%s", device_name);
                }
            }
          else
            {
              if (g_str_has_prefix (device_name, "sd"))
                drive->sort_key = append_fixedup_sd ("00coldplug/00fixed/", device_name);
              else
                drive->sort_key = g_strdup_printf ("00coldplug/00fixed/%s", device_name);
            }
        }
      else
        {
          drive->sort_key = g_strdup_printf ("01hotplug/%" G_GINT64_FORMAT, drive->time_detected);
        }
      udisks_drive_set_sort_key (iface, drive->sort_key);
    }

 out:
  if (device != NULL)
    g_object_unref (device);
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
handle_eject (UDisksDrive           *_drive,
              GDBusMethodInvocation *invocation,
              GVariant              *options)
{
  UDisksLinuxDrive *drive = UDISKS_LINUX_DRIVE (_drive);
  UDisksLinuxDriveObject *object;
  UDisksLinuxBlockObject *block_object = NULL;
  UDisksBlock *block;
  UDisksDaemon *daemon;
  const gchar *action_id;
  gchar *error_message;
  GError *error;

  daemon = NULL;
  block = NULL;
  error_message = NULL;

  error = NULL;
  object = udisks_daemon_util_dup_object (drive, &error);
  if (object == NULL)
    {
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }

  daemon = udisks_linux_drive_object_get_daemon (object);
  block_object = udisks_linux_drive_object_get_block (object, TRUE);
  if (block_object == NULL)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             UDISKS_ERROR,
                                             UDISKS_ERROR_FAILED,
                                             "Unable to find physical block device for drive");
      goto out;
    }
  block = udisks_object_peek_block (UDISKS_OBJECT (block_object));

  /* TODO: is it a good idea to overload modify-device? */
  action_id = "org.freedesktop.udisks2.modify-device";
  if (udisks_block_get_hint_system (block))
    action_id = "org.freedesktop.udisks2.modify-device-system";

  /* Check that the user is actually authorized */
  if (!udisks_daemon_util_check_authorization_sync (daemon,
                                                    UDISKS_OBJECT (block_object),
                                                    action_id,
                                                    options,
                                                    N_("Authentication is required to eject $(udisks2.device)"),
                                                    invocation))
    goto out;

  if (!udisks_daemon_launch_spawned_job_sync (daemon,
                                              UDISKS_OBJECT (object),
                                              NULL, /* GCancellable */
                                              0,    /* uid_t run_as_uid */
                                              0,    /* uid_t run_as_euid */
                                              NULL, /* gint *out_status */
                                              &error_message,
                                              NULL,  /* input_string */
                                              "eject \"%s\"",
                                              udisks_block_get_device (block)))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             UDISKS_ERROR,
                                             UDISKS_ERROR_FAILED,
                                             "Error eject %s: %s",
                                             udisks_block_get_device (block),
                                             error_message);
      goto out;
    }

  udisks_drive_complete_eject (UDISKS_DRIVE (drive), invocation);

 out:
  g_clear_object (&block_object);
  g_free (error_message);
  g_clear_object (&object);
  return TRUE; /* returning TRUE means that we handled the method invocation */
}

/* ---------------------------------------------------------------------------------------------------- */

static void
drive_iface_init (UDisksDriveIface *iface)
{
  iface->handle_eject = handle_eject;
}
