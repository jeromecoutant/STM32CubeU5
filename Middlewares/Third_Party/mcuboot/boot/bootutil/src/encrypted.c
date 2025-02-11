/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include "mcuboot_config/mcuboot_config.h"

#if defined(MCUBOOT_ENC_IMAGES)
#include <assert.h>
#include <stddef.h>
#include <inttypes.h>
#include <string.h>
#include <stdio.h>


#include "bootutil/bootutil_log.h"

#if defined(MCUBOOT_ENCRYPT_RSA)
#include "mbedtls/rsa.h"
#include "mbedtls/rsa_internal.h"
#include "mbedtls/asn1.h"
#endif

#if defined(MCUBOOT_ENCRYPT_KW)
#  if defined(MCUBOOT_USE_MBED_TLS)
#    include "mbedtls/nist_kw.h"
#    include "mbedtls/aes.h"
#  else
#    include "tinycrypt/aes.h"
#  endif
#endif

#if defined(MCUBOOT_ENCRYPT_EC256)
#include "tinycrypt/utils.h"
#include "tinycrypt/constants.h"
#include "tinycrypt/ecc.h"
#include "tinycrypt/ecc_dh.h"
#include "tinycrypt/ctr_mode.h"
#include "tinycrypt/hmac.h"
#include "mbedtls/oid.h"
#include "mbedtls/asn1.h"
#include "mbedtls/ecdh.h"
#include "mbedtls/hkdf.h"
#include "mbedtls/md.h"
#endif

#include "bootutil/image.h"
#include "bootutil/enc_key.h"
#include "bootutil/sign_key.h"

#include "bootutil_priv.h"

#if defined(MCUBOOT_ENCRYPT_KW)
#if defined(MCUBOOT_USE_MBED_TLS)
static int
key_unwrap(const uint8_t *wrapped, uint8_t *enckey)
{
    mbedtls_nist_kw_context kw;
    int rc;
    size_t olen;

    mbedtls_nist_kw_init(&kw);

    rc = mbedtls_nist_kw_setkey(&kw, MBEDTLS_CIPHER_ID_AES,
            bootutil_enc_key.key, *bootutil_enc_key.len * 8, 0);
    if (rc) {
        goto done;
    }
	BOOT_LOG_INF("set key Done");
    rc = mbedtls_nist_kw_unwrap(&kw, MBEDTLS_KW_MODE_KW, wrapped, TLV_ENC_KW_SZ,
            enckey, &olen, BOOT_ENC_KEY_SIZE);
   BOOT_LOG_INF("unwrap done");

       done:
    mbedtls_nist_kw_free(&kw);
    return rc;
}
#else /* !MCUBOOT_USE_MBED_TLS */
/*
 * Implements AES key unwrapping following RFC-3394 section 2.2.2, using
 * tinycrypt for AES-128 decryption.
 */
static int
key_unwrap(const uint8_t *wrapped, uint8_t *enckey)
{
    struct tc_aes_key_sched_struct aes;
    uint8_t A[8];
    uint8_t B[16];
    int8_t i, j, k;

    if (tc_aes128_set_decrypt_key(&aes, bootutil_enc_key.key) == 0) {
        return -1;
    }

    for (k = 0; k < 8; k++) {
        A[k]          = wrapped[k];
        enckey[k]     = wrapped[8 + k];
        enckey[8 + k] = wrapped[16 + k];
    }

    for (j = 5; j >= 0; j--) {
        for (i = 2; i > 0; i--) {
            for (k = 0; k < 8; k++) {
                B[k] = A[k];
                B[8 + k] = enckey[((i-1) * 8) + k];
            }
            B[7] ^= 2 * j + i;
            if (tc_aes_decrypt((uint8_t *)&B, (uint8_t *)&B, &aes) == 0) {
                return -1;
            }
            for (k = 0; k < 8; k++) {
                A[k] = B[k];
                enckey[((i-1) * 8) + k] = B[8 + k];
            }
        }
    }

    for (i = 0, k = 0; i < 8; i++) {
        k |= A[i] ^ 0xa6;
    }
    if (k) {
        return -1;
    }
    return 0;
}
#endif /* MCUBOOT_USE_MBED_TLS */
#endif /* MCUBOOT_ENCRYPT_KW */

