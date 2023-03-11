/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2022 Jules Maselbas */
#include <unistd.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <errno.h>
#include <libusb.h>

#include "arg.h"

#define __noreturn __attribute__((noreturn))

#ifdef DEBUG
#define dbg_printf printf
#else
#define dbg_printf(...) do { ; } while(0)
#endif

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;

#define BIT(x)		(1UL << (x))
#define MIN(a, b)	((a) < (b) ? (a) : (b))
#define MAX(a, b)	((a) < (b) ? (b) : (a))
#define LEN(a)		(sizeof(a) / sizeof(*a))
#define ALIGN(x, a)	(((x) + ((a) - 1)) & ~((a) - 1))

#include "devices.h"

#define MAX_PACKET_SIZE 64
#define SECTOR_SIZE  1024

#define BTVER_2_6 0x0206
#define BTVER_2_7 0x0207

/*
 *  All readable and writable registers.
 *  - `RDPR`: Read Protection
 *  - `USER`: User Config Byte (normally in Register Map datasheet)
 *  - `WPR`:  Write Protection Mask, 1=unprotected, 0=protected
 *
 *  | BYTE0  | BYTE1  | BYTE2  | BYTE3  |
 *  |--------|--------|--------|--------|
 *  | RDPR   | nRDPR  | USER   | nUSER  |
 *  | DATA0  | nDATA0 | DATA1  | nDATA1 |
 *  | WPR0   | WPR1   | WPR2   | WPR3   |
 */
#define CFG_MASK_RDPR_USER_DATA_WPR 0x07
#define CFG_MASK_BTVER 0x08 /* Bootloader version, in the format of `[0x00, major, minor, 0x00]` */
#define CFG_MASK_UID 0x10 /* Device Unique ID */
#define CFG_MASK_ALL 0x1f /* All mask bits of CFGs */

#define CMD_IDENTIFY	0xa1
#define CMD_ISP_END	0xa2
#define CMD_ISP_KEY	0xa3
#define CMD_ERASE	0xa4
#define CMD_PROGRAM	0xa5
#define CMD_VERIFY	0xa6
#define CMD_READ_CONFIG	0xa7
#define CMD_WRITE_CONFIG	0xa8
#define CMD_DATA_ERASE	0xa9
#define CMD_DATA_PROGRAM	0xaa
#define CMD_DATA_READ	0xab
#define CMD_WRITE_OTP	0xc3
#define CMD_READ_OTP	0xc4
#define CMD_SET_BAUD	0xc5

#define ISP_VID 0x4348
#define ISP_PID 0x55e0
#define ISP_EP_OUT (2 | LIBUSB_ENDPOINT_OUT)
#define ISP_EP_IN (2 | LIBUSB_ENDPOINT_IN)

struct isp_dev {
	u8 id;
	u8 type;
	u8 uid[8];
	char uid_str[3 * 8];
	u16 btver;
	u8 xor_key[8];
	libusb_device_handle *usb_dev;
	unsigned int kernel;
	/* info filled from db */
	const char *name;
	u32 flash_size;
	u32 eeprom_size;
	u32 flash_sector_size;
};
struct isp_dev *dev_list;
size_t dev_count;

static libusb_context *usb;
static u8 isp_key[30]; /* all zero key */

static int do_progress;
static int do_reset;
static int do_verify = 1;
static const char *do_match;

__noreturn static void die(const char *errstr, ...);
__noreturn static void version(void);
__noreturn static void usage(int help);
static void *xcalloc(size_t nmemb, size_t size);

static void usb_init(void);
static void usb_fini(void);
static void usb_open(struct isp_dev *dev, libusb_device *usb_dev);
static void usb_close(struct isp_dev *dev);

static void isp_init(struct isp_dev *dev);
static void isp_key_init(struct isp_dev *dev);
static void isp_fini(struct isp_dev *dev);
static size_t isp_send_cmd(struct isp_dev *dev, u8 cmd, u16 len, u8 *data);
static size_t isp_recv_cmd(struct isp_dev *dev, u8 cmd, u16 len, u8 *data);

static void
die(const char *errstr, ...)
{
	va_list ap;

	va_start(ap, errstr);
	vfprintf(stderr, errstr, ap);
	va_end(ap);

	exit(1);
}

static void *
xcalloc(size_t nmemb, size_t size)
{
	void *p = calloc(nmemb, size);
	if (p == NULL)
		die("calloc: %s\n", strerror(errno));
	return p;
}

