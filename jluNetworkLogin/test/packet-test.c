#include "drcom-packet.h"

#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void expect_hex(const uint8_t *bytes, size_t len, const char *hex)
{
	assert(strlen(hex) == len * 2);
	for (size_t i = 0; i < len; i++) {
		unsigned int expected;
		assert(sscanf(hex + 2 * i, "%2x", &expected) == 1);
		if (bytes[i] != expected) {
			fprintf(stderr, "byte %zu: expected %02x, got %02x\n",
				i, expected, (unsigned int)bytes[i]);
			assert(bytes[i] == expected);
		}
	}
}

static void test_login(const char *password, const char *expected)
{
	struct drcom_login_fields fields = {
		.username = "test-user", .password = password,
		.hostname = "test-host", .dns = "192.0.2.53",
		.local_ip = inet_addr("192.0.2.21"),
		.mac = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 },
		.salt = { 0x12, 0x34, 0x56, 0x78 },
	};
	uint8_t out[512], digest[16];
	size_t packet_len = strlen(expected) / 2;

	memset(out, 0xa5, sizeof(out));
	memset(digest, 0xa5, sizeof(digest));
	assert(drcom_build_login(&fields, out, packet_len - 1, digest) == -1);
	for (size_t i = 0; i < sizeof(out); i++)
		assert(out[i] == 0xa5);
	for (size_t i = 0; i < sizeof(digest); i++)
		assert(digest[i] == 0xa5);

	assert(drcom_build_login(&fields, out, packet_len, digest) == (ssize_t)packet_len);
	expect_hex(out, packet_len, expected);
	assert(out[packet_len] == 0xa5);
	assert(memcmp(digest, out + 4, sizeof(digest)) == 0);

	fields.password = "";
	assert(drcom_build_login(&fields, out, sizeof(out), digest) == -1);
}

static void test_keepalive(void)
{
	const uint8_t digest[] = { 0x01, 0x02, 0x03 };
	const uint8_t salt_tail[16] = { 0x11, 0x22, 0x33 };
	const uint8_t flux[4] = { 0xa1, 0xb2, 0xc3, 0xd4 };
	const in_addr_t ip = inet_addr("192.0.2.21");
	uint8_t out0[DRCOM_KEEPALIVE0_LEN], out12[DRCOM_KEEPALIVE12_LEN];
	uint8_t md5a[16] = {0};
	memcpy(md5a, digest, sizeof(digest));

	drcom_build_keepalive0(md5a, salt_tail, 0x1234, out0);
	expect_hex(out0, sizeof(out0), "ff01020300000000000000000000000000000000112233000000000000000000000000001234");

	drcom_build_keepalive12(1, 31, 0x1234, flux, ip, out12);
	expect_hex(out12, sizeof(out12), "071f28000b01dc020012340000000000a1b2c3d40000000000000000000000000000000000000000");

	drcom_build_keepalive12(2, 32, 0x1234, flux, ip, out12);
	expect_hex(out12, sizeof(out12), "070028000b03dc020012340000000000a1b2c3d40000000000000000c00002150000000000000000");
}

int main(void)
{
	/* Synthetic accounts and documentation-only IPs. Golden bytes pin the
	 * pre-refactor layout, including the checksum and >16-byte key cycling. */
	test_login("test-pass",
		"0301001de33c82d33e3cb0a3cae80702cf7efc17746573742d75736572000000"
		"0000000000000000000000000000000000000000000000002003e12da0e07a69"
		"4e4f087deff249251fd4bb1e3e711df801c00002150000000000000000000000"
		"009077f94ad633197e0100000000746573742d686f7374000000000000000000"
		"0000000000000000000000000000c00002350000000000000000000000000000"
		"0000940000000600000002000000f0230000020000004472434f4d00cf076800"
		"0000000000000000000000000000000000000000000000000000000000000000"
		"0000000000000000000000000000000000000000000033646337396635323132"
		"6538313730616366613965633935663164373439313635343262653762310000"
		"0000000000000000000000000000000000000000000068000009bcca8f3d9862"
		"8e86cd020c20a11d4f00000211223344550000006ee2");
	test_login("0123456789abcdefghijklmnopqrstuv",
		"0301001d5761fa27262ac6a93965c7f1275a337a746573742d75736572000000"
		"00000000000000000000000000000000000000000000000020035570d814627f"
		"9b590ccc7b394ebb3c3b1de405cba5ef01c00002150000000000000000000000"
		"004e2025c234a8845a0100000000746573742d686f7374000000000000000000"
		"0000000000000000000000000000c00002350000000000000000000000000000"
		"0000940000000600000002000000f0230000020000004472434f4d00cf076800"
		"0000000000000000000000000000000000000000000000000000000000000000"
		"0000000000000000000000000000000000000000000033646337396635323132"
		"6538313730616366613965633935663164373439313635343262653762310000"
		"00000000000000000000000000000000000000000000680000203b8246a090f8"
		"87f408e2359c22f1b2e081489c6a6a325d3eb2a8b51ca2713260020cd074c11d"
		"00000211223344550000006ee2");
	test_keepalive();
	puts("PASS: login vectors, bounds, and three keepalive packets");
	return 0;
}
