// SPDX-License-Identifier: GPL-2.0
/*
 * Key setup for v1 encryption policies
 *
 * Copyright 2015, 2019 Google LLC
 */

/*
 * This file implements compatibility functions for the original encryption
 * policy version ("v1"), including:
 *
 * - Deriving per-file encryption keys using the AES-128-ECB based KDF
 *   (rather than the new method of using HKDF-SHA512)
 *
 * - Retrieving fscrypt master keys from process-subscribed keyrings
 *   (rather than the new method of using a filesystem-level keyring)
 *
 * - Handling policies with the DIRECT_KEY flag set using a master key table
 *   (rather than the new method of implementing DIRECT_KEY with per-mode keys
 *    managed alongside the master keys in the filesystem-level keyring)
 */

#include <crypto/algapi.h>
#include <crypto/skcipher.h>
#include <keys/user-type.h>
#include <linux/hashtable.h>
#include <linux/scatterlist.h>
#include <linux/bio-crypt-ctx.h>
#include <linux/siphash.h>
#include <crypto/sha.h>
#include <dsm/dsm_pub.h>

#include "fscrypt_private.h"

/* Table of keys referenced by DIRECT_KEY policies */
static DEFINE_HASHTABLE(fscrypt_direct_keys, 6); /* 6 bits = 64 buckets */
static DEFINE_SPINLOCK(fscrypt_direct_keys_lock);

static struct crypto_shash *essiv_hash_tfm;

struct fscrypt_blk_crypto_key {
	struct blk_crypto_key base;
	int num_devs;
	struct request_queue *devs[];
};

/*
 * v1 key derivation function.  This generates the derived key by encrypting the
 * master key with AES-128-ECB using the nonce as the AES key.  This provides a
 * unique derived key with sufficient entropy for each inode.  However, it's
 * nonstandard, non-extensible, doesn't evenly distribute the entropy from the
 * master key, and is trivially reversible: an attacker who compromises a
 * derived key can "decrypt" it to get back to the master key, then derive any
 * other key.  For all new code, use HKDF instead.
 *
 * The master key must be at least as long as the derived key.  If the master
 * key is longer, then only the first 'derived_keysize' bytes are used.
 */
static int derive_key_aes(const u8 *master_key,
			  const u8 nonce[FS_KEY_DERIVATION_NONCE_SIZE],
			  u8 *derived_key, unsigned int derived_keysize)
{
	int res = 0;
	struct skcipher_request *req = NULL;
	DECLARE_CRYPTO_WAIT(wait);
	struct scatterlist src_sg, dst_sg;
	struct crypto_skcipher *tfm = crypto_alloc_skcipher("ecb(aes)", 0, 0);

	if (IS_ERR(tfm)) {
		res = PTR_ERR(tfm);
		tfm = NULL;
		goto out;
	}
	crypto_skcipher_set_flags(tfm, CRYPTO_TFM_REQ_WEAK_KEY);
	req = skcipher_request_alloc(tfm, GFP_NOFS);
	if (!req) {
		res = -ENOMEM;
		goto out;
	}
	skcipher_request_set_callback(req,
			CRYPTO_TFM_REQ_MAY_BACKLOG | CRYPTO_TFM_REQ_MAY_SLEEP,
			crypto_req_done, &wait);
	res = crypto_skcipher_setkey(tfm, nonce, FS_KEY_DERIVATION_NONCE_SIZE);
	if (res < 0)
		goto out;

	sg_init_one(&src_sg, master_key, derived_keysize);
	sg_init_one(&dst_sg, derived_key, derived_keysize);
	skcipher_request_set_crypt(req, &src_sg, &dst_sg, derived_keysize,
				   NULL);
	res = crypto_wait_req(crypto_skcipher_encrypt(req), &wait);
out:
	skcipher_request_free(req);
	crypto_free_skcipher(tfm);
	return res;
}