static size_t
isp_send_cmd(struct isp_dev *dev, u8 cmd, u16 len, u8 *data)
{
	u8 buf[64];
	int ret, got = 0;

	if ((size_t)(len + 3) > sizeof(buf))
		die("isp_send_cmd: invalid argument, length %d\n", len);

	buf[0] = cmd;
	/* length is sent in little endian... but it doesn't really matter
	 * as the usb maxpacket size is 64, thus len should never be greater
	 * than 61 (64 minus the 3 bytes header). */
	buf[1] = (len >> 0) & 0xff;
	buf[2] = (len >> 8) & 0xff;
	if (len != 0)
		memcpy(&buf[3], data, len);

	dbg_printf("isp send cmd %.2x len %.2x%.2x : ", buf[0], buf[2], buf[1]);
	for (int i = 0; i < len; i++)
		dbg_printf("%.2x", data[i]);
	dbg_printf("\n");

	ret = libusb_bulk_transfer(dev->usb_dev, ISP_EP_OUT, buf, len + 3, &got, 10000);
	if (ret)
		die("isp_send_cmd: %s\n", libusb_strerror(ret));
	return got;
}

static size_t
isp_recv_cmd(struct isp_dev *dev, u8 cmd, u16 len, u8 *data)
{
	u8 buf[64];
	int ret, got = 0;
	u16 hdrlen;

	if ((size_t)(len + 4) > sizeof(buf))
		die("isp_recv_cmd: invalid argument, length %d\n", len);

	ret = libusb_bulk_transfer(dev->usb_dev, ISP_EP_IN, buf, len + 4, &got, 10000);
	if (ret)
		die("isp_recv_cmd: %s\n", libusb_strerror(ret));

	if (got < 4)
		die("isp_recv_cmd: not enough data recv\n");
	if (buf[0] != cmd)
		die("isp_recv_cmd: got wrong command %#.x (exp %#.x)\n", buf[0], cmd);
	if (buf[1])
		die("isp_recv_cmd: cmd error %#.x\n", buf[1]);

	got -= 4;
	hdrlen = buf[2] | (buf[3] << 8);
	if (hdrlen != got)
		die("isp_recv_cmd: length mismatch, got %#.x (hdr %#.x)\n", got, hdrlen);
	len = MIN(len, got);

	if (data != NULL)
		memcpy(data, buf + 4, len);

	dbg_printf("isp recv cmd %.2x status %.2x len %.2x%.2x : ", buf[0], buf[1], buf[3], buf[2]);
	for (int i = 0; i < len; i++)
		dbg_printf("%.2x", data[i]);
	dbg_printf("\n");

	return got;
}

static void
cmd_identify(struct isp_dev *dev, u8 *dev_id, u8 *dev_type)
{
	u8 buf[] = "\0\0MCU ISP & WCH.CN";
	u8 ids[2];

	buf[0] = 0; // dev_id
	buf[1] = 0; // dev_type
	/* do not send the terminating nul byte, hence sizeof(buf) - 1 */
	isp_send_cmd(dev, CMD_IDENTIFY, sizeof(buf) - 1, buf);
	isp_recv_cmd(dev, CMD_IDENTIFY, sizeof(ids), ids);

	if (dev_id)
		*dev_id = ids[0];
	if (dev_type)
		*dev_type = ids[1];
}

static void
cmd_isp_key(struct isp_dev *dev, size_t len, u8 *key, u8 *sum)
{
	u8 rsp[2];

	isp_send_cmd(dev, CMD_ISP_KEY, len, key);
	isp_recv_cmd(dev, CMD_ISP_KEY, sizeof(rsp), rsp);
	if (sum)
		*sum = rsp[0];
}

static void
cmd_isp_end(struct isp_dev *dev, u8 reason)
{
	u8 buf[2];

	isp_send_cmd(dev, CMD_ISP_END, sizeof(reason), &reason);
	isp_recv_cmd(dev, CMD_ISP_END, sizeof(buf), buf);
}

static void
cmd_erase(struct isp_dev *dev, u32 sectors)
{
	u8 sec[4];

	sec[0] = (sectors >>  0) & 0xff;
	sec[1] = (sectors >>  8) & 0xff;
	sec[2] = (sectors >> 16) & 0xff;
	sec[3] = (sectors >> 24) & 0xff;

	isp_send_cmd(dev, CMD_ERASE, sizeof(sec), sec);
	isp_recv_cmd(dev, CMD_ERASE, 2, sec); /* receive two 0 bytes */
}

