#ifndef DRCOM_PACKET_H
#define DRCOM_PACKET_H

#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define DRCOM_KEEPALIVE0_LEN 38
#define DRCOM_KEEPALIVE12_LEN 40

struct drcom_login_fields {
	const char *username;
	const char *password;
	const char *hostname;
	const char *dns;
	in_addr_t local_ip; /* Network byte order */
	uint8_t mac[6];
	uint8_t salt[4];
};

/* Returns the encoded length, or -1 without writing either output. */
ssize_t drcom_build_login(const struct drcom_login_fields *fields, uint8_t *out,
			  size_t out_sz, uint8_t md5a[16]);
void drcom_build_keepalive0(const uint8_t md5a[16], const uint8_t tail[16],
			    uint16_t nonce, uint8_t out[DRCOM_KEEPALIVE0_LEN]);
/* stage is 1 or 2; local_ip is in network byte order. */
void drcom_build_keepalive12(int stage, uint32_t sequence, uint16_t nonce,
			     const uint8_t flux[4], in_addr_t local_ip,
			     uint8_t out[DRCOM_KEEPALIVE12_LEN]);

#endif