#if defined(MCUBOOT_ENCRYPT_RSA)
static int
parse_rsa_enckey(mbedtls_rsa_context *ctx, uint8_t **p, uint8_t *end)
{
    size_t len;

    if (mbedtls_asn1_get_tag(p, end, &len,
                MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE) != 0) {
        return -1;
    }

    if (*p + len != end) {
        return -2;
    }

    /* Non-optional fields. */
    if ( /* version */
        mbedtls_asn1_get_int(p, end, &ctx->ver) != 0 ||
         /* public modulus */
        mbedtls_asn1_get_mpi(p, end, &ctx->N) != 0 ||
         /* public exponent */
        mbedtls_asn1_get_mpi(p, end, &ctx->E) != 0 ||
         /* private exponent */
        mbedtls_asn1_get_mpi(p, end, &ctx->D) != 0 ||
         /* primes */
        mbedtls_asn1_get_mpi(p, end, &ctx->P) != 0 ||
        mbedtls_asn1_get_mpi(p, end, &ctx->Q) != 0) {

        return -3;
    }

#if !defined(MBEDTLS_RSA_NO_CRT)
    /*
     * DP/DQ/QP are only used inside mbedTLS if it was built with the
     * Chinese Remainder Theorem enabled (default). In case it is disabled
     * we parse, or if not available, we calculate those values.
     */
    if (*p < end) {
        if ( /* d mod (p-1) and d mod (q-1) */
            mbedtls_asn1_get_mpi(p, end, &ctx->DP) != 0 ||
            mbedtls_asn1_get_mpi(p, end, &ctx->DQ) != 0 ||
             /* q ^ (-1) mod p */
            mbedtls_asn1_get_mpi(p, end, &ctx->QP) != 0) {

            return -4;
        }
    } else {
        if (mbedtls_rsa_deduce_crt(&ctx->P, &ctx->Q, &ctx->D,
                    &ctx->DP, &ctx->DQ, &ctx->QP) != 0) {
            return -5;
        }
    }
#endif

    ctx->len = mbedtls_mpi_size(&ctx->N);

    if (mbedtls_rsa_check_privkey(ctx) != 0) {
        return -6;
    }

    return 0;
}
#endif

#if defined(MCUBOOT_ENCRYPT_EC256)
static const uint8_t ec_pubkey_oid[] = MBEDTLS_OID_EC_ALG_UNRESTRICTED;
static const uint8_t ec_secp256r1_oid[] = MBEDTLS_OID_EC_GRP_SECP256R1;

/*
 * Parses the output of `imgtool keygen`, which produces a PKCS#8 elliptic
 * curve keypair. See RFC5208 and RFC5915.
 */
static int
parse_ec256_enckey(uint8_t **p, uint8_t *end, uint8_t *pk)
{
    size_t len;
    int version;
    mbedtls_asn1_buf alg;
    mbedtls_asn1_buf param;

    if (mbedtls_asn1_get_tag(p, end, &len,
                    MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE) != 0) {
        return -1;
    }

    if (*p + len != end) {
        return -2;
    }

    version = 0;
    if (mbedtls_asn1_get_int(p, end, &version) || version != 0) {
        return -3;
    }

    if (mbedtls_asn1_get_alg(p, end, &alg, &param) != 0) {
        return -5;
    }

    if (alg.len != sizeof(ec_pubkey_oid) - 1 ||
        boot_secure_memequal(alg.p, ec_pubkey_oid, sizeof(ec_pubkey_oid) - 1)) {
        return -6;
    }
    if (param.len != sizeof(ec_secp256r1_oid) - 1 ||
        boot_secure_memequal(param.p, ec_secp256r1_oid, sizeof(ec_secp256r1_oid) - 1)) {
        return -7;
    }

    if (mbedtls_asn1_get_tag(p, end, &len, MBEDTLS_ASN1_OCTET_STRING) != 0) {
        return -8;
    }

    /* RFC5915 - ECPrivateKey */

    if (mbedtls_asn1_get_tag(p, end, &len,
                    MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE) != 0) {
        return -9;
    }

    version = 0;
    if (mbedtls_asn1_get_int(p, end, &version) || version != 1) {
        return -10;
    }

    /* privateKey */

    if (mbedtls_asn1_get_tag(p, end, &len, MBEDTLS_ASN1_OCTET_STRING) != 0) {
        return -11;
    }

    if (len != NUM_ECC_BYTES) {
        return -12;
    }

    memcpy(pk, *p, len);

    /* publicKey usually follows but is not parsed here */

    return 0;
}
#if defined(MCUBOOT_USE_TINYCRYPT)
/*
 * HKDF as described by RFC5869.
 *
 * @param ikm       The input data to be derived.
 * @param ikm_len   Length of the input data.
 * @param info      An information tag.
 * @param info_len  Length of the information tag.
 * @param okm       Output of the KDF computation.
 * @param okm_len   On input the requested length; on output the generated length
 */