static int fscrypt_do_sha256(const u8 *src, int srclen, u8 *dst)
{
	struct crypto_shash *tfm = READ_ONCE(essiv_hash_tfm);

	/* init hash transform on demand */
	if (unlikely(!tfm)) {
		struct crypto_shash *prev_tfm;

		tfm = crypto_alloc_shash("sha256", 0, 0);
		if (IS_ERR(tfm)) {
			fscrypt_warn(NULL,
				     "error allocating SHA-256 transform: %ld",
				     PTR_ERR(tfm));
			return PTR_ERR(tfm);
		}
		prev_tfm = cmpxchg(&essiv_hash_tfm, NULL, tfm);
		if (prev_tfm) {
			crypto_free_shash(tfm);
			tfm = prev_tfm;
		}
	}

	{
		SHASH_DESC_ON_STACK(desc, tfm);

		desc->tfm = tfm;
		desc->flags = 0;
		return crypto_shash_digest(desc, src, srclen, dst);
	}
}

/*
 * Search the current task's subscribed keyrings for a "logon" key with
 * description prefix:descriptor, and if found acquire a read lock on it and
 * return a pointer to its validated payload in *payload_ret.
 */
static struct key *
find_and_lock_process_key(const char *prefix,
			  const u8 descriptor[FSCRYPT_KEY_DESCRIPTOR_SIZE],
			  unsigned int min_keysize,
			  const struct fscrypt_key **payload_ret)
{
	char *description;
	struct key *key;
	const struct user_key_payload *ukp;
	const struct fscrypt_key *payload;

	description = kasprintf(GFP_NOFS, "%s%*phN", prefix,
				FSCRYPT_KEY_DESCRIPTOR_SIZE, descriptor);
	if (!description)
		return ERR_PTR(-ENOMEM);

	key = request_key(&key_type_logon, description, NULL);
	kfree(description);
	if (IS_ERR(key))
		return key;

	down_read(&key->sem);
	ukp = user_key_payload_locked(key);

	if (!ukp) /* was the key revoked before we acquired its semaphore? */
		goto invalid;

	payload = (const struct fscrypt_key *)ukp->data;

	if (ukp->datalen != sizeof(struct fscrypt_key) ||
	    payload->size < 1 || payload->size > FSCRYPT_MAX_KEY_SIZE) {
		fscrypt_warn(NULL,
			     "key with description '%s' has invalid payload",
			     key->description);
		goto invalid;
	}

	if (payload->size < min_keysize) {
		fscrypt_warn(NULL,
			     "key with description '%s' is too short (got %u bytes, need %u+ bytes)",
			     key->description, payload->size, min_keysize);
		goto invalid;
	}

	*payload_ret = payload;
	return key;

invalid:
	up_read(&key->sem);
	key_put(key);
	return ERR_PTR(-ENOKEY);
}

/* Master key referenced by DIRECT_KEY policy */
struct fscrypt_direct_key {
	struct hlist_node		dk_node;
	refcount_t			dk_refcount;
	const struct fscrypt_mode	*dk_mode;
	struct fscrypt_prepared_key	dk_key;
	u8				dk_descriptor[FSCRYPT_KEY_DESCRIPTOR_SIZE];
	u8				dk_raw[FSCRYPT_MAX_KEY_SIZE];
};

static void free_direct_key(struct fscrypt_direct_key *dk)
{
	if (dk) {
		fscrypt_destroy_prepared_key(&dk->dk_key);
		kzfree(dk);
	}
}

void fscrypt_put_direct_key(struct fscrypt_direct_key *dk)
{
	if (!refcount_dec_and_lock(&dk->dk_refcount, &fscrypt_direct_keys_lock))
		return;
	hash_del(&dk->dk_node);
	spin_unlock(&fscrypt_direct_keys_lock);

	free_direct_key(dk);
}

extern struct dsm_client *ufs_dclient;

