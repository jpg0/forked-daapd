/*
 * Copyright (C) 2010 Julien BLACHE <jb@jblache.org>
 *
 * RAOP AirTunes v2
 *
 * Crypto code adapted from VideoLAN
 *   Copyright (C) 2008 the VideoLAN team
 *   Author: Michael Hanselmann
 *   GPLv2+
 *
 * ALAC encoding adapted from raop_play
 *   Copyright (C) 2005 Shiro Ninomiya <shiron@snino.com>
 *   GPLv2+
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/* TODO:
 * - Support RTSP authentication in all requests (only OPTIONS so far)
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <inttypes.h>
#include <math.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <fcntl.h>
#include <time.h>
#include <endian.h>

#include <arpa/inet.h>

#include <event.h>
#include "evrtsp/evrtsp.h"

#include <gcrypt.h>

#include "conffile.h"
#include "logger.h"
#include "misc.h"
#include "player.h"
#include "raop.h"

#ifndef MIN
# define MIN(a, b) ((a < b) ? a : b)
#endif

#define AIRTUNES_V2_HEADER  12
#define ALAC_HEADER         3


struct raop_v2_packet
{
  uint8_t data[AIRTUNES_V2_HEADER + ALAC_HEADER + STOB(AIRTUNES_V2_PACKET_SAMPLES)];
};

struct raop_session
{
  struct evrtsp_connection *ctrl;

  enum raop_session_state state;
  int req_in_flight;
  int req_has_auth;

  int cseq;
  char *session;
  char session_url[128];

  char *realm;
  char *nonce;
  const char *password;

  char *devname;
  char *address;

  uint64_t start_rtptime;

  /* Do not dereference - only passed to the status cb */
  struct raop_device *dev;
  raop_status_cb status_cb;

  /* AirTunes v2 */
  unsigned short server_port;
  unsigned short control_port;
  unsigned short timing_port;

  int server_fd;

  union sockaddr_all sa;

  struct raop_session *next;
};

struct raop_deferred_eh
{
  struct event ev;
  struct raop_session *session;
};

typedef void (*evrtsp_req_cb)(struct evrtsp_request *req, void *arg);

/* Truncate RTP time to lower 32bits for RAOP */
#define RAOP_RTPTIME(x) ((uint32_t)((x) & (uint64_t)0xffffffff))

/* NTP timestamp definitions */
#define FRAC             4294967296. /* 2^32 as a double */
#define NTP_EPOCH_DELTA  0x83aa7e80  /* 2208988800 - that's 1970 - 1900 in seconds */

struct ntp_stamp
{
  uint32_t sec;
  uint32_t frac;
};


static const uint8_t raop_rsa_pubkey[] =
  "\xe7\xd7\x44\xf2\xa2\xe2\x78\x8b\x6c\x1f\x55\xa0\x8e\xb7\x05\x44"
  "\xa8\xfa\x79\x45\xaa\x8b\xe6\xc6\x2c\xe5\xf5\x1c\xbd\xd4\xdc\x68"
  "\x42\xfe\x3d\x10\x83\xdd\x2e\xde\xc1\xbf\xd4\x25\x2d\xc0\x2e\x6f"
  "\x39\x8b\xdf\x0e\x61\x48\xea\x84\x85\x5e\x2e\x44\x2d\xa6\xd6\x26"
  "\x64\xf6\x74\xa1\xf3\x04\x92\x9a\xde\x4f\x68\x93\xef\x2d\xf6\xe7"
  "\x11\xa8\xc7\x7a\x0d\x91\xc9\xd9\x80\x82\x2e\x50\xd1\x29\x22\xaf"
  "\xea\x40\xea\x9f\x0e\x14\xc0\xf7\x69\x38\xc5\xf3\x88\x2f\xc0\x32"
  "\x3d\xd9\xfe\x55\x15\x5f\x51\xbb\x59\x21\xc2\x01\x62\x9f\xd7\x33"
  "\x52\xd5\xe2\xef\xaa\xbf\x9b\xa0\x48\xd7\xb8\x13\xa2\xb6\x76\x7f"
  "\x6c\x3c\xcf\x1e\xb4\xce\x67\x3d\x03\x7b\x0d\x2e\xa3\x0c\x5f\xff"
  "\xeb\x06\xf8\xd0\x8a\xdd\xe4\x09\x57\x1a\x9c\x68\x9f\xef\x10\x72"
  "\x88\x55\xdd\x8c\xfb\x9a\x8b\xef\x5c\x89\x43\xef\x3b\x5f\xaa\x15"
  "\xdd\xe6\x98\xbe\xdd\xf3\x59\x96\x03\xeb\x3e\x6f\x61\x37\x2b\xb6"
  "\x28\xf6\x55\x9f\x59\x9a\x78\xbf\x50\x06\x87\xaa\x7f\x49\x76\xc0"
  "\x56\x2d\x41\x29\x56\xf8\x98\x9e\x18\xa6\x35\x5b\xd8\x15\x97\x82"
  "\x5e\x0f\xc8\x75\x34\x3e\xc7\x82\x11\x76\x25\xcd\xbf\x98\x44\x7b";

static const uint8_t raop_rsa_exp[] = "\x01\x00\x01";


/* From player.c */
extern struct event_base *evbase_player;

/* RAOP AES stream key */
static uint8_t raop_aes_key[16];
static uint8_t raop_aes_iv[16];
static gcry_cipher_hd_t raop_aes_ctx;

/* Base64-encoded AES key and IV for SDP */
static char *raop_aes_key_b64;
static char *raop_aes_iv_b64;

/* AirTunes v2 time synchronization */
static int timing_fd;
static unsigned short timing_port;
static struct event timing_ev;

/* AirTunes v2 playback synchronization */
static int control_fd;
static unsigned short control_port;
static int sync_counter;

/* AirTunes v2 audio stream */
static uint16_t stream_seq;

/* Volume */
static double raop_volume;

/* FLUSH timer */
static struct event flush_timer;

/* Sessions */
static struct raop_session *sessions;


/* ALAC bits writer - big endian
 * p    outgoing buffer pointer
 * val  bitfield value
 * blen bitfield length, max 8 bits
 * bpos bit position in the current byte (pointed by *p)
 */
static inline void
alac_write_bits(uint8_t **p, uint8_t val, int blen, int *bpos)
{
  int lb;
  int rb;
  int bd;

  /* Remaining bits in the current byte */
  lb = 7 - *bpos + 1;

  /* Number of bits overflowing */
  rb = lb - blen;

  if (rb >= 0)
    {
      bd = val << rb;
      if (*bpos == 0)
	**p = bd;
      else
	**p |= bd;

      /* No over- nor underflow, we're done with this byte */
      if (rb == 0)
	{
	  *p += 1;
	  *bpos = 0;
	}
      else
	*bpos += blen;
    }
  else
    {
      /* Fill current byte */
      bd = val >> -rb;
      **p |= bd;

      /* Overflow goes to the next byte */
      *p += 1;
      **p = val << (8 + rb);
      *bpos = -rb;
    }
}

/* Raw data must be little endian */
static void
alac_encode(uint8_t *raw, uint8_t *buf, int buflen)
{
  uint8_t *maxraw;
  int bpos;

  bpos = 0;
  maxraw = raw + buflen;

  alac_write_bits(&buf, 1, 3, &bpos); /* channel=1, stereo */
  alac_write_bits(&buf, 0, 4, &bpos); /* unknown */
  alac_write_bits(&buf, 0, 8, &bpos); /* unknown */
  alac_write_bits(&buf, 0, 4, &bpos); /* unknown */
  alac_write_bits(&buf, 0, 1, &bpos); /* hassize */

  alac_write_bits(&buf, 0, 2, &bpos); /* unused */
  alac_write_bits(&buf, 1, 1, &bpos); /* is-not-compressed */

  for (; raw < maxraw; raw += 4)
    {
      /* Byteswap to big endian */
      alac_write_bits(&buf, *(raw + 1), 8, &bpos);
      alac_write_bits(&buf, *raw, 8, &bpos);
      alac_write_bits(&buf, *(raw + 3), 8, &bpos);
      alac_write_bits(&buf, *(raw + 2), 8, &bpos);
    }
}

/* AirTunes v2 time synchronization helpers */
static inline void
timespec_to_ntp(struct timespec *ts, struct ntp_stamp *ns)
{
  /* Seconds since NTP Epoch (1900-01-01) */
  ns->sec = ts->tv_sec + NTP_EPOCH_DELTA;

  ns->frac = (uint32_t)((double)ts->tv_nsec * 1e-9 * FRAC);
}

static inline void
ntp_to_timespec(struct ntp_stamp *ns, struct timespec *ts)
{
  /* Seconds since Unix Epoch (1970-01-01) */
  ts->tv_sec = ns->sec - NTP_EPOCH_DELTA;

  ts->tv_nsec = (long)((double)ns->frac / (1e-9 * FRAC));
}

static inline int
raop_v2_timing_get_clock_ntp(struct ntp_stamp *ns)
{
  struct timespec ts;
  int ret;

  ret = clock_gettime(CLOCK_MONOTONIC, &ts);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Couldn't get clock: %s\n", strerror(errno));

      return -1;
    }

  timespec_to_ntp(&ts, ns);

  return 0;
}


/* RAOP crypto stuff - from VLC */
/* MGF1 is specified in RFC2437, section 10.2.1. Variables are named after the
 * specification.
 */
