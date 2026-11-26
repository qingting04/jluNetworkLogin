#include "drcom-packet.h"

#include <arpa/inet.h>
#include <string.h>

#include <libubox/md5.h>

static void checksum2(const uint8_t *buf, size_t len, uint8_t out[4]) {
	uint32_t sum = 1234;
	uint8_t tmp[4];
	for (size_t i = 0; i < len; i += 4) {
		memset(tmp, 0, sizeof(tmp));
		size_t n = (len - i >= 4) ? 4 : (len - i);
		memcpy(tmp, buf + i, n);
		uint32_t v = (uint32_t)tmp[0] | ((uint32_t)tmp[1] << 8) | ((uint32_t)tmp[2] << 16) | ((uint32_t)tmp[3] << 24);
		sum ^= v;
	}
	uint32_t r = 1968U * sum;
	out[0] = (uint8_t)r;
	out[1] = (uint8_t)(r >> 8);
	out[2] = (uint8_t)(r >> 16);
	out[3] = (uint8_t)(r >> 24);
}

static void rol3(const uint8_t md5a[16], const uint8_t *pass, size_t pass_len, uint8_t *out) {
	/* md5a is 16 bytes; for passwords longer than 16 the key cycles (i & 15),
	 * avoiding an out-of-bounds read into the adjacent struct fields. */
	for (size_t i = 0; i < pass_len; i++) {
		uint8_t x = (uint8_t)(md5a[i & 15] ^ pass[i]);
		out[i] = (uint8_t)(((x << 3) & 0xff) | (x >> 5));
	}
}

static void md5sum_bytes(const void *data, size_t len, uint8_t out[16]) {
	md5_ctx_t ctx;
	md5_begin(&ctx);
	md5_hash(data, len, &ctx);
	md5_end(out, &ctx);
}