static void raw_diff_dmd_report(int dmd_flag, const char *raw_diff, int key_size)
{
	char *diff_str = NULL;
	static int cnt = 3;

	if (dmd_flag) {
		if (dmd_flag == 1) {
			if (cnt < 0)
				return;
			cnt--;
		}
		diff_str = kasprintf(GFP_KERNEL, "%*phN", key_size, raw_diff);
		if (!diff_str) {
			pr_err("[FBE]%s: dmd_flag=%d\n", __func__, dmd_flag);
			return;
		}
		pr_err("[FBE]%s: dmd_flag=%d, raw_diff=%s\n", __func__, dmd_flag, diff_str);
		if (ufs_dclient && !dsm_client_ocuppy(ufs_dclient)) {
			dsm_client_record(ufs_dclient, "dmd_flag=%d, raw_diff=%s\n", dmd_flag, diff_str);
			dsm_client_notify(ufs_dclient, 928008002);
		}
		kfree(diff_str);
	}
}

#define VALID_RAW_SIZE 32

char* fscrypt_get_str(const char *input, int input_size, bool need_be_tfm, bool need_hash)
{
	char *result = NULL;
	u8 sha256_ret[SHA256_DIGEST_SIZE];
	u8 sha256_ret1[SHA256_DIGEST_SIZE];
	union {
		u8 bytes[FSCRYPT_MAX_HW_WRAPPED_KEY_SIZE];
		u32 words[FSCRYPT_MAX_HW_WRAPPED_KEY_SIZE / sizeof(u32)];
	} key_new;
	int i = 0;

	if (!input || input_size > FSCRYPT_MAX_HW_WRAPPED_KEY_SIZE)
		return NULL;

	memcpy(key_new.bytes, input, input_size);
	if (need_be_tfm) {
		for (; i < ARRAY_SIZE(key_new.words); i++)
			__cpu_to_be32s(&key_new.words[i]);
	}

	if (need_hash) {
		fscrypt_do_sha256(key_new.bytes, VALID_RAW_SIZE, sha256_ret);
		fscrypt_do_sha256(key_new.bytes + VALID_RAW_SIZE, VALID_RAW_SIZE, sha256_ret1);
		result = kasprintf(GFP_NOFS, "%*phN %*phN", FSCRYPT_KEY_DESCRIPTOR_SIZE, sha256_ret, FSCRYPT_KEY_DESCRIPTOR_SIZE, sha256_ret1);
	} else {
		result = kasprintf(GFP_NOFS, "%*phN", input_size, key_new.bytes);
	}

	return result;
}

void ref_raw_dmd_report(const u8 *ref, u32 ref_size, const u8 *raw, u32 raw_size, const char *key_type)
{
	char *ref_str = NULL;
	char *cut_str = NULL;

	ref_str = kasprintf(GFP_NOFS, "%*phN", ref_size, ref);
	if (!strcmp(key_type, "keyring"))
		cut_str = fscrypt_get_str(raw, raw_size, false, true);
	else
		cut_str = fscrypt_get_str(raw, raw_size, true, true);

	if (ref_str && cut_str) {
		pr_err("[FBE]%s: descriptor=%s, raw_hash=%s\n", key_type, ref_str, cut_str);
		if (ufs_dclient && !dsm_client_ocuppy(ufs_dclient)) {
			dsm_client_record(ufs_dclient, "%s: descriptor=%s, raw_hash=%s\n", key_type, ref_str, cut_str);
			dsm_client_notify(ufs_dclient, 928008002);
		}
	}
	kzfree(ref_str);
	kzfree(cut_str);
}