static int
hkdf(uint8_t *ikm, uint16_t ikm_len, uint8_t *info, uint16_t info_len,
        uint8_t *okm, uint16_t *okm_len)
{
    struct tc_hmac_state_struct hmac;
    uint8_t salt[TC_SHA256_DIGEST_SIZE];
    uint8_t prk[TC_SHA256_DIGEST_SIZE];
    uint8_t T[TC_SHA256_DIGEST_SIZE];
    uint16_t off;
    uint16_t len;
    uint8_t counter;
    bool first;
    int rc;

    /*
     * Extract
     */

    if (ikm == NULL || okm == NULL || ikm_len == 0) {
        return -1;
    }

    memset(salt, 0, TC_SHA256_DIGEST_SIZE);
    rc = tc_hmac_set_key(&hmac, salt, TC_SHA256_DIGEST_SIZE);
    if (rc != TC_CRYPTO_SUCCESS) {
        return -1;
    }

    rc = tc_hmac_init(&hmac);
    if (rc != TC_CRYPTO_SUCCESS) {
        return -1;
    }

    rc = tc_hmac_update(&hmac, ikm, ikm_len);
    if (rc != TC_CRYPTO_SUCCESS) {
        return -1;
    }

    rc = tc_hmac_final(prk, TC_SHA256_DIGEST_SIZE, &hmac);
    if (rc != TC_CRYPTO_SUCCESS) {
        return -1;
    }

    /*
     * Expand
     */

    len = *okm_len;
    counter = 1;
    first = true;
    for (off = 0; len > 0; off += TC_SHA256_DIGEST_SIZE, ++counter) {
        rc = tc_hmac_set_key(&hmac, prk, TC_SHA256_DIGEST_SIZE);
        if (rc != TC_CRYPTO_SUCCESS) {
            return -1;
        }

        rc = tc_hmac_init(&hmac);
        if (rc != TC_CRYPTO_SUCCESS) {
            return -1;
        }

        if (first) {
            first = false;
        } else {
            rc = tc_hmac_update(&hmac, T, TC_SHA256_DIGEST_SIZE);
            if (rc != TC_CRYPTO_SUCCESS) {
                return -1;
            }
        }

        rc = tc_hmac_update(&hmac, info, info_len);
        if (rc != TC_CRYPTO_SUCCESS) {
            return -1;
        }

        rc = tc_hmac_update(&hmac, &counter, 1);
        if (rc != TC_CRYPTO_SUCCESS) {
            return -1;
        }

        rc = tc_hmac_final(T, TC_SHA256_DIGEST_SIZE, &hmac);
        if (rc != TC_CRYPTO_SUCCESS) {
            return -1;
        }

        if (len > TC_SHA256_DIGEST_SIZE) {
            memcpy(&okm[off], T, TC_SHA256_DIGEST_SIZE);
            len -= TC_SHA256_DIGEST_SIZE;
        } else {
            memcpy(&okm[off], T, len);
            len = 0;
        }
    }

    return 0;
}
#endif
#endif