static int
raop_crypt_mgf1(uint8_t *mask, size_t l, const uint8_t *z, const size_t zlen, const int hash)
{
  char ebuf[64];
  gcry_md_hd_t md_hdl;
  gpg_error_t gc_err;
  uint8_t *md;
  uint32_t counter;
  uint8_t c[4];
  size_t copylen;
  int len;

  gc_err = gcry_md_open(&md_hdl, hash, 0);
  if (gc_err != GPG_ERR_NO_ERROR)
    {
      gpg_strerror_r(gc_err, ebuf, sizeof(ebuf));
      DPRINTF(E_LOG, L_RAOP, "Could not open hash: %s\n", ebuf);

      return -1;
    }

  len = gcry_md_get_algo_dlen(hash);

  counter = 0;
  while (l > 0)
    {
      /* 3. For counter from 0 to \lceil{l / len}\rceil-1, do the following:
       * a. Convert counter to an octet string C of length 4 with the
       *    primitive I2OSP: C = I2OSP (counter, 4)
       */
      c[0] = (counter >> 24) & 0xff;
      c[1] = (counter >> 16) & 0xff;
      c[2] = (counter >> 8) & 0xff;
      c[3] = counter & 0xff;
      ++counter;

      /* b. Concatenate the hash of the seed z and c to the octet string T:
       *    T = T || Hash (Z || C)
       */
      gcry_md_reset(md_hdl);
      gcry_md_write(md_hdl, z, zlen);
      gcry_md_write(md_hdl, c, 4);
      md = gcry_md_read(md_hdl, hash);

      /* 4. Output the leading l octets of T as the octet string mask. */
      copylen = MIN(l, len);
      memcpy(mask, md, copylen);
      mask += copylen;
      l -= copylen;
    }

  gcry_md_close(md_hdl);

  return 0;
}

/* EME-OAEP-ENCODE is specified in RFC2437, section 9.1.1.1. Variables are
 * named after the specification.
 */
static int
raop_crypt_add_oaep_padding(uint8_t *em, const size_t emlen, const uint8_t *m, const size_t mlen, const uint8_t *p, const size_t plen)
{
  uint8_t *seed;
  uint8_t *db;
  uint8_t *db_mask;
  uint8_t *seed_mask;
  size_t emlen_max;
  size_t pslen;
  size_t i;
  int hlen;
  int ret;

  /* Space for 0x00 prefix in EM. */
  emlen_max = emlen - 1;

  hlen = gcry_md_get_algo_dlen(GCRY_MD_SHA1);

  /* Step 2:
   * If ||M|| > emLen-2hLen-1 then output "message too long" and stop.
   */
  if (mlen > (emlen_max - (2 * hlen) - 1))
    {
      DPRINTF(E_LOG, L_RAOP, "Could not add OAEP padding: message too long\n");

      return -1;
    }

  /* Step 3:
   * Generate an octet string PS consisting of emLen-||M||-2hLen-1 zero
   * octets. The length of PS may be 0.
   */
  pslen = emlen_max - mlen - (2 * hlen) - 1;

  /*
   * Step 5:
   * Concatenate pHash, PS, the message M, and other padding to form a data
   * block DB as: DB = pHash || PS || 01 || M
   */
  db = calloc(1, hlen + pslen + 1 + mlen);
  db_mask = calloc(1, emlen_max - hlen);
  seed_mask = calloc(1, hlen);

  if (!db || !db_mask || !seed_mask)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not allocate memory for OAEP padding\n");

      if (db)
	free(db);
      if (db_mask)
	free(db_mask);
      if (seed_mask)
	free(seed_mask);

      return -1;
    }

  /* Step 4:
   * Let pHash = Hash(P), an octet string of length hLen.
   */
  gcry_md_hash_buffer(GCRY_MD_SHA1, db, p, plen);

  /* Step 3:
   * Generate an octet string PS consisting of emLen-||M||-2hLen-1 zero
   * octets. The length of PS may be 0.
   */
  memset(db + hlen, 0, pslen);

  /* Step 5:
   * Concatenate pHash, PS, the message M, and other padding to form a data
   * block DB as: DB = pHash || PS || 01 || M
   */
  db[hlen + pslen] = 0x01;
  memcpy(db + hlen + pslen + 1, m, mlen);

  /* Step 6:
   * Generate a random octet string seed of length hLen
   */
  seed = gcry_random_bytes(hlen, GCRY_STRONG_RANDOM);
  if (!seed)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not allocate memory for OAEP seed\n");

      ret = -1;
      goto out_free_alloced;
    }

  /* Step 7:
   * Let dbMask = MGF(seed, emLen-hLen).
   */
  ret = raop_crypt_mgf1(db_mask, emlen_max - hlen, seed, hlen, GCRY_MD_SHA1);
  if (ret < 0)
    goto out_free_all;

  /* Step 8:
   * Let maskedDB = DB \xor dbMask.
   */
  for (i = 0; i < (emlen_max - hlen); i++)
    db[i] ^= db_mask[i];

  /* Step 9:
   * Let seedMask = MGF(maskedDB, hLen).
   */
  ret = raop_crypt_mgf1(seed_mask, hlen, db, emlen_max - hlen, GCRY_MD_SHA1);
  if (ret < 0)
    goto out_free_all;

  /* Step 10:
   * Let maskedSeed = seed \xor seedMask.
   */
  for (i = 0; i < hlen; i++)
    seed[i] ^= seed_mask[i];

  /* Step 11:
   * Let EM = maskedSeed || maskedDB.
   */
  em[0] = 0x00;
  memcpy(em + 1, seed, hlen);
  memcpy(em + 1 + hlen, db, hlen + pslen + 1 + mlen);

  /* Step 12:
   * Output EM.
   */

  ret = 0;

 out_free_all:
  free(seed);
 out_free_alloced:
  free(db);
  free(db_mask);
  free(seed_mask);

  return ret;
}

static char *
raop_crypt_encrypt_aes_key_base64(void)
{
  char ebuf[64];
  uint8_t padded_key[256];
  gpg_error_t gc_err;
  gcry_sexp_t sexp_rsa_params;
  gcry_sexp_t sexp_input;
  gcry_sexp_t sexp_encrypted;
  gcry_sexp_t sexp_token_a;
  gcry_mpi_t mpi_pubkey;
  gcry_mpi_t mpi_exp;
  gcry_mpi_t mpi_input;
  gcry_mpi_t mpi_output;
  char *result;
  uint8_t *value;
  size_t value_size;
  int ret;

  result = NULL;

  /* Add RSA-OAES-SHA1 padding */
  ret = raop_crypt_add_oaep_padding(padded_key, sizeof(padded_key), raop_aes_key, sizeof(raop_aes_key), NULL, 0);
  if (ret < 0)
    return NULL;

  /* Read public key */
  gc_err = gcry_mpi_scan(&mpi_pubkey, GCRYMPI_FMT_USG, raop_rsa_pubkey, sizeof(raop_rsa_pubkey) - 1, NULL);
  if (gc_err != GPG_ERR_NO_ERROR)
    {
      gpg_strerror_r(gc_err, ebuf, sizeof(ebuf));
      DPRINTF(E_LOG, L_RAOP, "Could not read RAOP RSA pubkey: %s\n", ebuf);

      return NULL;
    }

  /* Read exponent */
  gc_err = gcry_mpi_scan(&mpi_exp, GCRYMPI_FMT_USG, raop_rsa_exp, sizeof(raop_rsa_exp) - 1, NULL);
  if (gc_err != GPG_ERR_NO_ERROR)
    {
      gpg_strerror_r(gc_err, ebuf, sizeof(ebuf));
      DPRINTF(E_LOG, L_RAOP, "Could not read RAOP RSA exponent: %s\n", ebuf);

      goto out_free_mpi_pubkey;
    }

  /* If the input data starts with a set bit (0x80), gcrypt thinks it's a
   * signed integer and complains. Prefixing it with a zero byte (\0)
   * works, but involves more work. Converting it to an MPI in our code is
   * cleaner.
   */
  gc_err = gcry_mpi_scan(&mpi_input, GCRYMPI_FMT_USG, padded_key, sizeof(padded_key), NULL);
  if (gc_err != GPG_ERR_NO_ERROR)
    {
      gpg_strerror_r(gc_err, ebuf, sizeof(ebuf));
      DPRINTF(E_LOG, L_RAOP, "Could not convert input data: %s\n", ebuf);

      goto out_free_mpi_exp;
    }

  /* Build S-expression with RSA parameters */
  gc_err = gcry_sexp_build(&sexp_rsa_params, NULL, "(public-key(rsa(n %m)(e %m)))", mpi_pubkey, mpi_exp);
  if (gc_err != GPG_ERR_NO_ERROR)
    {
      gpg_strerror_r(gc_err, ebuf, sizeof(ebuf));
      DPRINTF(E_LOG, L_RAOP, "Could not build RSA params S-exp: %s\n", ebuf);

      goto out_free_mpi_input;
    }

  /* Build S-expression for data */
  gc_err = gcry_sexp_build(&sexp_input, NULL, "(data(value %m))", mpi_input);
  if (gc_err != GPG_ERR_NO_ERROR)
    {
      gpg_strerror_r(gc_err, ebuf, sizeof(ebuf));
      DPRINTF(E_LOG, L_RAOP, "Could not build data S-exp: %s\n", ebuf);

      goto out_free_sexp_params;
    }

  /* Encrypt data */
  gc_err = gcry_pk_encrypt(&sexp_encrypted, sexp_input, sexp_rsa_params);
  if (gc_err != GPG_ERR_NO_ERROR)
    {
      gpg_strerror_r(gc_err, ebuf, sizeof(ebuf));
      DPRINTF(E_LOG, L_RAOP, "Could not encrypt data: %s\n", ebuf);

      goto out_free_sexp_input;
    }

  /* Extract encrypted data */
  sexp_token_a = gcry_sexp_find_token(sexp_encrypted, "a", 0);
  if (!sexp_token_a)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not find token 'a' in result S-exp\n");

      goto out_free_sexp_encrypted;
    }

  mpi_output = gcry_sexp_nth_mpi(sexp_token_a, 1, GCRYMPI_FMT_USG);
  if (!mpi_output)
    {
      DPRINTF(E_LOG, L_RAOP, "Cannot extract MPI from result\n");

      goto out_free_sexp_token_a;
    }

  /* Copy encrypted data into char array */
  gc_err = gcry_mpi_aprint(GCRYMPI_FMT_USG, &value, &value_size, mpi_output);
  if (gc_err != GPG_ERR_NO_ERROR)
    {
      gpg_strerror_r(gc_err, ebuf, sizeof(ebuf));
      DPRINTF(E_LOG, L_RAOP, "Could not copy encrypted data: %s\n", ebuf);

      goto out_free_mpi_output;
    }

  /* Encode in Base64 */
  result = b64_encode(value, value_size);

  free(value);

 out_free_mpi_output:
  gcry_mpi_release(mpi_output);
 out_free_sexp_token_a:
  gcry_sexp_release(sexp_token_a);
 out_free_sexp_encrypted:
  gcry_sexp_release(sexp_encrypted);
 out_free_sexp_input:
  gcry_sexp_release(sexp_input);
 out_free_sexp_params:
  gcry_sexp_release(sexp_rsa_params);
 out_free_mpi_input:
  gcry_mpi_release(mpi_input);
 out_free_mpi_exp:
  gcry_mpi_release(mpi_exp);
 out_free_mpi_pubkey:
  gcry_mpi_release(mpi_pubkey);

  return result;
}