ssize_t drcom_build_login(const struct drcom_login_fields *fields, uint8_t *out,
			  size_t out_sz, uint8_t md5a[16]) {
	const uint8_t control_check = 0x20;
	const uint8_t adapter_num = 0x03;
	const uint8_t ip_dog = 0x01;
	const uint8_t auth_ver[2] = { 0x68, 0x00 };

	size_t user_len = strnlen(fields->username, 36);
	size_t pass_len = strnlen(fields->password, 32);
	if (user_len == 0 || pass_len == 0)
		return -1;

	size_t pkt_len = 333 + pass_len;
	if (pkt_len > out_sz)
		return -1;
	memset(out, 0, pkt_len);

	uint8_t md5tmp[16];

	/* md5tmp = md5(0x01 + pass + salt + 0x00*4) */
	uint8_t tmpbuf[1 + 32 + 4 + 4];
	memset(tmpbuf, 0, sizeof(tmpbuf));
	tmpbuf[0] = 0x01;
	memcpy(tmpbuf + 1, fields->password, pass_len);
	memcpy(tmpbuf + 1 + pass_len, fields->salt, 4);
	md5sum_bytes(tmpbuf, 1 + pass_len + 4 + 4, md5tmp);

	/* md5a = md5(0x03 0x01 salt pass) */
	uint8_t tmp2[2 + 4 + 32];
	tmp2[0] = 0x03;
	tmp2[1] = 0x01;
	memcpy(tmp2 + 2, fields->salt, 4);
	memcpy(tmp2 + 6, fields->password, pass_len);
	md5sum_bytes(tmp2, 6 + pass_len, md5a);

	out[0] = 0x03;
	out[1] = 0x01;
	out[2] = 0x00;
	out[3] = (uint8_t)(user_len + 20);
	memcpy(out + 4, md5a, 16);
	memcpy(out + 20, fields->username, user_len);

	out[56] = control_check;
	out[57] = adapter_num;
	for (int i = 0; i < 6; i++)
		out[58 + i] = (uint8_t)(md5a[i] ^ fields->mac[i]);

	memcpy(out + 64, md5tmp, 16);

	out[80] = 0x01;
	memcpy(out + 81, &fields->local_ip, 4);

	/* md5 checksum field at [97..104] */
	memcpy(out + 97, "\x14\x00\x07\x0b", 4);
	md5sum_bytes(out, 101, md5tmp);
	memcpy(out + 97, md5tmp, 8);

	out[105] = ip_dog;

	/* hostname */
	memcpy(out + 110, fields->hostname, strnlen(fields->hostname, 32));

	/* dns + dhcp */
	struct in_addr dns_ip;

	if (inet_pton(AF_INET, fields->dns[0] ? fields->dns : "10.10.10.10", &dns_ip) != 1)
		dns_ip.s_addr = 0;
	memcpy(out + 142, &dns_ip.s_addr, 4);

	struct in_addr dhcp_ip = { .s_addr = 0 };
	memcpy(out + 146, &dhcp_ip.s_addr, 4);

	/* os / unknown block */
	static const uint8_t osblk[20] = {
		0x94, 0x00, 0x00, 0x00,
		0x06, 0x00, 0x00, 0x00,
		0x02, 0x00, 0x00, 0x00,
		0xf0, 0x23, 0x00, 0x00,
		0x02, 0x00, 0x00, 0x00,
	};
	memcpy(out + 162, osblk, sizeof(osblk));

	/* DRCOM check + padding */
	static const uint8_t drcomchk[64] = {
		0x44, 0x72, 0x43, 0x4f, 0x4d, 0x00, 0xcf, 0x07, 0x68,
		/* rest zeros */
	};
	memcpy(out + 182, drcomchk, sizeof(drcomchk));

	/* key3 */
	memcpy(out + 246, "3dc79f5212e8170acfa9ec95f1d74916542be7b1", 40);

	/* auth version + pass len */
	memcpy(out + 310, auth_ver, 2);
	out[313] = (uint8_t)pass_len;

	rol3(md5a, (const uint8_t *)fields->password, pass_len, out + 314);
	out[314 + pass_len] = 0x02;
	out[315 + pass_len] = 0x0c;

	/* checksum2 placement */
	memcpy(out + 316 + pass_len, "\x01\x26\x07\x11\x00\x00", 6);
	memcpy(out + 322 + pass_len, fields->mac, 6);
	checksum2(out, 328 + pass_len, out + 316 + pass_len);
	memset(out + 320 + pass_len, 0x00, 2);
	memcpy(out + 322 + pass_len, fields->mac, 6);

	/* tail */
	memcpy(out + 331 + pass_len, "\x6e\xe2", 2);

	return (ssize_t)pkt_len;
}

void drcom_build_keepalive0(const uint8_t md5a[16], const uint8_t tail[16],
			    uint16_t nonce, uint8_t out[DRCOM_KEEPALIVE0_LEN]) {
	memset(out, 0, DRCOM_KEEPALIVE0_LEN);
	out[0] = 0xff;
	memcpy(out + 1, md5a, 16);
	/* [17..19] zeros */
	memcpy(out + 20, tail, 16);
	out[36] = (uint8_t)(nonce >> 8);
	out[37] = (uint8_t)(nonce);
}

void drcom_build_keepalive12(int stage, uint32_t sequence, uint16_t nonce,
			     const uint8_t flux[4], in_addr_t local_ip,
			     uint8_t out[DRCOM_KEEPALIVE12_LEN]) {
	const uint8_t keepalive_ver[2] = { 0xdc, 0x02 };
	memset(out, 0, DRCOM_KEEPALIVE12_LEN);
	out[0] = 0x07;
	out[1] = (uint8_t)(sequence % 32);
	out[2] = 0x28;
	out[3] = 0x00;
	out[4] = 0x0b;
	out[5] = (uint8_t)(2 * stage - 1); /* 1 or 3 */
	memcpy(out + 6, keepalive_ver, 2);
	out[9] = (uint8_t)(nonce >> 8);
	out[10] = (uint8_t)(nonce);
	memcpy(out + 16, flux, 4);
	if (stage == 2)
		memcpy(out + 28, &local_ip, 4);
}