int
boot_enc_set_key(struct enc_key_data *enc_state, uint8_t slot,
        const struct boot_status *bs)
{
    int rc;

#if defined(MCUBOOT_USE_MBED_TLS)
    mbedtls_aes_init(&enc_state[slot].aes);
    rc = mbedtls_aes_setkey_enc(&enc_state[slot].aes, bs->enckey[slot],
            BOOT_ENC_KEY_SIZE_BITS);
    if (rc) {
        mbedtls_aes_free(&enc_state[slot].aes);
        return -1;
    }
#else
    (void)rc;

    /* set_encrypt and set_decrypt do the same thing in tinycrypt */
    tc_aes128_set_encrypt_key(&enc_state[slot].aes, bs->enckey[slot]);
#endif

    enc_state[slot].valid = 1;

    return 0;
}

#define EXPECTED_ENC_LEN        BOOT_ENC_TLV_SIZE

#if defined(MCUBOOT_ENCRYPT_RSA)
#    define EXPECTED_ENC_TLV    IMAGE_TLV_ENC_RSA2048
#elif defined(MCUBOOT_ENCRYPT_KW)
#    define EXPECTED_ENC_TLV    IMAGE_TLV_ENC_KW128
#elif defined(MCUBOOT_ENCRYPT_EC256)
#    define EXPECTED_ENC_TLV    IMAGE_TLV_ENC_EC256
#    define EC_PUBK_INDEX       (1)
#    define EC_TAG_INDEX        (65)
#    define EC_CIPHERKEY_INDEX  (65 + 32)
_Static_assert(EC_CIPHERKEY_INDEX + 16 == EXPECTED_ENC_LEN,
        "Please fix ECIES-P256 component indexes");
#endif

/*
 * Decrypt an encryption key TLV.
 *
 * @param buf An encryption TLV read from flash (build time fixed length)
 * @param enckey An AES-128 key sized buffer to store to plain key.
 */