/* Helpers */
static void
raop_add_headers(struct raop_session *rs, struct evrtsp_request *req)
{
  char buf[64];

  snprintf(buf, sizeof(buf), "%d", rs->cseq);
  evrtsp_add_header(req->output_headers, "CSeq", buf);

  evrtsp_add_header(req->output_headers, "User-Agent", "forked-daapd/" VERSION);

  snprintf(buf, sizeof(buf), "%" PRIX64, libhash);
  evrtsp_add_header(req->output_headers, "Client-Instance", buf);
  evrtsp_add_header(req->output_headers, "DACP-ID", buf);

  if (rs->session)
    evrtsp_add_header(req->output_headers, "Session", rs->session);

  /* Content-Length added automatically by evrtsp */
}

static int
raop_add_auth(struct raop_session *rs, struct evrtsp_request *req, char *method, char *uri)
{
  char ha1[33];
  char ha2[33];
  char ebuf[64];
  char auth[256];
  uint8_t *hash_bytes;
  size_t hashlen;
  gcry_md_hd_t hd;
  gpg_error_t gc_err;
  int i;
  int ret;

  rs->req_has_auth = 0;

  if (!rs->nonce)
    return 0;

  if (!rs->password)
    {
      DPRINTF(E_LOG, L_RAOP, "Authentication required but no password found for device %s\n", rs->devname);

      return -2;
    }

  gc_err = gcry_md_open(&hd, GCRY_MD_MD5, 0);
  if (gc_err != GPG_ERR_NO_ERROR)
    {
      gpg_strerror_r(gc_err, ebuf, sizeof(ebuf));
      DPRINTF(E_LOG, L_RAOP, "Could not open MD5: %s\n", ebuf);

      return -1;
    }

  memset(ha1, 0, sizeof(ha1));
  memset(ha2, 0, sizeof(ha2));
  hashlen = gcry_md_get_algo_dlen(GCRY_MD_MD5);

  /* HA 1 */
  /* No username */
  gcry_md_write(hd, ":", 1);
  gcry_md_write(hd, rs->realm, strlen(rs->realm));
  gcry_md_write(hd, ":", 1);
  gcry_md_write(hd, rs->password, strlen(rs->password));

  hash_bytes = gcry_md_read(hd, GCRY_MD_MD5);
  if (!hash_bytes)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not read MD5 hash\n");

      return -1;
    }

  for (i = 0; i < hashlen; i++)
    sprintf(ha1 + (2 * i), "%02x", hash_bytes[i]);

  /* RESET */
  gcry_md_reset(hd);

  /* HA 2 */
  gcry_md_write(hd, method, strlen(method));
  gcry_md_write(hd, ":", 1);
  gcry_md_write(hd, uri, strlen(uri));

  hash_bytes = gcry_md_read(hd, GCRY_MD_MD5);
  if (!hash_bytes)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not read MD5 hash\n");

      return -1;
    }

  for (i = 0; i < hashlen; i++)
    sprintf(ha2 + (2 * i), "%02x", hash_bytes[i]);

  /* RESET */
  gcry_md_reset(hd);

  /* Final value */
  gcry_md_write(hd, ha1, 32);
  gcry_md_write(hd, ":", 1);
  gcry_md_write(hd, rs->nonce, strlen(rs->nonce));
  gcry_md_write(hd, ":", 1);
  gcry_md_write(hd, ha2, 32);

  hash_bytes = gcry_md_read(hd, GCRY_MD_MD5);
  if (!hash_bytes)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not read MD5 hash\n");

      return -1;
    }

  for (i = 0; i < hashlen; i++)
    sprintf(ha1 + (2 * i), "%02x", hash_bytes[i]);

  gcry_md_close(hd);

  /* Build header */
  ret = snprintf(auth, sizeof(auth), "Digest username=\"\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", response=\"%s\"",
		 rs->realm, rs->nonce, uri, ha1);
  if ((ret < 0) || (ret >= sizeof(auth)))
    {
      DPRINTF(E_LOG, L_RAOP, "Authorization value header exceeds buffer size\n");

      return -1;
    }

  evrtsp_add_header(req->output_headers, "Authorization", auth);

  DPRINTF(E_DBG, L_RAOP, "Authorization header: %s\n", auth);

  rs->req_has_auth = 1;

  free(rs->realm);
  rs->realm = NULL;

  free(rs->nonce);
  rs->nonce = NULL;

  return 0;
}

static int
raop_parse_auth(struct raop_session *rs, struct evrtsp_request *req)
{
  const char *param;
  char *auth;
  char *token;
  char *ptr;

  if (rs->realm)
    {
      free(rs->realm);
      rs->realm = NULL;
    }

  if (rs->nonce)
    {
      free(rs->nonce);
      rs->nonce = NULL;
    }

  param = evrtsp_find_header(req->input_headers, "WWW-Authenticate");
  if (!param)
    {
      DPRINTF(E_LOG, L_RAOP, "WWW-Authenticate header not found\n");

      return -1;
    }

  if (strncmp(param, "Digest ", strlen("Digest ")) != 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Unsupported authentication method: %s\n", param);

      return -1;
    }

  auth = strdup(param);
  if (!auth)
    {
      DPRINTF(E_LOG, L_RAOP, "Out of memory for WWW-Authenticate header copy\n");

      return -1;
    }

  token = strchr(auth, ' ');
  token++;

  token = strtok_r(token, " =", &ptr);
  while (token)
    {
      if (strcmp(token, "realm") == 0)
	{
	  token = strtok_r(NULL, " =", &ptr);
	  if (!token)
	    break;

	  rs->realm = strndup(token + 1, strlen(token) - 2);
	}
      else if (strcmp(token, "nonce") == 0)
	{
	  token = strtok_r(NULL, " =", &ptr);
	  if (!token)
	    break;

	  rs->nonce = strndup(token + 1, strlen(token) - 2);
	}

      token = strtok_r(NULL, " =", &ptr);
    }

  free(auth);

  if (!rs->realm || !rs->nonce)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not find realm/nonce in WWW-Authenticate header\n");

      if (rs->realm)
	{
	  free(rs->realm);
	  rs->realm = NULL;
	}

      if (rs->nonce)
	{
	  free(rs->nonce);
	  rs->nonce = NULL;
	}

      return -1;
    }

  return 0;
}

static int
raop_check_cseq(struct raop_session *rs, struct evrtsp_request *req)
{
  const char *param;
  int cseq;
  int ret;

  param = evrtsp_find_header(req->input_headers, "CSeq");
  if (!param)
    {
      DPRINTF(E_LOG, L_RAOP, "No CSeq in reply\n");

      return -1;
    }

  ret = safe_atoi32(param, &cseq);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not convert CSeq value to integer (%s)\n", param);

      return -1;
    }

  /* CSeq is always incremented before checking */
  if (cseq == (rs->cseq - 1))
    return 0;

  DPRINTF(E_LOG, L_RAOP, "CSeq in reply does not match last CSeq: got %d expected %d\n", cseq, rs->cseq);

  return -1;
}