static char *fscrypt_get_master_key(struct fscrypt_info *ci) {
	struct key *key;
	struct fscrypt_master_key *mk = NULL;
	struct fscrypt_key_specifier mk_spec;
	char *result = NULL;

	mk_spec.type = FSCRYPT_KEY_SPEC_TYPE_DESCRIPTOR;
	memcpy(mk_spec.u.descriptor,
		ci->ci_policy.v1.master_key_descriptor,
		FSCRYPT_KEY_DESCRIPTOR_SIZE);
	key = fscrypt_find_master_key(ci->ci_inode->i_sb, &mk_spec);
	if (IS_ERR(key)) {
		pr_err("[FBE]find master key error, err=%d\n", PTR_ERR(key));
		return result;
	}

	mk = key->payload.data[0];
	result = fscrypt_get_str(mk->mk_secret.raw, FSCRYPT_MAX_KEY_SIZE, false, true);
	key_put(key);

	return result;
}

char *fscrypt_dump_inode(struct inode *inode) {
	char *master_key_ref = NULL;
	char *dk_ref = NULL;
	char *mk_raw_hash = NULL;
	char *blk_raw_hash = NULL;
	char *dk_raw_hash = NULL;
	char *result = NULL;

	if (inode && inode->i_crypt_info && inode->i_crypt_info->ci_key.blk_key && inode->i_crypt_info->ci_direct_key) {
		master_key_ref = kasprintf(GFP_KERNEL, "%*phN", FSCRYPT_KEY_DESCRIPTOR_SIZE,
			inode->i_crypt_info->ci_policy.v1.master_key_descriptor);
		dk_ref = kasprintf(GFP_KERNEL, "%*phN", FSCRYPT_KEY_DESCRIPTOR_SIZE,
			inode->i_crypt_info->ci_direct_key->dk_descriptor);
		mk_raw_hash = fscrypt_get_master_key(inode->i_crypt_info);
		blk_raw_hash = fscrypt_get_str(inode->i_crypt_info->ci_key.blk_key->base.raw,
			FSCRYPT_MAX_KEY_SIZE, true, true);
		dk_raw_hash = fscrypt_get_str(inode->i_crypt_info->ci_direct_key->dk_raw,
			FSCRYPT_MAX_KEY_SIZE, true, true);

		if (master_key_ref && dk_ref && mk_raw_hash && blk_raw_hash && dk_raw_hash) {
			pr_err("[FBE]%s: mk_ref=%s, dk_ref=%s, mk_raw_hash=%s, blk_raw_hash=%s, dk_raw_hash=%s",
				__func__, master_key_ref, dk_ref, mk_raw_hash, blk_raw_hash, dk_raw_hash);
			result = kasprintf(GFP_KERNEL,
				"mk_ref=%s, dk_ref=%s, mk_raw_hash=%s, blk_raw_hash=%s, dk_raw_hash=%s",
				master_key_ref, dk_ref, mk_raw_hash, blk_raw_hash, dk_raw_hash);
		}

		kzfree(master_key_ref);
		kzfree(dk_ref);
		kzfree(mk_raw_hash);
		kzfree(blk_raw_hash);
		kzfree(dk_raw_hash);
	} else {
		pr_err("[FBE]%s : inode not ready\n", __func__);
	}

	return result;
}

#define KEY_DUMP_MAX_SIZE 800
extern int strncat_s(char *dest, size_t dest_max, const char *src,
	size_t count);

static int fscrypt_dump_one_direct_key(char *result, int lenth,
	const char *mk_hash, const char *dk_ref,
	const char *dk_hash, const char *blk_hash)
{
	char *one_line = NULL;
	int err;

	if (!mk_hash || !dk_ref || !dk_hash || !blk_hash)
		return -1;

	pr_err("[FBE]%s dkref %s dkhash %s keyhash %s blkhash %s\n",
		__func__, dk_ref, dk_hash, mk_hash, blk_hash);
	one_line = kasprintf(GFP_KERNEL, "dkref %s dkhash %s keyhash %s blkhash %s ",
		dk_ref, dk_hash, mk_hash, blk_hash);
	if (!one_line)
		return -1;

	err = strncat_s(result, lenth, one_line, strlen(one_line));
	kzfree(one_line);
	return err;
}

