/*
 * ----------------------------------------------------------------------------
 *  Copyright (c) 2011-2014 Qualcomm Technologies, Inc.  All Rights Reserved.
 *  Qualcomm Technologies Proprietary and Confidential.
 *  ---------------------------------------------------------------------------
 *
 */

#define LOG_TAG "rmt_storage"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <inttypes.h>

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/uio.h>

#ifndef RMTS_LE_BUILD
	#include "cutils/log.h"
	#include "common_log.h"

#else /* Is an LE build */

	#include <sys/syslog.h>
	#define LOGI(...) syslog(LOG_NOTICE, "INFO:"__VA_ARGS__)
	#define LOGV(...) syslog (LOG_NOTICE,"VERB:" __VA_ARGS__)
	#define LOGE(...) syslog (LOG_ERR,"ERR:"__VA_ARGS__)
	#define LOGW(...) syslog (LOG_WARNING,"WRN:"__VA_ARGS__)

#endif /* RMTS_LE_BUILD */

#define KMSG_BUFFER_SIZE 512
#define KMSG_RMTS_MODEM_SSR_TIME_MS 5000UL  /* 5 seconds */
void kmsg_log_init(void);
void kmsg_log(const char *fmt, ...) __attribute__ ((format(printf, 1, 2)));
void kmsg_enable(void);
void kmsg_disable(void);

#define KMSGI(format, args...) kmsg_log("<6>rmt_storage:INFO:%s: " format "\n", __func__, ##args)
#define KMSGE(format, args...) kmsg_log("<3>rmt_storage:ERR:%s: " format "\n", __func__, ##args)

#include "common_v01.h"
#include "qmi_idl_lib.h"
#include "qmi_csi.h"
#include "remote_storage_v01.h"
#include "qmi_csi_common.h"

#ifndef RMTS_LE_BUILD
	#include <private/android_filesystem_config.h>
#endif /* RMTS_LE_BUILD */

#include <cutils/properties.h>

/* For capabilities */
#include <sys/prctl.h>
#include <sys/capability.h>

/* For sys calls */
#include <unistd.h>
#include <sys/syscall.h>

/*
 * CAP_BLOCK_SUSPEND was introduced in 3.5 so it may not be present if we are
 * building in older builds.
 */
#ifndef CAP_BLOCK_SUSPEND
	#define CAP_BLOCK_SUSPEND 36
#endif

#ifndef CAP_NET_BIND_SERVICE
	#define CAP_NET_BIND_SERVICE 10
#endif

/* QMI defines */
#ifndef QMI_RESULT_FAILURE
	#define QMI_RESULT_FAILURE  QMI_RESULT_FAILURE_V01
#endif

#ifndef QMI_ERR_INTERNAL
	#define QMI_ERR_INTERNAL QMI_ERR_INTERNAL_V01
#endif

#ifdef USE_GLIB
    #include <glib.h>
    #define strlcpy g_strlcpy
#endif