static int
raop_make_sdp(struct raop_session *rs, struct evrtsp_request *req, char *address, uint32_t session_id)
{
#define SDP_PLD_FMT							\
  "v=0\r\n"								\
    "o=iTunes %u 0 IN IP4 %s\r\n"					\
    "s=iTunes\r\n"							\
    "c=IN IP4 %s\r\n"							\
    "t=0 0\r\n"								\
    "m=audio 0 RTP/AVP 96\r\n"						\
    "a=rtpmap:96 AppleLossless\r\n"					\
    "a=fmtp:96 %d 0 16 40 10 14 2 255 0 0 44100\r\n"			\
    "a=rsaaeskey:%s\r\n"						\
    "a=aesiv:%s\r\n"

  int ret;

  /* Add SDP payload */
  ret = evbuffer_add_printf(req->output_buffer, SDP_PLD_FMT,
			    session_id, address, rs->address, AIRTUNES_V2_PACKET_SAMPLES,
			    raop_aes_key_b64, raop_aes_iv_b64);

  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Out of memory for SDP payload\n");

      return -1;
    }

  return 0;

#undef SDP_PLD_FMT
}


/* RAOP/RTSP requests */
static int
raop_send_req_teardown(struct raop_session *rs, evrtsp_req_cb cb)
{
  struct evrtsp_request *req;
  int ret;

  req = evrtsp_request_new(cb, rs);
  if (!req)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not create RTSP request for TEARDOWN\n");

      return -1;
    }

  raop_add_headers(rs, req);

  ret = evrtsp_make_request(rs->ctrl, req, EVRTSP_REQ_TEARDOWN, rs->session_url);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not make TEARDOWN request\n");

      goto cleanup;
    }

  rs->req_in_flight = 1;

  evrtsp_connection_set_closecb(rs->ctrl, NULL, NULL);

  return 0;

 cleanup:
  evrtsp_request_free(req);

  return -1;
}

static int
raop_send_req_flush(struct raop_session *rs, evrtsp_req_cb cb, uint64_t rtptime)
{
  char buf[64];
  struct evrtsp_request *req;
  int ret;

  req = evrtsp_request_new(cb, rs);
  if (!req)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not create RTSP request for FLUSH\n");

      return -1;
    }

  raop_add_headers(rs, req);

  /* Restart sequence: last sequence + 1 */
  ret = snprintf(buf, sizeof(buf), "seq=%u;rtptime=%u", stream_seq + 1, RAOP_RTPTIME(rtptime));
  if ((ret < 0) || (ret >= sizeof(buf)))
    {
      DPRINTF(E_LOG, L_RAOP, "RTP-Info too big for buffer in FLUSH request\n");

      goto cleanup;
    }
  evrtsp_add_header(req->output_headers, "RTP-Info", buf);

  ret = evrtsp_make_request(rs->ctrl, req, EVRTSP_REQ_FLUSH, rs->session_url);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not make FLUSH request\n");

      goto cleanup;
    }

  rs->req_in_flight = 1;

  evrtsp_connection_set_closecb(rs->ctrl, NULL, NULL);

  return 0;

 cleanup:
  evrtsp_request_free(req);

  return -1;
}

static int
raop_send_req_set_parameter(struct raop_session *rs, struct evbuffer *evbuf, evrtsp_req_cb cb)
{
  struct evrtsp_request *req;
  int ret;

  req = evrtsp_request_new(cb, rs);
  if (!req)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not create RTSP request for SET_PARAMETER\n");

      return -1;
    }

  ret = evbuffer_add_buffer(req->output_buffer, evbuf);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Out of memory for SET_PARAMETER payload\n");

      goto cleanup;
    }

  raop_add_headers(rs, req);
  evrtsp_add_header(req->output_headers, "Content-Type", "text/parameters");

  ret = evrtsp_make_request(rs->ctrl, req, EVRTSP_REQ_SET_PARAMETER, rs->session_url);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not make SET_PARAMETER request\n");

      goto cleanup;
    }

  rs->req_in_flight = 1;

  evrtsp_connection_set_closecb(rs->ctrl, NULL, NULL);

  return 0;

 cleanup:
  evrtsp_request_free(req);

  return -1;
}

static int
raop_send_req_record(struct raop_session *rs, evrtsp_req_cb cb)
{
  char buf[64];
  struct evrtsp_request *req;
  int ret;

  req = evrtsp_request_new(cb, rs);
  if (!req)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not create RTSP request for RECORD\n");

      return -1;
    }

  raop_add_headers(rs, req);

  evrtsp_add_header(req->output_headers, "Range", "npt=0-");

  /* Start sequence: next sequence */
  ret = snprintf(buf, sizeof(buf), "seq=%u;rtptime=%u", stream_seq + 1, RAOP_RTPTIME(rs->start_rtptime));
  if ((ret < 0) || (ret >= sizeof(buf)))
    {
      DPRINTF(E_LOG, L_RAOP, "RTP-Info too big for buffer in RECORD request\n");

      goto cleanup;
    }
  evrtsp_add_header(req->output_headers, "RTP-Info", buf);

  ret = evrtsp_make_request(rs->ctrl, req, EVRTSP_REQ_RECORD, rs->session_url);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not make RECORD request\n");

      goto cleanup;
    }

  rs->req_in_flight = 1;

  return 0;

 cleanup:
  evrtsp_request_free(req);

  return -1;
}

static int
raop_send_req_setup(struct raop_session *rs, evrtsp_req_cb cb)
{
  char hdr[128];
  struct evrtsp_request *req;
  int ret;

  req = evrtsp_request_new(cb, rs);
  if (!req)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not create RTSP request for SETUP\n");

      return -1;
    }

  raop_add_headers(rs, req);

  /* Request UDP transport, AirTunes v2 streaming */
  ret = snprintf(hdr, sizeof(hdr), "RTP/AVP/UDP;unicast;interleaved=0-1;mode=record;control_port=%u;timing_port=%u", control_port, timing_port);
  if ((ret < 0) || (ret >= sizeof(hdr)))
    {
      DPRINTF(E_LOG, L_RAOP, "Transport header exceeds buffer length\n");

      goto cleanup;
    }

  evrtsp_add_header(req->output_headers, "Transport", hdr);

  ret = evrtsp_make_request(rs->ctrl, req, EVRTSP_REQ_SETUP, rs->session_url);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not make SETUP request\n");

      goto cleanup;
    }

  rs->req_in_flight = 1;

  return 0;

 cleanup:
  evrtsp_request_free(req);

  return -1;
}

static int
raop_send_req_announce(struct raop_session *rs, evrtsp_req_cb cb)
{
  uint8_t challenge[16];
  char *challenge_b64;
  char *ptr;
  struct evrtsp_request *req;
  char *address;
  unsigned short port;
  uint32_t session_id;
  int ret;

  /* Determine local address, needed for SDP and session URL */
  evrtsp_connection_get_local_address(rs->ctrl, &address, &port);
  if (!address || (port == 0))
    {
      DPRINTF(E_LOG, L_RAOP, "Could not determine local address\n");

      if (address)
	free(address);

      return -1;
    }

  DPRINTF(E_DBG, L_RAOP, "Local address: %s port %d\n", address, port);

  req = evrtsp_request_new(cb, rs);
  if (!req)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not create RTSP request for ANNOUNCE\n");

      free(address);
      return -1;
    }

  /* Session ID and session URL */
  gcry_randomize(&session_id, sizeof(session_id), GCRY_STRONG_RANDOM);

  ret = snprintf(rs->session_url, sizeof(rs->session_url), "rtsp://%s/%u", address, session_id);
  if ((ret < 0) || (ret >= sizeof(rs->session_url)))
    {
      DPRINTF(E_LOG, L_RAOP, "Session URL length exceeds 127 characters\n");

      free(address);
      goto cleanup;
    }

  /* SDP payload */
  ret = raop_make_sdp(rs, req, address, session_id);
  free(address);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not generate SDP payload for ANNOUNCE\n");

      goto cleanup;
    }

  raop_add_headers(rs, req);
  evrtsp_add_header(req->output_headers, "Content-Type", "application/sdp");

  /* Challenge */
  gcry_randomize(challenge, sizeof(challenge), GCRY_STRONG_RANDOM);
  challenge_b64 = b64_encode(challenge, sizeof(challenge));
  if (!challenge_b64)
    {
      DPRINTF(E_LOG, L_RAOP, "Couldn't encode challenge\n");

      goto cleanup;
    }

  /* Remove base64 padding */
  ptr = strchr(challenge_b64, '=');
  if (ptr)
    *ptr = '\0';

  evrtsp_add_header(req->output_headers, "Apple-Challenge", challenge_b64);

  free(challenge_b64);

  ret = evrtsp_make_request(rs->ctrl, req, EVRTSP_REQ_ANNOUNCE, rs->session_url);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not make ANNOUNCE request\n");

      goto cleanup;
    }

  rs->req_in_flight = 1;

  return 0;

 cleanup:
  evrtsp_request_free(req);

  return -1;
}

static int
raop_send_req_options(struct raop_session *rs, evrtsp_req_cb cb)
{
  struct evrtsp_request *req;
  int ret;

  req = evrtsp_request_new(cb, rs);
  if (!req)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not create RTSP request for OPTIONS\n");

      return -1;
    }

  raop_add_headers(rs, req);

  ret = raop_add_auth(rs, req, "OPTIONS", "*");
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not add authentication data to OPTIONS request\n");

      if (ret == -2)
	rs->state = RAOP_PASSWORD;

      goto cleanup;
    }

  ret = evrtsp_make_request(rs->ctrl, req, EVRTSP_REQ_OPTIONS, "*");
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not make OPTIONS request\n");

      goto cleanup;
    }

  rs->req_in_flight = 1;

  evrtsp_connection_set_closecb(rs->ctrl, NULL, NULL);

  return 0;

 cleanup:
  evrtsp_request_free(req);

  return -1;
}