static size_t
cmd_program(struct isp_dev *dev, uint32_t addr, size_t len, const u8 *data, const u8 key[8])
{
	u8 unk[61];
	u8 rsp[2];
	size_t i;

	unk[0] = (addr >>  0) & 0xff;
	unk[1] = (addr >>  8) & 0xff;
	unk[2] = (addr >> 16) & 0xff;
	unk[3] = (addr >> 24) & 0xff;
	unk[4] = 0; /* carefully choosen random number */

	len = MIN(sizeof(unk) - 5, len);
	for (i = 0; i < len; i++)
		unk[5 + i] = data[i] ^ key[i % 8];

	isp_send_cmd(dev, CMD_PROGRAM, len + 5, unk);
	isp_recv_cmd(dev, CMD_PROGRAM, sizeof(rsp), rsp);

	if (rsp[0] != 0 || rsp[1] != 0)
		die("Fail to program chunk @ %#x error: %.2x %.2x\n", addr, rsp[0], rsp[1]);

	return len;
}

static size_t
cmd_verify(struct isp_dev *dev, uint32_t addr, size_t len, const u8 *data, const u8 key[8])
{
	u8 unk[61];
	u8 rsp[2];
	size_t i;

	unk[0] = (addr >>  0) & 0xff;
	unk[1] = (addr >>  8) & 0xff;
	unk[2] = (addr >> 16) & 0xff;
	unk[3] = (addr >> 24) & 0xff;
	unk[4] = 0; /* carefully choosen random number */

	len = MIN(sizeof(unk) - 5, len);
	for (i = 0; i < len; i++)
		unk[5 + i] = data[i] ^ key[i % 8];

	isp_send_cmd(dev, CMD_VERIFY, len + 5, unk);
	isp_recv_cmd(dev, CMD_VERIFY, sizeof(rsp), rsp);

	if (rsp[0] != 0 || rsp[1] != 0)
		die("Fail to verify chunk @ %#x error: %.2x %.2x\n", addr, rsp[0], rsp[1]);

	return len;
}

static size_t
cmd_read_conf(struct isp_dev *dev, u16 cfgmask, size_t len, u8 *cfg)
{
	u8 buf[60];
	u8 req[2];
	u16 mask;
	size_t got;

	req[0] = (cfgmask >>  0) & 0xff;
	req[1] = (cfgmask >>  8) & 0xff;

	isp_send_cmd(dev, CMD_READ_CONFIG, sizeof(req), req);
	got = isp_recv_cmd(dev, CMD_READ_CONFIG, sizeof(buf), buf);
	if (got < 2)
		die("read conf fail: not received enough bytes\n");
	mask = buf[0] | (buf[1] << 8);
	if (cfgmask != mask)
		die("read conf fail: received conf does not match\n");
	len = MIN(got - 2, len);
	memcpy(cfg, &buf[2], len);

	return len;
}

static u16
read_btver(struct isp_dev *dev)
{
	u8 buf[4];

	/* format: [0x00, major, minor, 0x00] */
	cmd_read_conf(dev, CFG_MASK_BTVER, sizeof(buf), buf);

	return (buf[1] << 8) | buf[2];
}

static void
usb_init(void)
{
	struct libusb_device_descriptor desc;
	ssize_t i, count;
	libusb_device **list;
	int err;

	err = libusb_init(&usb);
	if (err)
		die("libusb_init: %s\n", libusb_strerror(err));

	count = libusb_get_device_list(usb, &list);
	if (count < 0)
		die("Fail to get a list of USB devices: %s\n", strerror(errno));

	dev_list = xcalloc(count, sizeof(*dev_list));
	for (i = 0; i < count; i++) {
		err = libusb_get_device_descriptor(list[i], &desc);
		if (err < 0)
			continue;
		if (desc.idVendor == ISP_VID && desc.idProduct == ISP_PID)
			usb_open(&dev_list[dev_count++], libusb_ref_device(list[i]));
	}

	libusb_free_device_list(list, 1);
}