int
boot_enc_decrypt(const uint8_t *buf, uint8_t *enckey)
{
#if defined(MCUBOOT_ENCRYPT_RSA)
    mbedtls_rsa_context rsa;
    uint8_t *cp;
    uint8_t *cpend;
    size_t olen;
#endif
#if defined(MCUBOOT_USE_TINYCRYPT)
	struct tc_hmac_state_struct hmac;
    struct tc_aes_key_sched_struct aes;
#endif
#if defined(MCUBOOT_ENCRYPT_EC256)
    uint8_t tag[TC_SHA256_DIGEST_SIZE];
    uint8_t shared[NUM_ECC_BYTES];
    uint8_t derived_key[TC_AES_KEY_SIZE + TC_SHA256_DIGEST_SIZE];
    uint8_t *cp;
    uint8_t *cpend;
    uint8_t pk[NUM_ECC_BYTES];
    uint8_t counter[TC_AES_BLOCK_SIZE];
    uint16_t len;
#endif
    int rc = -1;

#if defined(MCUBOOT_ENCRYPT_RSA)

    mbedtls_rsa_init(&rsa, MBEDTLS_RSA_PKCS_V21, MBEDTLS_MD_SHA256);

    cp = (uint8_t *)bootutil_enc_key.key;
    cpend = cp + *bootutil_enc_key.len;

    rc = parse_rsa_enckey(&rsa, &cp, cpend);
    if (rc) {
        mbedtls_rsa_free(&rsa);
        return rc;
    }

    rc = mbedtls_rsa_rsaes_oaep_decrypt(&rsa, NULL, NULL, MBEDTLS_RSA_PRIVATE,
            NULL, 0, &olen, buf, enckey, BOOT_ENC_KEY_SIZE);
    mbedtls_rsa_free(&rsa);
	while(olen!=0)
	{ BOOT_LOG_INF("%x, %x, %x, %x, %x, %x , %x ,%x,",
	   enckey[BOOT_ENC_KEY_SIZE-olen],
	   enckey[BOOT_ENC_KEY_SIZE+1-(olen)],
	   enckey[BOOT_ENC_KEY_SIZE+2-olen],
	   enckey[BOOT_ENC_KEY_SIZE+3-olen],
	   enckey[BOOT_ENC_KEY_SIZE+4-olen],
	   enckey[BOOT_ENC_KEY_SIZE+5-olen],
	   enckey[BOOT_ENC_KEY_SIZE+6-olen],
	   enckey[BOOT_ENC_KEY_SIZE+7-olen]);
	if (olen > 8)
	olen = olen -8;
	else
	olen = 0;
   }


#elif defined(MCUBOOT_ENCRYPT_KW)

    assert(*bootutil_enc_key.len == 16);
    rc = key_unwrap(buf, enckey);

#elif defined(MCUBOOT_ENCRYPT_EC256)

    cp = (uint8_t *)bootutil_enc_key.key;
    cpend = cp + *bootutil_enc_key.len;

    /*
     * Load the stored EC256 decryption private key
     */

    rc = parse_ec256_enckey(&cp, cpend, pk);
    if (rc) {
        return rc;
    }

    /* is EC point uncompressed? */
    if (buf[0] != 0x04) {
        return -1;
    }
#if !defined(MCUBOOT_USE_TINYCRYPT)
{
    size_t olen; /* output len */
    uint8_t stream[TC_AES_BLOCK_SIZE];

    mbedtls_aes_context aes;
    mbedtls_md_context_t ctx;
    mbedtls_md_type_t md_type = MBEDTLS_MD_SHA256;

    mbedtls_ecdh_context ecdh;
    mbedtls_ecdh_setup(&ecdh,MBEDTLS_ECP_DP_SECP256R1);
    /* import the public key of the peer */
    rc = mbedtls_ecp_point_read_binary( &ecdh.ctx.mbed_ecdh.grp,
			&ecdh.ctx.mbed_ecdh.Qp,
			buf, 2*NUM_ECC_BYTES+1);
    if (rc) {
        mbedtls_ecdh_free(&ecdh);		
	return -1;
    }
    if (mbedtls_ecp_check_pubkey(&ecdh.ctx.mbed_ecdh.grp, &ecdh.ctx.mbed_ecdh.Qp)) {
	mbedtls_ecdh_free(&ecdh);
        return -11;
    }
    /* import the private key */
    rc = mbedtls_mpi_read_binary(&ecdh.ctx.mbed_ecdh.d, pk, NUM_ECC_BYTES);
    memset(pk,0,sizeof(pk));
    if (rc) {
	mbedtls_ecdh_free(&ecdh);
	return -2;
    }
    /* compute secret */
    rc = mbedtls_ecdh_calc_secret( &ecdh,
                                   &olen, shared,
                                   NUM_ECC_BYTES,
                                   NULL,
                                   NULL);
     mbedtls_ecdh_free(&ecdh);
     if (rc) {
	memset(shared,0,sizeof(shared));
	return -3;
    }
    /* get derive key */
    len = TC_AES_KEY_SIZE + TC_SHA256_DIGEST_SIZE;
    rc = mbedtls_hkdf(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256)
			, NULL, 0, shared, NUM_ECC_BYTES, (uint8_t *)"MCUBoot_ECIES_v1", 16, derived_key, len);
    memset(shared,0,sizeof(shared));
    if (rc) {
	memset(derived_key,0,sizeof(derived_key));
	return -4;
    }
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(md_type) , 1); //use hmac
    rc = mbedtls_md_hmac_starts(&ctx, &derived_key[16], 32);
    if (rc)
    {	
        memset(derived_key,0,sizeof(derived_key));
        mbedtls_md_free(&ctx);
	return -5;
    }
    rc = mbedtls_md_hmac_update(&ctx, (const unsigned char *) &buf[EC_CIPHERKEY_INDEX], 16);
    if (rc) 
    {
        memset(derived_key,0,sizeof(derived_key));
		mbedtls_md_free(&ctx);
        return -6;
     }
     rc = mbedtls_md_hmac_finish(&ctx, tag);
     mbedtls_md_free(&ctx);
     if (boot_secure_memequal(tag, &buf[EC_TAG_INDEX], 32) != 0)
     {
	memset(derived_key,0,sizeof(derived_key));
        return -7;
     }
     mbedtls_aes_init(&aes);
     rc = mbedtls_aes_setkey_enc(&aes, derived_key, 128);
     memset(derived_key,0,sizeof(derived_key));
     if (rc) {
	mbedtls_aes_free(&aes);
	return -8;
     }
     memset(counter, 0, TC_AES_BLOCK_SIZE);
     olen = 0;
     rc = mbedtls_aes_crypt_ctr(&aes, 16, &olen, counter, stream,  &buf[EC_CIPHERKEY_INDEX], enckey);
     mbedtls_aes_free(&aes);
     memset(stream, 0,sizeof(stream));
     memset(counter, 0, TC_AES_BLOCK_SIZE);
     if (rc) 
     {
       *enckey = 0;
       return -10;
     }
     olen=16;
     while(olen!=0)
     { BOOT_LOG_INF("%x, %x, %x, %x, %x, %x , %x ,%x,",
	   enckey[BOOT_ENC_KEY_SIZE-olen],
	   enckey[BOOT_ENC_KEY_SIZE+1-(olen)],
	   enckey[BOOT_ENC_KEY_SIZE+2-olen],
	   enckey[BOOT_ENC_KEY_SIZE+3-olen],
	   enckey[BOOT_ENC_KEY_SIZE+4-olen],
	   enckey[BOOT_ENC_KEY_SIZE+5-olen],
	   enckey[BOOT_ENC_KEY_SIZE+6-olen],
	   enckey[BOOT_ENC_KEY_SIZE+7-olen]);
        if (olen > 8)
	    olen = olen -8;
	else
	    olen = 0;
     }
}
#else
    /*
     * First "element" in the TLV is the curve point (public key)
     */
    rc = uECC_valid_public_key(&buf[EC_PUBK_INDEX], uECC_secp256r1());
    if (rc != 0) {
        return -1;
    }

    rc = uECC_shared_secret(&buf[EC_PUBK_INDEX], pk, shared, uECC_secp256r1());
    if (rc != TC_CRYPTO_SUCCESS) {
        return -1;
    }

    /*
     * Expand shared secret to create keys for AES-128-CTR + HMAC-SHA256
     */

    len = TC_AES_KEY_SIZE + TC_SHA256_DIGEST_SIZE;
    rc = hkdf(shared, NUM_ECC_BYTES, (uint8_t *)"MCUBoot_ECIES_v1", 16,
            derived_key, &len);
    if (rc != 0 || len != (TC_AES_KEY_SIZE + TC_SHA256_DIGEST_SIZE)) {
        return -1;
    }

    /*
     * HMAC the key and check that our received MAC matches the generated tag
     */

    rc = tc_hmac_set_key(&hmac, &derived_key[16], 32);
    if (rc != TC_CRYPTO_SUCCESS) {
        return -1;
    }

    rc = tc_hmac_init(&hmac);
    if (rc != TC_CRYPTO_SUCCESS) {
        return -1;
    }

    rc = tc_hmac_update(&hmac, &buf[EC_CIPHERKEY_INDEX], 16);
    if (rc != TC_CRYPTO_SUCCESS) {
        return -1;
    }

    /* Assumes the tag bufer is at least sizeof(hmac_tag_size(state)) bytes */
    rc = tc_hmac_final(tag, TC_SHA256_DIGEST_SIZE, &hmac);
    if (rc != TC_CRYPTO_SUCCESS) {
        return -1;
    }

    if (_compare(tag, &buf[EC_TAG_INDEX], 32) != 0) {
        return -1;
    }

    /*
     * Finally decrypt the received ciphered key
     */

    rc = tc_aes128_set_decrypt_key(&aes, derived_key);
    if (rc != TC_CRYPTO_SUCCESS) {
        return -1;
    }
    memset(derived_key,0,sizeo(derived_key));
    memset(counter, 0, TC_AES_BLOCK_SIZE);
    rc = tc_ctr_mode(enckey, TC_AES_KEY_SIZE, &buf[EC_CIPHERKEY_INDEX],
            TC_AES_KEY_SIZE, counter, &aes);
    if (rc != TC_CRYPTO_SUCCESS) {
        return -1;
    }

    rc = 0;