static void
raop_session_cleanup(struct raop_session *rs)
{
  struct raop_session *s;

  if (rs == sessions)
    sessions = sessions->next;
  else
    {
      for (s = sessions; s && (s->next != rs); s = s->next)
	; /* EMPTY */

      if (!s)
	DPRINTF(E_WARN, L_RAOP, "WARNING: struct raop_session not found in list; BUG!\n");
      else
	s->next = rs->next;
    }

  evrtsp_connection_set_closecb(rs->ctrl, NULL, NULL);

  evrtsp_connection_free(rs->ctrl);

  close(rs->server_fd);

  if (rs->realm)
    free(rs->realm);
  if (rs->nonce)
    free(rs->nonce);

  if (rs->session)
    free(rs->session);

  if (rs->address)
    free(rs->address);
  if (rs->devname)
    free(rs->devname);

  free(rs);
}

static struct raop_session *
raop_session_make(struct raop_device *rd, raop_status_cb cb)
{
  struct raop_session *rs;
  int ret;

  rs = (struct raop_session *)malloc(sizeof(struct raop_session));
  if (!rs)
    {
      DPRINTF(E_LOG, L_RAOP, "Out of memory for RAOP session\n");
      return NULL;
    }

  memset(rs, 0, sizeof(struct raop_session));

  rs->state = RAOP_STOPPED;
  rs->cseq = 1;

  rs->dev = rd;
  rs->status_cb = cb;
  rs->server_fd = -1;

  rs->password = rd->password;

  rs->sa.sin.sin_family = AF_INET;
  ret = inet_pton(AF_INET, rd->address, &rs->sa.sin.sin_addr.s_addr);
  if (ret <= 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Device address not valid (%s)\n", rd->address);

      goto out_free_rs;
    }

  rs->ctrl = evrtsp_connection_new(rd->address, rd->port);
  if (!rs->ctrl)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not create control connection\n");

      goto out_free_rs;
    }

  evrtsp_connection_set_base(rs->ctrl, evbase_player);

  rs->devname = strdup(rd->name);
  rs->address = strdup(rd->address);

  rs->next = sessions;
  sessions = rs;

  return rs;

 out_free_rs:
  free(rs);

  return NULL;
}

static void
raop_session_failure(struct raop_session *rs)
{
  /* Session failed, let our user know */
  if (rs->state != RAOP_PASSWORD)
    rs->state = RAOP_FAILED;
  rs->status_cb(rs->dev, rs, rs->state);

  raop_session_cleanup(rs);
}

static void
raop_session_failure_cb(struct evrtsp_request *req, void *arg)
{
  struct raop_session *rs;

  rs = (struct raop_session *)arg;

  raop_session_failure(rs);
}

static void
raop_deferred_eh_cb(int fd, short what, void *arg)
{
  struct raop_deferred_eh *deh;
  struct raop_session *rs;

  deh = (struct raop_deferred_eh *)arg;

  rs = deh->session;

  free(deh);

  DPRINTF(E_DBG, L_RAOP, "Cleaning up failed session (deferred) on device %s\n", rs->devname);

  raop_session_failure(rs);
}

static void
raop_rtsp_close_cb(struct evrtsp_connection *evcon, void *arg)
{
  struct timeval tv;
  struct raop_deferred_eh *deh;
  struct raop_session *rs;

  rs = (struct raop_session *)arg;

  DPRINTF(E_LOG, L_RAOP, "ApEx %s closed RTSP connection\n", rs->devname);

  rs->state = RAOP_FAILED;

  deh = (struct raop_deferred_eh *)malloc(sizeof(struct raop_deferred_eh));
  if (!deh)
    {
      DPRINTF(E_LOG, L_RAOP, "Out of memory for deferred error handling!\n");

      return;
    }

  deh->session = rs;

  evtimer_set(&deh->ev, raop_deferred_eh_cb, deh);
  event_base_set(evbase_player, &deh->ev);
  evutil_timerclear(&tv);
  evtimer_add(&deh->ev, &tv);
}


/* Volume handling */
static int
raop_set_volume_internal(struct raop_session *rs, evrtsp_req_cb cb)
{
  struct evbuffer *evbuf;
  int ret;

  evbuf = evbuffer_new();
  if (!evbuf)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not allocate evbuffer for volume payload\n");

      return -1;
    }

  /* Don't let locales get in the way here */
  ret = evbuffer_add_printf(evbuf, "volume: %d.%06d", (int)raop_volume, (int)(1000000.0 * (raop_volume - floor(raop_volume))));
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Out of memory for SET_PARAMETER payload (volume)\n");

      evbuffer_free(evbuf);
      return -1;
    }

  ret = raop_send_req_set_parameter(rs, evbuf, cb);
  if (ret < 0)
    DPRINTF(E_LOG, L_RAOP, "Could not send SET_PARAMETER request for volume\n");

  evbuffer_free(evbuf);

  return ret;
}

static void
raop_set_volume_cb(struct evrtsp_request *req, void *arg)
{
  raop_status_cb status_cb;
  struct raop_session *rs;
  int ret;

  rs = (struct raop_session *)arg;

  if (!req)
    goto error;

  rs->req_in_flight = 0;
  rs->cseq++;

  if (req->response_code != RTSP_OK)
    {
      DPRINTF(E_LOG, L_RAOP, "SET_PARAMETER request failed for stream volume: %d %s\n", req->response_code, req->response_code_line);

      goto error;
    }

  ret = raop_check_cseq(rs, req);
  if (ret < 0)
    goto error;

  /* Let our user know */
  status_cb = rs->status_cb;
  rs->status_cb = NULL;
  status_cb(rs->dev, rs, rs->state);

  evrtsp_connection_set_closecb(rs->ctrl, raop_rtsp_close_cb, rs);

  return;

 error:
  raop_session_failure(rs);
}

/* Volume in [0 - 100] */
int
raop_set_volume(int volume, raop_status_cb cb)
{
  struct raop_session *rs;
  int pending;
  int ret;

  /* RAOP volume
   *  -144.0 is off
   *  0 - -30.0 maps to 100 - 0
   */
  if (volume == 0)
    raop_volume = -144.0;
  else
    raop_volume = -30.0 + ((double)volume * 30.0) / 100.0;

  pending = 0;
  for (rs = sessions; rs; rs = rs->next)
    {
      if (!(rs->state & RAOP_CONNECTED))
	continue;

      ret = raop_set_volume_internal(rs, raop_set_volume_cb);
      if (ret < 0)
	{
	  raop_session_failure(rs);

	  continue;
	}

      rs->status_cb = cb;
      pending++;
    }

  return pending;
}


static void
raop_flush_cb(struct evrtsp_request *req, void *arg)
{
  raop_status_cb status_cb;
  struct raop_session *rs;
  int ret;

  rs = (struct raop_session *)arg;

  if (!req)
    goto error;

  rs->req_in_flight = 0;
  rs->cseq++;

  if (req->response_code != RTSP_OK)
    {
      DPRINTF(E_LOG, L_RAOP, "FLUSH request failed: %d %s\n", req->response_code, req->response_code_line);

      goto error;
    }

  ret = raop_check_cseq(rs, req);
  if (ret < 0)
    goto error;

  rs->state = RAOP_CONNECTED;

  /* Let our user know */
  status_cb = rs->status_cb;
  rs->status_cb = NULL;
  status_cb(rs->dev, rs, rs->state);

  evrtsp_connection_set_closecb(rs->ctrl, raop_rtsp_close_cb, rs);

  return;

 error:
  raop_session_failure(rs);
}

static void
raop_flush_timer_cb(int fd, short what, void *arg)
{
  struct raop_session *rs;

  DPRINTF(E_DBG, L_RAOP, "Flush timer expired; tearing down RAOP sessions\n");

  for (rs = sessions; rs; rs = rs->next)
    {
      if (!(rs->state & RAOP_F_CONNECTED))
	continue;

      raop_device_stop(rs);
    }
}

int
raop_flush(raop_status_cb cb, uint64_t rtptime)
{
  struct timeval tv;
  struct raop_session *rs;
  int pending;
  int ret;

  pending = 0;
  for (rs = sessions; rs; rs = rs->next)
    {
      if (rs->state != RAOP_STREAMING)
	continue;

      ret = raop_send_req_flush(rs, raop_flush_cb, rtptime);
      if (ret < 0)
	{
	  raop_session_failure(rs);

	  continue;
	}

      rs->status_cb = cb;
      pending++;
    }

  if (pending > 0)
    {
      evtimer_set(&flush_timer, raop_flush_timer_cb, NULL);
      event_base_set(evbase_player, &flush_timer);
      evutil_timerclear(&tv);
      tv.tv_sec = 10;
      evtimer_add(&flush_timer, &tv);
    }

  return pending;
}