static void
usb_open(struct isp_dev *dev, libusb_device *usb_dev)
{
	int err;

	err = libusb_open(usb_dev, &dev->usb_dev);
	if (err < 0)
		die("libusb_open: %s\n", libusb_strerror(err));

	err = libusb_kernel_driver_active(dev->usb_dev, 0);
	if (err == LIBUSB_ERROR_NOT_SUPPORTED)
		err = 0;
	if (err < 0)
		die("libusb_kernel_driver_active: %s\n", libusb_strerror(err));
	dev->kernel = err;
	if (dev->kernel == 1)
		if (libusb_detach_kernel_driver(dev->usb_dev, 0))
			die("Couldn't detach kernel driver!\n");

	err = libusb_claim_interface(dev->usb_dev, 0);
	if (err)
		die("libusb_claim_interface: %s\n", libusb_strerror(err));
}

static void
usb_close(struct isp_dev *dev)
{
	int err = 0;

	if (dev->usb_dev)
		err = libusb_release_interface(dev->usb_dev, 0);
	if (err)
		fprintf(stderr, "libusb_release_interface: %s\n", libusb_strerror(err));
	if (dev->kernel == 1)
		libusb_attach_kernel_driver(dev->usb_dev, 0);

	if (dev->usb_dev)
		libusb_close(dev->usb_dev);
}

static void
usb_fini(void)
{
	libusb_device *dev;

	while (dev_count > 0) {
		dev_count--;
		dev = libusb_get_device(dev_list[dev_count].usb_dev);
		usb_close(&dev_list[dev_count]);
		libusb_unref_device(dev);
	}
	if (dev_list)
		free(dev_list);

	if (usb)
		libusb_exit(usb);
}

static size_t
db_flash_size(struct isp_dev *dev)
{
	return dev->flash_size;
}

static size_t
db_flash_sector_size(struct isp_dev *dev)
{
	return dev->flash_sector_size;
}

static void
isp_init_from_db(struct isp_dev *dev)
{
	struct db *db = NULL;
	struct db_dev *db_dev = NULL;
	size_t i;

	dev->flash_sector_size = SECTOR_SIZE;
	dev->name = "unknown";
	dev->flash_size = 0xffff;
	dev->eeprom_size = 0;

	for (i = 0; i < LEN(devices); i++) {
		if (devices[i].type == dev->type) {
			db = &devices[i];
			break;
		}
	}
	if (db) {
		for (db_dev = db->devs; db_dev->id != 0; db_dev++) {
			if (db_dev->id == dev->id)
				break;
		}
		if (db_dev->id == 0)
			db_dev = NULL;
	}

	if (db) {
		dev->flash_sector_size = db->flash_sector_size;
	}
	if (db_dev) {
		dev->name = db_dev->name;
		dev->flash_size = db_dev->flash_size;
		dev->eeprom_size = db_dev->eeprom_size;
	}
}

static void
isp_init(struct isp_dev *dev)
{
	size_t i;

	/* get the device type and id */
	cmd_identify(dev, &dev->id, &dev->type);
	/* match the detected device */
	isp_init_from_db(dev);
	/* get the bootloader version */
	dev->btver = read_btver(dev);

	/* get the device uid */
	cmd_read_conf(dev, CFG_MASK_UID, sizeof(dev->uid), dev->uid);

	for (i = 0; i < sizeof(dev->uid); i++) {
		snprintf(dev->uid_str + 3 * i, sizeof(dev->uid_str) - 3 * i,
			 "%.2x-", dev->uid[i]);
	}
}

static void
isp_key_init(struct isp_dev *dev)
{
	size_t i;
	u8 sum;
	u8 rsp;

	/* initialize xor_key */
	for (sum = 0, i = 0; i < sizeof(dev->uid); i++)
		sum += dev->uid[i];
	memset(dev->xor_key, sum, sizeof(dev->xor_key));
	dev->xor_key[7] = dev->xor_key[0] + dev->id;

	/* send the isp key */
	cmd_isp_key(dev, sizeof(isp_key), isp_key, &rsp);

	if (dev->btver >= BTVER_2_7) {
		/* bootloader version 2.7 (and maybe onward) simply send zero */
		sum = 0;
	} else {
		/* bootloader version 2.6 (and maybe prior versions) send back
		 * the checksum of xor_key. This response is used to make sure
		 * we are in sync. */
		for (sum = 0, i = 0; i < sizeof(dev->xor_key); i++)
			sum += dev->xor_key[i];
	}
	if (rsp != sum)
		die("failed set isp key, wrong reply, got %x (exp %x)\n", rsp, sum);
}

static void
progress_bar(const char *act, size_t current, size_t total)
{
	static char f[] = "####################################################";
	static char e[] = "                                                    ";
	int l = strlen(f);
	int n = (current * l) / total;

	if (!do_progress)
		return;
	printf("\r[%.*s%.*s] %s %zu/%zu", n, f, l - n, e, act, current, total);
	if (current == total)
		puts("");
	fflush(stdout);
}