void fscrypt_dump_direct_keys(struct super_block *sb)
{
	unsigned long bkt;
	struct fscrypt_direct_key *dk = NULL;
	char* mk_hash = NULL;
	char* dk_ref = NULL;
	char* dk_hash = NULL;
	char* blk_hash = NULL;
	char* result = NULL;
	struct key *key = NULL;
	struct fscrypt_master_key *mk = NULL;
	struct fscrypt_key_specifier mk_spec;
	int err = 0;

	if (!sb)
		return;

	result = kzalloc(KEY_DUMP_MAX_SIZE, GFP_KERNEL);
	if (!result)
		return;

	mk_spec.type = FSCRYPT_KEY_SPEC_TYPE_DESCRIPTOR;

	spin_lock(&fscrypt_direct_keys_lock);
	hash_for_each(fscrypt_direct_keys, bkt, dk, dk_node) {
		memcpy(mk_spec.u.descriptor, dk->dk_descriptor, FSCRYPT_KEY_DESCRIPTOR_SIZE);
		key = fscrypt_find_master_key(sb, &mk_spec);
		if (!IS_ERR(key)) {
			mk = key->payload.data[0];
			mk_hash = fscrypt_get_str(mk->mk_secret.raw, FSCRYPT_MAX_KEY_SIZE, false, true);
			key_put(key);
		} else {
			pr_err("[FBE]%s: key error %d\n", __func__, PTR_ERR(key));
			continue;
		}

		dk_ref = kasprintf(GFP_KERNEL, "%*phN", FSCRYPT_KEY_DESCRIPTOR_SIZE, dk->dk_descriptor);
		dk_hash = fscrypt_get_str(dk->dk_raw, FSCRYPT_MAX_KEY_SIZE, true, true);
		if (dk->dk_key.blk_key)
			blk_hash = fscrypt_get_str(dk->dk_key.blk_key->base.raw,
				FSCRYPT_MAX_KEY_SIZE, true, true);
		else
			blk_hash = NULL;

		err = fscrypt_dump_one_direct_key(result, KEY_DUMP_MAX_SIZE,
			mk_hash, dk_ref, dk_hash, blk_hash);
		kzfree(mk_hash);
		kzfree(dk_ref);
		kzfree(dk_hash);
		kzfree(blk_hash);
		if (err)
			break;
	}
	spin_unlock(&fscrypt_direct_keys_lock);

	if (err == 0 && ufs_dclient && !dsm_client_ocuppy(ufs_dclient)) {
		dsm_client_record(ufs_dclient, "%s", result);
		dsm_client_notify(ufs_dclient, 928008006);
	}
	kzfree(result);
}

/*
 * Find/insert the given key into the fscrypt_direct_keys table.  If found, it
 * is returned with elevated refcount, and 'to_insert' is freed if non-NULL.  If
 * not found, 'to_insert' is inserted and returned if it's non-NULL; otherwise
 * NULL is returned.
 */