#endif
#endif

    return rc;
}

/*
 * Load encryption key.
 */
int
boot_enc_load(struct enc_key_data *enc_state, int image_index,
        const struct image_header *hdr, const struct flash_area *fap,
        struct boot_status *bs)
{
    uint32_t off;
    uint16_t len;
    struct image_tlv_iter it;
#if MCUBOOT_SWAP_SAVE_ENCTLV
    uint8_t *buf;
#else
    uint8_t buf[EXPECTED_ENC_LEN];
#endif
    uint8_t slot;
    int rc;

    rc = flash_area_id_to_multi_image_slot(image_index, fap->fa_id);
    if (rc < 0) {
        return rc;
    }
    slot = rc;

    /* Already loaded... */
    if (enc_state[slot].valid) {
        return 1;
    }

    rc = bootutil_tlv_iter_begin(&it, hdr, fap, EXPECTED_ENC_TLV, false);
    if (rc) {
        return -1;
    }

    rc = bootutil_tlv_iter_next(&it, &off, &len, NULL);
    if (rc != 0) {
        return rc;
    }

    if (len != EXPECTED_ENC_LEN) {
        return -1;
    }

#if MCUBOOT_SWAP_SAVE_ENCTLV
    buf = bs->enctlv[slot];
    memset(buf, 0xff, BOOT_ENC_TLV_ALIGN_SIZE);
#endif

