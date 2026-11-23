/*
 * Minimal <xf86drm.h> for the S21 iOS Mesa port.
 *
 * Mesa's Panfrost kmod code includes this header and calls a small subset of
 * libdrm. On iOS there is no system libdrm, so the embedding dylib supplies
 * these symbols (see ../shim/s21_drm.c). This header is deliberately small:
 * it is not a replacement for system libdrm on Linux.
 */
#ifndef S21_XF86DRM_H
#define S21_XF86DRM_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* macOS omits sched_setscheduler() from its system headers; the Panfrost
 * kmod perf thread calls it. Declared here because this is the compatibility
 * header that panfrost_kmod.c already includes. struct sched_param comes from
 * <pthread.h>, which that file includes first. */
#ifdef __APPLE__
int sched_setscheduler(int pid, int policy, const struct sched_param *param);
#endif

/* Use libdrm's name so this stays compatible if drm-uapi/drm.h is included
 * first. It deliberately does not define struct drm_version, which drm.h owns. */
typedef struct _drmVersion {
   int version_major;
   int version_minor;
   int version_patchlevel;
   int name_len;
   char *name;
   int date_len;
   char *date;
   int desc_len;
   char *desc;
} drmVersion, *drmVersionPtr;

/* Device-enumeration types. Only the field names Mesa's loader.c touches are
 * modelled here; the functions always report "no devices" on iOS. */
typedef struct _drmPciBusInfo {
   uint16_t domain;
   uint8_t bus, dev, func;
} drmPciBusInfo, *drmPciBusInfoPtr;

typedef struct _drmPciDeviceInfo {
   uint16_t vendor_id, device_id, subvendor_id, subdevice_id;
   uint8_t revision_id;
} drmPciDeviceInfo, *drmPciDeviceInfoPtr;

typedef struct _drmUsbBusInfo {
   uint8_t bus, dev;
} drmUsbBusInfo, *drmUsbBusInfoPtr;

typedef struct _drmUsbDeviceInfo {
   uint16_t vendor, product;
} drmUsbDeviceInfo, *drmUsbDeviceInfoPtr;

#define DRM_PLATFORM_DEVICE_NAME_LEN 512
typedef struct _drmPlatformBusInfo {
   char fullname[DRM_PLATFORM_DEVICE_NAME_LEN];
} drmPlatformBusInfo, *drmPlatformBusInfoPtr;

typedef struct _drmPlatformDeviceInfo {
   char **compatible;
} drmPlatformDeviceInfo, *drmPlatformDeviceInfoPtr;

#define DRM_HOST1X_DEVICE_NAME_LEN 512
typedef struct _drmHost1xBusInfo {
   char fullname[DRM_HOST1X_DEVICE_NAME_LEN];
} drmHost1xBusInfo, *drmHost1xBusInfoPtr;

typedef struct _drmHost1xDeviceInfo {
   char **compatible;
} drmHost1xDeviceInfo, *drmHost1xDeviceInfoPtr;

#define DRM_FAUX_DEVICE_NAME_LEN 256
typedef struct _drmFauxBusInfo {
   char name[DRM_FAUX_DEVICE_NAME_LEN];
} drmFauxBusInfo, *drmFauxBusInfoPtr;

typedef struct _drmDevice {
   char **nodes;
   int available_nodes;
   int bustype;
   union {
      drmPciBusInfoPtr pci;
      drmUsbBusInfoPtr usb;
      drmPlatformBusInfoPtr platform;
      drmHost1xBusInfoPtr host1x;
      drmFauxBusInfoPtr faux;
   } businfo;
   union {
      drmPciDeviceInfoPtr pci;
      drmUsbDeviceInfoPtr usb;
      drmPlatformDeviceInfoPtr platform;
      drmHost1xDeviceInfoPtr host1x;
   } deviceinfo;
} drmDevice, *drmDevicePtr;

int drmIoctl(int fd, unsigned long request, void *arg);
int drmCommandWriteRead(int fd, unsigned int command, void *data, unsigned int size);
drmVersionPtr drmGetVersion(int fd);
void drmFreeVersion(drmVersionPtr v);
int drmGetCap(int fd, uint64_t capability, uint64_t *value);
int drmClose(int fd);

int drmGetDevice(int fd, drmDevicePtr *device);
int drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device);
int drmGetDevices(drmDevicePtr devices[], int max_devices);
int drmGetDevices2(uint32_t flags, drmDevicePtr devices[], int max_devices);
int drmGetDeviceFromDevId(dev_t dev_id, uint32_t flags, drmDevicePtr *device);
char *drmGetDeviceNameFromFd2(int fd);
void drmFreeDevice(drmDevicePtr *device);
void drmFreeDevices(drmDevicePtr devices[], int count);

int drmCloseBufferHandle(int fd, uint32_t handle);
int drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle);
int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd);

int drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle);
int drmSyncobjDestroy(int fd, uint32_t handle);
int drmSyncobjWait(int fd, uint32_t *handles, unsigned num_handles,
                   int64_t timeout_ns, unsigned flags, uint32_t *first_signaled);
int drmSyncobjTimelineWait(int fd, uint32_t *handles, uint64_t *points,
                           unsigned num_handles, int64_t timeout_ns,
                           unsigned flags, uint64_t *first_signaled);
int drmSyncobjReset(int fd, const uint32_t *handles, uint32_t handle_count);
int drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t handle_count);
int drmSyncobjTransfer(int fd, uint32_t dst_handle, uint64_t dst_point,
                       uint32_t src_handle, uint64_t src_point, uint32_t flags);
int drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd);
int drmSyncobjImportSyncFile(int fd, int sync_file_fd, uint32_t handle);
int drmSyncobjQuery2(int fd, uint32_t *handles, uint32_t handle_count,
                     uint64_t *points, uint32_t flags);

#ifdef __cplusplus
}
#endif

#endif