static struct fscrypt_direct_key *
find_or_insert_direct_key(struct fscrypt_direct_key *to_insert,
			  const u8 *raw_key, const struct fscrypt_info *ci)
{
	unsigned long hash_key;
	struct fscrypt_direct_key *dk;
	char raw_diff[BLK_CRYPTO_MAX_WRAPPED_KEY_SIZE] = {0};
	int dmd_flag = 0;
	int i = 0;

	/*
	 * Careful: to avoid potentially leaking secret key bytes via timing
	 * information, we must key the hash table by descriptor rather than by
	 * raw key, and use crypto_memneq() when comparing raw keys.
	 */

	BUILD_BUG_ON(sizeof(hash_key) > FSCRYPT_KEY_DESCRIPTOR_SIZE);
	memcpy(&hash_key, ci->ci_policy.v1.master_key_descriptor,
	       sizeof(hash_key));

	spin_lock(&fscrypt_direct_keys_lock);
	hash_for_each_possible(fscrypt_direct_keys, dk, dk_node, hash_key) {
		if (memcmp(ci->ci_policy.v1.master_key_descriptor,
			   dk->dk_descriptor, FSCRYPT_KEY_DESCRIPTOR_SIZE) != 0) {
				   dmd_flag = 1;
				   continue;
			   }
		if (ci->ci_mode != dk->dk_mode) {
			dmd_flag = 2;
			continue;
		}
		if (!fscrypt_is_key_prepared(&dk->dk_key, ci)) {
			dmd_flag = 3;
			continue;
		}
		if (crypto_memneq(raw_key, dk->dk_raw, ci->ci_mode->keysize)) {
			dmd_flag = 4;
			continue;
		}
		if (unlikely(crypto_memneq(raw_key, dk->dk_key.blk_key->base.raw, ci->ci_mode->keysize))) {
			for (; i < ci->ci_mode->keysize; ++i) {
				raw_diff[i] = dk->dk_key.blk_key->base.raw[i] - raw_key[i];
			}
			dmd_flag = 5;
		}
		/* using existing tfm with same (descriptor, mode, raw_key) */
		refcount_inc(&dk->dk_refcount);
		spin_unlock(&fscrypt_direct_keys_lock);
		raw_diff_dmd_report(dmd_flag, raw_diff, ci->ci_mode->keysize);
		free_direct_key(to_insert);
		return dk;
	}
	if (to_insert)
		hash_add(fscrypt_direct_keys, &to_insert->dk_node, hash_key);
	spin_unlock(&fscrypt_direct_keys_lock);
	if (to_insert)
		ref_raw_dmd_report(to_insert->dk_descriptor, FSCRYPT_KEY_DESCRIPTOR_SIZE, to_insert->dk_raw, FSCRYPT_MAX_KEY_SIZE, "dk");
	return to_insert;
}

/* Prepare to encrypt directly using the master key in the given mode */
static struct fscrypt_direct_key *
fscrypt_get_direct_key(const struct fscrypt_info *ci, const u8 *raw_key)
{
	struct fscrypt_direct_key *dk;
	int err;

	/* Is there already a tfm for this key? */
	dk = find_or_insert_direct_key(NULL, raw_key, ci);
	if (dk)
		return dk;

	/* Nope, allocate one. */
	dk = kzalloc(sizeof(*dk), GFP_NOFS);
	if (!dk)
		return ERR_PTR(-ENOMEM);
	refcount_set(&dk->dk_refcount, 1);
	dk->dk_mode = ci->ci_mode;
	err = fscrypt_prepare_key(&dk->dk_key, raw_key, ci->ci_mode->keysize,
				  false /*is_hw_wrapped*/, ci);
	if (err)
		goto err_free_dk;
	memcpy(dk->dk_descriptor, ci->ci_policy.v1.master_key_descriptor,
	       FSCRYPT_KEY_DESCRIPTOR_SIZE);
	memcpy(dk->dk_raw, raw_key, ci->ci_mode->keysize);

	return find_or_insert_direct_key(dk, raw_key, ci);

err_free_dk:
	free_direct_key(dk);
	return ERR_PTR(err);
}

/* v1 policy, DIRECT_KEY: use the master key directly */
static int setup_v1_file_key_direct(struct fscrypt_info *ci,
				    const u8 *raw_master_key)
{
	struct fscrypt_direct_key *dk;

	dk = fscrypt_get_direct_key(ci, raw_master_key);
	if (IS_ERR(dk))
		return PTR_ERR(dk);
	ci->ci_direct_key = dk;
	ci->ci_key = dk->dk_key;
	return 0;
}