static void
isp_flash(struct isp_dev *dev, size_t size, u8 *data)
{
	size_t sector_size = db_flash_sector_size(dev);
	u32 nr_sectors = ALIGN(size, sector_size) / sector_size;
	size_t off = 0;
	size_t rem = size;
	size_t len;

	cmd_erase(dev, nr_sectors);

	while (off < size) {
		progress_bar("write", off, size);

		len = cmd_program(dev, off, rem, data + off, dev->xor_key);
		off += len;
		rem -= len;
	}
	cmd_program(dev, off, 0, NULL, dev->xor_key);
	progress_bar("write", size, size);
}

static void
isp_verify(struct isp_dev *dev, size_t size, u8 *data)
{
	size_t off = 0;
	size_t rem = size;
	size_t len;

	while (off < size) {
		progress_bar("verify", off, size);

		len = cmd_verify(dev, off, rem, data + off, dev->xor_key);
		off += len;
		rem -= len;
	}
	progress_bar("verify", size, size);
}

static void
isp_fini(struct isp_dev *dev)
{
	if (do_reset)
		cmd_isp_end(dev, 1);
}

static void
file_read_all(const char *name, size_t *size_p, void **bin_p)
{
	FILE *f;
	size_t len, size;
	void *bin;
	int ret;

	f = fopen(name, "rb");
	if (f == NULL)
		die("%s: %s\n", name, strerror(errno));

	ret = fseek(f, 0, SEEK_END);
	if (ret == -1)
		die("fseek: %s\n", strerror(errno));

	len = ftell(f);
	ret = fseek(f, 0, SEEK_SET);
	if (ret == -1)
		die("fseek: %s\n", strerror(errno));

	/* binary image needs to be aligned to a 64 bytes boundary */
	size = ALIGN(len, 64);
	bin = calloc(1, size);
	if (bin == NULL)
		die("calloc: %s\n", strerror(errno));

	ret = fread(bin, len, 1, f);
	if (ret != 1)
		die("fread: %s\n", strerror(errno));

	fclose(f);

	*size_p = size;
	*bin_p = bin;
}

static void
write_flash(struct isp_dev *dev, const char *name)
{
	size_t size;
	void *bin;

	file_read_all(name, &size, &bin);
	if (size > db_flash_size(dev))
		die("%s: file too big, flash size is %d", name, db_flash_size(dev));

	isp_flash(dev, size, bin);
	if (do_verify)
		isp_verify(dev, size, bin);

	free(bin);
}

static void
verify_flash(struct isp_dev *dev, const char *name)
{
	size_t size;
	void *bin;

	file_read_all(name, &size, &bin);
	if (size > db_flash_size(dev))
		die("%s: file too big, flash size is %d", name, db_flash_size(dev));

	isp_verify(dev, size, bin);

	free(bin);
}

/**
 * fmtb formating function to print a binary number to a char buf
 * b the output buffer
 * n the buffer size
 * p the "precision", how much bit to be printed
 * v the value to be printed
 */
static char *
fmtb(char *b, size_t n, int p, u32 v)
{
	char *s = b + n;

	if ((size_t)p > 32) p = 32;
	if ((size_t)p > n)  p = n;

	*--s = '\0';
	for (; b < s && v ; v >>= 1, p--)
		*--s = (v & 1) ? '1' : '0';
	while (b < s && p-- > 0)
		*--s = '0';
	return s;
}

static void
ch569_print_config(size_t len, u8 *cfg)
{
	char buf[4];
	u32 nv;

	if (len < 12)
		return;
	nv = (cfg[8] << 0) | (cfg[9] << 8) | (cfg[10] << 16) | (cfg[11] << 24);
	printf("[4] RESET_EN %d: %s\n", !!(nv & BIT(4)),
	       (nv & BIT(4)) ? "enabled" : "disabled");
	printf("[5] DEBUG_EN %d: %s\n", !!(nv & BIT(5)),
	       (nv & BIT(5)) ? "enabled" : "disabled");
	printf("[6] BOOT_EN %d: %s\n", !!(nv & BIT(6)),
	       (nv & BIT(6)) ? "enabled" : "disabled");
	printf("[7] CODE_READ_EN %d: %s\n", !!(nv & BIT(7)),
	       (nv & BIT(7)) ? "enabled" : "disabled");
	printf("[29] LOCKUP_RST_EN %d: %s\n", !!(nv & BIT(29)),
	       (nv & BIT(29)) ? "enabled" : "disabled");
	printf("[31:30] USER_MEM 0b%s: %s\n",
	       fmtb(buf, sizeof(buf), 2, (nv >> 30) & 0b11),
	       ((nv >> 30) & 0b11) == 0 ? "RAMX 32KB + ROM 96KB" :
	       ((nv >> 30) & 0b11) == 1 ? "RAMX 64KB + ROM 64KB" :
	       "RAMX 96KB + ROM 32KB");
}