/* AirTunes v2 time synchronization */
static void
raop_v2_timing_cb(int fd, short what, void *arg)
{
  char address[INET_ADDRSTRLEN];
  union sockaddr_all sa;
  uint8_t req[32];
  uint8_t res[32];
  struct ntp_stamp recv_stamp;
  struct ntp_stamp xmit_stamp;
  struct raop_session *rs;
  int len;
  int ret;

  ret = raop_v2_timing_get_clock_ntp(&recv_stamp);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Couldn't get receive timestamp\n");

      return;
    }

  len = sizeof(sa.ss);
  ret = recvfrom(timing_fd, req, sizeof(req), 0, &sa.sa, (socklen_t *)&len);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Error reading timing request: %s\n", strerror(errno));
      return;
    }

  if (ret != 32)
    {
      DPRINTF(E_DBG, L_RAOP, "Got timing request with size %d\n", ret);
      return;
    }

  if (len != sizeof(struct sockaddr_in))
    return;

  for (rs = sessions; rs; rs = rs->next)
    {
      if (sa.sin.sin_addr.s_addr == rs->sa.sin.sin_addr.s_addr)
	break;
    }

  if (!rs)
    {
      if (!inet_ntop(AF_INET, &sa.sin.sin_addr.s_addr, address, sizeof(address)))
	DPRINTF(E_LOG, L_RAOP, "Time sync request from [error: %s]; not a RAOP client\n", strerror(errno));
      else
	DPRINTF(E_LOG, L_RAOP, "Time sync request from %s; not a RAOP client\n", address);

      return;
    }

  if ((req[0] != 0x80) || (req[1] != 0xd2))
    {
      DPRINTF(E_LOG, L_RAOP, "Packet header doesn't match timing request\n");
      return;
    }

  memset(res, 0, sizeof(res));

  /* Header */
  res[0] = 0x80;
  res[1] = 0xd3;
  res[2] = req[2];

  /* Copy client timestamp */
  memcpy(res + 8, req + 24, 8);

  /* Receive timestamp */
  recv_stamp.sec = htobe32(recv_stamp.sec);
  recv_stamp.frac = htobe32(recv_stamp.frac);
  memcpy(res + 16, &recv_stamp.sec, 4);
  memcpy(res + 20, &recv_stamp.frac, 4);

  /* Transmit timestamp */
  ret = raop_v2_timing_get_clock_ntp(&xmit_stamp);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Couldn't get transmit timestamp, falling back to receive timestamp\n");

      /* Still better than failing altogether
       * recv/xmit are close enough that it shouldn't matter much
       */
      memcpy(res + 24, &recv_stamp.sec, 4);
      memcpy(res + 28, &recv_stamp.frac, 4);
    }
  else
    {
      xmit_stamp.sec = htobe32(xmit_stamp.sec);
      xmit_stamp.frac = htobe32(xmit_stamp.frac);
      memcpy(res + 24, &xmit_stamp.sec, 4);
      memcpy(res + 28, &xmit_stamp.frac, 4);
    }

  ret = sendto(timing_fd, res, sizeof(res), 0, &sa.sa, len);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not send timing reply: %s\n", strerror(errno));

      return;
    }

  ret = event_add(&timing_ev, NULL);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Couldn't re-add event for timing requests\n");

      return;
    }
}

static int
raop_v2_timing_start(void)
{
  union sockaddr_all sa;
  int len;
  int ret;

#ifdef __linux__
  timing_fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
#else
  timing_fd = socket(AF_INET, SOCK_DGRAM, 0);
#endif
  if (timing_fd < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Couldn't make timing socket: %s\n", strerror(errno));

      return -1;
    }

  sa.sin.sin_family = AF_INET;
  sa.sin.sin_addr.s_addr = INADDR_ANY;
  sa.sin.sin_port = 0;

  ret = bind(timing_fd, &sa.sa, sizeof(struct sockaddr_in));
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Couldn't bind timing socket: %s\n", strerror(errno));

      close(timing_fd);
      return -1;
    }

  len = sizeof(sa.ss);
  ret = getsockname(timing_fd, &sa.sa, (socklen_t *)&len);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Couldn't get timing socket name: %s\n", strerror(errno));

      close(timing_fd);
      return -1;
    }

  timing_port = ntohs(sa.sin.sin_port);

  DPRINTF(E_DBG, L_RAOP, "Timing port: %d\n", timing_port);

  event_set(&timing_ev, timing_fd, EV_READ, raop_v2_timing_cb, NULL);
  event_base_set(evbase_player, &timing_ev);
  ret = event_add(&timing_ev, NULL);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Couldn't add event for timing requests\n");

      close(timing_fd);
      return -1;
    }

  return 0;
}

static void
raop_v2_timing_stop(void)
{
  if (event_initialized(&timing_ev))
    event_del(&timing_ev);

  close(timing_fd);

  timing_fd = -1;
  timing_port = 0;
}

/* AirTunes v2 playback synchronization */
static void
raop_v2_control_send_sync(uint64_t next_pkt, struct timespec *init)
{
  uint8_t msg[20];
  struct timespec ts;
  struct ntp_stamp cur_stamp;
  struct raop_session *rs;
  uint64_t cur_pos;
  uint32_t cur_pos32;
  uint32_t next_pkt32;
  int ret;

  memset(msg, 0, sizeof(msg));

  msg[0] = (sync_counter == 0) ? 0x90 : 0x80;
  msg[1] = 0xd4;
  msg[3] = 0x07;

  next_pkt32 = htobe32(RAOP_RTPTIME(next_pkt));
  memcpy(msg + 16, &next_pkt32, 4);

  if (!init)
    {
      ret = player_get_current_pos(&cur_pos, &ts, 1);
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_RAOP, "Could not get current playback position and clock\n");

	  return;
	}

      timespec_to_ntp(&ts, &cur_stamp);
    }
  else
    {
      cur_pos = next_pkt - 88200;
      timespec_to_ntp(init, &cur_stamp);
    }

  cur_pos32 = htobe32(RAOP_RTPTIME(cur_pos));
  cur_stamp.sec = htobe32(cur_stamp.sec);
  cur_stamp.frac = htobe32(cur_stamp.frac);

  memcpy(msg + 4, &cur_pos32, 4);
  memcpy(msg + 8, &cur_stamp.sec, 4);
  memcpy(msg + 12, &cur_stamp.frac, 4);

  for (rs = sessions; rs; rs = rs->next)
    {
      if (rs->state != RAOP_STREAMING)
	continue;

      rs->sa.sin.sin_port = htons(rs->control_port);

      ret = sendto(control_fd, msg, sizeof(msg), 0, &rs->sa.sa, sizeof(struct sockaddr_in));
      if (ret < 0)
	DPRINTF(E_LOG, L_RAOP, "Could not send playback sync to device %s: %s\n", rs->devname, strerror(errno));
    }
}

static int
raop_v2_control_start(void)
{
  union sockaddr_all sa;
  int len;
  int ret;

#ifdef __linux__
  control_fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
#else
  control_fd = socket(AF_INET, SOCK_DGRAM, 0);
#endif
  if (control_fd < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Couldn't make control socket: %s\n", strerror(errno));

      return -1;
    }

  sa.sin.sin_family = AF_INET;
  sa.sin.sin_addr.s_addr = INADDR_ANY;
  sa.sin.sin_port = 0;

  ret = bind(control_fd, &sa.sa, sizeof(struct sockaddr_in));
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Couldn't bind control socket: %s\n", strerror(errno));

      close(control_fd);
      return -1;
    }

  len = sizeof(sa.ss);
  ret = getsockname(control_fd, &sa.sa, (socklen_t *)&len);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Couldn't get control socket name: %s\n", strerror(errno));

      close(control_fd);
      return -1;
    }

  control_port = ntohs(sa.sin.sin_port);

  DPRINTF(E_DBG, L_RAOP, "Control port: %d\n", control_port);

  return 0;
}

static void
raop_v2_control_stop(void)
{
  close(control_fd);

  control_fd = -1;
  control_port = 0;
}


/* AirTunes v2 streaming */
static int
raop_v2_make_packet(struct raop_v2_packet *pkt, uint8_t *rawbuf, uint64_t rtptime)
{
  char ebuf[64];
  gpg_error_t gc_err;
  uint32_t rtptime32;
  uint16_t seq;

  memset(pkt, 0, sizeof(struct raop_v2_packet));

  alac_encode(rawbuf, pkt->data + AIRTUNES_V2_HEADER, STOB(AIRTUNES_V2_PACKET_SAMPLES));

  stream_seq++;

  seq = htobe16(stream_seq);
  rtptime32 = htobe32(RAOP_RTPTIME(rtptime));

  pkt->data[0] = 0x80;
  pkt->data[1] = (sync_counter == 0) ? 0xe0 : 0x60;

  memcpy(pkt->data + 2, &seq, 2);
  memcpy(pkt->data + 4, &rtptime32, 4);

  /* 4 bytes unknown */

  /* Reset cipher */
  gc_err = gcry_cipher_reset(raop_aes_ctx);
  if (gc_err != GPG_ERR_NO_ERROR)
    {
      gpg_strerror_r(gc_err, ebuf, sizeof(ebuf));
      DPRINTF(E_LOG, L_RAOP, "Could not reset AES cipher: %s\n", ebuf);

      return -1;
    }

  /* Set IV */
  gc_err = gcry_cipher_setiv(raop_aes_ctx, raop_aes_iv, sizeof(raop_aes_iv));
  if (gc_err != GPG_ERR_NO_ERROR)
    {
      gpg_strerror_r(gc_err, ebuf, sizeof(ebuf));
      DPRINTF(E_LOG, L_RAOP, "Could not set AES IV: %s\n", ebuf);

      return -1;
    }

  /* Encrypt in place, in blocks of 16 bytes */
  gc_err = gcry_cipher_encrypt(raop_aes_ctx, pkt->data + 12, ((sizeof(pkt->data) - 12) / 16) * 16, NULL, 0);
  if (gc_err != GPG_ERR_NO_ERROR)
    {
      gpg_strerror_r(gc_err, ebuf, sizeof(ebuf));
      DPRINTF(E_LOG, L_RAOP, "Could not encrypt payload: %s\n", ebuf);

      return -1;
    }

  return 0;
}