/* General logging */
#define RMTS_LOG(format, args...) \
		LOGI("%s: " format "\n", __func__, ## args)

/* Track internal client ops */
#define RMTS_LOG_CLNT(format, args...) do { \
	if (client) \
		LOGI("%s: %s: " format "\n", \
			__func__, client->path, ## args); \
	else \
		LOGI("***" format, ## args); \
	} while (0)

/* Track QCSI requests */
#define RMTS_LOG_REQ(format, args...) do { \
	if (client) \
		LOGI("%s: %s: req_h=%p msg_id=%d: "format"\n",\
		     __func__, client->path,\
		     req_handle, msg_id, ## args); \
	else \
		LOGI("***" format, ## args); \
	} while (0)

#define RMT_STORAGE_CHECK_WORD   0X12345678
#define RMT_STORAGE_MAX_IOVEC_XFR_CNT 5
#define MAX_NUM_CLIENTS 10
#define MAX_PATH_NAME 32
#define MAX_PART_NAME 128

#define MMC_BLK_SDCC_NUM           1

#define WAKE_LOCK_FILE		"/sys/power/wake_lock"
#define WAKE_UNLOCK_FILE	"/sys/power/wake_unlock"

#ifdef ANDROID
  #define MMC_BLOCK_DEV_NAME       "/dev/block/mmcblk0"
#else
  #define MMC_BLOCK_DEV_NAME       "/dev/mmcblk0"
#endif

#define SECTOR_SIZE 	512
#define BOOT_REC_SIG 	0xAA55

#define MAX_MBR_ENTRIES 4
#define MAX_EBR_ENTRIES 2

#define EXTENDED_PARTITION_TYPE	5

#define PROPERTY_BASEBAND		"ro.baseband"
#define PROPERTY_BASEBAND_SIZE          PROPERTY_VALUE_MAX
#define PROPERTY_CTLSTOP		"ctl.stop"
#define BASEBAND_VALUE_MSM		"msm"
#define BASEBAND_VALUE_SDM		"sdm"
#define BASEBAND_VALUE_SDA		"sda"
#define BASEBAND_VALUE_APQ		"apq"
#define BASEBAND_VALUE_SGLTE		"sglte"
#define BASEBAND_VALUE_DSDA3		"dsda3"
#define BASEBAND_VALUE_UNDEFINED	"undefined"
#define SERVICE_NAME			LOG_TAG

#define UIO_QCOM_VER	"1.0"
#define UIO_QCOM_NAME	"rmtfs"

#define UIO_SYSFS_DIR_PREFIX  "uio"

#define UIO_DEV		"/dev/uio%u"
#define UIO_SYSFS_DIR	"/sys/class/uio"
#define UIO_NAME	"/sys/class/uio/uio%u/name"
#define UIO_VER		"/sys/class/uio/uio%u/version"
#define UIO_ADDR	"/sys/class/uio/uio%u/maps/map0/addr"
#define UIO_SIZE	"/sys/class/uio/uio%u/maps/map0/size"

/*FOR DEBUG FS*/
#define DEBUGFS_FILE           "/sys/kernel/debug/rmt_storage/rmts" /* rmts */
#define DEBUGFS_MAGIC_STRING   "1\n"

/****************************************************************************
* Definitions                                                               *
****************************************************************************/

struct kmsg_desc {
	int fd;
	char buffer[KMSG_BUFFER_SIZE + 1];
	pthread_mutex_t buffer_mutex; /* mutex to make kmsg buffer thread safe*/
	pthread_mutex_t enable_mutex; /* mutex to make kmsg enable thread safe*/
	int is_enabled;
	int is_inited;
};

struct uio_mmap_info {
	int32_t		uio_fd;
	uint32_t	uio_dev_num;
	uint8_t		*shared_mem;
	uint32_t	shared_mem_size;
	uint32_t	shared_mem_physical;
};

struct partition_lookup_entry {
	uint8_t partition_type;
	char part_name[MAX_PART_NAME];
	char path[MAX_PATH_NAME];
	char devpath[MAX_PATH_NAME];
	uint32_t alternate_open_flags;
	int fd;
};

struct partition_entry {
	uint8_t status;
	uint8_t rsvd0[3];
	uint8_t type;
	uint8_t rsvd1[3];
	uint32_t start_sector;
	uint32_t partition_size;
} __attribute__((__packed__));

struct boot_rec {
	uint8_t rsvd0[446];
	struct partition_entry part_entry[MAX_MBR_ENTRIES];
	uint16_t sig;
} __attribute__((__packed__));


struct rmt_storage_iovec_desc {
	uint32_t sector_addr;
	uint32_t data_phy_addr;
	uint32_t num_sector;
};

struct rmt_storage_cinfo {
	uint32_t is_client_ready;
	qmi_client_handle clnt;
};

struct rmt_storage_client {
	int fd;
	uint32_t check_word;
	pthread_t th_id;
	pthread_mutex_t th_mutex;
	pthread_cond_t cond;
	unsigned close;
	char path[MAX_PATH_NAME];
	struct rmt_storage_iovec_desc xfer_desc[RMT_STORAGE_MAX_IOVEC_XFR_CNT];
	uint32_t xfer_dir;
	uint32_t xfer_cnt;
	uint32_t error_code;
	int msg_id;
	qmi_req_handle req_handle;
	char wakelock_name[MAX_PATH_NAME];
	uint32_t wakelock_strlen;
};

struct rmt_storage_svc {
	qmi_csi_service_handle service_handle;
	struct uio_mmap_info mmap_info;
	uint32_t shared_mem_size_sectors;   /* Size of shared memory in sectors */
	uint8_t iovec_address_is_in_offset; /* Indicates that clients will use
				offsets in iovec address and not physical address of buffer */
	uint64_t alloc_buffer_timestamp_ms;
	pthread_mutex_t smem_mutex; /* shared memory mutex */
};


/****************************************************************************
* GLOBALS                                                             *
****************************************************************************/
static struct rmt_storage_svc svc;
static struct rmt_storage_client clients[MAX_NUM_CLIENTS];

static int wakelock_fd, wakeunlock_fd;
static pthread_mutex_t wakelock_mutex, wakeunlock_mutex;

static struct kmsg_desc kmsg;

/****************************************************************************
* CODE                                                               *
****************************************************************************/

static int rmt_storage_init_wakelock(void)
{
	/* Open wakelock files */
	wakelock_fd = open(WAKE_LOCK_FILE, O_WRONLY|O_APPEND);
	if(wakelock_fd < 0) {
		LOGE("Unable to open %s file: return val: %d, error no: %d\n",
			WAKE_LOCK_FILE, wakelock_fd, errno);
		KMSGE("Unable to open %s file: return val: %d, error no: %d",
			WAKE_LOCK_FILE, wakelock_fd, errno);
		if(errno == ENOENT) {
			LOGI("No wakelock support and so not using them.\n");
			wakeunlock_fd = -1;
			return 0;
		}
		return -1;
	} else {
		wakeunlock_fd = open(WAKE_UNLOCK_FILE, O_WRONLY|O_APPEND);
		if(wakeunlock_fd < 0) {
			LOGE("Unable to open %s file: return val: %d, error "
				"no: %d\n", WAKE_UNLOCK_FILE,
				wakeunlock_fd, errno);
			KMSGE("Unable to open %s file: return val: %d, error no: %d",
				WAKE_UNLOCK_FILE, wakeunlock_fd, errno);
			close(wakelock_fd);
			return -1;
		}
	}

	pthread_mutex_init(&wakelock_mutex, NULL);
	pthread_mutex_init(&wakeunlock_mutex, NULL);
	return 0;
}

static struct partition_lookup_entry part_lookup_table[] = {
	{0x4A, "modemst1", "/boot/modem_fs1", "", O_RDWR|O_SYNC, -1},
	{0x4B, "modemst2", "/boot/modem_fs2", "", O_RDWR|O_SYNC, -1},
	{0xFF, "fsc",      "/boot/modem_fsc", "", O_RDWR|O_SYNC, -1},
	{0x58, "fsg",      "/boot/modem_fsg", "", O_RDONLY,      -1}, /* Read only */
};

static int open_partition(char *path)
{
   unsigned int i;

   if (!path)
      return -1;

   for (i = 0;
        i < sizeof(part_lookup_table)/sizeof(struct partition_lookup_entry);
        i++)
		if (!strncmp(part_lookup_table[i].path, path, MAX_PATH_NAME))
			return part_lookup_table[i].fd;

   return -1;
}

static qmi_csi_cb_error rmt_storage_connect_cb (qmi_client_handle client_handle,
				void *service_cookie, void **connection_handle)
{
	struct rmt_storage_cinfo *cinfo;

	(void)service_cookie;

	cinfo = malloc(sizeof(struct rmt_storage_cinfo));
	if (!cinfo)
		return QMI_CSI_CB_NO_MEM;
	cinfo->clnt = client_handle;
	cinfo->is_client_ready = 1;
	*connection_handle = cinfo;
	RMTS_LOG("clnt_h=%p conn_h=%p", cinfo->clnt, cinfo);
	return QMI_CSI_CB_NO_ERR;
}

static struct rmt_storage_client *rmt_storage_init_client(void)
{
	int i;

	for (i = 0; i < MAX_NUM_CLIENTS; i++) {
		if (clients[i].check_word != RMT_STORAGE_CHECK_WORD)
			break;
	}

	if (i == MAX_NUM_CLIENTS) {
		RMTS_LOG("Maximum number of clients reached");
		return NULL;
	}

	clients[i].check_word = RMT_STORAGE_CHECK_WORD;
	clients[i].close = 0;
	pthread_mutex_init(&clients[i].th_mutex, NULL);
	pthread_cond_init(&clients[i].cond, NULL);
	return &clients[i];
}

static void rmt_storage_free_client(struct rmt_storage_client *client)
{
	client->check_word = 0;
	pthread_mutex_destroy(&client->th_mutex);
	pthread_cond_destroy(&client->cond);
}

static struct rmt_storage_client *rmt_storage_get_client(int handle)
{
	int i;

	for (i = 0; i < MAX_NUM_CLIENTS; i++) {
		if (clients[i].fd == handle)
			break;
	}
	if (i == MAX_NUM_CLIENTS) {
		RMTS_LOG("No client found for hndl=%d", handle);
		return NULL;
	}
	return &clients[i];
}

static uint8_t* compute_smem_buff_addr (uint32_t data_addr)
{
	uint8_t *buff;

	if (svc.iovec_address_is_in_offset == 1) {
		buff = svc.mmap_info.shared_mem + data_addr;
	}
	else {
		buff = svc.mmap_info.shared_mem +
			(data_addr - svc.mmap_info.shared_mem_physical);
	}

	return buff;
}

static void * rmt_storage_client_thread(void *data)
{
	uint32_t i;
	struct rmt_storage_iovec_desc *xfer;
	ssize_t ret = 0;
	struct rmt_storage_client *client = data;
	uint8_t *buf;
	rmtfs_rw_iovec_resp_msg_v01 rw_resp;
	qmi_csi_error resp_err;
	int msg_id;
	qmi_req_handle req_handle;

	RMTS_LOG_CLNT("Worker thread started");

	ret = snprintf(client->wakelock_name, sizeof(client->wakelock_name),
			"rmt_storage_%ld", (long)(client->th_id));
	LOGI("wake lock name: %s, name creation success: %zd\n",
		client->wakelock_name, ret);
	client->wakelock_strlen = strlen(client->wakelock_name);

	pthread_mutex_lock(&client->th_mutex);
	while(1) {
		req_handle = client->req_handle;
		msg_id = client->msg_id;

		if (client->close)
			break;

		/* Multiple threads can be accessing smem, lock it before use */
		pthread_mutex_lock(&svc.smem_mutex);

		for(i=0; i<client->xfer_cnt; i++) {
			xfer = &client->xfer_desc[i];
			lseek(client->fd, xfer->sector_addr * (off_t)SECTOR_SIZE, SEEK_SET);
			buf = compute_smem_buff_addr (xfer->data_phy_addr);

			KMSGI("Calling %s [offset=%lu, size=%u]for %s!",
			      (client->xfer_dir == RMTFS_DIRECTION_WRITE_V01) ?
			      "Write" : "Read",
			      xfer->sector_addr * (off_t)SECTOR_SIZE,
			      xfer->num_sector * SECTOR_SIZE, client->path);

			if(client->xfer_dir == RMTFS_DIRECTION_WRITE_V01)
				ret = write(client->fd, buf, xfer->num_sector * SECTOR_SIZE);
			else if (client->xfer_dir == RMTFS_DIRECTION_READ_V01)
				ret = read(client->fd, buf, xfer->num_sector * SECTOR_SIZE);

			if (ret >= 0 && (unsigned int)ret == xfer->num_sector * SECTOR_SIZE) {
				KMSGI("Done %s (bytes = %zd) for %s!",
				      (client->xfer_dir == RMTFS_DIRECTION_WRITE_V01) ?
				      "Write" : "Read",  ret, client->path);
			} else {
				KMSGI("%s failed (ret = %zd) [err= %d (%s)] for %s!",
				      (client->xfer_dir == RMTFS_DIRECTION_WRITE_V01) ?
				      "Write" : "Read",  ret, errno,
				      strerror(errno), client->path);
			}

			if ((int) ret < 0) {
				RMTS_LOG_REQ("%s failed errno = %d error_string = %s",
				        (client->xfer_dir == RMTFS_DIRECTION_WRITE_V01) ?
				        "Write" : "Read", errno, strerror(errno));
				break;
			}
			RMTS_LOG_REQ("Bytes %s = %d",
				      (client->xfer_dir == RMTFS_DIRECTION_WRITE_V01) ?
				      "written" : "read", (int) ret);
		}

		pthread_mutex_unlock(&svc.smem_mutex);

		if (client->xfer_cnt) {
			client->error_code = ((int) ret > 0) ? 0 : ret;
			memset(&rw_resp, 0, sizeof(rmtfs_rw_iovec_resp_msg_v01));
			if (client->error_code) {
				rw_resp.resp.result = QMI_RESULT_FAILURE;
				rw_resp.resp.error = QMI_ERR_INTERNAL;
			}
			RMTS_LOG_REQ("Send response: res=%d err=%d",
				     rw_resp.resp.result, rw_resp.resp.error);
			resp_err = qmi_csi_send_resp(client->req_handle, client->msg_id, &rw_resp, sizeof(rw_resp));
			if(resp_err != QMI_CSI_NO_ERR)
				RMTS_LOG_REQ("qmi_csi_send_resp returned error %d",
					     resp_err);
			client->xfer_cnt = 0;

			if(wakeunlock_fd > 0) {
				pthread_mutex_lock(&wakeunlock_mutex);
				ret = write(wakeunlock_fd, client->wakelock_name,
						client->wakelock_strlen);
				pthread_mutex_unlock(&wakeunlock_mutex);
				RMTS_LOG_CLNT("About to block rmt_storage client thread "
					"(th_id: %ld) wakelock released: %d, error no: %d\n",
					client->th_id, (ret == (int)client->wakelock_strlen), errno);
			} else {
				RMTS_LOG_CLNT("About to block rmt_storage client thread "
					"(th_id: %ld)\n", client->th_id);
			}

		}

		/* Wait for subsequent events and process them */
		pthread_cond_wait(&client->cond, &client->th_mutex);
		RMTS_LOG_CLNT("Unblock worker thread (th_id: %ld)",
				client->th_id);
	}
	pthread_mutex_unlock(&client->th_mutex);
	/* free client structure */
	rmt_storage_free_client(client);
	RMTS_LOG_CLNT("Worker thread exiting");

	return NULL;
}

static qmi_csi_cb_error rmt_storage_open_cb (struct rmt_storage_cinfo *clnt_info,
				qmi_req_handle req_handle, unsigned int msg_id,
				void *req_c_struct, unsigned int req_c_struct_len,
				void *service_cookie)
{
	qmi_csi_cb_error rc = QMI_CSI_CB_INTERNAL_ERR;
	qmi_csi_error resp_err;
	rmtfs_open_resp_msg_v01 open_resp;
	rmtfs_open_req_msg_v01 *data;
	struct rmt_storage_client *client;
	int is_valid_partition = 0;
	int ret, i;

	(void) clnt_info;
	(void) service_cookie;

	if (req_c_struct_len != sizeof(rmtfs_open_req_msg_v01))
		LOGW("open request structure doesn't match expected size %u != %zu !",
		req_c_struct_len, sizeof(rmtfs_open_req_msg_v01));

	memset(&open_resp, 0, sizeof(rmtfs_open_resp_msg_v01));
	data = (rmtfs_open_req_msg_v01*)req_c_struct;

	for (i = 0;  i < sizeof(part_lookup_table)/sizeof(struct partition_lookup_entry);  i++) {
		if (!strncmp(part_lookup_table[i].path, data->path, MAX_PATH_NAME)) {
			is_valid_partition = 1;
			KMSGI("Processing: Open Request for %s!", data->path);
			break;
		}
	}

	for (i = 0; i < MAX_NUM_CLIENTS; i++) {
		if (clients[i].check_word == RMT_STORAGE_CHECK_WORD) {
			if (clients[i].fd > 0 && !strcmp(clients[i].path, data->path)) {
				/* This must be subsystem restart case where
				 * open can be called twice without a close
				 * during subsystem restart
				 */
				client = &clients[i];
				goto skip_init;
			}
		}
	}

	client = rmt_storage_init_client();
	if (!client || client->check_word != RMT_STORAGE_CHECK_WORD) {
		RMTS_LOG("Invalid rmt_storage client for %s", data->path);
		goto err;
	}

	client->fd = open_partition(data->path);
	if (client->fd < 0) {
		RMTS_LOG("Unable to open %s", data->path);
		if (is_valid_partition)
			KMSGE("Unable to open %s", data->path);
		rmt_storage_free_client(client);
		goto err;
	}

	strlcpy(client->path, data->path, MAX_PATH_NAME);
	ret = pthread_create(&client->th_id, NULL, rmt_storage_client_thread, (void *)client);
	if (ret) {
		RMTS_LOG_CLNT("Unable to create a pthread");
		KMSGE("Unable to create a pthread for %s", data->path);
		close(client->fd);
		rmt_storage_free_client(client);
		goto err;
	}

skip_init:
	RMTS_LOG_REQ("Client found");
	open_resp.caller_id_valid = 1;
	open_resp.caller_id = client->fd;
	goto send_resp;
err:
	if (is_valid_partition)
		KMSGE("Request complete Sending error packet");
	open_resp.resp.result = QMI_RESULT_FAILURE;
	open_resp.resp.error = QMI_ERR_INTERNAL;
send_resp:
	RMTS_LOG_REQ("Send response: res=%d err=%d", open_resp.resp.result, open_resp.resp.error);
	resp_err = qmi_csi_send_resp(req_handle, msg_id, &open_resp, sizeof(open_resp));
	if(resp_err != QMI_CSI_NO_ERR) {
		RMTS_LOG_REQ("qmi_csi_send_resp returned error %d", resp_err);
	} else {
		rc = QMI_CSI_CB_NO_ERR;
	}
	return rc;
}

static int is_iovec_address_valid (struct rmt_storage_client *client,
			 struct rmt_storage_iovec_desc *xfer)
{
	uint32_t xfer_size;

	if (xfer->num_sector > svc.shared_mem_size_sectors) {
		RMTS_LOG_CLNT ("IOVEC num sectors to write %d beyond max %d",
			xfer->num_sector, svc.shared_mem_size_sectors);
		return 0;
	}

	if ((xfer->sector_addr > svc.shared_mem_size_sectors) ||
		 ((xfer->sector_addr + xfer->num_sector) >
		   svc.shared_mem_size_sectors)) {
		LOGE ("IOVEC gave bad sector addr to write to %d, %d, %d",
			xfer->sector_addr, xfer->num_sector, svc.shared_mem_size_sectors);
		return 0;
	}

	xfer_size = xfer->num_sector * SECTOR_SIZE;

	if (svc.iovec_address_is_in_offset == 1) {
		uint32_t offset = xfer->data_phy_addr;

		if ((offset > svc.mmap_info.shared_mem_size) ||
			((offset + xfer_size) > svc.mmap_info.shared_mem_size)) {
			LOGE ("IOVEC gave bad buff offset %d, %d, %d", offset, xfer_size,
				svc.mmap_info.shared_mem_size);
			return 0;
		}
	}
	else {
		uint32_t start_addr, allowed_end_addr;

		start_addr = xfer->data_phy_addr;
		allowed_end_addr = svc.mmap_info.shared_mem_physical +
							svc.mmap_info.shared_mem_size;

		if ((start_addr < svc.mmap_info.shared_mem_physical)  ||
			(start_addr > allowed_end_addr)                   ||
			((start_addr + xfer_size) > allowed_end_addr)) {
			LOGE ("IOVEC gave bad buff addr %d, %d, %d, %d", start_addr, xfer_size,
				svc.mmap_info.shared_mem_physical, allowed_end_addr);
			return 0;
		}
	}

	return 1;
}

static qmi_csi_cb_error rmt_storage_rw_iovec_cb (
			struct rmt_storage_cinfo *clnt_info,
			qmi_req_handle req_handle, unsigned int msg_id,
			void *req_c_struct, unsigned int req_c_struct_len,
			void *service_cookie)
{
	qmi_csi_cb_error rc = QMI_CSI_CB_INTERNAL_ERR;
	qmi_csi_error resp_err;
	rmtfs_rw_iovec_req_msg_v01 *data;
	struct rmt_storage_client *client;
	struct rmt_storage_iovec_desc *xfer;
	rmtfs_iovec_desc_type_v01 *desc_args;
	rmtfs_rw_iovec_resp_msg_v01 rw_resp;
	uint32_t i;
	ssize_t ret = 0;

	(void) clnt_info;
	(void) service_cookie;

	if (req_c_struct_len != sizeof(rmtfs_rw_iovec_req_msg_v01))
		LOGW("rw_iovec request structure doesn't match expected size %u != %zu !",
		req_c_struct_len, sizeof(rmtfs_rw_iovec_req_msg_v01));

	memset(&rw_resp, 0, sizeof(rmtfs_rw_iovec_resp_msg_v01));
	data = (rmtfs_rw_iovec_req_msg_v01*)req_c_struct;
	client = rmt_storage_get_client(data->caller_id);
	if (!client || client->check_word != RMT_STORAGE_CHECK_WORD) {
		RMTS_LOG("Invalid rmt_storage client");
		KMSGE("Invalid rmt_storage client");
		goto err;
	}

	/* Re-enable logs if its a write i.e a sync */
	if (data->direction == RMTFS_DIRECTION_WRITE_V01)
		kmsg_enable();

	RMTS_LOG_REQ("R/W request received");
	KMSGI("%s iovec request received for %s",
	      (data->direction == RMTFS_DIRECTION_WRITE_V01) ?
	      "Write" : "Read", client->path);

	pthread_mutex_lock(&client->th_mutex);

	client->xfer_dir = data->direction;
	client->xfer_cnt = data->iovec_struct_len;
	for (i = 0; i < data->iovec_struct_len; i++) {
		xfer = &client->xfer_desc[i];
		desc_args = &data->iovec_struct[i];
		xfer->sector_addr = desc_args->sector_addr;
		xfer->data_phy_addr = desc_args->data_phy_addr_offset;
		xfer->num_sector = desc_args->num_sector;
		if (is_iovec_address_valid (client, xfer) == 0) {
			LOGE("Invalid iovec_request by client %s", client->path);
			KMSGE("Invalid iovec_request by client %s", client->path);
			pthread_mutex_unlock(&client->th_mutex);
			goto err;
		}
	}

	client->msg_id = msg_id;
	client->req_handle = req_handle;

	if((unsigned)wakelock_fd > 0) {
		pthread_mutex_lock(&wakelock_mutex);
		ret = write(wakelock_fd, client->wakelock_name,
				client->wakelock_strlen);
		pthread_mutex_unlock(&wakelock_mutex);
		LOGI("wakelock acquired: %d, error no: %d\n",
			(ret == (int)client->wakelock_strlen), errno);
	}

	pthread_cond_signal(&client->cond);
	pthread_mutex_unlock(&client->th_mutex);
	return QMI_CSI_CB_NO_ERR;

err:
	rw_resp.resp.result = QMI_RESULT_FAILURE;
	rw_resp.resp.error = QMI_ERR_INTERNAL;
	RMTS_LOG_REQ("Send response: res=%d err=%d", rw_resp.resp.result,
		rw_resp.resp.error);

	resp_err = qmi_csi_send_resp(req_handle, msg_id, &rw_resp, sizeof(rw_resp));
	if(resp_err != QMI_CSI_NO_ERR) {
		RMTS_LOG_REQ("qmi_csi_send_resp returned error %d", resp_err);
		return rc;
	 } else {
		return QMI_CSI_CB_NO_ERR;
	 }
}

static qmi_csi_cb_error rmt_storage_close_cb (struct rmt_storage_cinfo *clnt_info,
				qmi_req_handle req_handle, unsigned int msg_id,
				void *req_c_struct, unsigned int req_c_struct_len,
				void *service_cookie)
{
	qmi_csi_cb_error rc = QMI_CSI_CB_INTERNAL_ERR;
	qmi_csi_error resp_err;
	rmtfs_close_req_msg_v01 *data;
	rmtfs_close_resp_msg_v01 close_resp;
	struct rmt_storage_client *client;

	(void) clnt_info;
	(void) service_cookie;

	if (req_c_struct_len != sizeof(rmtfs_close_req_msg_v01))
		LOGW("close request structure doesn't match expected size %u != %zu !",
		req_c_struct_len, sizeof(rmtfs_close_req_msg_v01));

	memset(&close_resp, 0, sizeof(rmtfs_close_resp_msg_v01));
	data = (rmtfs_close_req_msg_v01*)req_c_struct;
	client = rmt_storage_get_client(data->caller_id);
	if (!client || client->check_word != RMT_STORAGE_CHECK_WORD) {
		RMTS_LOG("Invalid rmt_storage client");
		goto err;
	}
	RMTS_LOG_REQ("Close request received");

	pthread_mutex_lock(&client->th_mutex);
	client->close = 1;
	close(client->fd);
	pthread_cond_signal(&client->cond);
	pthread_mutex_unlock(&client->th_mutex);
	goto send_resp;
err:
	close_resp.resp.result = QMI_RESULT_FAILURE;
	close_resp.resp.error = QMI_ERR_INTERNAL;
send_resp:
	RMTS_LOG_REQ("Send response: res=%d err=%d", close_resp.resp.result, close_resp.resp.error);
	resp_err = qmi_csi_send_resp(req_handle, msg_id, &close_resp, sizeof(close_resp) );
	if(resp_err != QMI_CSI_NO_ERR)
		RMTS_LOG_REQ("qmi_csi_send_resp returned error %d", resp_err);
	else
		rc =  QMI_CSI_CB_NO_ERR;

	return rc;
}

static uint64_t get_timestamp_ms(void)
{
	struct timespec ts;
	int result;

	uint16_t curr_time_ms = 0;

	result = clock_gettime(CLOCK_MONOTONIC, &ts);
	if(result != 0) {
		KMSGE("clock_gettime failed with %d (%s)", errno,strerror(errno));
		kmsg_disable();
	} else {
		curr_time_ms = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
	}
	return curr_time_ms;
}

static qmi_csi_cb_error rmt_storage_alloc_buff_cb (struct rmt_storage_cinfo *clnt_info,
				qmi_req_handle req_handle, unsigned int msg_id,
				void *req_c_struct, unsigned int req_c_struct_len,
				void *service_cookie)
{
	qmi_csi_cb_error rc = QMI_CSI_CB_INTERNAL_ERR;
	qmi_csi_error resp_err;
	rmtfs_alloc_buff_req_msg_v01 *data;
	rmtfs_alloc_buff_resp_msg_v01 alloc_buff_resp;
	struct rmt_storage_client *client;
	uint16_t curr_time_ms;

	(void) clnt_info;
	(void) service_cookie;

	curr_time_ms = get_timestamp_ms();
	if (curr_time_ms) {
		uint64_t elapsed_time = curr_time_ms - svc.alloc_buffer_timestamp_ms;
		if(elapsed_time < KMSG_RMTS_MODEM_SSR_TIME_MS) {
			KMSGE("elapsed time(ms) is %" PRIu64 " < %lu", elapsed_time, KMSG_RMTS_MODEM_SSR_TIME_MS);
			kmsg_disable();
		}else {
			if (svc.alloc_buffer_timestamp_ms)
				KMSGI("elapsed time(ms) is %" PRIu64 " >= %lu", elapsed_time, KMSG_RMTS_MODEM_SSR_TIME_MS);

			kmsg_enable();
		}
		svc.alloc_buffer_timestamp_ms = curr_time_ms;
	}

	if (req_c_struct_len != sizeof(rmtfs_alloc_buff_req_msg_v01))
		LOGW("Alloc buffer request structure doesn't match expected size %u != %zu !",
		req_c_struct_len, sizeof(rmtfs_alloc_buff_req_msg_v01));

	data = (rmtfs_alloc_buff_req_msg_v01*)req_c_struct;
	client = rmt_storage_get_client(data->caller_id);
	memset(&alloc_buff_resp, 0, sizeof(rmtfs_alloc_buff_resp_msg_v01));

	if (!client || client->check_word != RMT_STORAGE_CHECK_WORD) {
		RMTS_LOG("Invalid rmt_storage client");
		KMSGE("Invalid rmt_storage client");
		goto err;
	}
	RMTS_LOG_REQ("Alloc request received: Size: %d", data->buff_size);
	KMSGI("Received req_size: %d! avail: %d", data->buff_size, svc.mmap_info.shared_mem_size);

	if(data->buff_size > svc.mmap_info.shared_mem_size)
		goto err;

	alloc_buff_resp.buff_address_valid = 1;
	alloc_buff_resp.buff_address = svc.mmap_info.shared_mem_physical;

	/* If we get a special request for a buffer of zero bytes it means it is a
	 * new client that gets the address from Kernel driver and will use offsets
	 * in IOVEC requests */
	if (data->buff_size == 0) {
		svc.iovec_address_is_in_offset = 1;
		RMTS_LOG_REQ("New client making a dummy request with buffer req size 0");
	}
	else {
		svc.iovec_address_is_in_offset = 0;
		RMTS_LOG_REQ("Old Client requesting buffer from RMTS Server");
	}


	goto send_resp;
err:
	KMSGE("Sending error packet");
	alloc_buff_resp.resp.result = QMI_RESULT_FAILURE;
	alloc_buff_resp.resp.error = QMI_ERR_INTERNAL;
send_resp:
	RMTS_LOG_REQ("Send response: res=%d err=%d", alloc_buff_resp.resp.result, alloc_buff_resp.resp.error);
	resp_err = qmi_csi_send_resp(req_handle, msg_id, &alloc_buff_resp, sizeof(alloc_buff_resp) );
	if(resp_err != QMI_CSI_NO_ERR)
		RMTS_LOG_REQ("qmi_csi_send_resp returned error %d", resp_err);
	else
		rc =  QMI_CSI_CB_NO_ERR;

	return rc;
}

static void rmt_storage_disconnect_cb(void *connection_handle, void *service_cookie)
{
	struct rmt_storage_cinfo *cinfo = connection_handle;

	(void) service_cookie;

	RMTS_LOG("clnt_h=%p conn_h=%p", cinfo->clnt, cinfo);
	if (connection_handle)
		free(connection_handle);
}

static qmi_csi_cb_error (* const req_handle_table[])
(
 struct rmt_storage_cinfo  *clnt_info,
 qmi_req_handle            req_handle,
 unsigned int              msg_id,
 void                      *req_c_struct,
 unsigned int              req_c_struct_len,
 void                      *service_cookie
) =
{
	NULL,
	&rmt_storage_open_cb,
	&rmt_storage_close_cb,
	&rmt_storage_rw_iovec_cb,
	&rmt_storage_alloc_buff_cb,
	//&rmt_storage_get_dev_err_cb,
	//&rmt_storage_force_sync
};

static qmi_csi_cb_error rmt_storage_handle_req_cb (void *connection_handle,
				qmi_req_handle req_handle, unsigned int msg_id,
				void *req_c_struct, unsigned int req_c_struct_len,
				void *service_cookie)
{
	qmi_csi_cb_error rc = QMI_CSI_CB_INTERNAL_ERR;
	struct rmt_storage_cinfo *cinfo = (struct rmt_storage_cinfo*)connection_handle;

	if (!cinfo || !cinfo->is_client_ready) {
		RMTS_LOG("Invalid client");
		KMSGE("Handling new Request-- Invalid client!");
		return rc;
	}
	if((uint32_t) msg_id < (sizeof(req_handle_table) / sizeof(*req_handle_table))) {
		if(req_handle_table[msg_id]) {
			rc = req_handle_table[msg_id] (cinfo,req_handle,
					msg_id,req_c_struct, req_c_struct_len,
					service_cookie);
		} else {
			RMTS_LOG("No function defined to handle request for message ID: %d",
				 msg_id);
			KMSGE("No function defined to handle request for message ID: %d",
				 msg_id);
		}
	} else {
		RMTS_LOG("Message ID: %d greater than maximum known message ID", msg_id);
	}
	return rc;
}


/*
 * partition_open_helper (helper function):
 * Try to open with the default open flags to allow writes for the partition
 * and fall back to the alternate_open_flags passed in case of failure.
 */
static int partition_open_helper (char *partition_path, uint32_t alternate_open_flags)
{
	int fd;
	uint32_t default_open_flags = (O_RDWR | O_SYNC);

	fd = open(partition_path, default_open_flags);

	/* If open failed try the alternate_open_flags */
	if (fd < 0)
		fd = open(partition_path, alternate_open_flags);

	return fd;
}

static int parse_gpt_partition(void)
{
	struct partition_lookup_entry *part_entry;
	int fd;
	int parts_found = 0;
	char part_path[MAX_PART_NAME];
	unsigned int i;
	DIR *bootdevdir;
	struct dirent *bootdirent;

	for (i = 0; i < sizeof(part_lookup_table) /
		sizeof(struct partition_lookup_entry); i++) {

		part_entry = &part_lookup_table[i];
		if (!part_entry->part_name[0])
			continue;
		snprintf(part_path, MAX_PART_NAME, "/dev/block/platform/msm_sdcc.%d/by-name/%s",
			MMC_BLK_SDCC_NUM, part_entry->part_name);
		fd = partition_open_helper(part_path, part_entry->alternate_open_flags);
		if (fd < 0) {
			snprintf(part_path, MAX_PART_NAME, "/dev/block/bootdevice/by-name/%s",
				part_entry->part_name);
			fd = partition_open_helper(part_path, part_entry->alternate_open_flags);
			if (fd < 0) {
				LOGE("Failed to open partition %s err %d (%s). This is very bad!!!\n", part_path, errno, strerror(errno));
				KMSGE("Failed to open partition %s err %d (%s)", part_path, errno, strerror(errno));
				bootdevdir = opendir("/dev/block/bootdevice/by-name");
				if(!bootdevdir) {
					LOGE("Unable to open /dev/block/bootdevice/by-name\n");
				} else {
					LOGE("files present within /dev/block/bootdevice/by-name:\n");
					while((bootdirent = readdir(bootdevdir)))
						LOGE("%s\n", bootdirent->d_name);
					closedir(bootdevdir);
				}
				continue;
			}
		}
		part_entry->fd = fd;
		LOGI("Registering %s: 0x%x %s\n",
			part_entry->part_name,
			part_entry->partition_type,
			part_entry->path);
		parts_found++;
	}
	return parts_found;
}

static int string_to_uint64(uint64_t *output, char *string)
{
	char *end;
	unsigned long long value;


	value = strtoull(string, &end, 0);

	if (*end != 0)
		return -1;

	if ((errno == ERANGE && value == ULLONG_MAX) || value > UINT64_MAX)
		return -1;

	*output = (uint64_t)value;
	return 0;
}

static int get_uio_dev_num(uint32_t *dev_num, char *dir_name)
{
	int index, result;
	uint64_t output;
	index = strlen(UIO_SYSFS_DIR_PREFIX);

	result = string_to_uint64(&output, &dir_name[index]);
	if (result != 0)
		return -1;

	if(output > UINT32_MAX)
		return -1;

	*dev_num = (uint32_t)output;
	return 0;
}

static int read_file_to_buffer(char *file, char *buffer, uint32_t buffer_size)
{
	FILE *fd;
	char *fgets_ret;
	uint32_t str_len;

	fd = fopen(file, "r");
	if (!fd) {
		LOGE("Unable to open file %s\n", file);
		return -1;
	}
	fgets_ret = fgets(buffer, buffer_size, fd);
	fclose(fd);

	if (!fgets_ret) {
		LOGE("fgets failed with %d for %s\n", errno, file);
		return -1;
	}
	str_len = strlen(buffer);
	if(str_len > 0)
		buffer[str_len - 1] = 0;
	return 0;
}

static int check_uio_dev_version(uint32_t dev_num)
{
	int result = -1;
	char path_buffer[50], device_version[50];

	snprintf(path_buffer, sizeof(path_buffer), UIO_VER, dev_num);

	result = read_file_to_buffer(path_buffer, device_version,
		sizeof(device_version));

	if (result != 0)
		return -1;

	if(strcmp(device_version, UIO_QCOM_VER) != 0) {
		LOGE("Device version %s is not supported", device_version);
		return -1;
	}
	return 0;
}

static int check_uio_dev_name(char *client_name, uint32_t dev_num)
{
	int result = -1;
	char path_buffer[50], device_name[50];

	snprintf(path_buffer, sizeof(path_buffer), UIO_NAME, dev_num);

	result = read_file_to_buffer(path_buffer, device_name,
		sizeof(device_name));

	if (result != 0)
		return -1;

	if(strcasecmp(device_name, client_name) != 0)
		return -1;

	LOGV("Found dev for client %s", client_name);
	return 0;
}


static int do_mmap(char *dev_path, struct uio_mmap_info *mmap_info)
{
	uint8_t *virtual_address = NULL;

	mmap_info->uio_fd = open(dev_path, O_RDWR | O_DSYNC);
	if (mmap_info->uio_fd < 0) {
		LOGE("Unable to open %s\n", dev_path);
		return -1;
	}

	virtual_address = mmap(NULL, mmap_info->shared_mem_size,
				PROT_READ | PROT_WRITE, MAP_SHARED,
				mmap_info->uio_fd, 0);

	if (virtual_address == MAP_FAILED) {
		LOGE("\n mmap() failed for %s\n", dev_path);
		close(mmap_info->uio_fd);
		return -1;
	}
	mmap_info->shared_mem = virtual_address;
	return 0;
}

static int setup_client_mmap_buffer(struct uio_mmap_info *mmap_info)
{
	char path_buffer[50], value[50];
	uint64_t size, address;
	int result;
	uint32_t dev_num = mmap_info->uio_dev_num;

	snprintf(path_buffer, sizeof(path_buffer), UIO_SIZE, dev_num);

	result = read_file_to_buffer(path_buffer, value, sizeof(value));
	if (result != 0)
		return -1;

	result = string_to_uint64(&size, value);
	if (result != 0)
		return -1;

	if(size > UINT32_MAX)
		return -1;

	snprintf(path_buffer, sizeof(path_buffer), UIO_ADDR, dev_num);

	result = read_file_to_buffer(path_buffer, value, sizeof(value));
	if (result != 0)
		return -1;

	result = string_to_uint64(&address, value);
	if (result != 0)
		return -1;

	if(address > UINT32_MAX)
		return -1;

	mmap_info->shared_mem_size = size;
	mmap_info->shared_mem_physical = (uint32_t)address;

	snprintf(path_buffer, sizeof(path_buffer), UIO_DEV, dev_num);

	result = do_mmap(path_buffer, mmap_info);
	if (result != 0)
		return -1;

	LOGV("\n Setup mmap buffers complete\n");
	return 0;
}

static int32_t uio_device_setup_shared_mem (char *client_name,
					struct uio_mmap_info *mmap_info)
{
	DIR *uiodir = NULL;
	struct dirent *uiodirent = NULL;
	int result, is_dev_found = 0;
	size_t uio_sysfs_dir_prefix_len;
	uint32_t dev_num;


	LOGV("Setting up shared mem for client %s", client_name);

	uiodir = opendir(UIO_SYSFS_DIR);
	if(!uiodir) {
		LOGE("open dir failed on %s, errno = %d\n", UIO_SYSFS_DIR,
			errno);
		return -1;
	}

	is_dev_found = 0;
	uio_sysfs_dir_prefix_len = strlen(UIO_SYSFS_DIR_PREFIX);

	/* Parse through every UIO device under /sys/class/uio */
	while((uiodirent = readdir(uiodir))) {
		if(strlen(uiodirent->d_name) <= uio_sysfs_dir_prefix_len)
			continue;

		result = strncasecmp(uiodirent->d_name, UIO_SYSFS_DIR_PREFIX,
					uio_sysfs_dir_prefix_len);

		if(result != 0)
			continue;

		result = get_uio_dev_num(&dev_num, uiodirent->d_name);
		if(result != 0)
			continue;

		result = check_uio_dev_name(client_name, dev_num);
		if(result != 0)
			continue;

		result = check_uio_dev_version(dev_num);
		if(result == 0) {
			mmap_info->uio_dev_num = dev_num;
			is_dev_found = 1;
			break;
		}
	}
	closedir(uiodir);

	if(is_dev_found != 1)
		return -1;

	/* we found the device so set it up */
	result = setup_client_mmap_buffer(mmap_info);
	if(result != 0)
		return -1;

	return 0;
}

static int32_t rmts_server_mmap(void)
{
	int32_t result = -1;
	struct uio_mmap_info *mmap_info = &svc.mmap_info;

	result = uio_device_setup_shared_mem(UIO_QCOM_NAME, mmap_info);
	if(result != 0)
		return -1;

	/* Paranoid check: Validate the shared memory. */
	if(svc.mmap_info.shared_mem == NULL) {
		LOGE("ERROR! Shared memory null after successful mmap");
		return -1;
	}

	svc.shared_mem_size_sectors = svc.mmap_info.shared_mem_size / SECTOR_SIZE;
	return 0;
}

#ifndef RMTS_LE_BUILD

/* Add the supplementary groups */
static int add_suplementary_groups(void)
{
	int ret_val;
	gid_t gid_list[] = {AID_SYSTEM, AID_WAKELOCK};
	int gid_list_size = sizeof(gid_list) / sizeof(gid_list[0]);

	ret_val = setgroups (gid_list_size, gid_list);
	if (ret_val != 0) {
		KMSGE("setgroups failed : %d", errno);
		LOGE("Failed to set supplementary-groups(ret_val %d) errno = %d (%s)",
			ret_val, errno, strerror(errno));
	}
	return ret_val;
}

/* Drop unused capabilities */
static void drop_excess_capabilities(void)
{
	unsigned long cap;
	int err;

	/* Allow the caps to be retained after change uid.
	   If this fails only wakelock capability is lost. */
	if (prctl(PR_SET_KEEPCAPS, 1, 0, 0, 0) != 0)
		LOGE("set keepcaps failed! errno=%d ( %s )", errno,
			strerror(errno));

	for (cap = 0; prctl(PR_CAPBSET_READ, cap, 0, 0, 0) >= 0; cap++) {

		if ((cap == CAP_SETUID) || (cap == CAP_SETGID) || (cap == CAP_BLOCK_SUSPEND) || (cap == CAP_NET_BIND_SERVICE))
			continue;

		err = prctl(PR_CAPBSET_DROP, cap, 0, 0, 0);
		if ((err < 0) && (errno != EINVAL))
			LOGE("Drop capability for cap=%lu failed errno=%d ( %s )", cap,
				errno, strerror(errno));

	}
}

static void enable_capabilities(void)
{
	int ret;
	struct __user_cap_header_struct capheader;
	struct __user_cap_data_struct capdata[2];

	memset(&capheader, 0, sizeof(capheader));
	memset(&capdata, 0, sizeof(capdata));

	capheader.version = _LINUX_CAPABILITY_VERSION_3;
	capheader.pid = 0; /* self  */

	/* Enable block suspend */
	capdata[CAP_TO_INDEX(CAP_BLOCK_SUSPEND)].permitted |= CAP_TO_MASK(CAP_BLOCK_SUSPEND);
	capdata[CAP_TO_INDEX(CAP_BLOCK_SUSPEND)].effective |= CAP_TO_MASK(CAP_BLOCK_SUSPEND);

	/* Enable net bind for qmi */
	capdata[CAP_TO_INDEX(CAP_NET_BIND_SERVICE)].permitted |= CAP_TO_MASK(CAP_NET_BIND_SERVICE);
	capdata[CAP_TO_INDEX(CAP_NET_BIND_SERVICE)].effective |= CAP_TO_MASK(CAP_NET_BIND_SERVICE);

	/* If this fails only wakelock capability is lost. */
	if ((ret = capset(&capheader, capdata)) < 0) {
		LOGE("capset failed: errno=%d ( %s ), ret = %d\n", errno,
			strerror(errno), ret);
		KMSGE("capset failed: errno=%d ( %s ), ret = %d", errno,
				strerror(errno), ret);
	} else {
		LOGI("Capset success!");
	}
}

#endif /* RMTS_LE_BUILD */

void kmsg_log_init(void)
{
	kmsg.fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
	if (kmsg.fd < 0) {
		LOGE("kmsg device open failed with errno=%d(%s)", errno,
			strerror(errno));
		kmsg.is_inited = 0;
		return;
	}
	pthread_mutex_init(&kmsg.buffer_mutex, NULL);
	pthread_mutex_init(&kmsg.enable_mutex, NULL);
	kmsg.is_enabled = 1;
	kmsg.is_inited = 1;
}

void kmsg_log(const char *format, ...)
{
	va_list ap;
	struct iovec iov[1];
	int ret;
	int is_enabled;

	if (!kmsg.is_inited)
		return;

	pthread_mutex_lock(&kmsg.enable_mutex);
	is_enabled = kmsg.is_enabled;
	pthread_mutex_unlock(&kmsg.enable_mutex);

	if (!is_enabled)
		return;

	/*LOCK*/
	pthread_mutex_lock(&kmsg.buffer_mutex);

	va_start(ap, format);
	vsnprintf(kmsg.buffer, sizeof(kmsg.buffer), format, ap);
	va_end(ap);

	kmsg.buffer[KMSG_BUFFER_SIZE] = 0;

	iov[0].iov_base = kmsg.buffer;
	iov[0].iov_len = strlen(kmsg.buffer);

	do {
		ret = writev(kmsg.fd, iov, 1);
	} while (ret == -1 && errno == EINTR);

	/*UNLOCK*/
	pthread_mutex_unlock(&kmsg.buffer_mutex);
}

void kmsg_enable(void)
{
	int do_print = 0;

	pthread_mutex_lock(&kmsg.enable_mutex);
	if (!kmsg.is_enabled) {
		kmsg.is_enabled = 1;
		do_print = 1;
	}
	pthread_mutex_unlock(&kmsg.enable_mutex);

	if (do_print) {
		LOGI("Enabling KMSG-LOGS!");
		KMSGI("Enabling KMSG-LOGS!");
	}
}

void kmsg_disable(void)
{
	KMSGI("Disabling KMSG-LOGS!");
	pthread_mutex_lock(&kmsg.enable_mutex);
	if (kmsg.is_enabled) {
		kmsg.is_enabled = 0;
		LOGI("Disabling KMSG-LOGS!");
	}
	pthread_mutex_unlock(&kmsg.enable_mutex);
}

static void write_debugfs_info(void)
{
	int fd;
	fd = open(DEBUGFS_FILE, O_WRONLY);
	if(fd == -1) {
		LOGE("debug open failed with errno %d", errno);
		return;
	}
	write(fd, DEBUGFS_MAGIC_STRING, strlen(DEBUGFS_MAGIC_STRING));
	close(fd);
}

int main(int argc, char **argv)
{
	qmi_csi_os_params os_params, os_params_in;
	fd_set fds;
	int ret;
	char args[PROPERTY_BASEBAND_SIZE];
	char def[PROPERTY_BASEBAND_SIZE];

	(void)argc;
	(void)argv;

#ifdef RMTS_LE_BUILD

	openlog(LOG_TAG, LOG_PID, LOG_USER);
	LOGI("Starting...\n");

#endif /* RMTS_LE_BUILD */
	kmsg_log_init();

	(void)strlcpy(def, BASEBAND_VALUE_UNDEFINED, PROPERTY_BASEBAND_SIZE);
	memset(args, 0, sizeof(args));

#ifndef RMTS_LE_BUILD
	ret = property_get(PROPERTY_BASEBAND, args, def);
	if ((ret > 0) && (ret <= PROPERTY_BASEBAND_SIZE)) {
		/* RemoteFS service is not supported on targets that have off chip modem */
		if(strncmp(args, BASEBAND_VALUE_MSM, sizeof(BASEBAND_VALUE_MSM)) &&
		   strncmp(args, BASEBAND_VALUE_SDM, sizeof(BASEBAND_VALUE_SDM)) &&
		   strncmp(args, BASEBAND_VALUE_SDA, sizeof(BASEBAND_VALUE_SDA)) &&
		   strncmp(args, BASEBAND_VALUE_APQ, sizeof(BASEBAND_VALUE_APQ)) &&
		   strncmp(args, BASEBAND_VALUE_DSDA3, sizeof(BASEBAND_VALUE_DSDA3)) &&
		   strncmp(args, BASEBAND_VALUE_SGLTE, sizeof(BASEBAND_VALUE_SGLTE))) {
			LOGE("Remote storage service is not supported on %s target\n", args);
			KMSGE("Remote storage service is not supported on %s target", args);
			ret = property_set(PROPERTY_CTLSTOP, SERVICE_NAME);
			exit(1);
		}
	}

	ret = add_suplementary_groups();
	if (ret != 0)
		exit(1);

#endif /* RMTS_LE_BUILD */

	ret = rmt_storage_init_wakelock();
	if (ret)
		exit(1);

	qmi_idl_service_object_type rmt_storage_service_object = rmtfs_get_service_object_v01();
	qmi_csi_error rc = QMI_CSI_INTERNAL_ERR;

	memset(&svc, 0, sizeof(svc));
	pthread_mutex_init(&svc.smem_mutex, NULL);

	ret = rmts_server_mmap();
	if (ret < 0) {
		LOGE("Mmap Failed for rmts...pausing execution!");
		KMSGE("Mmap Failed for rmts...pausing execution!");
		pause();

		LOGE("Mmap Failed for rmts. exiting!");
		exit(1);
	}
	else {
		LOGI("Shared memory initialised successfully.");
	}

	ret = parse_gpt_partition();
	if (ret < 0) {
		KMSGE("error with gpt partitions : %d", ret);
		LOGI("Error (%d) parsing partitions\n", ret);
		exit(1);
	} else {
		LOGI("%d GPT partitions found\n", ret);
		if (!ret)
			exit(1);
	}

	/* Write to debugfs */
	write_debugfs_info();

#ifndef RMTS_LE_BUILD
	/* Drop the excess capabilities. */
	drop_excess_capabilities();

	ret = setgid(AID_SYSTEM);
	if (ret < 0) {
		KMSGE("setgid failed : %d", ret);
		LOGE("Error changing gid (%d)\n", ret);
		exit(1);
	}

	ret = setuid(AID_NOBODY);
	if (ret < 0) {
		KMSGE("setuid failed : %d", ret);
		LOGE("Error changing uid (%d)\n", ret);
		exit(1);
	}

	/* enable the capabilities after uid changed. */
	enable_capabilities();

#endif /* RMTS_LE_BUILD */

	rc = qmi_csi_register(rmt_storage_service_object, rmt_storage_connect_cb,
			rmt_storage_disconnect_cb, rmt_storage_handle_req_cb,
			&svc, &os_params, &svc.service_handle);
	if(rc != QMI_NO_ERR) {
		KMSGE("qmi register failed : %d", rc);
		LOGE("Unable to register service!\n");
		exit(1);
	}

	KMSGI("Done with init now waiting for messages!");
	while(1) {
		fds = os_params.fds;
		select(os_params.max_fd+1, &fds, NULL, NULL, NULL);
		os_params_in.fds = fds;
		qmi_csi_handle_event(svc.service_handle, &os_params_in);
	}
	qmi_csi_unregister(svc.service_handle);
	LOGI("Exiting remote storage service");

	return 0;
}
