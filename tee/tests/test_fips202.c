/* test_fips202.c —— Keccak 核 KAT：SHA3/SHAKE 标准向量 + 增量 API 一致性 */
#include <stdio.h>
#include <string.h>

#include "ta_fips202.h"
#include "config/pqchsm_fips202_mlk.h"
#include "config/pqchsm_fips202_mld.h"

static int fails;

static void hex(const uint8_t *p, size_t n, char *out)
{
	size_t i;

	for (i = 0; i < n; i++)
		sprintf(out + 2 * i, "%02x", p[i]);
	out[2 * n] = '\0';
}

static void check_hex(const char *name, const uint8_t *got, size_t len,
                      const char *want)
{
	char buf[2 * 80 + 1];

	hex(got, len, buf);
	if (strcmp(buf, want) != 0) {
		printf("FAIL %-28s\n  got  %s\n  want %s\n", name, buf, want);
		fails++;
	} else {
		printf("ok   %s\n", name);
	}
}

int test_fips202(void)
{
	uint8_t out[64];
	uint8_t abc[3] = { 'a', 'b', 'c' };
	pqchsm_sponge_t sp;
	uint8_t blk[SHAKE128_RATE * 3];

	fails = 0;

	mlk_sha3_256(out, NULL, 0);
	check_hex("sha3_256(\"\")", out, 32,
	          "a7ffc6f8bf1ed76651c14756a061d662f580ff4de43b49fa82d80a4b80f8434a");
	mlk_sha3_256(out, abc, 3);
	check_hex("sha3_256(abc)", out, 32,
	          "3a985da74fe225b2045c172d6bd390bd855f086e3e9d525b46bfe24511431532");
	mlk_sha3_512(out, abc, 3);
	check_hex("sha3_512(abc)", out, 64,
	          "b751850b1a57168a5693cd924b6b096e08f621827444f70d884f5d0240d2712e"
	          "10e116e9192af3c91a7ec57647e3934057340b4cf408d5a56592f8274eec53f0");

	/* SHAKE128 空输入 32B（经 mlk 皮肤：init/absorb_once/squeezeblocks） */
	{
		mlk_shake128ctx ctx;
		mlk_shake128_init(&ctx);
		mlk_shake128_absorb_once(&ctx, NULL, 0);
		mlk_shake128_squeezeblocks(blk, 1, &ctx); /* 整块 168B，取前 32 */
		mlk_shake128_release(&ctx);
		memcpy(out, blk, 32);
	}
	check_hex("shake128(\"\")[0:32]", out, 32,
	          "7f9c2ba4e88f827d616045507605853ed73b8093f6efbc88eb1a6eacfa66ef26");

	mlk_shake256(out, 64, NULL, 0);
	check_hex("shake256(\"\")64", out, 64,
	          "46b9dd2b0ba88d13233b3feb743eeb243fcd52ea62b81b82b50c27646ed5762f"
	          "d75dc4ddd8c0f200cb05019d67b592f6fc821c49479ab48640292eacb3b7c4be");

	/* 增量 API（mld 皮肤）分段吸收 == 一次性吸收；跨块 squeeze == 单发 */
	mld_shake256(out, 32, abc, 3);
	pqchsm_sponge_init(&sp, PQCHSM_SHAKE256_RATE);
	pqchsm_sponge_absorb(&sp, abc, 1);
	pqchsm_sponge_absorb(&sp, abc + 1, 2);
	pqchsm_sponge_pad(&sp, PQCHSM_DOMAIN_SHAKE);
	{
		uint8_t out2[32];
		pqchsm_sponge_squeeze(&sp, out2, 7);
		pqchsm_sponge_squeeze(&sp, out2 + 7, 25);
		if (memcmp(out, out2, 32) != 0) {
			printf("FAIL incremental absorb/squeeze mismatch\n");
			fails++;
		} else {
			printf("ok   incremental absorb/squeeze\n");
		}
	}

	/* mlk squeezeblocks 跨多块 == 单发 SHAKE128 */
	mlk_shake256(out, 32, abc, 3); /* 占位防止未用警告路径变化 */
	{
		mlk_shake128ctx ctx;
		uint8_t ref[SHAKE128_RATE * 3];
		mlk_shake128_init(&ctx);
		mlk_shake128_absorb_once(&ctx, abc, 3);
		mlk_shake128_squeezeblocks(blk, 3, &ctx);
		mlk_shake128_release(&ctx);
		pqchsm_xof(PQCHSM_SHAKE128_RATE, PQCHSM_DOMAIN_SHAKE,
		           abc, 3, ref, sizeof(ref));
		if (memcmp(blk, ref, sizeof(ref)) != 0) {
			printf("FAIL squeezeblocks(3) != one-shot\n");
			fails++;
		} else {
			printf("ok   squeezeblocks multi-block\n");
		}
	}

	return fails;
}