void
raop_v2_write(uint8_t *buf, uint64_t rtptime)
{
  struct raop_v2_packet pkt;
  struct raop_session *rs;
  int ret;

  ret = raop_v2_make_packet(&pkt, buf, rtptime);
  if (ret < 0)
    {
      raop_playback_stop();

      return;
    }

  if (sync_counter == 126)
    {
      raop_v2_control_send_sync(rtptime, NULL);

      sync_counter = 1;
    }
  else
    sync_counter++;

  for (rs = sessions; rs; rs = rs->next)
    {
      if (rs->state != RAOP_STREAMING)
	continue;

      ret = send(rs->server_fd, pkt.data, sizeof(pkt.data), 0);
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_RAOP, "Send error for %s: %s\n", rs->devname, strerror(errno));

	  raop_session_failure(rs);
	}
      else if (ret != sizeof(pkt.data))
	DPRINTF(E_WARN, L_RAOP, "Partial send (%d) for %s\n", ret, rs->devname);
    }

  return;
}

static int
raop_v2_stream_open(struct raop_session *rs)
{
  int ret;

#ifdef __linux__
  rs->server_fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
#else
  rs->server_fd = socket(AF_INET, SOCK_DGRAM, 0);
#endif
  if (rs->server_fd < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not create socket for streaming: %s\n", strerror(errno));

      return -1;
    }

  rs->sa.sin.sin_port = htons(rs->server_port);

  ret = connect(rs->server_fd, &rs->sa.sa, sizeof(struct sockaddr_in));
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "connect() to %s:%u failed: %s\n", rs->address, rs->server_port, strerror(errno));

      goto out_fail;
    }

  /* Include the device into the set of active devices if
   * playback is in progress.
   */
  if (sync_counter != 0)
    rs->state = RAOP_STREAMING;
  else
    rs->state = RAOP_CONNECTED;

  return 0;

 out_fail:
  close(rs->server_fd);
  rs->server_fd = -1;

  return -1;
}


/* Session startup */
static void
raop_startup_cancel(struct raop_session *rs)
{
  /* Try being nice to our peer */
  if (rs->session)
    raop_send_req_teardown(rs, raop_session_failure_cb);
  else
    raop_session_failure(rs);
}

static void
raop_cb_startup_volume(struct evrtsp_request *req, void *arg)
{
  raop_status_cb status_cb;
  struct raop_session *rs;
  int ret;

  rs = (struct raop_session *)arg;

  if (!req)
    goto cleanup;

  rs->req_in_flight = 0;
  rs->cseq++;

  if (req->response_code != RTSP_OK)
    {
      DPRINTF(E_LOG, L_RAOP, "SET_PARAMETER request failed for startup volume: %d %s\n", req->response_code, req->response_code_line);

      goto cleanup;
    }

  ret = raop_check_cseq(rs, req);
  if (ret < 0)
    goto cleanup;

  ret = raop_v2_stream_open(rs);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not open streaming socket\n");

      goto cleanup;
    }

  /* Session startup and setup is done, tell our user */
  status_cb = rs->status_cb;
  rs->status_cb = NULL;

  status_cb(rs->dev, rs, rs->state);

  evrtsp_connection_set_closecb(rs->ctrl, raop_rtsp_close_cb, rs);

  return;

 cleanup:
  raop_startup_cancel(rs);
}

static void
raop_cb_startup_record(struct evrtsp_request *req, void *arg)
{
  struct raop_session *rs;
  const char *param;
  int ret;

  rs = (struct raop_session *)arg;

  if (!req)
    goto cleanup;

  rs->req_in_flight = 0;
  rs->cseq++;

  if (req->response_code != RTSP_OK)
    {
      DPRINTF(E_LOG, L_RAOP, "SETUP request failed in session startup: %d %s\n", req->response_code, req->response_code_line);

      goto cleanup;
    }

  ret = raop_check_cseq(rs, req);
  if (ret < 0)
    goto cleanup;

  /* Audio latency */
  param = evrtsp_find_header(req->input_headers, "Audio-Latency");
  if (!param)
    {
      DPRINTF(E_LOG, L_RAOP, "Missing Audio-Latency header in RECORD reply\n");

      goto cleanup;
    }

  DPRINTF(E_DBG, L_RAOP, "RAOP audio latency is %s\n", param);

  rs->state = RAOP_RECORD;

  /* Set initial volume */
  raop_set_volume_internal(rs, raop_cb_startup_volume);

  return;

 cleanup:
  raop_startup_cancel(rs);
}

static void
raop_cb_startup_setup(struct evrtsp_request *req, void *arg)
{
  struct raop_session *rs;
  const char *param;
  char *transport;
  char *token;
  char *ptr;
  int tmp;
  int ret;

  rs = (struct raop_session *)arg;

  if (!req)
    goto cleanup;

  rs->req_in_flight = 0;
  rs->cseq++;

  if (req->response_code != RTSP_OK)
    {
      DPRINTF(E_LOG, L_RAOP, "SETUP request failed in session startup: %d %s\n", req->response_code, req->response_code_line);

      goto cleanup;
    }

  ret = raop_check_cseq(rs, req);
  if (ret < 0)
    goto cleanup;

  /* Server-side session ID */
  param = evrtsp_find_header(req->input_headers, "Session");
  if (!param)
    {
      DPRINTF(E_LOG, L_RAOP, "Missing Session header in SETUP reply\n");

      goto cleanup;
    }

  rs->session = strdup(param);

  /* Check transport and get remote streaming port */
  param = evrtsp_find_header(req->input_headers, "Transport");
  if (!param)
    {
      DPRINTF(E_LOG, L_RAOP, "Missing Transport header in SETUP reply\n");

      goto cleanup;
    }

  /* Check transport is really UDP, AirTunes v2 streaming */
  if (strncmp(param, "RTP/AVP/UDP;", strlen("RTP/AVP/UDP;")) != 0)
    {
      DPRINTF(E_LOG, L_RAOP, "ApEx replied with unsupported Transport: %s\n", param);

      goto cleanup;
    }

  transport = strdup(param);
  if (!transport)
    {
      DPRINTF(E_LOG, L_RAOP, "Out of memory for Transport header copy\n");

      goto cleanup;
    }

  token = strchr(transport, ';');
  token++;

  token = strtok_r(token, ";=", &ptr);
  while (token)
    {
      DPRINTF(E_LOG, L_RAOP, "token: %s\n", token);

      if (strcmp(token, "server_port") == 0)
        {
          token = strtok_r(NULL, ";=", &ptr);
          if (!token)
            break;

	  ret = safe_atoi32(token, &tmp);
	  if (ret < 0)
	    {
	      DPRINTF(E_LOG, L_RAOP, "Could not read server_port\n");

	      break;
	    }

	  rs->server_port = tmp;
        }
      else if (strcmp(token, "control_port") == 0)
        {
          token = strtok_r(NULL, ";=", &ptr);
          if (!token)
            break;

	  ret = safe_atoi32(token, &tmp);
	  if (ret < 0)
	    {
	      DPRINTF(E_LOG, L_RAOP, "Could not read control_port\n");

	      break;
	    }

	  rs->control_port = tmp;
        }
      else if (strcmp(token, "timing_port") == 0)
        {
          token = strtok_r(NULL, ";=", &ptr);
          if (!token)
            break;

	  ret = safe_atoi32(token, &tmp);
	  if (ret < 0)
	    {
	      DPRINTF(E_LOG, L_RAOP, "Could not read timing_port\n");

	      break;
	    }

	  rs->timing_port = tmp;
        }

      token = strtok_r(NULL, ";=", &ptr);
    }

  free(transport);

  if ((rs->server_port == 0) || (rs->control_port == 0) || (rs->timing_port == 0))
    {
      DPRINTF(E_LOG, L_RAOP, "Transport header lacked some port numbers in SETUP reply\n");
      DPRINTF(E_LOG, L_RAOP, "Transport header was: %s\n", param);

      goto cleanup;
    }

  DPRINTF(E_DBG, L_RAOP, "Negotiated AirTunes v2 UDP streaming session %s; ports s=%u c=%u t=%u\n", rs->session, rs->server_port, rs->control_port, rs->timing_port);

  rs->state = RAOP_SETUP;

  /* Send RECORD */
  ret = raop_send_req_record(rs, raop_cb_startup_record);
  if (ret < 0)
    goto cleanup;

  return;

 cleanup:
  raop_startup_cancel(rs);
}

static void
raop_cb_startup_announce(struct evrtsp_request *req, void *arg)
{
  struct raop_session *rs;
  int ret;

  rs = (struct raop_session *)arg;

  if (!req)
    goto cleanup;

  rs->req_in_flight = 0;
  rs->cseq++;

  if (req->response_code != RTSP_OK)
    {
      DPRINTF(E_LOG, L_RAOP, "ANNOUNCE request failed in session startup: %d %s\n", req->response_code, req->response_code_line);

      goto cleanup;
    }

  ret = raop_check_cseq(rs, req);
  if (ret < 0)
    goto cleanup;

  rs->state = RAOP_ANNOUNCE;

  /* Send SETUP */
  ret = raop_send_req_setup(rs, raop_cb_startup_setup);
  if (ret < 0)
    goto cleanup;

  return;

 cleanup:
  raop_startup_cancel(rs);
}

