#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>
#include <linux/mmc/ioctl.h>

#define MMC_SWITCH		6
#define MMC_SEND_EXT_CSD	8
#define MMC_SWITCH_MODE_WRITE_BYTE	0x03
#define EXT_CSD_CMD_SET_NORMAL		(1<<0)
#define MMC_CMD_AC	(0 << 5)
#define MMC_CMD_ADTC	(1 << 5)
#define MMC_RSP_SPI_S1	(1 << 7)
#define MMC_RSP_SPI_BUSY (1 << 10)
#define MMC_RSP_SPI_R1	(MMC_RSP_SPI_S1)
#define MMC_RSP_SPI_R1B	(MMC_RSP_SPI_S1|MMC_RSP_SPI_BUSY)

#define MMC_RSP_PRESENT	(1 << 0)
#define MMC_RSP_CRC	(1 << 2)
#define MMC_RSP_BUSY	(1 << 3)
#define MMC_RSP_OPCODE	(1 << 4)
#define MMC_RSP_R1	(MMC_RSP_PRESENT|MMC_RSP_CRC|MMC_RSP_OPCODE)
#define MMC_RSP_R1B	(MMC_RSP_PRESENT|MMC_RSP_CRC|MMC_RSP_OPCODE|MMC_RSP_BUSY)

#define EXT_CSD_PARTITION_CONFIG 179

int set_boot_part(int fd, uint8_t *ext_csd, uint8_t boot_part) {
	int ret = 0;
	struct mmc_ioc_cmd idata = {0};
	uint8_t reg = ext_csd[EXT_CSD_PARTITION_CONFIG];

	if (boot_part) {
		reg &= ~((1 << 3) | (1 << 5));
		reg |= 1 << 4;
	} else {
		reg &= ~((1 << 4) | (1 << 5));
		reg |= 1 << 3;
	}

	idata.opcode = MMC_SWITCH;
	idata.write_flag = 1;
	idata.arg = (MMC_SWITCH_MODE_WRITE_BYTE << 24) | (EXT_CSD_PARTITION_CONFIG << 16) |
		   (reg << 8) | EXT_CSD_CMD_SET_NORMAL;
	idata.flags = MMC_RSP_SPI_R1B | MMC_RSP_R1B | MMC_CMD_AC;
	idata.cmd_timeout_ms = 0;

	ret = ioctl(fd, MMC_IOC_CMD, &idata);
	if (ret < 0)
		return errno;
	return 0;
}

int write_firmware(char *firmware_path, uint8_t boot_part) {
	int firmware_fd, boot_part_fd, ret;
	char filename[32];
	struct stat stat_buf;
	ssize_t bytes_acc = 0;

	ret = snprintf(filename, 32, "/dev/mmcblk0boot%d", boot_part);
	if (ret >= 32)
		return -EINVAL;
	if (ret < 0)
		return ret;

	boot_part_fd = open(filename, O_RDWR);
	if (boot_part_fd < 0)
		return errno;

	firmware_fd = open(firmware_path, O_RDWR);
	if (firmware_fd < 0) {
		close(boot_part_fd);
		fprintf(stderr, "Could not open firmware file: %s\n", strerror(firmware_fd));
		return errno;
	}

	ret = fstat(firmware_fd, &stat_buf);	
	if (ret < 0 ) {
		close(boot_part_fd);
		close(firmware_fd);
		fprintf(stderr, "Could not get firmware stat: %s\n", strerror(errno));
		return errno;
	}

	do{
		ret = sendfile(boot_part_fd, firmware_fd, NULL, stat_buf.st_size - bytes_acc);
		if (ret < 0) {
			close(boot_part_fd);
			close(firmware_fd);
			fprintf(stderr, "Could not write firmware: %s\n", strerror(errno));
			return errno;
		}
		bytes_acc += ret;
	} while (bytes_acc < stat_buf.st_size);

	close(boot_part_fd);
	close(firmware_fd);
	return 0;
}

int disable_ro(uint8_t boot_part) {
	int fd, ret;
	char filename[64];

	ret = snprintf(filename, 64, "/sys/class/block/mmcblk0boot%d/force_ro", boot_part);
	if (ret >= 64)
		return -EINVAL;
	if (ret < 0)
		return ret;

	fd = open(filename, O_RDWR);
	if (fd < 0)
		return errno;

	ret = write(fd, "0", 2);
	if (ret != 2)
		ret = -EIO;

	close(fd);
	return ret;	
}

int read_extcsd(int fd, uint8_t *ext_csd)
{
	int ret = 0;
	struct mmc_ioc_cmd idata = {0};
	idata.write_flag = 0;
	idata.opcode = MMC_SEND_EXT_CSD;
	idata.arg = 0;
	idata.flags = MMC_RSP_SPI_R1 | MMC_RSP_R1 | MMC_CMD_ADTC;
	idata.blksz = 512;
	idata.blocks = 1;
	mmc_ioc_cmd_set_data(idata, ext_csd);

	ret = ioctl(fd, MMC_IOC_CMD, &idata);

	return ret;
}

int main(int argc, char **argv) {
	uint8_t ext_csd[512] = {0};
	int blk_dev, ret;
	uint8_t next_boot_part;

	if (argc != 2) {
		fprintf(stderr, "Usage: go-update-firmware <path to firmware>\n");
		return -1;
	}

	blk_dev = open("/dev/mmcblk0", O_RDWR);
	if (blk_dev < 0) {
		fprintf(stderr, "Could not open block device: %s\n", strerror(blk_dev));
		return errno;
	}

	ret = read_extcsd(blk_dev, ext_csd);
	if (ret) {
		fprintf(stderr, "Could not read extended csd: %s\n", strerror(ret));
		goto err_close_blkdev;
	}

	switch ((ext_csd[EXT_CSD_PARTITION_CONFIG] >> 3) & 0x7) {
		case 0x1:
			next_boot_part = 1;
			break;
		default:
			next_boot_part = 0;
	}

	printf("Installing new firware on /dev/mmcblk0boot%d\n", next_boot_part);

	ret = disable_ro(next_boot_part);
	if (ret < 0) {
		fprintf(stderr, "Could not disable read only on boot part %d: %s\n", next_boot_part, strerror(ret));
		goto err_close_blkdev;
	}

	ret = write_firmware(argv[1], next_boot_part);
	if (ret) {
		fprintf(stderr, "Could not write firmware to boot part %d: %s\n", next_boot_part, strerror(ret));
		goto err_close_blkdev;
	}

	ret = set_boot_part(blk_dev, ext_csd, next_boot_part);
	if (ret)
		fprintf(stderr, "Could not change selected boot part: %s\n", strerror(ret));
	else
		printf("Selected boot partion %d for the next boot\n", next_boot_part);

err_close_blkdev:
	close(blk_dev);
	return ret;
}