static void
config_show(struct isp_dev *dev)
{
	u8 cfg[16];
	size_t len, i;

	len = cmd_read_conf(dev, 0x7, sizeof(cfg), cfg);

	if (dev->type == 0x10 && dev->id == 0x69) {
		ch569_print_config(len, cfg);
		return;
	}

	for (i = 0; i < len; i++)
		printf("%.2x%c", cfg[i], ((i % 4) == 3) ? '\n' : ' ');
}

static struct isp_dev *
dev_by_uid(const char *uid)
{
	size_t i;

	for (i = 0; i < dev_count; i++)
		if (strcmp(uid, dev_list[i].uid_str) == 0)
			return &dev_list[i];

	return NULL;
}

char *argv0;

static void
usage(int help)
{
	printf("usage: %s [-Vnpr] [-d <uid>] COMMAND [ARG ...]\n", argv0);
	printf("       %s [-Vnpr] [-d <uid>] [flash|write|verify|reset] FILE\n", argv0);
	printf("       %s [-Vnpr] list\n", argv0);
	if (!help)
		die("");

	printf("options:\n");
	printf("  -d <uid> Select the usb device that matches the uid\n");
	printf("  -n       No verify after writing to flash, done by default\n");
	printf("  -p       Print a progress-bar during command operation\n");
	printf("  -r       Reset after command completed\n");
	printf("  -V       Print version and exits\n");

	die("");
}

static void
version(void)
{
	printf("%s %s\n", argv0, VERSION);
	die("");
}

int
main(int argc, char *argv[])
{
	struct isp_dev *dev;
	size_t i;

	argv0 = argv[0];

	ARGBEGIN {
	case 'p':
		do_progress = 1;
		break;
	case 'r':
		do_reset = 1;
		break;
	case 'v':
		do_verify = 1;
		break;
	case 'n':
		do_verify = 0;
		break;
	case 'd':
		do_match = EARGF(usage(0));
		break;
	case 'V':
		version();
	case 'h':
		usage(1);
	default:
		usage(0);
	} ARGEND;

	usb_init();

	if (argc < 1)
		die("missing command\n");

	if (dev_count == 0)
		die("no device detected\n");

	for (i = 0; i < dev_count; i++)
		isp_init(&dev_list[i]);

	if (strcmp(argv[0], "list") == 0) {
		for (i = 0; i < dev_count; i++) {
			dev = &dev_list[i];
			printf("%zd: BTVER v%d.%d UID %s [0x%.2x%.2x] %s\n", i,
			       dev->btver >> 8, dev->btver & 0xff,
			       dev->uid_str, dev->type, dev->id,
			       dev->name);
		}
		goto out;
	}

	/* by default select the first device */
	dev = &dev_list[0];
	if (do_match) {
		dev = dev_by_uid(do_match);
		if (!dev)
			die("no device match for '%s'\n", do_match);
	}

	isp_key_init(dev);
	printf("BTVER v%d.%d UID %s [0x%.2x%.2x] %s\n",
	       dev->btver >> 8, dev->btver & 0xff,
	       dev->uid_str, dev->type, dev->id,
	       dev->name);

	if ((strcmp(argv[0], "flash") == 0) ||
	    (strcmp(argv[0], "write") == 0)) {
		if (argc < 2)
			die("%s: missing file\n", argv[0]);
		write_flash(dev, argv[1]);
	}
	if (strcmp(argv[0], "verify") == 0) {
		if (argc < 2)
			die("%s: missing file\n", argv[0]);
		verify_flash(dev, argv[1]);
	}
	if (strcmp(argv[0], "reset") == 0) {
		cmd_isp_end(dev, 1);
	}
	if (strcmp(argv[0], "config") == 0) {
		config_show(dev);
	}

	isp_fini(dev);
out:
	usb_fini();

	return 0;
}