    rc = flash_area_read(fap, off, buf, EXPECTED_ENC_LEN);
    if (rc) {
        return -1;
    }

    return boot_enc_decrypt(buf, bs->enckey[slot]);
}

bool
boot_enc_valid(struct enc_key_data *enc_state, int image_index,
        const struct flash_area *fap)
{
    int rc;

    rc = flash_area_id_to_multi_image_slot(image_index, fap->fa_id);
    if (rc < 0) {
        /* can't get proper slot number - skip encryption, */
        /* postpone the error for a upper layer */
        return false;
    }

    return enc_state[rc].valid;
}

void
boot_encrypt(struct enc_key_data *enc_state, int image_index,
        const struct flash_area *fap, uint32_t off, uint32_t sz,
        uint32_t blk_off, uint8_t *buf)
{
    struct enc_key_data *enc;
    uint32_t i, j;
    uint8_t u8;
    uint8_t nonce[16];
    uint8_t blk[16];
    int rc;

    memset(nonce, 0, 12);
    off >>= 4;
    nonce[12] = (uint8_t)(off >> 24);
    nonce[13] = (uint8_t)(off >> 16);
    nonce[14] = (uint8_t)(off >> 8);
    nonce[15] = (uint8_t)off;

    rc = flash_area_id_to_multi_image_slot(image_index, fap->fa_id);
    if (rc < 0) {
        assert(0);
        return;
    }

    enc = &enc_state[rc];
    assert(enc->valid == 1);
    for (i = 0; i < sz; i++) {
        if (i == 0 || blk_off == 0) {
#if defined(MCUBOOT_USE_MBED_TLS)
            mbedtls_aes_crypt_ecb(&enc->aes, MBEDTLS_AES_ENCRYPT, nonce, blk);
#else
            tc_aes_encrypt(blk, nonce, &enc->aes);
#endif

            for (j = 16; j > 0; --j) {
                if (++nonce[j - 1] != 0) {
                    break;
                }
            }
        }

        u8 = *buf;
        *buf++ = u8 ^ blk[blk_off];
        blk_off = (blk_off + 1) & 0x0f;
    }
}

/**
 * Clears encrypted state after use.
 */
void
boot_enc_zeroize(struct enc_key_data *enc_state)
{
    memset(enc_state, 0, sizeof(struct enc_key_data) * BOOT_NUM_SLOTS);
}

#endif /* MCUBOOT_ENC_IMAGES */
