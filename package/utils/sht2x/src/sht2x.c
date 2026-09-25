// SPDX-License-Identifier: GPL-2.0
/*
 * sht2x - read temperature and relative humidity from a Sensirion SHT2x/SHT21.
 *
 * The Panther X2 carries this sensor on its mini-PCIe LoRa concentrator card,
 * wired to i2c2 (m1 pin group: GPIO4_12/13) at address 0x40.  This kernel has
 * no SHT2x driver (mainline only has sht3x/sht4x, which use a different
 * protocol), so the measurement is done from userspace over /dev/i2c-N.
 *
 * SHT2x "no hold master" sequence: write 0xF3 (temperature) or 0xF5 (relative
 * humidity), wait, then read 3 bytes (MSB, LSB, CRC-8 with polynomial 0x31).
 *
 *   T  [C]  = -46.85 + 175.72 * raw / 65536
 *   RH [%]  =  -6.00 + 125.00 * raw / 65536
 *
 * Usage: sht2x [-b bus] [-a addr] [-n count] [-i interval_ms] [-q]
 *   -b  i2c bus number (default 2, i.e. /dev/i2c-2)
 *   -a  i2c address     (default 0x40)
 *   -n  readings to take (default 1, 0 = forever)
 *   -i  milliseconds between readings (default 2000)
 *   -q  quiet: print only the numbers, "T RH"
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>

#define CMD_TEMP 0xF3
#define CMD_RH   0xF5

static uint8_t crc8(const uint8_t *data, int len)
{
	uint8_t crc = 0x00;
	int i, b;

	for (i = 0; i < len; i++) {
		crc ^= data[i];
		for (b = 8; b > 0; b--)
			crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31)
					   : (uint8_t)(crc << 1);
	}
	return crc;
}

static int measure(int fd, uint8_t cmd, int delay_us, double *out)
{
	uint8_t d[3];

	if (write(fd, &cmd, 1) != 1) {
		fprintf(stderr, "sht2x: write 0x%02x failed: %s\n", cmd, strerror(errno));
		return -1;
	}
	usleep(delay_us);
	if (read(fd, d, 3) != 3) {
		fprintf(stderr, "sht2x: read failed: %s\n", strerror(errno));
		return -1;
	}
	if (crc8(d, 2) != d[2]) {
		fprintf(stderr, "sht2x: CRC mismatch (got %02x, want %02x) - bad reading\n",
			d[2], crc8(d, 2));
		return -1;
	}

	*out = (double)((d[0] << 8) | d[1]);
	return 0;
}

int main(int argc, char **argv)
{
	int bus = 2, count = 1, interval = 2000, quiet = 0, opt, fd;
	unsigned int addr = 0x40;
	char path[64];

	while ((opt = getopt(argc, argv, "b:a:n:i:qh")) != -1) {
		switch (opt) {
		case 'b': bus = atoi(optarg); break;
		case 'a': addr = (unsigned int)strtoul(optarg, NULL, 0); break;
		case 'n': count = atoi(optarg); break;
		case 'i': interval = atoi(optarg); break;
		case 'q': quiet = 1; break;
		default:
			fprintf(stderr, "usage: %s [-b bus] [-a addr] [-n count] [-i ms] [-q]\n", argv[0]);
			return 1;
		}
	}

	snprintf(path, sizeof(path), "/dev/i2c-%d", bus);
	fd = open(path, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "sht2x: cannot open %s: %s\n", path, strerror(errno));
		return 1;
	}
	if (ioctl(fd, I2C_SLAVE, addr) < 0) {
		fprintf(stderr, "sht2x: cannot select address 0x%02x on %s: %s\n",
			addr, path, strerror(errno));
		close(fd);
		return 1;
	}

	for (int i = 0; count == 0 || i < count; i++) {
		double raw, t, rh;

		if (measure(fd, CMD_TEMP, 85000, &raw) < 0)
			break;
		t = -46.85 + 175.72 * raw / 65536.0;

		if (measure(fd, CMD_RH, 30000, &raw) < 0)
			break;
		rh = -6.0 + 125.0 * raw / 65536.0;

		if (quiet)
			printf("%.2f %.2f\n", t, rh);
		else
			printf("temperature: %.2f C   humidity: %.2f %%RH   (%s, 0x%02x)\n",
			       t, rh, path, addr);
		fflush(stdout);

		if (count == 0 || i + 1 < count)
			usleep(interval * 1000);
	}

	close(fd);
	return 0;
}