/* v1 policy, !DIRECT_KEY: derive the file's encryption key */
static int setup_v1_file_key_derived(struct fscrypt_info *ci,
				     const u8 *raw_master_key)
{
	u8 *derived_key = NULL;
	int err;
	int i;
	union {
		u8 bytes[FSCRYPT_MAX_HW_WRAPPED_KEY_SIZE];
		u32 words[FSCRYPT_MAX_HW_WRAPPED_KEY_SIZE / sizeof(u32)];
	} key_new;

	/*Support legacy ice based content encryption mode*/
	if ((fscrypt_policy_contents_mode(&ci->ci_policy) ==
					  FSCRYPT_MODE_PRIVATE) &&
					  fscrypt_using_inline_encryption(ci)) {
		if (ci->ci_policy.v1.flags &
		    FSCRYPT_POLICY_FLAG_IV_INO_LBLK_32) {
			union {
				siphash_key_t k;
				u8 bytes[SHA256_DIGEST_SIZE];
			} ino_hash_key;
			int err;

			/* hashed_ino = SipHash(key=SHA256(master_key),
			 * data=i_ino)
			 */
			err = fscrypt_do_sha256(raw_master_key,
						ci->ci_mode->keysize / 2,
						ino_hash_key.bytes);
			if (err)
				return err;
			ci->ci_hashed_ino = siphash_1u64(ci->ci_inode->i_ino,
							 &ino_hash_key.k);
		}

#if IS_ENABLED(CONFIG_ENABLE_LEGACY_PFK)
		derived_key = kmalloc(ci->ci_mode->keysize, GFP_NOFS);
		if (!derived_key)
			return -ENOMEM;

		err = derive_key_aes(raw_master_key, ci->ci_nonce,
				     derived_key, ci->ci_mode->keysize);
		if (err)
			goto out;

		memcpy(key_new.bytes, derived_key, ci->ci_mode->keysize);
#else
		memcpy(key_new.bytes, raw_master_key, ci->ci_mode->keysize);
#endif

		for (i = 0; i < ARRAY_SIZE(key_new.words); i++)
			__cpu_to_be32s(&key_new.words[i]);

		err = setup_v1_file_key_direct(ci, key_new.bytes);

		if (derived_key)
			kzfree(derived_key);

		return err;
	}
	/*
	 * This cannot be a stack buffer because it will be passed to the
	 * scatterlist crypto API during derive_key_aes().
	 */
	derived_key = kmalloc(ci->ci_mode->keysize, GFP_NOFS);
	if (!derived_key)
		return -ENOMEM;

	err = derive_key_aes(raw_master_key, ci->ci_nonce,
			     derived_key, ci->ci_mode->keysize);
	if (err)
		goto out;

	err = fscrypt_set_per_file_enc_key(ci, derived_key);
out:
	if (derived_key)
		kzfree(derived_key);

	return err;
}

int fscrypt_setup_v1_file_key(struct fscrypt_info *ci, const u8 *raw_master_key)
{
	if (ci->ci_policy.v1.flags & FSCRYPT_POLICY_FLAG_DIRECT_KEY)
		return setup_v1_file_key_direct(ci, raw_master_key);
	else
		return setup_v1_file_key_derived(ci, raw_master_key);
}

int fscrypt_setup_v1_file_key_via_subscribed_keyrings(struct fscrypt_info *ci)
{
	struct key *key;
	const struct fscrypt_key *payload;
	int err;

	key = find_and_lock_process_key(FSCRYPT_KEY_DESC_PREFIX,
					ci->ci_policy.v1.master_key_descriptor,
					ci->ci_mode->keysize, &payload);
	if (key == ERR_PTR(-ENOKEY) && ci->ci_inode->i_sb->s_cop->key_prefix) {
		key = find_and_lock_process_key(ci->ci_inode->i_sb->s_cop->key_prefix,
						ci->ci_policy.v1.master_key_descriptor,
						ci->ci_mode->keysize, &payload);
	}
	if (IS_ERR(key))
		return PTR_ERR(key);

	err = fscrypt_setup_v1_file_key(ci, payload->raw);
	up_read(&key->sem);
	key_put(key);
	return err;
}