static void
raop_cb_startup_options(struct evrtsp_request *req, void *arg)
{
  struct raop_session *rs;
  int ret;

  rs = (struct raop_session *)arg;

  if (!req)
    goto cleanup;

  rs->req_in_flight = 0;
  rs->cseq++;

  if ((req->response_code != RTSP_OK) && (req->response_code != RTSP_UNAUTHORIZED))
    {
      DPRINTF(E_LOG, L_RAOP, "OPTIONS request failed in session startup: %d %s\n", req->response_code, req->response_code_line);

      goto cleanup;
    }

  ret = raop_check_cseq(rs, req);
  if (ret < 0)
    goto cleanup;

  if (req->response_code == RTSP_UNAUTHORIZED)
    {
      if (rs->req_has_auth)
	{
	  DPRINTF(E_LOG, L_RAOP, "Bad password for device %s\n", rs->devname);

	  rs->state = RAOP_PASSWORD;
	  goto cleanup;
	}

      ret = raop_parse_auth(rs, req);
      if (ret < 0)
	goto cleanup;

      ret = raop_send_req_options(rs, raop_cb_startup_options);
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_RAOP, "Could not re-run OPTIONS request with authentication\n");

	  goto cleanup;
	}

      return;
    }

  rs->state = RAOP_OPTIONS;

  /* Send ANNOUNCE */
  ret = raop_send_req_announce(rs, raop_cb_startup_announce);
  if (ret < 0)
    goto cleanup;

  return;

 cleanup:
  raop_startup_cancel(rs);
}


static void
raop_cb_shutdown_teardown(struct evrtsp_request *req, void *arg)
{
  raop_status_cb status_cb;
  struct raop_session *rs;
  int ret;

  rs = (struct raop_session *)arg;

  if (!req)
    goto error;

  rs->req_in_flight = 0;
  rs->cseq++;

  if (req->response_code != RTSP_OK)
    {
      DPRINTF(E_LOG, L_RAOP, "TEARDOWN request failed in session shutdown: %d %s\n", req->response_code, req->response_code_line);

      goto error;
    }

  ret = raop_check_cseq(rs, req);
  if (ret < 0)
    goto error;

  rs->state = RAOP_STOPPED;

  /* Session shut down, tell our user */
  status_cb = rs->status_cb;
  rs->status_cb = NULL;

  status_cb(rs->dev, rs, rs->state);

  raop_session_cleanup(rs);

  return;

 error:
  raop_session_failure(rs);
}

static void
raop_cb_probe_options(struct evrtsp_request *req, void *arg)
{
  raop_status_cb status_cb;
  struct raop_session *rs;
  int ret;

  rs = (struct raop_session *)arg;

  if (!req)
    goto cleanup;

  rs->req_in_flight = 0;
  rs->cseq++;

  if ((req->response_code != RTSP_OK) && (req->response_code != RTSP_UNAUTHORIZED))
    {
      DPRINTF(E_LOG, L_RAOP, "OPTIONS request failed in device probe: %d %s\n", req->response_code, req->response_code_line);

      goto cleanup;
    }

  ret = raop_check_cseq(rs, req);
  if (ret < 0)
    goto cleanup;

  if (req->response_code == RTSP_UNAUTHORIZED)
    {
      if (rs->req_has_auth)
	{
	  DPRINTF(E_LOG, L_RAOP, "Bad password for device %s\n", rs->devname);

	  rs->state = RAOP_PASSWORD;
	  goto cleanup;
	}

      ret = raop_parse_auth(rs, req);
      if (ret < 0)
	goto cleanup;

      ret = raop_send_req_options(rs, raop_cb_probe_options);
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_RAOP, "Could not re-run OPTIONS request with authentication\n");

	  goto cleanup;
	}

      return;
    }

  rs->state = RAOP_OPTIONS;

  /* Device probed successfully, tell our user */
  status_cb = rs->status_cb;
  rs->status_cb = NULL;

  status_cb(rs->dev, rs, rs->state);

  /* We're not going further with this session */
  raop_session_cleanup(rs);

  return;

 cleanup:
  raop_session_failure(rs);
}


int
raop_device_probe(struct raop_device *rd, raop_status_cb cb)
{
  struct raop_session *rs;
  int ret;

  rs = raop_session_make(rd, cb);
  if (!rs)
    return -1;

  /* Send an OPTIONS request to test our ability to connect to the device,
   * including the need for and/or validity of the password
   */
  ret = raop_send_req_options(rs, raop_cb_probe_options);
  if (ret < 0)
    goto cleanup;

  return 0;

 cleanup:
  raop_session_cleanup(rs);

  return -1;
}

int
raop_device_start(struct raop_device *rd, raop_status_cb cb, uint64_t rtptime)
{
  struct raop_session *rs;
  int ret;

  rs = raop_session_make(rd, cb);
  if (!rs)
    return -1;

  rs->start_rtptime = rtptime;

  /* Send an OPTIONS request to establish the connection
   * After that, we can determine our local address and build our session URL
   * for all subsequent requests.
   */
  ret = raop_send_req_options(rs, raop_cb_startup_options);
  if (ret < 0)
    goto cleanup;

  return 0;

 cleanup:
  raop_session_cleanup(rs);

  return -1;
}

void
raop_device_stop(struct raop_session *rs)
{
  if (!(rs->state & RAOP_F_CONNECTED))
    raop_session_cleanup(rs);
  else
    raop_send_req_teardown(rs, raop_cb_shutdown_teardown);
}


void
raop_playback_start(uint64_t next_pkt, struct timespec *ts)
{
  struct raop_session *rs;

  if (event_initialized(&flush_timer))
    event_del(&flush_timer);

  sync_counter = 0;

  for (rs = sessions; rs; rs = rs->next)
    {
      if (rs->state == RAOP_CONNECTED)
	rs->state = RAOP_STREAMING;
    }

  /* Send initial playback sync */
  raop_v2_control_send_sync(next_pkt, ts);
}

void
raop_playback_stop(void)
{
  struct raop_session *rs;
  int ret;

  for (rs = sessions; rs; rs = rs->next)
    {
      ret = raop_send_req_teardown(rs, raop_cb_shutdown_teardown);
      if (ret < 0)
	DPRINTF(E_LOG, L_RAOP, "shutdown: TEARDOWN request failed!\n");
    }
}

void
raop_set_status_cb(struct raop_session *rs, raop_status_cb cb)
{
  rs->status_cb = cb;
}


int
raop_init(void)
{
  char ebuf[64];
  char *ptr;
  gpg_error_t gc_err;
  int ret;

  timing_fd = -1;
  timing_port = 0;

  control_fd = -1;
  control_port = 0;

  sessions = NULL;

  /* Random RTP sequence start */
  gcry_randomize(&stream_seq, sizeof(stream_seq), GCRY_STRONG_RANDOM);

  /* Generate AES key and IV */
  gcry_randomize(raop_aes_key, sizeof(raop_aes_key), GCRY_STRONG_RANDOM);
  gcry_randomize(raop_aes_iv, sizeof(raop_aes_iv), GCRY_STRONG_RANDOM);

  /* Setup AES */
  gc_err = gcry_cipher_open(&raop_aes_ctx, GCRY_CIPHER_AES, GCRY_CIPHER_MODE_CBC, 0);
  if (gc_err != GPG_ERR_NO_ERROR)
    {
      gpg_strerror_r(gc_err, ebuf, sizeof(ebuf));
      DPRINTF(E_LOG, L_RAOP, "Could not open AES cipher: %s\n", ebuf);

      return -1;
    }

  /* Set key */
  gc_err = gcry_cipher_setkey(raop_aes_ctx, raop_aes_key, sizeof(raop_aes_key));
  if (gc_err != GPG_ERR_NO_ERROR)
    {
      gpg_strerror_r(gc_err, ebuf, sizeof(ebuf));
      DPRINTF(E_LOG, L_RAOP, "Could not set AES key: %s\n", ebuf);

      goto out_close_cipher;
    }

  /* Prepare Base64-encoded key & IV for SDP */
  raop_aes_key_b64 = raop_crypt_encrypt_aes_key_base64();
  if (!raop_aes_key_b64)
    {
      DPRINTF(E_LOG, L_RAOP, "Couldn't encrypt and encode AES session key\n");

      goto out_close_cipher;
    }

  raop_aes_iv_b64 = b64_encode(raop_aes_iv, sizeof(raop_aes_iv));
  if (!raop_aes_iv_b64)
    {
      DPRINTF(E_LOG, L_RAOP, "Couldn't encode AES IV\n");

      goto out_free_b64_key;
    }

  /* Remove base64 padding */
  ptr = strchr(raop_aes_key_b64, '=');
  if (ptr)
    *ptr = '\0';

  ptr = strchr(raop_aes_iv_b64, '=');
  if (ptr)
    *ptr = '\0';

  ret = raop_v2_timing_start();
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "AirTunes v2 time synchronization failed to start\n");

      goto out_free_b64_iv;
    }

  ret = raop_v2_control_start();
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "AirTunes v2 playback synchronization failed to start\n");

      goto out_stop_timing;
    }

  return 0;

 out_stop_timing:
  raop_v2_timing_stop();
 out_free_b64_iv:
  free(raop_aes_iv_b64);
 out_free_b64_key:
  free(raop_aes_key_b64);
 out_close_cipher:
  gcry_cipher_close(raop_aes_ctx);

  return -1;
}

void
raop_deinit(void)
{
  raop_v2_timing_stop();
  raop_v2_control_stop();

  gcry_cipher_close(raop_aes_ctx);

  free(raop_aes_key_b64);
  free(raop_aes_iv_b64);
}
