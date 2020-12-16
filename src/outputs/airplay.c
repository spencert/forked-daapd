/*
 * Crypto code adapted from VideoLAN
 *   Copyright (C) 2008 the VideoLAN team
 *   Author: Michael Hanselmann
 *   GPLv2+
 *
 * ALAC encoding adapted from airplay_play
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

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdio.h>
#include <stdbool.h>
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

#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>

#include <event2/event.h>
#include <event2/buffer.h>
#include <gcrypt.h>

#include "plist_wrap.h"

#include "evrtsp/evrtsp.h"
#include "conffile.h"
#include "logger.h"
#include "mdns.h"
#include "misc.h"
#include "player.h"
#include "db.h"
#include "artwork.h"
#include "dmap_common.h"
#include "rtp_common.h"
#include "outputs.h"

#ifdef RAOP_VERIFICATION
#include "pair.h"
#endif

#define ALAC_HEADER_LEN                      3

#define RAOP_QUALITY_SAMPLE_RATE_DEFAULT     44100
#define RAOP_QUALITY_BITS_PER_SAMPLE_DEFAULT 16
#define RAOP_QUALITY_CHANNELS_DEFAULT        2

// AirTunes v2 number of samples per packet
// Probably using this value because 44100/352 and 48000/352 has good 32 byte
// alignment, which improves performance of some encoders
#define RAOP_SAMPLES_PER_PACKET              352

#define RAOP_RTP_PAYLOADTYPE                 0x60

// How many RTP packets keep in a buffer for retransmission
#define RAOP_PACKET_BUFFER_SIZE    1000

#define RAOP_MD_DELAY_STARTUP      15360
#define RAOP_MD_DELAY_SWITCH       (RAOP_MD_DELAY_STARTUP * 2)
#define RAOP_MD_WANTS_TEXT         (1 << 0)
#define RAOP_MD_WANTS_ARTWORK      (1 << 1)
#define RAOP_MD_WANTS_PROGRESS     (1 << 2)

// ATV4 and Homepod disconnect for reasons that are not clear, but sending them
// progress metadata at regular intervals reduces the problem. The below
// interval was determined via testing, see:
// https://github.com/ejurgensen/forked-daapd/issues/734#issuecomment-622959334
#define RAOP_KEEP_ALIVE_INTERVAL   25

// This is an arbitrary value which just needs to be kept in sync with the config
#define RAOP_CONFIG_MAX_VOLUME     11

const char *pair_device_id = "AABBCCDD11223344"; // TODO use actual ID

union sockaddr_all
{
  struct sockaddr_in sin;
  struct sockaddr_in6 sin6;
  struct sockaddr sa;
  struct sockaddr_storage ss;
};

enum airplay_devtype {
  RAOP_DEV_APEX1_80211G,
  RAOP_DEV_APEX2_80211N,
  RAOP_DEV_APEX3_80211N,
  RAOP_DEV_APPLETV,
  RAOP_DEV_APPLETV4,
  RAOP_DEV_HOMEPOD,
  RAOP_DEV_OTHER,
};

// Session is starting up
#define RAOP_STATE_F_STARTUP    (1 << 13)
// Streaming is up (connection established)
#define RAOP_STATE_F_CONNECTED  (1 << 14)
// Couldn't start device
#define RAOP_STATE_F_FAILED     (1 << 15)

enum airplay_state {
  // Device is stopped (no session)
  RAOP_STATE_STOPPED   = 0,
  // Session startup
  RAOP_STATE_STARTUP   = RAOP_STATE_F_STARTUP | 0x01,
  RAOP_STATE_OPTIONS   = RAOP_STATE_F_STARTUP | 0x02,
  RAOP_STATE_ANNOUNCE  = RAOP_STATE_F_STARTUP | 0x03,
  RAOP_STATE_SETUP     = RAOP_STATE_F_STARTUP | 0x04,
  RAOP_STATE_RECORD    = RAOP_STATE_F_STARTUP | 0x05,
  // Session established
  // - streaming ready (RECORD sent and acked, connection established)
  // - commands (SET_PARAMETER) are possible
  RAOP_STATE_CONNECTED = RAOP_STATE_F_CONNECTED | 0x01,
  // Media data is being sent
  RAOP_STATE_STREAMING = RAOP_STATE_F_CONNECTED | 0x02,
  // Session teardown in progress (-> going to STOPPED state)
  RAOP_STATE_TEARDOWN  = RAOP_STATE_F_CONNECTED | 0x03,
  // Session is failed, couldn't startup or error occurred
  RAOP_STATE_FAILED    = RAOP_STATE_F_FAILED | 0x01,
  // Password issue: unknown password or bad password, or pending PIN from user
  RAOP_STATE_PASSWORD  = RAOP_STATE_F_FAILED | 0x02,
};

// Info about the device, which is not required by the player, only internally
struct airplay_extra
{
  enum airplay_devtype devtype;

  uint16_t wanted_metadata;
  bool encrypt;
  bool supports_auth_setup;
};

struct airplay_master_session
{
  struct evbuffer *evbuf;
  int evbuf_samples;

  struct rtp_session *rtp_session;

  struct rtcp_timestamp cur_stamp;

  uint8_t *rawbuf;
  size_t rawbuf_size;
  int samples_per_packet;
  bool encrypt;

  // Number of samples that we tell the output to buffer (this will mean that
  // the position that we send in the sync packages are offset by this amount
  // compared to the rtptimes of the corresponding RTP packages we are sending)
  int output_buffer_samples;

  struct airplay_master_session *next;
};

struct airplay_session
{
  uint64_t device_id;
  int callback_id;

  struct airplay_master_session *master_session;

  struct evrtsp_connection *ctrl;
  struct evrtsp_connection *event;

  enum airplay_state state;

  uint16_t wanted_metadata;
  bool req_has_auth;
  bool encrypt;
  bool auth_quirk_itunes;
  bool supports_post;
  bool supports_auth_setup;

  bool only_probe;

  struct event *deferredev;

  int reqs_in_flight;
  int cseq;
  char *session;
  char session_url[128];

  char *realm;
  char *nonce;
  const char *password;

  char *devname;
  char *address;
  int family;

  int volume;

  char *local_address;
  unsigned short data_port;
  unsigned short control_port;
  unsigned short event_port; // What is this used for?
  unsigned short timing_port; // ATV4 has this set to 0, but it is not used by forked-daapd anyway

#ifdef RAOP_VERIFICATION
  /* Homekit pairing, see pair.h */
  struct pair_cipher_context *pair_cipher_ctx;
  struct pair_verify_context *pair_verify_ctx;
  struct pair_setup_context *pair_setup_ctx;
#endif

  int server_fd;

  union sockaddr_all sa;

  struct airplay_service *timing_svc;
  struct airplay_service *control_svc;

  struct airplay_session *next;
};

struct airplay_metadata
{
  struct evbuffer *metadata;
  struct evbuffer *artwork;
  int artwork_fmt;
};

struct airplay_service
{
  int fd;
  unsigned short port;
  struct event *ev;
};

typedef void (*evrtsp_req_cb)(struct evrtsp_request *req, void *arg);

/* NTP timestamp definitions */
#define FRAC             4294967296. /* 2^32 as a double */
#define NTP_EPOCH_DELTA  0x83aa7e80  /* 2208988800 - that's 1970 - 1900 in seconds */

// TODO move to rtp_common
struct ntp_stamp
{
  uint32_t sec;
  uint32_t frac;
};


static const uint8_t airplay_rsa_pubkey[] =
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

static const uint8_t airplay_rsa_exp[] = "\x01\x00\x01";

static const uint8_t airplay_auth_setup_pubkey[] =
  "\x59\x02\xed\xe9\x0d\x4e\xf2\xbd\x4c\xb6\x8a\x63\x30\x03\x82\x07"
  "\xa9\x4d\xbd\x50\xd8\xaa\x46\x5b\x5d\x8c\x01\x2a\x0c\x7e\x1d\x4e";

struct features_type_map
{
  uint32_t bit;
  char *name;
};

// List of features announced by AirPlay 2 speakers
// Credit @invano, see https://emanuelecozzi.net/docs/airplay2
static const struct features_type_map features_map[] =
  {
    { 0, "SupportsAirPlayVideoV1" },
    { 1, "SupportsAirPlayPhoto" },
    { 5, "SupportsAirPlaySlideshow" },
    { 7, "SupportsAirPlayScreen" },
    { 9, "SupportsAirPlayAudio" },
    { 11, "AudioRedunant" },
    { 14, "Authentication_4" }, // FairPlay authentication
    { 15, "MetadataFeatures_0" }, // Send artwork image to receiver
    { 16, "MetadataFeatures_1" }, // Send track progress status to receiver
    { 17, "MetadataFeatures_2" }, // Send NowPlaying info via DAAP
    { 18, "AudioFormats_0" },
    { 19, "AudioFormats_1" },
    { 20, "AudioFormats_2" },
    { 21, "AudioFormats_3" },
    { 23, "Authentication_1" }, // RSA authentication (NA)
    { 26, "Authentication_8" }, // 26 || 51, MFi authentication
    { 27, "SupportsLegacyPairing" },
    { 30, "HasUnifiedAdvertiserInfo" },
    { 32, "IsCarPlay" },
    { 32, "SupportsVolume" }, // !32
    { 33, "SupportsAirPlayVideoPlayQueue" },
    { 34, "SupportsAirPlayFromCloud" }, // 34 && flags_6_SupportsAirPlayFromCloud
    { 35, "SupportsTLS_PSK" },
    { 38, "SupportsUnifiedMediaControl" },
    { 40, "SupportsBufferedAudio" }, // srcvers >= 354.54.6 && 40
    { 41, "SupportsPTP" }, // srcvers >= 366 && 41
    { 42, "SupportsScreenMultiCodec" },
    { 43, "SupportsSystemPairing" },
    { 44, "IsAPValeriaScreenSender" },
    { 46, "SupportsHKPairingAndAccessControl" },
    { 48, "SupportsCoreUtilsPairingAndEncryption" }, // 38 || 46 || 43 || 48
    { 49, "SupportsAirPlayVideoV2" },
    { 50, "MetadataFeatures_3" }, // Send NowPlaying info via bplist
    { 51, "SupportsUnifiedPairSetupAndMFi" },
    { 52, "SupportsSetPeersExtendedMessage" },
    { 54, "SupportsAPSync" },
    { 55, "SupportsWoL" }, // 55 || 56
    { 56, "SupportsWoL" }, // 55 || 56
    { 58, "SupportsHangdogRemoteControl" }, // ((isAppleTV || isAppleAudioAccessory) && 58) || (isThirdPartyTV && flags_10)
    { 59, "SupportsAudioStreamConnectionSetup" }, // 59 && !disableStreamConnectionSetup
    { 60, "SupportsAudioMediaDataControl" }, // 59 && 60 && !disableMediaDataControl
    { 61, "SupportsRFC2198Redundancy" },
  };

/* Keep in sync with enum airplay_devtype */
static const char *airplay_devtype[] =
{
  "AirPort Express 1 - 802.11g",
  "AirPort Express 2 - 802.11n",
  "AirPort Express 3 - 802.11n",
  "AppleTV",
  "AppleTV4",
  "HomePod",
  "Other",
};

/* Struct with default quality levels */
static struct media_quality airplay_quality_default =
{
  RAOP_QUALITY_SAMPLE_RATE_DEFAULT,
  RAOP_QUALITY_BITS_PER_SAMPLE_DEFAULT,
  RAOP_QUALITY_CHANNELS_DEFAULT
};

/* From player.c */
extern struct event_base *evbase_player;

/* RAOP AES stream key */
static uint8_t airplay_aes_key[16];
static uint8_t airplay_aes_iv[16];
static gcry_cipher_hd_t airplay_aes_ctx;

/* Base64-encoded AES key and IV for SDP */
static char *airplay_aes_key_b64;
static char *airplay_aes_iv_b64;

/* AirTunes v2 time synchronization */
static struct airplay_service timing_4svc;
static struct airplay_service timing_6svc;

/* AirTunes v2 playback synchronization / control */
static struct airplay_service control_4svc;
static struct airplay_service control_6svc;

/* Metadata */
static struct output_metadata *airplay_cur_metadata;

/* Keep-alive timer - hack for ATV's with tvOS 10 */
static struct event *keep_alive_timer;
static struct timeval keep_alive_tv = { RAOP_KEEP_ALIVE_INTERVAL, 0 };

/* Sessions */
static struct airplay_master_session *airplay_master_sessions;
static struct airplay_session *airplay_sessions;

// Forwards
static int
airplay_device_start(struct output_device *rd, int callback_id);


/* ------------------------------- MISC HELPERS ----------------------------- */

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
alac_encode(uint8_t *dst, uint8_t *raw, int len)
{
  uint8_t *maxraw;
  int bpos;

  bpos = 0;
  maxraw = raw + len;

  alac_write_bits(&dst, 1, 3, &bpos); /* channel=1, stereo */
  alac_write_bits(&dst, 0, 4, &bpos); /* unknown */
  alac_write_bits(&dst, 0, 8, &bpos); /* unknown */
  alac_write_bits(&dst, 0, 4, &bpos); /* unknown */
  alac_write_bits(&dst, 0, 1, &bpos); /* hassize */

  alac_write_bits(&dst, 0, 2, &bpos); /* unused */
  alac_write_bits(&dst, 1, 1, &bpos); /* is-not-compressed */

  for (; raw < maxraw; raw += 4)
    {
      /* Byteswap to big endian */
      alac_write_bits(&dst, *(raw + 1), 8, &bpos);
      alac_write_bits(&dst, *raw, 8, &bpos);
      alac_write_bits(&dst, *(raw + 3), 8, &bpos);
      alac_write_bits(&dst, *(raw + 2), 8, &bpos);
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
airplay_v2_timing_get_clock_ntp(struct ntp_stamp *ns)
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


/* ----------------------- RAOP crypto stuff - from VLC --------------------- */

// MGF1 is specified in RFC2437, section 10.2.1. Variables are named after the
// specification.
static int
airplay_crypt_mgf1(uint8_t *mask, size_t l, const uint8_t *z, const size_t zlen, const int hash)
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
airplay_crypt_add_oaep_padding(uint8_t *em, const size_t emlen, const uint8_t *m, const size_t mlen, const uint8_t *p, const size_t plen)
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
  ret = airplay_crypt_mgf1(db_mask, emlen_max - hlen, seed, hlen, GCRY_MD_SHA1);
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
  ret = airplay_crypt_mgf1(seed_mask, hlen, db, emlen_max - hlen, GCRY_MD_SHA1);
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
airplay_crypt_encrypt_aes_key_base64(void)
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
  ret = airplay_crypt_add_oaep_padding(padded_key, sizeof(padded_key), airplay_aes_key, sizeof(airplay_aes_key), NULL, 0);
  if (ret < 0)
    return NULL;

  /* Read public key */
  gc_err = gcry_mpi_scan(&mpi_pubkey, GCRYMPI_FMT_USG, airplay_rsa_pubkey, sizeof(airplay_rsa_pubkey) - 1, NULL);
  if (gc_err != GPG_ERR_NO_ERROR)
    {
      gpg_strerror_r(gc_err, ebuf, sizeof(ebuf));
      DPRINTF(E_LOG, L_RAOP, "Could not read RAOP RSA pubkey: %s\n", ebuf);

      return NULL;
    }

  /* Read exponent */
  gc_err = gcry_mpi_scan(&mpi_exp, GCRYMPI_FMT_USG, airplay_rsa_exp, sizeof(airplay_rsa_exp) - 1, NULL);
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


/* ------------------ Helpers for sending RAOP/RTSP requests ---------------- */

static int
airplay_add_auth(struct airplay_session *rs, struct evrtsp_request *req, const char *method, const char *uri)
{
  char ha1[33];
  char ha2[33];
  char ebuf[64];
  char auth[256];
  const char *hash_fmt;
  const char *username;
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
      DPRINTF(E_LOG, L_RAOP, "Authentication required but no password found for device '%s'\n", rs->devname);

      return -2;
    }

  if (rs->auth_quirk_itunes)
    {
      hash_fmt = "%02X"; /* Uppercase hex */
      username = "iTunes";
    }
  else
    {
      hash_fmt = "%02x";
      username = ""; /* No username */
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

  gcry_md_write(hd, username, strlen(username));
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
    sprintf(ha1 + (2 * i), hash_fmt, hash_bytes[i]);

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
    sprintf(ha2 + (2 * i), hash_fmt, hash_bytes[i]);

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
    sprintf(ha1 + (2 * i), hash_fmt, hash_bytes[i]);

  gcry_md_close(hd);

  /* Build header */
  ret = snprintf(auth, sizeof(auth), "Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", response=\"%s\"",
		 username, rs->realm, rs->nonce, uri, ha1);
  if ((ret < 0) || (ret >= sizeof(auth)))
    {
      DPRINTF(E_LOG, L_RAOP, "Authorization value header exceeds buffer size\n");

      return -1;
    }

  evrtsp_add_header(req->output_headers, "Authorization", auth);

  DPRINTF(E_DBG, L_RAOP, "Authorization header: %s\n", auth);

  rs->req_has_auth = 1;

  return 0;
}

static int
airplay_parse_auth(struct airplay_session *rs, struct evrtsp_request *req)
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

  DPRINTF(E_DBG, L_RAOP, "WWW-Authenticate: %s\n", param);

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
	  token = strtok_r(NULL, "=\"", &ptr);
	  if (!token)
	    break;

	  rs->realm = strdup(token);
	}
      else if (strcmp(token, "nonce") == 0)
	{
	  token = strtok_r(NULL, "=\"", &ptr);
	  if (!token)
	    break;

	  rs->nonce = strdup(token);
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

  DPRINTF(E_DBG, L_RAOP, "Found realm: [%s], nonce: [%s]\n", rs->realm, rs->nonce);

  return 0;
}

static int
airplay_add_headers(struct airplay_session *rs, struct evrtsp_request *req, enum evrtsp_cmd_type req_method)
{
  char buf[64];
  const char *method;
  const char *url;
  const char *user_agent;
  int ret;

  method = evrtsp_method(req_method);

  snprintf(buf, sizeof(buf), "%d", rs->cseq);
  evrtsp_add_header(req->output_headers, "CSeq", buf);

  rs->cseq++;

  user_agent = cfg_getstr(cfg_getsec(cfg, "general"), "user_agent");
  evrtsp_add_header(req->output_headers, "User-Agent", user_agent);

  /* Add Authorization header */
  url = (req_method == EVRTSP_REQ_OPTIONS) ? "*" : rs->session_url;

  ret = airplay_add_auth(rs, req, method, url);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not add Authorization header\n");

      if (ret == -2)
	rs->state = RAOP_STATE_PASSWORD;

      return -1;
    }

  snprintf(buf, sizeof(buf), "%" PRIX64, libhash);
  evrtsp_add_header(req->output_headers, "Client-Instance", buf);
  evrtsp_add_header(req->output_headers, "DACP-ID", buf);

  // We set Active-Remote as 32 bit unsigned decimal, as at least my device
  // can't handle any larger. Must be aligned with volume_byactiveremote().
  snprintf(buf, sizeof(buf), "%" PRIu32, (uint32_t)rs->device_id);
  evrtsp_add_header(req->output_headers, "Active-Remote", buf);

  if (rs->session)
    evrtsp_add_header(req->output_headers, "Session", rs->session);

  /* Content-Length added automatically by evrtsp */

  return 0;
}

/* This check should compare the reply CSeq with the request CSeq, but it has
 * been removed because RAOP targets like Reflector and AirFoil don't return
 * the CSeq according to the rtsp spec, and the CSeq is not really important
 * anyway.
 */
static int
airplay_check_cseq(struct airplay_session *rs, struct evrtsp_request *req)
{
  return 0;
}

/*
static int
airplay_make_sdp(struct airplay_session *rs, struct evrtsp_request *req, char *address, int family, uint32_t session_id)
{
#define SDP_PLD_FMT							\
  "v=0\r\n"								\
    "o=iTunes %u 0 IN %s %s\r\n"					\
    "s=iTunes\r\n"							\
    "c=IN %s %s\r\n"							\
    "t=0 0\r\n"								\
    "m=audio 0 RTP/AVP 96\r\n"						\
    "a=rtpmap:96 AppleLossless\r\n"					\
    "a=fmtp:96 %d 0 16 40 10 14 2 255 0 0 44100\r\n"			\
    "a=rsaaeskey:%s\r\n"						\
    "a=aesiv:%s\r\n"
#define SDP_PLD_FMT_NO_ENC						\
  "v=0\r\n"								\
    "o=iTunes %u 0 IN %s %s\r\n"					\
    "s=iTunes\r\n"							\
    "c=IN %s %s\r\n"							\
    "t=0 0\r\n"								\
    "m=audio 0 RTP/AVP 96\r\n"						\
    "a=rtpmap:96 AppleLossless\r\n"					\
    "a=fmtp:96 %d 0 16 40 10 14 2 255 0 0 44100\r\n"

  const char *af;
  const char *rs_af;
  char *p;
  int ret;

  af = (family == AF_INET) ? "IP4" : "IP6";
  rs_af = (rs->family == AF_INET) ? "IP4" : "IP6";

  p = strchr(rs->address, '%');
  if (p)
    *p = '\0';

  // Add SDP payload - but don't add RSA/AES key/iv if no encryption - important for ATV3 update 6.0
  if (rs->encrypt)
    ret = evbuffer_add_printf(req->output_buffer, SDP_PLD_FMT,
			      session_id, af, address, rs_af, rs->address, RAOP_SAMPLES_PER_PACKET,
			      airplay_aes_key_b64, airplay_aes_iv_b64);
  else
    ret = evbuffer_add_printf(req->output_buffer, SDP_PLD_FMT_NO_ENC,
			      session_id, af, address, rs_af, rs->address, RAOP_SAMPLES_PER_PACKET);

  if (p)
    *p = '%';

  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Out of memory for SDP payload\n");
      return -1;
    }

  DPRINTF(E_INFO, L_RAOP, "Setting up AirPlay session %u (%s -> %s)\n", session_id, address, rs->address);

  return 0;

#undef SDP_PLD_FMT
#undef SDP_PLD_FMT_NO_ENC
}
*/

/* ----------------- Handlers for sending RAOP/RTSP requests ---------------- */

/*
 * Request queueing HOWTO
 *
 * Sending:
 * - increment rs->reqs_in_flight
 * - set evrtsp connection closecb to NULL
 *
 * Request callback:
 * - decrement rs->reqs_in_flight first thing, even if the callback is
 *   called for error handling (req == NULL or HTTP error code)
 * - if rs->reqs_in_flight == 0, setup evrtsp connection closecb
 *
 * When a request fails, the whole RAOP session is declared failed and
 * torn down by calling session_failure(), even if there are requests
 * queued on the evrtsp connection. There is no reason to think pending
 * requests would work out better than the one that just failed and recovery
 * would be tricky to get right.
 *
 * evrtsp behaviour with queued requests:
 * - request callback is called with req == NULL to indicate a connection
 *   error; if there are several requests queued on the connection, this can
 *   happen for each request if the connection isn't destroyed
 * - the connection is reset, and the closecb is called if the connection was
 *   previously connected. There is no closecb set when there are requests in
 *   flight
 */

static int
airplay_send_req_teardown(struct airplay_session *rs, evrtsp_req_cb cb, const char *log_caller)
{
  struct evrtsp_request *req;
  int ret;

  DPRINTF(E_DBG, L_RAOP, "%s: Sending TEARDOWN to '%s'\n", log_caller, rs->devname);

  req = evrtsp_request_new(cb, rs);
  if (!req)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not create RTSP request for TEARDOWN\n");

      return -1;
    }

  ret = airplay_add_headers(rs, req, EVRTSP_REQ_TEARDOWN);
  if (ret < 0)
    {
      evrtsp_request_free(req);
      return -1;
    }

  ret = evrtsp_make_request(rs->ctrl, req, EVRTSP_REQ_TEARDOWN, rs->session_url);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not make TEARDOWN request to '%s'\n", rs->devname);

      return -1;
    }

  rs->reqs_in_flight++;

  evrtsp_connection_set_closecb(rs->ctrl, NULL, NULL);

  return 0;
}

static int
airplay_send_req_flush(struct airplay_session *rs, evrtsp_req_cb cb, const char *log_caller)
{
  struct airplay_master_session *rms = rs->master_session;
  struct evrtsp_request *req;
  char buf[64];
  int ret;

  DPRINTF(E_DBG, L_RAOP, "%s: Sending FLUSH to '%s'\n", log_caller, rs->devname);

  req = evrtsp_request_new(cb, rs);
  if (!req)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not create RTSP request for FLUSH\n");

      return -1;
    }

  ret = airplay_add_headers(rs, req, EVRTSP_REQ_FLUSH);
  if (ret < 0)
    {
      evrtsp_request_free(req);
      return -1;
    }

  /* Restart sequence */
  ret = snprintf(buf, sizeof(buf), "seq=%" PRIu16 ";rtptime=%u", rms->rtp_session->seqnum, rms->rtp_session->pos);
  if ((ret < 0) || (ret >= sizeof(buf)))
    {
      DPRINTF(E_LOG, L_RAOP, "RTP-Info too big for buffer in FLUSH request\n");

      evrtsp_request_free(req);
      return -1;
    }
  evrtsp_add_header(req->output_headers, "RTP-Info", buf);

  ret = evrtsp_make_request(rs->ctrl, req, EVRTSP_REQ_FLUSH, rs->session_url);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not make FLUSH request to '%s'\n", rs->devname);

      return -1;
    }

  rs->reqs_in_flight++;

  evrtsp_connection_set_closecb(rs->ctrl, NULL, NULL);

  return 0;
}

static int
airplay_send_req_set_parameter(struct airplay_session *rs, struct evbuffer *evbuf, char *ctype, char *rtpinfo, evrtsp_req_cb cb, const char *log_caller)
{
  struct evrtsp_request *req;
  int ret;

  DPRINTF(E_DBG, L_RAOP, "%s: Sending SET_PARAMETER to '%s'\n", log_caller, rs->devname);

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

      evrtsp_request_free(req);
      return -1;
    }

  ret = airplay_add_headers(rs, req, EVRTSP_REQ_SET_PARAMETER);
  if (ret < 0)
    {
      evrtsp_request_free(req);
      return -1;
    }

  evrtsp_add_header(req->output_headers, "Content-Type", ctype);

  if (rtpinfo)
    evrtsp_add_header(req->output_headers, "RTP-Info", rtpinfo);

  ret = evrtsp_make_request(rs->ctrl, req, EVRTSP_REQ_SET_PARAMETER, rs->session_url);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not make SET_PARAMETER request to '%s'\n", rs->devname);

      return -1;
    }

  rs->reqs_in_flight++;

  evrtsp_connection_set_closecb(rs->ctrl, NULL, NULL);

  return 0;
}


/*
Audio formats

Bit 	Value 	Type
2 	0x4 	PCM/8000/16/1
3 	0x8 	PCM/8000/16/2
4 	0x10 	PCM/16000/16/1
5 	0x20 	PCM/16000/16/2
6 	0x40 	PCM/24000/16/1
7 	0x80 	PCM/24000/16/2
8 	0x100 	PCM/32000/16/1
9 	0x200 	PCM/32000/16/2
10 	0x400 	PCM/44100/16/1
11 	0x800 	PCM/44100/16/2
12 	0x1000 	PCM/44100/24/1
13 	0x2000 	PCM/44100/24/2
14 	0x4000 	PCM/48000/16/1
15 	0x8000 	PCM/48000/16/2
16 	0x10000 	PCM/48000/24/1
17 	0x20000 	PCM/48000/24/2
18 	0x40000 	ALAC/44100/16/2
19 	0x80000 	ALAC/44100/24/2
20 	0x100000 	ALAC/48000/16/2
21 	0x200000 	ALAC/48000/24/2
22 	0x400000 	AAC-LC/44100/2
23 	0x800000 	AAC-LC/48000/2
24 	0x1000000 	AAC-ELD/44100/2
25 	0x2000000 	AAC-ELD/48000/2
26 	0x4000000 	AAC-ELD/16000/1
27 	0x8000000 	AAC-ELD/24000/1
28 	0x10000000 	OPUS/16000/1
29 	0x20000000 	OPUS/24000/1
30 	0x40000000 	OPUS/48000/1
31 	0x80000000 	AAC-ELD/44100/1
32 	0x100000000 	AAC-ELD/48000/1
*/


// {'streams': [{'audioFormat': 262144,
//           'audioMode': 'default',
//           'controlPort': 60242,
//           'ct': 2,
//           'isMedia': True,
//           'latencyMax': 88200,
//           'latencyMin': 11025,
//           'shk': b'\xdbc\x9b,\xdb\x15\x82\x19\x0b\xbf\xd3\xd0\x81\xc5\x7f7'
//                  b'\xaf\x7f\xb2l\xec\xca\xc8\xd0\x8d\x9d\x1d\xa8'
//                  b'\xda\xa2\xf7\xd0',
//           'spf': 352,
//           'supportsDynamicStreamID': True,
//           'type': 96}]}

static int
req_setup2_payload_create(uint8_t **data, size_t *len, int control_port)
{
  plist_t root;
  plist_t streams;
  plist_t stream;
  uint8_t shk[32] = { 0 };
  int ret;

  stream = plist_new_dict();
  wplist_dict_add_uint(stream, "audioFormat", 262144); // 0x40000 ALAC/44100/16/2
  wplist_dict_add_string(stream, "audioMode", "default");
  wplist_dict_add_uint(stream, "controlPort", control_port);
  wplist_dict_add_uint(stream, "ct", 2); // Compression type, 1 LPCM, 2 ALAC, 3 AAC, 4 AAC ELD, 32 OPUS
  wplist_dict_add_bool(stream, "isMedia", true); // ?
  wplist_dict_add_uint(stream, "latencyMax", 88200);
  wplist_dict_add_uint(stream, "latencyMin", 11025);
  wplist_dict_add_data(stream, "shk", shk, sizeof(shk));
  wplist_dict_add_uint(stream, "spf", 352); // frames per packet
  wplist_dict_add_bool(stream, "supportsDynamicStreamID", false);
  wplist_dict_add_uint(stream, "type", 96); // RTP type, 96 real time, 103 buffered
  streams = plist_new_array();
  plist_array_append_item(streams, stream);

  root = plist_new_dict();
  plist_dict_set_item(root, "streams", streams);

  ret = wplist_to_bin(data, len, root);
  plist_free(root);
  return ret;
}

static int
airplay_send_req_setup2(struct airplay_session *rs, evrtsp_req_cb cb, const char *log_caller)
{
  struct evrtsp_request *req = NULL;
  uint8_t *data;
  size_t len;
  int ret;

  req = evrtsp_request_new(cb, rs);
  if (!req)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not create RTSP request for SETUP\n");
      goto error;
    }

  ret = airplay_add_headers(rs, req, EVRTSP_REQ_SETUP);
  if (ret < 0)
    {
      goto error;
    }

  evrtsp_add_header(req->output_headers, "Content-Type", "application/x-apple-binary-plist");

  ret = req_setup2_payload_create(&data, &len, rs->control_svc->port);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not create plist body for SETUP\n");
      goto error;
    }

  evbuffer_add(req->output_buffer, (unsigned char *)data, len);

  ret = evrtsp_make_request(rs->ctrl, req, EVRTSP_REQ_SETUP, rs->session_url);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not make SETUP request to '%s'\n", rs->devname);
      goto error;
    }

  rs->reqs_in_flight++;

  return 0;

 error:
  evrtsp_request_free(req);
  return -1;
}

static int
req_setpeers_payload_create(uint8_t **data, size_t *len, const char *sender_address, const char *receiver_address)
{
  plist_t root;
  int ret;

  // TODO also have ipv6
  root = plist_new_array();
  plist_t receiver = plist_new_string(receiver_address);
  plist_array_append_item(root, receiver);
  plist_t sender = plist_new_string(sender_address);
  plist_array_append_item(root, sender);

  ret = wplist_to_bin(data, len, root);
  plist_free(root);
  return ret;
}

static int
airplay_send_req_setpeers(struct airplay_session *rs, evrtsp_req_cb cb, const char *log_caller)
{
  struct evrtsp_request *req;
  uint8_t *data;
  size_t len;
  int ret;

  DPRINTF(E_DBG, L_RAOP, "%s: Sending SETPEERS to '%s'\n", log_caller, rs->devname);

  req = evrtsp_request_new(cb, rs);
  if (!req)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not create RTSP request for SETPEERS\n");
      return -1;
    }

  ret = airplay_add_headers(rs, req, EVRTSP_REQ_SETPEERS);
  if (ret < 0)
    {
      evrtsp_request_free(req);
      return -1;
    }

  evrtsp_add_header(req->output_headers, "Content-Type", "/peer-list-changed"); // Weird contenttype...

  ret = req_setpeers_payload_create(&data, &len, rs->local_address, rs->address);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not create plist body for SETPEERS\n");
      return -1;
    }

  evbuffer_add(req->output_buffer, data, len);

  ret = evrtsp_make_request(rs->ctrl, req, EVRTSP_REQ_SETPEERS, rs->session_url);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not make SETPEERS request to '%s'\n", rs->devname);

      return -1;
    }

  rs->reqs_in_flight++;

  return 0;
}

static int
airplay_send_req_record(struct airplay_session *rs, evrtsp_req_cb cb, const char *log_caller)
{
//  struct airplay_master_session *rms = rs->master_session;
  struct evrtsp_request *req;
//  char buf[64];
  int ret;

  DPRINTF(E_DBG, L_RAOP, "%s: Sending RECORD to '%s'\n", log_caller, rs->devname);

  req = evrtsp_request_new(cb, rs);
  if (!req)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not create RTSP request for RECORD\n");

      return -1;
    }

  ret = airplay_add_headers(rs, req, EVRTSP_REQ_RECORD);
  if (ret < 0)
    {
      evrtsp_request_free(req);
      return -1;
    }

//  evrtsp_add_header(req->output_headers, "X-Apple-ProtocolVersion", "1");

/*  evrtsp_add_header(req->output_headers, "Range", "npt=0-");

  // Start sequence: next sequence
  ret = snprintf(buf, sizeof(buf), "seq=%" PRIu16 ";rtptime=%u", rms->rtp_session->seqnum, rms->rtp_session->pos);
  if ((ret < 0) || (ret >= sizeof(buf)))
    {
      DPRINTF(E_LOG, L_RAOP, "RTP-Info too big for buffer in RECORD request\n");

      evrtsp_request_free(req);
      return -1;
    }
  evrtsp_add_header(req->output_headers, "RTP-Info", buf);

  DPRINTF(E_DBG, L_RAOP, "RTP-Info is %s\n", buf);
*/
  ret = evrtsp_make_request(rs->ctrl, req, EVRTSP_REQ_RECORD, rs->session_url);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not make RECORD request to '%s'\n", rs->devname);

      return -1;
    }

  rs->reqs_in_flight++;

  return 0;
}

// {'deviceID': '11:22:33:44:55:66',
//  'eiv': b'=o\xa0\xc24\xcd\xee\xcb9\x99~l\x140\x08\x9c',
//  'ekey': b'\x08\x90x\xa6\x0e\x87$C\x88l\xc1MS[Q\xaf',
//  'et': 0,
//  'groupContainsGroupLeader': False,
//  'groupUUID': '67EAD1FA-7EAB-4810-82F7-A9132FD2D0BB',
//  'isMultiSelectAirPlay': True,
//  'macAddress': '11:22:33:44:55:68',
//  'model': 'iPhone10,6',
//  'name': 'iPXema',
//  'osBuildVersion': '17B111',
//  'osName': 'iPhone OS',
//  'osVersion': '13.2.3',
//  'senderSupportsRelay': True,
//  'sessionUUID': '3195C737-1E6E-4487-BECB-4D287B7C7626',
//  'sourceVersion': '409.16',
//  'timingPeerInfo': {'Addresses': ['192.168.1.86', 'fe80::473:74c7:28a7:3bee'],
//                     'ID': '67EAD1FA-7EAB-4810-82F7-A9132FD2D0BB',
//                     'SupportsClockPortMatchingOverride': True},
//  'timingPeerList': [{'Addresses': ['192.168.1.86', 'fe80::473:74c7:28a7:3bee'],
//                      'ID': '67EAD1FA-7EAB-4810-82F7-A9132FD2D0BB',
//                      'SupportsClockPortMatchingOverride': True}],
//  'timingProtocol': 'PTP'}

static int
req_setup1_payload_create(uint8_t **data, size_t *len, int timing_port, const char *local_address)
{
  plist_t root;
//  plist_t timingpeerinfo;
//  plist_t timingpeerlist;
  plist_t addresses;
  plist_t address;
  int ret;

  address = plist_new_string(local_address);
  addresses = plist_new_array();
  plist_array_append_item(addresses, address);

/*  timingpeerinfo = plist_new_dict();
  plist_dict_set_item(timingpeerinfo, "Addresses", addresses);
  wplist_dict_add_string(timingpeerinfo, "ID", "67EAD1FA-7EAB-4810-82F7-A9132FD2D0BB");
  wplist_dict_add_bool(timingpeerinfo, "SupportsClockPortMatchingOverride", false);
*/
/*  timingpeerlist = plist_new_dict();
  plist_dict_set_item(timingpeerlist, "Addresses", addresses);
  wplist_dict_add_string(timingpeerlist, "ID", "67EAD1FA-7EAB-4810-82F7-A9132FD2D0BB");
  wplist_dict_add_bool(timingpeerlist, "SupportsClockPortMatchingOverride", false);
*/
  root = plist_new_dict();
  wplist_dict_add_string(root, "deviceID", "11:22:33:44:55:66");
//  wplist_dict_add_data(root, "eiv", airplay_aes_iv, sizeof(airplay_aes_iv));
//  wplist_dict_add_data(root, "ekey", airplay_aes_key, sizeof(airplay_aes_key));
//  wplist_dict_add_uint(root, "et", 0); // No encryption?
  wplist_dict_add_bool(root, "groupContainsGroupLeader", true);
  wplist_dict_add_string(root, "groupUUID", "3195C737-1E6E-4487-BECB-4D287B7C1234");
  wplist_dict_add_bool(root, "internalBuild", false);
  wplist_dict_add_bool(root, "isMultiSelectAirPlay", true);
  wplist_dict_add_string(root, "macAddress", "00:0c:29:f6:4a:f9");
  wplist_dict_add_string(root, "model", "iPhone10,4");
  wplist_dict_add_string(root, "osBuildVersion", "18B92");
  wplist_dict_add_string(root, "osName", "iPhone OS");
  wplist_dict_add_string(root, "osVersion", "14.2");
  wplist_dict_add_bool(root, "senderSupportsRelay", true);
  wplist_dict_add_string(root, "sessionUUID", "3195C737-1E6E-4487-BECB-4D287B7C7626");
  wplist_dict_add_string(root, "sourceVersion", "525.38.2");
//  plist_dict_set_item(root, "timingPeerInfo", timingpeerinfo); // only for PTP timing?
//  plist_dict_set_item(root, "timingPeerList", timingpeerlist); // only for PTP timing?
  wplist_dict_add_uint(root, "timingPort", timing_port);
  wplist_dict_add_string(root, "timingProtocol", "NTP");

  ret = wplist_to_bin(data, len, root);
  plist_free(root);
  return ret;
}

static int
session_url_set(struct airplay_session *rs)
{
  char *address = NULL;
  char *intf;
  unsigned short port;
  int family;
  uint32_t session_id;
  int ret;

  // Determine local address, needed for SDP and session URL
  evrtsp_connection_get_local_address(rs->ctrl, &address, &port, &family);
  if (!address || (port == 0))
    {
      DPRINTF(E_LOG, L_RAOP, "Could not determine local address\n");
      goto error;
    }

  intf = strchr(address, '%');
  if (intf)
    {
      *intf = '\0';
      intf++;
    }

  DPRINTF(E_DBG, L_RAOP, "Local address: %s (LL: %s) port %d\n", address, (intf) ? intf : "no", port);

  // Session ID and session URL
  gcry_randomize(&session_id, sizeof(session_id), GCRY_STRONG_RANDOM);

  if (family == AF_INET)
    ret = snprintf(rs->session_url, sizeof(rs->session_url), "rtsp://%s/%u", address, session_id);
  else
    ret = snprintf(rs->session_url, sizeof(rs->session_url), "rtsp://[%s]/%u", address, session_id);
  if ((ret < 0) || (ret >= sizeof(rs->session_url)))
    {
      DPRINTF(E_LOG, L_RAOP, "Session URL length exceeds 127 characters\n");
      goto error;
    }

  rs->local_address = address;
  return 0;

 error:
  free(address);
  return -1;
}

static int
airplay_send_req_setup(struct airplay_session *rs, evrtsp_req_cb cb, const char *log_caller)
{
  struct evrtsp_request *req = NULL;
  uint8_t *data;
  size_t len;
  int ret;

  DPRINTF(E_DBG, L_RAOP, "%s: Sending SETUP to '%s'\n", log_caller, rs->devname);

  ret = session_url_set(rs);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not make session url for device '%s'\n", rs->devname);
      goto error;
    }

  req = evrtsp_request_new(cb, rs);
  if (!req)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not create RTSP request for SETUP\n");
      goto error;
    }

  ret = airplay_add_headers(rs, req, EVRTSP_REQ_SETUP);
  if (ret < 0)
    {
      goto error;
    }

  evrtsp_add_header(req->output_headers, "Content-Type", "application/x-apple-binary-plist");

  ret = req_setup1_payload_create(&data, &len, rs->timing_svc->port, rs->local_address);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not create plist body for SETUP\n");
      goto error;
    }

  evbuffer_add(req->output_buffer, (unsigned char *)data, len);

  // Request UDP transport, AirTunes v2 streaming
/*  ret = snprintf(hdr, sizeof(hdr), "RTP/AVP/UDP;unicast;interleaved=0-1;mode=record;control_port=%u;timing_port=%u",
		 rs->control_svc->port, rs->timing_svc->port);
  if ((ret < 0) || (ret >= sizeof(hdr)))
    {
      DPRINTF(E_LOG, L_RAOP, "Transport header exceeds buffer length\n");
      goto error;
    }

  evrtsp_add_header(req->output_headers, "Transport", hdr);
*/

  ret = evrtsp_make_request(rs->ctrl, req, EVRTSP_REQ_SETUP, rs->session_url);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not make SETUP request to '%s'\n", rs->devname);
      goto error;
    }

  rs->reqs_in_flight++;

  return 0;

 error:
  evrtsp_request_free(req);
  return -1;
}

/*
static int
airplay_send_req_announce(struct airplay_session *rs, evrtsp_req_cb cb, const char *log_caller)
{
  uint8_t challenge[16];
  char *challenge_b64;
  char *ptr;
  struct evrtsp_request *req;
  char *address;
  char *intf;
  unsigned short port;
  int family;
  uint32_t session_id;
  int ret;

  DPRINTF(E_DBG, L_RAOP, "%s: Sending ANNOUNCE to '%s'\n", log_caller, rs->devname);

  // Determine local address, needed for SDP and session URL
  evrtsp_connection_get_local_address(rs->ctrl, &address, &port, &family);
  if (!address || (port == 0))
    {
      DPRINTF(E_LOG, L_RAOP, "Could not determine local address\n");

      if (address)
	free(address);

      return -1;
    }

  intf = strchr(address, '%');
  if (intf)
    {
      *intf = '\0';
      intf++;
    }

  DPRINTF(E_DBG, L_RAOP, "Local address: %s (LL: %s) port %d\n", address, (intf) ? intf : "no", port);

  req = evrtsp_request_new(cb, rs);
  if (!req)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not create RTSP request for ANNOUNCE\n");

      free(address);
      return -1;
    }

  // Session ID and session URL
  gcry_randomize(&session_id, sizeof(session_id), GCRY_STRONG_RANDOM);

  if (family == AF_INET)
    ret = snprintf(rs->session_url, sizeof(rs->session_url), "rtsp://%s/%u", address, session_id);
  else
    ret = snprintf(rs->session_url, sizeof(rs->session_url), "rtsp://[%s]/%u", address, session_id);
  if ((ret < 0) || (ret >= sizeof(rs->session_url)))
    {
      DPRINTF(E_LOG, L_RAOP, "Session URL length exceeds 127 characters\n");

      free(address);
      goto cleanup_req;
    }

  // SDP payload
  ret = airplay_make_sdp(rs, req, address, family, session_id);
  free(address);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not generate SDP payload for ANNOUNCE\n");

      goto cleanup_req;
    }

  ret = airplay_add_headers(rs, req, EVRTSP_REQ_ANNOUNCE);
  if (ret < 0)
    {
      evrtsp_request_free(req);
      return -1;
    }

  evrtsp_add_header(req->output_headers, "Content-Type", "application/sdp");

  // Challenge - but only if session is encrypted (important for ATV3 after update 6.0)
  if (rs->encrypt)
    {
      gcry_randomize(challenge, sizeof(challenge), GCRY_STRONG_RANDOM);
      challenge_b64 = b64_encode(challenge, sizeof(challenge));
      if (!challenge_b64)
	{
	  DPRINTF(E_LOG, L_RAOP, "Couldn't encode challenge\n");

	  goto cleanup_req;
	}

      // Remove base64 padding
      ptr = strchr(challenge_b64, '=');
      if (ptr)
	*ptr = '\0';

      evrtsp_add_header(req->output_headers, "Apple-Challenge", challenge_b64);

      free(challenge_b64);
    }

  ret = evrtsp_make_request(rs->ctrl, req, EVRTSP_REQ_ANNOUNCE, rs->session_url);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not make ANNOUNCE request to '%s'\n", rs->devname);

      return -1;
    }

  rs->reqs_in_flight++;

  return 0;

 cleanup_req:
  evrtsp_request_free(req);

  return -1;
}
*/

/*
The purpose of auth-setup is to authenticate the device and to exchange keys
for encryption. We don't do that, but some AirPlay 2 speakers (Sonos beam,
Airport Express fw 7.8) require this step anyway, otherwise we get a 403 to
our ANNOUNCE. So we do it with a flag for no encryption, and without actually
authenticating the device.

Good to know (source Apple's MFi Accessory Interface Specification):
- Curve25519 Elliptic-Curve Diffie-Hellman technology for key exchange
- RSA for signing and verifying and AES-128 in counter mode for encryption
- We start by sending a Curve25519 public key + no encryption flag
- The device responds with public key, MFi certificate and a signature, which
  is created by the device signing the two public keys with its RSA private
  key and then encrypting the result with the AES master key derived from the
  Curve25519 shared secret (generated from device private key and our public
  key)
- The AES key derived from the Curve25519 shared secret can then be used to
  encrypt future content
- New keys should be generated for each authentication attempt, but we don't
  do that because we don't really use this + it adds a libsodium dependency

Since we don't do auth or encryption, we currently just ignore the reponse.
*/
static int
airplay_send_req_auth_setup(struct airplay_session *rs, evrtsp_req_cb cb, const char *log_caller)
{
  struct evrtsp_request *req;
  int ret;

  DPRINTF(E_DBG, L_RAOP, "%s: Sending auth-setup to '%s'\n", log_caller, rs->devname);

  req = evrtsp_request_new(cb, rs);
  if (!req)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not create RTSP request for auth-setup\n");
      return -1;
    }

  ret = airplay_add_headers(rs, req, EVRTSP_REQ_POST);
  if (ret < 0)
    {
      evrtsp_request_free(req);
      return -1;
    }

  evrtsp_add_header(req->output_headers, "Content-Type", "application/octet-stream");

  // Flag for no encryption. 0x10 may mean encryption.
  evbuffer_add(req->output_buffer, "\x01", 1);

  evbuffer_add(req->output_buffer, airplay_auth_setup_pubkey, sizeof(airplay_auth_setup_pubkey) - 1);

  ret = evrtsp_make_request(rs->ctrl, req, EVRTSP_REQ_POST, "/auth-setup");
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not make auth-setup request to '%s'\n", rs->devname);
      return -1;
    }

  rs->reqs_in_flight++;

  evrtsp_connection_set_closecb(rs->ctrl, NULL, NULL);

  return 0;
}

static int
airplay_send_req_options(struct airplay_session *rs, evrtsp_req_cb cb, const char *log_caller)
{
  struct evrtsp_request *req;
  int ret;

  DPRINTF(E_DBG, L_RAOP, "%s: Sending OPTIONS to '%s'\n", log_caller, rs->devname);

  req = evrtsp_request_new(cb, rs);
  if (!req)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not create RTSP request for OPTIONS\n");
      return -1;
    }

  ret = airplay_add_headers(rs, req, EVRTSP_REQ_OPTIONS);
  if (ret < 0)
    {
      evrtsp_request_free(req);
      return -1;
    }

  ret = evrtsp_make_request(rs->ctrl, req, EVRTSP_REQ_OPTIONS, "*");
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not make OPTIONS request to '%s'\n", rs->devname);
      return -1;
    }

  rs->reqs_in_flight++;

  evrtsp_connection_set_closecb(rs->ctrl, NULL, NULL);

  return 0;
}

#ifdef RAOP_VERIFICATION
static int
airplay_send_req_pin_start(struct airplay_session *rs, evrtsp_req_cb cb, const char *log_caller)
{
  struct evrtsp_request *req;
  int ret;

  DPRINTF(E_DBG, L_RAOP, "%s: Sending pair-pin-start to '%s'\n", log_caller, rs->devname);

  req = evrtsp_request_new(cb, rs);
  if (!req)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not create RTSP request to '%s' for pair-pin-start\n", rs->devname);
      return -1;
    }

  ret = airplay_add_headers(rs, req, EVRTSP_REQ_POST);
  if (ret < 0)
    {
      evrtsp_request_free(req);
      return -1;
    }

  DPRINTF(E_LOG, L_RAOP, "Starting device verification for '%s', go to the web interface and enter PIN\n", rs->devname);

  ret = evrtsp_make_request(rs->ctrl, req, EVRTSP_REQ_POST, "/pair-pin-start");
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not make pair-pin-start request\n");
      return -1;
    }

  rs->reqs_in_flight++;

  evrtsp_connection_set_closecb(rs->ctrl, NULL, NULL);

  return 0;
}
#else
static int
airplay_send_req_pin_start(struct airplay_session *rs, evrtsp_req_cb cb, const char *log_caller)
{
  DPRINTF(E_LOG, L_RAOP, "Device '%s' requires verification, but forked-daapd was built with --disable-verification\n", rs->devname);
  return -1;
}
#endif


/* ------------------------------ Session handling -------------------------- */

// Maps our internal state to the generic output state and then makes a callback
// to the player to tell that state
static void
airplay_status(struct airplay_session *rs)
{
  enum output_device_state state;

  switch (rs->state)
    {
      case RAOP_STATE_PASSWORD:
	state = OUTPUT_STATE_PASSWORD;
	break;
      case RAOP_STATE_FAILED:
	state = OUTPUT_STATE_FAILED;
	break;
      case RAOP_STATE_STOPPED:
	state = OUTPUT_STATE_STOPPED;
	break;
      case RAOP_STATE_STARTUP ... RAOP_STATE_RECORD:
	state = OUTPUT_STATE_STARTUP;
	break;
      case RAOP_STATE_CONNECTED:
	state = OUTPUT_STATE_CONNECTED;
	break;
      case RAOP_STATE_STREAMING:
	state = OUTPUT_STATE_STREAMING;
	break;
      case RAOP_STATE_TEARDOWN:
	DPRINTF(E_LOG, L_RAOP, "Bug! airplay_status() called with transitional state (TEARDOWN)\n");
	state = OUTPUT_STATE_STOPPED;
	break;
      default:
	DPRINTF(E_LOG, L_RAOP, "Bug! Unhandled state in airplay_status(): %d\n", rs->state);
	state = OUTPUT_STATE_FAILED;
    }

  outputs_cb(rs->callback_id, rs->device_id, state);
  rs->callback_id = -1;
}

static struct airplay_master_session *
master_session_make(struct media_quality *quality, bool encrypt)
{
  struct airplay_master_session *rms;
  int ret;

  // First check if we already have a suitable session
  for (rms = airplay_master_sessions; rms; rms = rms->next)
    {
      if (encrypt == rms->encrypt && quality_is_equal(quality, &rms->rtp_session->quality))
	return rms;
    }

  // Let's create a master session
  ret = outputs_quality_subscribe(quality);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not subscribe to required audio quality (%d/%d/%d)\n", quality->sample_rate, quality->bits_per_sample, quality->channels);
      return NULL;
    }

  CHECK_NULL(L_RAOP, rms = calloc(1, sizeof(struct airplay_master_session)));

  rms->rtp_session = rtp_session_new(quality, RAOP_PACKET_BUFFER_SIZE, 0);
  if (!rms->rtp_session)
    {
      outputs_quality_unsubscribe(quality);
      free(rms);
      return NULL;
    }

  rms->encrypt = encrypt;
  rms->samples_per_packet = RAOP_SAMPLES_PER_PACKET;
  rms->rawbuf_size = STOB(rms->samples_per_packet, quality->bits_per_sample, quality->channels);
  rms->output_buffer_samples = OUTPUTS_BUFFER_DURATION * quality->sample_rate;

  CHECK_NULL(L_RAOP, rms->rawbuf = malloc(rms->rawbuf_size));
  CHECK_NULL(L_RAOP, rms->evbuf = evbuffer_new());

  rms->next = airplay_master_sessions;
  airplay_master_sessions = rms;

  return rms;
}

static void
master_session_free(struct airplay_master_session *rms)
{
  if (!rms)
    return;

  outputs_quality_unsubscribe(&rms->rtp_session->quality);
  rtp_session_free(rms->rtp_session);
  evbuffer_free(rms->evbuf);
  free(rms->rawbuf);
  free(rms);
}

static void
master_session_cleanup(struct airplay_master_session *rms)
{
  struct airplay_master_session *s;
  struct airplay_session *rs;

  // First check if any other session is using the master session
  for (rs = airplay_sessions; rs; rs=rs->next)
    {
      if (rs->master_session == rms)
	return;
    }

  if (rms == airplay_master_sessions)
    airplay_master_sessions = airplay_master_sessions->next;
  else
    {
      for (s = airplay_master_sessions; s && (s->next != rms); s = s->next)
	; /* EMPTY */

      if (!s)
	DPRINTF(E_WARN, L_RAOP, "WARNING: struct airplay_master_session not found in list; BUG!\n");
      else
	s->next = rms->next;
    }

  master_session_free(rms);
}

static void
session_free(struct airplay_session *rs)
{
  if (!rs)
    return;

  if (rs->master_session)
    master_session_cleanup(rs->master_session);

  if (rs->ctrl)
    {
      evrtsp_connection_set_closecb(rs->ctrl, NULL, NULL);
      evrtsp_connection_free(rs->ctrl);
    }

  if (rs->deferredev)
    event_free(rs->deferredev);

  if (rs->server_fd >= 0)
    close(rs->server_fd);

  pair_cipher_free(rs->pair_cipher_ctx);

  free(rs->local_address);
  free(rs->realm);
  free(rs->nonce);
  free(rs->session);
  free(rs->address);
  free(rs->devname);

  free(rs);
}

static void
session_cleanup(struct airplay_session *rs)
{
  struct airplay_session *s;

  if (rs == airplay_sessions)
    airplay_sessions = airplay_sessions->next;
  else
    {
      for (s = airplay_sessions; s && (s->next != rs); s = s->next)
	; /* EMPTY */

      if (!s)
	DPRINTF(E_WARN, L_RAOP, "WARNING: struct airplay_session not found in list; BUG!\n");
      else
	s->next = rs->next;
    }

  outputs_device_session_remove(rs->device_id);

  session_free(rs);
}

static void
session_failure(struct airplay_session *rs)
{
  /* Session failed, let our user know */
  if (rs->state != RAOP_STATE_PASSWORD)
    rs->state = RAOP_STATE_FAILED;

  airplay_status(rs);

  session_cleanup(rs);
}

static void
deferred_session_failure(struct airplay_session *rs)
{
  struct timeval tv;

  rs->state = RAOP_STATE_FAILED;

  evutil_timerclear(&tv);
  evtimer_add(rs->deferredev, &tv);
}

static void
airplay_rtsp_close_cb(struct evrtsp_connection *evcon, void *arg)
{
  struct airplay_session *rs = arg;

  DPRINTF(E_LOG, L_RAOP, "Device '%s' closed RTSP connection\n", rs->devname);

  deferred_session_failure(rs);
}

static void
session_teardown_cb(struct evrtsp_request *req, void *arg)
{
  struct airplay_session *rs = arg;

  rs->reqs_in_flight--;

  if (!req)
    DPRINTF(E_LOG, L_RAOP, "TEARDOWN request failed in session shutdown\n");
  else if (req->response_code != RTSP_OK)
    DPRINTF(E_LOG, L_RAOP, "TEARDOWN request failed in session shutdown: %d %s\n", req->response_code, req->response_code_line);

  rs->state = RAOP_STATE_STOPPED;

  airplay_status(rs);

  session_cleanup(rs);
}

static int
session_teardown(struct airplay_session *rs, const char *log_caller)
{
  int ret;

  ret = airplay_send_req_teardown(rs, session_teardown_cb, log_caller);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "%s: TEARDOWN request failed!\n", log_caller);
      deferred_session_failure(rs);
    }

  // Change state immediately so we won't write any more to the device
  rs->state = RAOP_STATE_TEARDOWN;

  return ret;
}

static void
deferredev_cb(int fd, short what, void *arg)
{
  struct airplay_session *rs = arg;

  if (rs->state == RAOP_STATE_FAILED)
    {
      DPRINTF(E_DBG, L_RAOP, "Cleaning up failed session (deferred) on device '%s'\n", rs->devname);
      session_failure(rs);
    }
  else
    {
      DPRINTF(E_DBG, L_RAOP, "Flush timer expired; tearing down RAOP session on '%s'\n", rs->devname);
      session_teardown(rs, "deferredev_cb");
    }
}

static int
session_connection_setup(struct airplay_session *rs, struct output_device *rd, int family)
{
  char *address;
  char *intf;
  unsigned short port;
  int ret;

  rs->sa.ss.ss_family = family;
  switch (family)
    {
      case AF_INET:
	/* We always have the v4 services, so no need to check */
	if (!rd->v4_address)
	  return -1;

	address = rd->v4_address;
	port = rd->v4_port;

	rs->timing_svc = &timing_4svc;
	rs->control_svc = &control_4svc;

	ret = inet_pton(AF_INET, address, &rs->sa.sin.sin_addr);
	break;

      case AF_INET6:
	if (!rd->v6_address || rd->v6_disabled || (timing_6svc.fd < 0) || (control_6svc.fd < 0))
	  return -1;

	address = rd->v6_address;
	port = rd->v6_port;

	rs->timing_svc = &timing_6svc;
	rs->control_svc = &control_6svc;

	intf = strchr(address, '%');
	if (intf)
	  *intf = '\0';

	ret = inet_pton(AF_INET6, address, &rs->sa.sin6.sin6_addr);

	if (intf)
	  {
	    *intf = '%';

	    intf++;

	    rs->sa.sin6.sin6_scope_id = if_nametoindex(intf);
	    if (rs->sa.sin6.sin6_scope_id == 0)
	      {
		DPRINTF(E_LOG, L_RAOP, "Could not find interface %s\n", intf);

		ret = -1;
		break;
	      }
	  }

	break;

      default:
	return -1;
    }

  if (ret <= 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Device '%s' has invalid address (%s) for %s\n", rd->name, address, (family == AF_INET) ? "ipv4" : "ipv6");
      return -1;
    }

  rs->ctrl = evrtsp_connection_new(address, port);
  if (!rs->ctrl)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not create control connection to '%s' (%s)\n", rd->name, address);
      return -1;
    }

  evrtsp_connection_set_base(rs->ctrl, evbase_player);

  rs->address = strdup(address);
  rs->family = family;

  return 0;
}

static struct airplay_session *
session_make(struct output_device *rd, int callback_id, bool only_probe)
{
  struct airplay_session *rs;
  struct airplay_extra *re;
  int ret;

  re = rd->extra_device_info;


  CHECK_NULL(L_RAOP, rs = calloc(1, sizeof(struct airplay_session)));
  CHECK_NULL(L_RAOP, rs->deferredev = evtimer_new(evbase_player, deferredev_cb, rs));

  rs->devname = strdup(rd->name);
  rs->volume = rd->volume;

  rs->state = RAOP_STATE_STOPPED;
  rs->only_probe = only_probe;
  rs->reqs_in_flight = 0;
  rs->cseq = 1;

  rs->device_id = rd->id;
  rs->callback_id = callback_id;

  rs->server_fd = -1;

  rs->password = rd->password;

  rs->supports_auth_setup = re->supports_auth_setup;
  rs->wanted_metadata = re->wanted_metadata;

  switch (re->devtype)
    {
      case RAOP_DEV_APEX1_80211G:
	rs->encrypt = 1;
	rs->auth_quirk_itunes = 1;
	break;

      case RAOP_DEV_APEX2_80211N:
	rs->encrypt = 1;
	rs->auth_quirk_itunes = 0;
	break;

      case RAOP_DEV_APEX3_80211N:
	rs->encrypt = 0;
	rs->auth_quirk_itunes = 0;
	break;

      case RAOP_DEV_APPLETV:
	rs->encrypt = 0;
	rs->auth_quirk_itunes = 0;
	break;

      case RAOP_DEV_APPLETV4:
	rs->encrypt = 0;
	rs->auth_quirk_itunes = 0;
	break;

      default:
	rs->encrypt = re->encrypt;
	rs->auth_quirk_itunes = 0;
    }

  ret = session_connection_setup(rs, rd, AF_INET6);
  if (ret < 0)
    {
      ret = session_connection_setup(rs, rd, AF_INET);
      if (ret < 0)
	goto error;
    }

  rs->master_session = master_session_make(&rd->quality, rs->encrypt);
  if (!rs->master_session)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not attach a master session for device '%s'\n", rd->name);
      goto error;
    }

  // Attach to list of sessions
  rs->next = airplay_sessions;
  airplay_sessions = rs;

  // rs is now the official device session
  outputs_device_session_add(rd->id, rs);

  return rs;

 error:
  session_free(rs);

  return NULL;
}


/* ----------------------------- Metadata handling -------------------------- */

static void
airplay_metadata_free(struct airplay_metadata *rmd)
{
  if (!rmd)
    return;

  if (rmd->metadata)
    evbuffer_free(rmd->metadata);
  if (rmd->artwork)
    evbuffer_free(rmd->artwork);

  free(rmd);
}

static void
airplay_metadata_purge(void)
{
  if (!airplay_cur_metadata)
    return;

  airplay_metadata_free(airplay_cur_metadata->priv);
  free(airplay_cur_metadata);
  airplay_cur_metadata = NULL;
}

// *** Thread: worker ***
static void *
airplay_metadata_prepare(struct output_metadata *metadata)
{
  struct db_queue_item *queue_item;
  struct airplay_metadata *rmd;
  struct evbuffer *tmp;
  int ret;

  queue_item = db_queue_fetch_byitemid(metadata->item_id);
  if (!queue_item)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not fetch queue item\n");
      return NULL;
    }

  CHECK_NULL(L_RAOP, rmd = calloc(1, sizeof(struct airplay_metadata)));
  CHECK_NULL(L_RAOP, rmd->artwork = evbuffer_new());
  CHECK_NULL(L_RAOP, rmd->metadata = evbuffer_new());
  CHECK_NULL(L_RAOP, tmp = evbuffer_new());

  ret = artwork_get_item(rmd->artwork, queue_item->file_id, ART_DEFAULT_WIDTH, ART_DEFAULT_HEIGHT, 0);
  if (ret < 0)
    {
      DPRINTF(E_INFO, L_RAOP, "Failed to retrieve artwork for file '%s'; no artwork will be sent\n", queue_item->path);
      evbuffer_free(rmd->artwork);
      rmd->artwork = NULL;
    }

  rmd->artwork_fmt = ret;

  ret = dmap_encode_queue_metadata(rmd->metadata, tmp, queue_item);
  evbuffer_free(tmp);
  free_queue_item(queue_item, 0);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not encode file metadata; metadata will not be sent\n");
      airplay_metadata_free(rmd);
      return NULL;
    }

  return rmd;
}

static void
airplay_cb_metadata(struct evrtsp_request *req, void *arg)
{
  struct airplay_session *rs = arg;
  int ret;

  rs->reqs_in_flight--;

  if (!req)
    goto error;

  if (req->response_code != RTSP_OK)
    DPRINTF(E_WARN, L_RAOP, "SET_PARAMETER metadata/artwork/progress request to '%s' failed (proceeding anyway): %d %s\n", rs->devname, req->response_code, req->response_code_line);

  ret = airplay_check_cseq(rs, req);
  if (ret < 0)
    goto error;

  /* No callback to player, user doesn't want/need to know about the status
   * of metadata requests unless they cause the session to fail.
   */

  if (!rs->reqs_in_flight)
    evrtsp_connection_set_closecb(rs->ctrl, airplay_rtsp_close_cb, rs);

  return;

 error:
  session_failure(rs);
}

static void
airplay_metadata_rtptimes_get(uint32_t *start, uint32_t *display, uint32_t *pos, uint32_t *end, struct airplay_master_session *rms, struct output_metadata *metadata)
{
  struct rtp_session *rtp_session = rms->rtp_session;
  // All the calculations with long ints to avoid surprises
  int64_t sample_rate;
  int64_t diff_ms;
  int64_t elapsed_ms;
  int64_t elapsed_samples;
  int64_t len_samples;

  sample_rate = rtp_session->quality.sample_rate;

  // First calculate the rtptime that streaming of this item started:
  // - at time metadata->pts the elapsed time was metadata->pos_ms
  // - the time is now rms->cur_stamp.ts and the position is rms->cur_stamp.pos
  // -> time since item started is elapsed_ms = metadata->pos_ms + (rms->cur_stamp.ts - metadata->pts)
  // -> start must then be start = rms->cur_stamp.pos - elapsed_ms * sample_rate;
  diff_ms         = (rms->cur_stamp.ts.tv_sec - metadata->pts.tv_sec) * 1000L + (rms->cur_stamp.ts.tv_nsec - metadata->pts.tv_nsec) / 1000000L;
  elapsed_ms      = (int64_t)metadata->pos_ms + diff_ms;
  elapsed_samples = elapsed_ms * sample_rate / 1000;
  *start          = rms->cur_stamp.pos - elapsed_samples;

/*  DPRINTF(E_DBG, L_RAOP, "pos_ms=%u, len_ms=%u, startup=%d, metadata.pts=%ld.%09ld, player.ts=%ld.%09ld, diff_ms=%" PRIi64 ", elapsed_ms=%" PRIi64 "\n",
    metadata->pos_ms, metadata->len_ms, metadata->startup, metadata->pts.tv_sec, metadata->pts.tv_nsec, rms->cur_stamp.ts.tv_sec, rms->cur_stamp.ts.tv_nsec, diff_ms, elapsed_ms);
*/
  // Here's the deal with progress values:
  // - display is always start minus a delay
  //    -> delay x1 if streaming is starting for this device (joining or not)
  //    -> delay x2 if stream is switching to a new song
  //    TODO what if we are just sending a keep_alive?
  // - pos is the RTP time of the first sample for this song for this device
  //    -> start of song
  //    -> start of song + offset if device is joining in the middle of a song,
  //       or getting out of a pause or seeking
  // - end is the RTP time of the last sample for this song
  len_samples     = (int64_t)metadata->len_ms * sample_rate / 1000;
  *display        = metadata->startup ? *start - RAOP_MD_DELAY_STARTUP : *start - RAOP_MD_DELAY_SWITCH;
  *pos            = MAX(rms->cur_stamp.pos, *start);
  *end            = len_samples ? *start + len_samples : *pos;

  DPRINTF(E_SPAM, L_RAOP, "start=%u, display=%u, pos=%u, end=%u, rtp_session.pos=%u, cur_stamp.pos=%u\n",
    *start, *display, *pos, *end, rtp_session->pos, rms->cur_stamp.pos);
}

static int
airplay_metadata_send_progress(struct airplay_session *rs, struct evbuffer *evbuf, struct airplay_metadata *rmd, uint32_t display, uint32_t pos, uint32_t end)
{
  int ret;

  ret = evbuffer_add_printf(evbuf, "progress: %u/%u/%u\r\n", display, pos, end);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not build progress string for sending\n");
      return -1;
    }

  ret = airplay_send_req_set_parameter(rs, evbuf, "text/parameters", NULL, airplay_cb_metadata, "send_progress");
  if (ret < 0)
    DPRINTF(E_LOG, L_RAOP, "Could not send SET_PARAMETER progress request to '%s'\n", rs->devname);

  return ret;
}

static int
airplay_metadata_send_artwork(struct airplay_session *rs, struct evbuffer *evbuf, struct airplay_metadata *rmd, char *rtptime)
{
  char *ctype;
  uint8_t *buf;
  size_t len;
  int ret;

  switch (rmd->artwork_fmt)
    {
      case ART_FMT_PNG:
	ctype = "image/png";
	break;

      case ART_FMT_JPEG:
	ctype = "image/jpeg";
	break;

      default:
	DPRINTF(E_LOG, L_RAOP, "Unsupported artwork format %d\n", rmd->artwork_fmt);
	return -1;
    }

  buf = evbuffer_pullup(rmd->artwork, -1);
  len = evbuffer_get_length(rmd->artwork);

  ret = evbuffer_add(evbuf, buf, len);
  if (ret != 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not copy artwork for sending\n");
      return -1;
    }

  ret = airplay_send_req_set_parameter(rs, evbuf, ctype, rtptime, airplay_cb_metadata, "send_artwork");
  if (ret < 0)
    DPRINTF(E_LOG, L_RAOP, "Could not send SET_PARAMETER artwork request to '%s'\n", rs->devname);

  return ret;
}

static int
airplay_metadata_send_text(struct airplay_session *rs, struct evbuffer *evbuf, struct airplay_metadata *rmd, char *rtptime)
{
  uint8_t *buf;
  size_t len;
  int ret;

  buf = evbuffer_pullup(rmd->metadata, -1);
  len = evbuffer_get_length(rmd->metadata);

  ret = evbuffer_add(evbuf, buf, len);
  if (ret != 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not copy metadata for sending\n");
      return -1;
    }

  ret = airplay_send_req_set_parameter(rs, evbuf, "application/x-dmap-tagged", rtptime, airplay_cb_metadata, "send_text");
  if (ret < 0)
    DPRINTF(E_LOG, L_RAOP, "Could not send SET_PARAMETER metadata request to '%s'\n", rs->devname);

  return ret;
}

static int
airplay_metadata_send_generic(struct airplay_session *rs, struct output_metadata *metadata, bool only_progress)
{
  struct airplay_metadata *rmd = metadata->priv;
  struct evbuffer *evbuf;
  uint32_t start;
  uint32_t display;
  uint32_t pos;
  uint32_t end;
  char rtptime[32];
  int ret;

  airplay_metadata_rtptimes_get(&start, &display, &pos, &end, rs->master_session, metadata);

  ret = snprintf(rtptime, sizeof(rtptime), "rtptime=%u", start);
  if ((ret < 0) || (ret >= sizeof(rtptime)))
    {
      DPRINTF(E_LOG, L_RAOP, "RTP-Info too big for buffer while sending metadata\n");
      return -1;
    }

  CHECK_NULL(L_RAOP, evbuf = evbuffer_new());

  if (rs->wanted_metadata & RAOP_MD_WANTS_PROGRESS)
    {
      ret = airplay_metadata_send_progress(rs, evbuf, rmd, display, pos, end);
      if (ret < 0)
	goto error;
    }

  if (!only_progress && (rs->wanted_metadata & RAOP_MD_WANTS_TEXT))
    {
      ret = airplay_metadata_send_text(rs, evbuf, rmd, rtptime);
      if (ret < 0)
	goto error;
    }

  if (!only_progress && (rs->wanted_metadata & RAOP_MD_WANTS_ARTWORK) && rmd->artwork)
    {
      ret = airplay_metadata_send_artwork(rs, evbuf, rmd, rtptime);
      if (ret < 0)
	goto error;
    }

  evbuffer_free(evbuf);
  return 0;

 error:
  evbuffer_free(evbuf);
  return -1;
}

static int
airplay_metadata_startup_send(struct airplay_session *rs)
{
  if (!rs->wanted_metadata || !airplay_cur_metadata)
    return 0;

  airplay_cur_metadata->startup = true;

  return airplay_metadata_send_generic(rs, airplay_cur_metadata, false);
}

static int
airplay_metadata_keep_alive_send(struct airplay_session *rs)
{
  if (!rs->wanted_metadata || !airplay_cur_metadata)
    return 0;

  airplay_cur_metadata->startup = false;

  return airplay_metadata_send_generic(rs, airplay_cur_metadata, true);
}

static void
airplay_metadata_send(struct output_metadata *metadata)
{
  struct airplay_session *rs;
  struct airplay_session *next;
  int ret;

  for (rs = airplay_sessions; rs; rs = next)
    {
      next = rs->next;

      if (!(rs->state & RAOP_STATE_F_CONNECTED) || !rs->wanted_metadata)
	continue;

      ret = airplay_metadata_send_generic(rs, metadata, false);
      if (ret < 0)
	{
	  session_failure(rs);
	  continue;
	}
    }

  // Replace current metadata with the new stuff
  airplay_metadata_purge();
  airplay_cur_metadata = metadata;
}


/* ------------------------------ Volume handling --------------------------- */

static float
airplay_volume_from_pct(int volume, char *name)
{
  float airplay_volume;
  cfg_t *airplay;
  int max_volume;

  max_volume = RAOP_CONFIG_MAX_VOLUME;

  airplay = cfg_gettsec(cfg, "airplay", name);
  if (airplay)
    max_volume = cfg_getint(airplay, "max_volume");

  if ((max_volume < 1) || (max_volume > RAOP_CONFIG_MAX_VOLUME))
    {
      DPRINTF(E_LOG, L_RAOP, "Config has bad max_volume (%d) for device '%s', using default instead\n", max_volume, name);

      max_volume = RAOP_CONFIG_MAX_VOLUME;
    }

  /* RAOP volume
   *  -144.0 is off
   *  0 - 100 maps to -30.0 - 0
   */
  if (volume > 0 && volume <= 100)
    airplay_volume = -30.0 + ((float)max_volume * (float)volume * 30.0) / (100.0 * RAOP_CONFIG_MAX_VOLUME);
  else
    airplay_volume = -144.0;

  return airplay_volume;
}

static int
airplay_volume_to_pct(struct output_device *rd, const char *volume)
{
  float airplay_volume;
  cfg_t *airplay;
  int max_volume;

  airplay_volume = atof(volume);

  // Basic sanity check
  if (airplay_volume == 0.0 && volume[0] != '0')
    {
      DPRINTF(E_LOG, L_RAOP, "RAOP device volume is invalid: '%s'\n", volume);
      return -1;
    }

  max_volume = RAOP_CONFIG_MAX_VOLUME;

  airplay = cfg_gettsec(cfg, "airplay", rd->name);
  if (airplay)
    max_volume = cfg_getint(airplay, "max_volume");

  if ((max_volume < 1) || (max_volume > RAOP_CONFIG_MAX_VOLUME))
    {
      DPRINTF(E_LOG, L_RAOP, "Config has bad max_volume (%d) for device '%s', using default instead\n", max_volume, rd->name);
      max_volume = RAOP_CONFIG_MAX_VOLUME;
    }

  // RAOP volume: -144.0 is off, -30.0 - 0 scaled by max_volume maps to 0 - 100
  if (airplay_volume > -30.0 && airplay_volume <= 0.0)
    return (int)(100.0 * (airplay_volume / 30.0 + 1.0) * RAOP_CONFIG_MAX_VOLUME / (float)max_volume);
  else
    return 0;
}

static int
airplay_set_volume_internal(struct airplay_session *rs, int volume, evrtsp_req_cb cb)
{
  struct evbuffer *evbuf;
  float airplay_volume;
  int ret;

  evbuf = evbuffer_new();
  if (!evbuf)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not allocate evbuffer for volume payload\n");

      return -1;
    }

  airplay_volume = airplay_volume_from_pct(volume, rs->devname);

  /* Don't let locales get in the way here */
  /* We use -%d and -(int)airplay_volume so -0.3 won't become 0.3 */
  ret = evbuffer_add_printf(evbuf, "volume: -%d.%06d\r\n", -(int)airplay_volume, -(int)(1000000.0 * (airplay_volume - (int)airplay_volume)));
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Out of memory for SET_PARAMETER payload (volume)\n");

      evbuffer_free(evbuf);
      return -1;
    }

  ret = airplay_send_req_set_parameter(rs, evbuf, "text/parameters", NULL, cb, "volume_internal");
  if (ret < 0)
    DPRINTF(E_LOG, L_RAOP, "Could not send SET_PARAMETER request for volume to '%s'\n", rs->devname);

  evbuffer_free(evbuf);

  rs->volume = volume;

  return ret;
}

static void
airplay_cb_set_volume(struct evrtsp_request *req, void *arg)
{
  struct airplay_session *rs = arg;
  int ret;

  rs->reqs_in_flight--;

  if (!req)
    goto error;

  if (req->response_code != RTSP_OK)
    {
      DPRINTF(E_LOG, L_RAOP, "SET_PARAMETER request to '%s' failed for stream volume: %d %s\n", rs->devname, req->response_code, req->response_code_line);

      goto error;
    }

  ret = airplay_check_cseq(rs, req);
  if (ret < 0)
    goto error;

  /* Let our user know */
  airplay_status(rs);

  if (!rs->reqs_in_flight)
    evrtsp_connection_set_closecb(rs->ctrl, airplay_rtsp_close_cb, rs);

  return;

 error:
  session_failure(rs);
}

/* Volume in [0 - 100] */
static int
airplay_set_volume_one(struct output_device *device, int callback_id)
{
  struct airplay_session *rs = device->session;
  int ret;

  if (!rs || !(rs->state & RAOP_STATE_F_CONNECTED))
    return 0;

  ret = airplay_set_volume_internal(rs, device->volume, airplay_cb_set_volume);
  if (ret < 0)
    {
      session_failure(rs);

      return 0;
    }

  rs->callback_id = callback_id;

  return 1;
}

static void
airplay_cb_flush(struct evrtsp_request *req, void *arg)
{
  struct airplay_session *rs = arg;
  int ret;

  rs->reqs_in_flight--;

  if (!req)
    goto error;

  if (req->response_code != RTSP_OK)
    {
      DPRINTF(E_LOG, L_RAOP, "FLUSH request to '%s' failed: %d %s\n", rs->devname, req->response_code, req->response_code_line);

      goto error;
    }

  ret = airplay_check_cseq(rs, req);
  if (ret < 0)
    goto error;

  rs->state = RAOP_STATE_CONNECTED;

  /* Let our user know */
  airplay_status(rs);

  if (!rs->reqs_in_flight)
    evrtsp_connection_set_closecb(rs->ctrl, airplay_rtsp_close_cb, rs);

  return;

 error:
  session_failure(rs);
}

static void
airplay_keep_alive_timer_cb(int fd, short what, void *arg)
{
  struct airplay_session *rs;

  if (!airplay_sessions)
    {
      event_del(keep_alive_timer);
      return;
    }

  for (rs = airplay_sessions; rs; rs = rs->next)
    {
      if (!(rs->state & RAOP_STATE_F_CONNECTED))
	continue;

      airplay_metadata_keep_alive_send(rs);
    }

  evtimer_add(keep_alive_timer, &keep_alive_tv);
}


/* -------------------- Creation and sending of RTP packets  ---------------- */

static int
packet_prepare(struct rtp_packet *pkt, uint8_t *rawbuf, size_t rawbuf_size, bool encrypt)
{
  char ebuf[64];
  gpg_error_t gc_err;

  alac_encode(pkt->payload, rawbuf, rawbuf_size);

  if (!encrypt)
    return 0;

  // Reset cipher
  gc_err = gcry_cipher_reset(airplay_aes_ctx);
  if (gc_err != GPG_ERR_NO_ERROR)
    {
      gpg_strerror_r(gc_err, ebuf, sizeof(ebuf));
      DPRINTF(E_LOG, L_RAOP, "Could not reset AES cipher: %s\n", ebuf);
      return -1;
    }

  // Set IV
  gc_err = gcry_cipher_setiv(airplay_aes_ctx, airplay_aes_iv, sizeof(airplay_aes_iv));
  if (gc_err != GPG_ERR_NO_ERROR)
    {
      gpg_strerror_r(gc_err, ebuf, sizeof(ebuf));
      DPRINTF(E_LOG, L_RAOP, "Could not set AES IV: %s\n", ebuf);
      return -1;
    }

  // Encrypt in blocks of 16 bytes
  gc_err = gcry_cipher_encrypt(airplay_aes_ctx, pkt->payload, (pkt->payload_len / 16) * 16, NULL, 0);
  if (gc_err != GPG_ERR_NO_ERROR)
    {
      gpg_strerror_r(gc_err, ebuf, sizeof(ebuf));
      DPRINTF(E_LOG, L_RAOP, "Could not encrypt payload: %s\n", ebuf);
      return -1;
    }

  return 0;
}

static int
packet_send(struct airplay_session *rs, struct rtp_packet *pkt)
{
  int ret;

  if (!rs)
    return -1;

  ret = send(rs->server_fd, pkt->data, pkt->data_len, 0);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Send error for '%s': %s\n", rs->devname, strerror(errno));

      // Can't free it right away, it would make the ->next in the calling
      // master_session and session loops invalid
      deferred_session_failure(rs);
      return -1;
    }
  else if (ret != pkt->data_len)
    {
      DPRINTF(E_WARN, L_RAOP, "Partial send (%d) for '%s'\n", ret, rs->devname);
      return -1;
    }

/*  DPRINTF(E_DBG, L_RAOP, "RTP PACKET seqnum %u, rtptime %u, payload 0x%x, pktbuf_s %zu\n",
    rs->master_session->rtp_session->seqnum,
    rs->master_session->rtp_session->pos,
    pkt->header[1],
    rs->master_session->rtp_session->pktbuf_len
    );
*/
  return 0;
}

static void
control_packet_send(struct airplay_session *rs, struct rtp_packet *pkt)
{
  int len;
  int ret;

  switch (rs->sa.ss.ss_family)
    {
      case AF_INET:
	rs->sa.sin.sin_port = htons(rs->control_port);
	len = sizeof(rs->sa.sin);
	break;

      case AF_INET6:
	rs->sa.sin6.sin6_port = htons(rs->control_port);
	len = sizeof(rs->sa.sin6);
	break;

      default:
	DPRINTF(E_WARN, L_RAOP, "Unknown family %d\n", rs->sa.ss.ss_family);
	return;
    }

  ret = sendto(rs->control_svc->fd, pkt->data, pkt->data_len, 0, &rs->sa.sa, len);
  if (ret < 0)
    DPRINTF(E_LOG, L_RAOP, "Could not send playback sync to device '%s': %s\n", rs->devname, strerror(errno));
}

static void
packets_resend(struct airplay_session *rs, uint16_t seqnum, int len)
{
  struct rtp_session *rtp_session;
  struct rtp_packet *pkt;
  uint16_t s;
  int i;
  bool pkt_missing = false;

  rtp_session = rs->master_session->rtp_session;

  DPRINTF(E_DBG, L_RAOP, "Got retransmit request from '%s': seqnum %" PRIu16 " (len %d), last RTP session seqnum %" PRIu16 " (len %zu)\n",
    rs->devname, seqnum, len, rtp_session->seqnum - 1, rtp_session->pktbuf_len);

  // Note that seqnum may wrap around, so we don't use it for counting
  for (i = 0, s = seqnum; i < len; i++, s++)
    {
      pkt = rtp_packet_get(rtp_session, s);
      if (pkt)
	packet_send(rs, pkt);
      else
	pkt_missing = true;
    }

  if (pkt_missing)
    DPRINTF(E_WARN, L_RAOP, "Device '%s' retransmit request for seqnum %" PRIu16 " (len %d) is outside buffer range (last seqnum %" PRIu16 ", len %zu)\n",
      rs->devname, seqnum, len, rtp_session->seqnum - 1, rtp_session->pktbuf_len);
}

static int
packets_send(struct airplay_master_session *rms)
{
  struct rtp_packet *pkt;
  struct airplay_session *rs;
  int ret;

  pkt = rtp_packet_next(rms->rtp_session, ALAC_HEADER_LEN + rms->rawbuf_size, rms->samples_per_packet, RAOP_RTP_PAYLOADTYPE, 0);

  ret = packet_prepare(pkt, rms->rawbuf, rms->rawbuf_size, rms->encrypt);
  if (ret < 0)
    return -1;

  for (rs = airplay_sessions; rs; rs = rs->next)
    {
      if (rs->master_session != rms)
	continue;

      // Device just joined
      if (rs->state == RAOP_STATE_CONNECTED)
	{
	  pkt->header[1] = 0xe0;
	  packet_send(rs, pkt);
	}
      else if (rs->state == RAOP_STATE_STREAMING)
	{
	  pkt->header[1] = 0x60;
	  packet_send(rs, pkt);
	}
    }

  // Commits packet to retransmit buffer, and prepares the session for the next packet
  rtp_packet_commit(rms->rtp_session, pkt);

  return 0;
}

// Overview of rtptimes as they should be when starting a stream, and assuming
// the first rtptime (pos) is 88200:
//   sync pkt:  cur_pos = 0, rtptime = 88200
//   audio pkt: rtptime = 88200
//   RECORD:    rtptime = 88200
//   SET_PARAMETER text/artwork:
//              rtptime = 88200
//   SET_PARAMETER progress:
//              progress = 72840/~88200/[len]
static inline void
timestamp_set(struct airplay_master_session *rms, struct timespec ts)
{
  // The last write from the player had a timestamp which has been passed to
  // this function as ts. This is the player clock, which is more precise than
  // the actual clock because it gives us a calculated time reference, which is
  // independent of how busy the thread is. We save that here, we need this for
  // reference when sending sync packets and progress.
  rms->cur_stamp.ts = ts;

  // So what rtptime should be playing, i.e. coming out of the speaker, at time
  // ts (which is normally "now")? Let's calculate by example:
  //   - we started playback with a rtptime (pos) of X
  //   - up until time ts we have received a 1000 samples from the player
  //   - rms->output_buffer_samples is configured to 400 samples
  //   -> we should be playing rtptime X + 600
  //
  // So how do we measure samples received from player? We know that from the
  // pos, which says how much has been sent to the device, and from rms->evbuf,
  // which is the unsent stuff being buffered:
  //   - received = (pos - X) + rms->evbuf_samples
  //
  // This means the rtptime is computed as:
  //   - rtptime = X + received - rms->output_buffer_samples
  //   -> rtptime = X + (pos - X) + rms->evbuf_samples - rms->out_buffer_samples
  //   -> rtptime = pos + rms->evbuf_samples - rms->output_buffer_samples
  rms->cur_stamp.pos = rms->rtp_session->pos + rms->evbuf_samples - rms->output_buffer_samples;
}

static void
packets_sync_send(struct airplay_master_session *rms)
{
  struct rtp_packet *sync_pkt;
  struct airplay_session *rs;
  struct timespec ts;
  bool is_sync_time;

  // Check if it is time send a sync packet to sessions that are already running
  is_sync_time = rtp_sync_is_time(rms->rtp_session);

  // Just used for logging, the clock shouldn't be too far from rms->cur_stamp.ts
  clock_gettime(CLOCK_MONOTONIC, &ts);

  for (rs = airplay_sessions; rs; rs = rs->next)
    {
      if (rs->master_session != rms)
	continue;

      // A device has joined and should get an init sync packet
      if (rs->state == RAOP_STATE_CONNECTED)
	{
	  sync_pkt = rtp_sync_packet_next(rms->rtp_session, rms->cur_stamp, 0x90);
	  control_packet_send(rs, sync_pkt);

	  DPRINTF(E_DBG, L_RAOP, "Start sync packet sent to '%s': cur_pos=%" PRIu32 ", cur_ts=%ld.%09ld, clock=%ld.%09ld, rtptime=%" PRIu32 "\n",
	    rs->devname, rms->cur_stamp.pos, rms->cur_stamp.ts.tv_sec, rms->cur_stamp.ts.tv_nsec, ts.tv_sec, ts.tv_nsec, rms->rtp_session->pos);
	}
      else if (is_sync_time && rs->state == RAOP_STATE_STREAMING)
	{
	  sync_pkt = rtp_sync_packet_next(rms->rtp_session, rms->cur_stamp, 0x80);
	  control_packet_send(rs, sync_pkt);
	}
    }
}


/* ------------------------------ Time service ------------------------------ */

static void
airplay_v2_timing_cb(int fd, short what, void *arg)
{
  union sockaddr_all sa;
  uint8_t req[32];
  uint8_t res[32];
  struct ntp_stamp recv_stamp;
  struct ntp_stamp xmit_stamp;
  struct airplay_service *svc;
  int len;
  int ret;

  svc = (struct airplay_service *)arg;

  ret = airplay_v2_timing_get_clock_ntp(&recv_stamp);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Couldn't get receive timestamp\n");

      goto readd;
    }

  len = sizeof(sa.ss);
  ret = recvfrom(svc->fd, req, sizeof(req), 0, &sa.sa, (socklen_t *)&len);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Error reading timing request: %s\n", strerror(errno));

      goto readd;
    }

  if (ret != 32)
    {
      DPRINTF(E_DBG, L_RAOP, "Got timing request with size %d\n", ret);

      goto readd;
    }

  if ((req[0] != 0x80) || (req[1] != 0xd2))
    {
      DPRINTF(E_LOG, L_RAOP, "Packet header doesn't match timing request (got 0x%02x%02x, expected 0x80d2)\n", req[0], req[1]);

      goto readd;
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
  ret = airplay_v2_timing_get_clock_ntp(&xmit_stamp);
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

  ret = sendto(svc->fd, res, sizeof(res), 0, &sa.sa, len);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not send timing reply: %s\n", strerror(errno));

      goto readd;
    }

 readd:
  ret = event_add(svc->ev, NULL);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Couldn't re-add event for timing requests\n");

      return;
    }
}

static int
airplay_v2_timing_start_one(struct airplay_service *svc, int family)
{
  union sockaddr_all sa;
  int on;
  int len;
  int ret;
  int timing_port;

#ifdef SOCK_CLOEXEC
  svc->fd = socket(family, SOCK_DGRAM | SOCK_CLOEXEC, 0);
#else
  svc->fd = socket(family, SOCK_DGRAM, 0);
#endif
  if (svc->fd < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Couldn't make timing socket: %s\n", strerror(errno));

      return -1;
    }

  if (family == AF_INET6)
    {
      on = 1;
      ret = setsockopt(svc->fd, IPPROTO_IPV6, IPV6_V6ONLY, &on, sizeof(on));
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_RAOP, "Could not set IPV6_V6ONLY on timing socket: %s\n", strerror(errno));

	  goto out_fail;
	}
    }

  memset(&sa, 0, sizeof(union sockaddr_all));
  sa.ss.ss_family = family;

  timing_port = cfg_getint(cfg_getsec(cfg, "airplay_shared"), "timing_port");
  switch (family)
    {
      case AF_INET:
	sa.sin.sin_addr.s_addr = INADDR_ANY;
	sa.sin.sin_port = htons(timing_port);
	len = sizeof(sa.sin);
	break;

      case AF_INET6:
	sa.sin6.sin6_addr = in6addr_any;
	sa.sin6.sin6_port = htons(timing_port);
	len = sizeof(sa.sin6);
	break;
    }

  ret = bind(svc->fd, &sa.sa, len);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Couldn't bind timing socket: %s\n", strerror(errno));

      goto out_fail;
    }

  len = sizeof(sa.ss);
  ret = getsockname(svc->fd, &sa.sa, (socklen_t *)&len);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Couldn't get timing socket name: %s\n", strerror(errno));

      goto out_fail;
    }

  switch (family)
    {
      case AF_INET:
	svc->port = ntohs(sa.sin.sin_port);
	DPRINTF(E_DBG, L_RAOP, "Timing IPv4 port: %d\n", svc->port);
	break;

      case AF_INET6:
	svc->port = ntohs(sa.sin6.sin6_port);
	DPRINTF(E_DBG, L_RAOP, "Timing IPv6 port: %d\n", svc->port);
	break;
    }

  svc->ev = event_new(evbase_player, svc->fd, EV_READ, airplay_v2_timing_cb, svc);
  if (!svc->ev)
    {
      DPRINTF(E_LOG, L_RAOP, "Out of memory for airplay_service event\n");

      goto out_fail;
    }

  event_add(svc->ev, NULL);

  return 0;

 out_fail:
  close(svc->fd);
  svc->fd = -1;
  svc->port = 0;

  return -1;
}

static void
airplay_v2_timing_stop(void)
{
  if (timing_4svc.ev)
    event_free(timing_4svc.ev);

  if (timing_6svc.ev)
    event_free(timing_6svc.ev);

  close(timing_4svc.fd);

  timing_4svc.fd = -1;
  timing_4svc.port = 0;

  close(timing_6svc.fd);

  timing_6svc.fd = -1;
  timing_6svc.port = 0;
}

static int
airplay_v2_timing_start(int v6enabled)
{
  int ret;

  if (v6enabled)
    {
      ret = airplay_v2_timing_start_one(&timing_6svc, AF_INET6);
      if (ret < 0)
	DPRINTF(E_WARN, L_RAOP, "Could not start timing service on IPv6\n");
    }

  ret = airplay_v2_timing_start_one(&timing_4svc, AF_INET);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not start timing service on IPv4\n");

      airplay_v2_timing_stop();
      return -1;
    }

  return 0;
}


/* ----------------- Control service (retransmission and sync) ---------------*/

static void
airplay_v2_control_cb(int fd, short what, void *arg)
{
  char address[INET6_ADDRSTRLEN];
  union sockaddr_all sa;
  uint8_t req[8];
  struct airplay_session *rs;
  struct airplay_service *svc;
  uint16_t seq_start;
  uint16_t seq_len;
  int len;
  int ret;

  svc = (struct airplay_service *)arg;

  len = sizeof(sa.ss);
  ret = recvfrom(svc->fd, req, sizeof(req), 0, &sa.sa, (socklen_t *)&len);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Error reading control request: %s\n", strerror(errno));

      goto readd;
    }

  if (ret != 8)
    {
      DPRINTF(E_DBG, L_RAOP, "Got control request with size %d\n", ret);

      goto readd;
    }

  switch (sa.ss.ss_family)
    {
      case AF_INET:
	if (svc != &control_4svc)
	  goto readd;

	for (rs = airplay_sessions; rs; rs = rs->next)
	  {
	    if ((rs->sa.ss.ss_family == AF_INET)
		&& (sa.sin.sin_addr.s_addr == rs->sa.sin.sin_addr.s_addr))
	      break;
	  }

	if (!rs)
	  ret = (inet_ntop(AF_INET, &sa.sin.sin_addr.s_addr, address, sizeof(address)) != NULL);

	break;

      case AF_INET6:
	if (svc != &control_6svc)
	  goto readd;

	for (rs = airplay_sessions; rs; rs = rs->next)
	  {
	    if ((rs->sa.ss.ss_family == AF_INET6)
		&& IN6_ARE_ADDR_EQUAL(&sa.sin6.sin6_addr, &rs->sa.sin6.sin6_addr))
	      break;
	  }

	if (!rs)
	  ret = (inet_ntop(AF_INET6, &sa.sin6.sin6_addr.s6_addr, address, sizeof(address)) != NULL);

	break;

      default:
	DPRINTF(E_LOG, L_RAOP, "Control svc: Unknown address family %d\n", sa.ss.ss_family);
	goto readd;
    }

  if (!rs)
    {
      if (!ret)
	DPRINTF(E_LOG, L_RAOP, "Control request from [error: %s]; not a RAOP client\n", strerror(errno));
      else
	DPRINTF(E_LOG, L_RAOP, "Control request from %s; not a RAOP client\n", address);

      goto readd;
    }

  if ((req[0] != 0x80) || (req[1] != 0xd5))
    {
      DPRINTF(E_LOG, L_RAOP, "Packet header doesn't match retransmit request (got 0x%02x%02x, expected 0x80d5)\n", req[0], req[1]);

      goto readd;
    }

  memcpy(&seq_start, req + 4, 2);
  memcpy(&seq_len, req + 6, 2);

  seq_start = be16toh(seq_start);
  seq_len = be16toh(seq_len);

  packets_resend(rs, seq_start, seq_len);

 readd:
  ret = event_add(svc->ev, NULL);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Couldn't re-add event for control requests\n");

      return;
    }
}

static int
airplay_v2_control_start_one(struct airplay_service *svc, int family)
{
  union sockaddr_all sa;
  int on;
  int len;
  int ret;
  int control_port;

#ifdef SOCK_CLOEXEC
  svc->fd = socket(family, SOCK_DGRAM | SOCK_CLOEXEC, 0);
#else
  svc->fd = socket(family, SOCK_DGRAM, 0);
#endif
  if (svc->fd < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Couldn't make control socket: %s\n", strerror(errno));

      return -1;
    }

  if (family == AF_INET6)
    {
      on = 1;
      ret = setsockopt(svc->fd, IPPROTO_IPV6, IPV6_V6ONLY, &on, sizeof(on));
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_RAOP, "Could not set IPV6_V6ONLY on control socket: %s\n", strerror(errno));

	  goto out_fail;
	}
    }

  memset(&sa, 0, sizeof(union sockaddr_all));
  sa.ss.ss_family = family;

  control_port = cfg_getint(cfg_getsec(cfg, "airplay_shared"), "control_port");
  switch (family)
    {
      case AF_INET:
	sa.sin.sin_addr.s_addr = INADDR_ANY;
	sa.sin.sin_port = htons(control_port);
	len = sizeof(sa.sin);
	break;

      case AF_INET6:
	sa.sin6.sin6_addr = in6addr_any;
	sa.sin6.sin6_port = htons(control_port);
	len = sizeof(sa.sin6);
	break;
    }

  ret = bind(svc->fd, &sa.sa, len);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Couldn't bind control socket: %s\n", strerror(errno));

      goto out_fail;
    }

  len = sizeof(sa.ss);
  ret = getsockname(svc->fd, &sa.sa, (socklen_t *)&len);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Couldn't get control socket name: %s\n", strerror(errno));

      goto out_fail;
    }

  switch (family)
    {
      case AF_INET:
	svc->port = ntohs(sa.sin.sin_port);
	DPRINTF(E_DBG, L_RAOP, "Control IPv4 port: %d\n", svc->port);
	break;

      case AF_INET6:
	svc->port = ntohs(sa.sin6.sin6_port);
	DPRINTF(E_DBG, L_RAOP, "Control IPv6 port: %d\n", svc->port);
	break;
    }

  svc->ev = event_new(evbase_player, svc->fd, EV_READ, airplay_v2_control_cb, svc);
  if (!svc->ev)
    {
      DPRINTF(E_LOG, L_RAOP, "Out of memory for control event\n");

      goto out_fail;
    }

  event_add(svc->ev, NULL);

  return 0;

 out_fail:
  close(svc->fd);
  svc->fd = -1;
  svc->port = 0;

  return -1;
}

static void
airplay_v2_control_stop(void)
{
  if (control_4svc.ev)
    event_free(control_4svc.ev);

  if (control_6svc.ev)
    event_free(control_6svc.ev);

  close(control_4svc.fd);

  control_4svc.fd = -1;
  control_4svc.port = 0;

  close(control_6svc.fd);

  control_6svc.fd = -1;
  control_6svc.port = 0;
}

static int
airplay_v2_control_start(int v6enabled)
{
  int ret;

  if (v6enabled)
    {
      ret = airplay_v2_control_start_one(&control_6svc, AF_INET6);
      if (ret < 0)
	DPRINTF(E_WARN, L_RAOP, "Could not start control service on IPv6\n");
    }

  ret = airplay_v2_control_start_one(&control_4svc, AF_INET);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not start control service on IPv4\n");

      airplay_v2_control_stop();
      return -1;
    }

  return 0;
}


/* ------------------------------ Session startup --------------------------- */

static void
airplay_cb_startup_retry(struct evrtsp_request *req, void *arg)
{
  struct airplay_session *rs = arg;
  struct output_device *device;
  int callback_id = rs->callback_id;

  device = outputs_device_get(rs->device_id);
  if (!device)
    {
      session_failure(rs);
      return;
    }

  session_cleanup(rs);
  airplay_device_start(device, callback_id);
}

static void
airplay_cb_startup_cancel(struct evrtsp_request *req, void *arg)
{
  struct airplay_session *rs = arg;

  session_failure(rs);
}

static void
airplay_startup_cancel(struct airplay_session *rs)
{
  struct output_device *device;
  int ret;

  device = outputs_device_get(rs->device_id);
  if (!device || !rs->session)
    {
      session_failure(rs);
      return;
    }

  // Some devices don't seem to work with ipv6, so if the error wasn't a hard
  // failure (bad password) we fall back to ipv4 and flag device as bad for ipv6
  if (rs->family == AF_INET6 && !(rs->state & RAOP_STATE_F_FAILED))
    {
      // This flag is permanent and will not be overwritten by mdns advertisements
      device->v6_disabled = 1;

      // Stop current session and wait for call back
      ret = airplay_send_req_teardown(rs, airplay_cb_startup_retry, "startup_cancel");
      if (ret < 0)
	airplay_cb_startup_retry(NULL, rs); // No connection at all, call retry directly

      return;
    }

  rs->state = RAOP_STATE_TEARDOWN;

  ret = airplay_send_req_teardown(rs, airplay_cb_startup_cancel, "startup_cancel");
  if (ret < 0)
    session_failure(rs);
}

static void
airplay_cb_pin_start(struct evrtsp_request *req, void *arg)
{
  struct airplay_session *rs = arg;
  int ret;

  rs->reqs_in_flight--;

  if (!req)
    goto error;

  if (req->response_code != RTSP_OK)
    {
      DPRINTF(E_LOG, L_RAOP, "Request for starting PIN verification failed: %d %s\n", req->response_code, req->response_code_line);

      goto error;
    }

  ret = airplay_check_cseq(rs, req);
  if (ret < 0)
    goto error;

  rs->state = RAOP_STATE_PASSWORD;

 error:
  session_failure(rs);
}

static int
airplay_v2_stream_open(struct airplay_session *rs)
{
  int len;
  int ret;

#ifdef SOCK_CLOEXEC
  rs->server_fd = socket(rs->sa.ss.ss_family, SOCK_DGRAM | SOCK_CLOEXEC, 0);
#else
  rs->server_fd = socket(rs->sa.ss.ss_family, SOCK_DGRAM, 0);
#endif
  if (rs->server_fd < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not create socket for streaming: %s\n", strerror(errno));

      return -1;
    }

  switch (rs->sa.ss.ss_family)
    {
      case AF_INET:
	rs->sa.sin.sin_port = htons(rs->data_port);
	len = sizeof(rs->sa.sin);
	break;

      case AF_INET6:
	rs->sa.sin6.sin6_port = htons(rs->data_port);
	len = sizeof(rs->sa.sin6);
	break;

      default:
	DPRINTF(E_WARN, L_RAOP, "Unknown family %d\n", rs->sa.ss.ss_family);
	goto out_fail;
    }

  ret = connect(rs->server_fd, &rs->sa.sa, len);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "connect() to [%s]:%u failed: %s\n", rs->address, rs->data_port, strerror(errno));

      goto out_fail;
    }

  rs->state = RAOP_STATE_CONNECTED;

  return 0;

 out_fail:
  close(rs->server_fd);
  rs->server_fd = -1;

  return -1;
}

static void
airplay_rtsp_cipher(struct evbuffer *evbuf, void *arg, int encrypt)
{
  struct airplay_session *rs = arg;
  uint8_t *out = NULL;
  size_t out_len = 0;
  int ret;

  uint8_t *in = evbuffer_pullup(evbuf, -1);
  size_t in_len = evbuffer_get_length(evbuf);

  if (encrypt)
    {
      DHEXDUMP(E_DBG, L_RAOP, in, in_len, "Encrypting outgoing request\n");
      ret = pair_encrypt(&out, &out_len, in, in_len, rs->pair_cipher_ctx);
    }
  else
    {
      ret = pair_decrypt(&out, &out_len, in, in_len, rs->pair_cipher_ctx);
      DHEXDUMP(E_DBG, L_RAOP, out, out_len, "Decrypted incoming response\n");
    }

  evbuffer_drain(evbuf, in_len);

  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Error while ciphering: %s\n", pair_cipher_errmsg(rs->pair_cipher_ctx));
      return;
    }

  evbuffer_add(evbuf, out, out_len);
}

static int
airplay_v2_event_open(struct airplay_session *rs)
{
  rs->event = evrtsp_connection_new(rs->address, rs->event_port);
  if (!rs->event)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not create event connection to '%s' (%s)\n", rs->devname, rs->address);
      return -1;
    }

  evrtsp_connection_set_base(rs->event, evbase_player);

  evrtsp_connection_set_ciphercb(rs->event, airplay_rtsp_cipher, rs);

  return 0;
}

static void
airplay_cb_startup_record(struct evrtsp_request *req, void *arg)
{
  struct airplay_session *rs = arg;
  const char *param;
  int ret;

  rs->reqs_in_flight--;

  if (!req)
    goto cleanup;

  if (req->response_code != RTSP_OK)
    {
      DPRINTF(E_LOG, L_RAOP, "RECORD request failed in session startup: %d %s\n", req->response_code, req->response_code_line);

      goto cleanup;
    }

  ret = airplay_check_cseq(rs, req);
  if (ret < 0)
    goto cleanup;

  /* Audio latency */
  param = evrtsp_find_header(req->input_headers, "Audio-Latency");
  if (!param)
    DPRINTF(E_INFO, L_RAOP, "RECORD reply from '%s' did not have an Audio-Latency header\n", rs->devname);
  else
    DPRINTF(E_DBG, L_RAOP, "RAOP audio latency is %s\n", param);

  rs->state = RAOP_STATE_RECORD;

//  ret = airplay_send_req_setpeers(rs, airplay_cb_startup_setpeers, "startup_record");
//  if (ret < 0)
//    goto cleanup;


  /* Set initial volume */
//  airplay_set_volume_internal(rs, rs->volume, airplay_cb_startup_volume);

  return;

 cleanup:
  airplay_startup_cancel(rs);
}

static void
airplay_cb_startup_setup2(struct evrtsp_request *req, void *arg)
{
  struct airplay_session *rs = arg;
  int ret;

  rs->reqs_in_flight--;

  if (!req)
    goto cleanup;

  if (req->response_code != RTSP_OK)
    {
      DPRINTF(E_LOG, L_RAOP, "SETUP request failed in session startup: %d %s\n", req->response_code, req->response_code_line);

      goto cleanup;
    }

  ret = airplay_check_cseq(rs, req);
  if (ret < 0)
    goto cleanup;

  uint8_t *data = evbuffer_pullup(req->input_buffer, -1);
  plist_t response;
  plist_from_bin((char *)data, evbuffer_get_length(req->input_buffer), &response);
  char *xml;
  uint32_t xml_len;
  plist_to_xml(response, &xml, &xml_len);

  DPRINTF(E_LOG, L_RAOP, "Response from %s to request is %s\n", rs->devname, xml);

  plist_t streams = plist_dict_get_item(response, "streams");
  plist_t stream = plist_array_get_item(streams, 0);

  uint64_t uintval;
  plist_t tmp = plist_dict_get_item(stream, "dataPort");
  if (tmp)
    {
      DPRINTF(E_LOG, L_RAOP, "Found data port\n");
      plist_get_uint_val(tmp, &uintval);
      rs->data_port = uintval;
    }
  tmp = plist_dict_get_item(stream, "controlPort");
  if (tmp)
    {
      DPRINTF(E_LOG, L_RAOP, "Found control port\n");
      plist_get_uint_val(tmp, &uintval);
      rs->control_port = uintval;
    }

  DPRINTF(E_DBG, L_RAOP, "Negotiated AirTunes v2 UDP streaming session %s; ports d=%u c=%u t=%u e=%u\n", rs->session, rs->data_port, rs->control_port, rs->timing_port, rs->event_port);

  ret = airplay_v2_stream_open(rs);
  if (ret < 0)
    goto cleanup;

  rs->state = RAOP_STATE_SETUP;

//  goto cleanup;

  if (!rs->reqs_in_flight)
    evrtsp_connection_set_closecb(rs->ctrl, airplay_rtsp_close_cb, rs);

  ret = airplay_send_req_record(rs, airplay_cb_startup_record, "startup_setup2");
  if (ret < 0)
    goto cleanup;

  /* Session startup and setup is done, tell our user */
  airplay_status(rs);

  return;

 cleanup:
  airplay_startup_cancel(rs);
}

static void
airplay_cb_startup_volume(struct evrtsp_request *req, void *arg)
{
  struct airplay_session *rs = arg;
  int ret;

  rs->reqs_in_flight--;

  if (!req)
    goto cleanup;

  if (req->response_code != RTSP_OK)
    {
      DPRINTF(E_LOG, L_RAOP, "SET_PARAMETER request failed for startup volume: %d %s\n", req->response_code, req->response_code_line);

      goto cleanup;
    }

  ret = airplay_check_cseq(rs, req);
  if (ret < 0)
    goto cleanup;

  ret = airplay_metadata_startup_send(rs);
  if (ret < 0)
    goto cleanup;

  ret = airplay_send_req_setup2(rs, airplay_cb_startup_setup2, "startup_volume");
  if (ret < 0)
    goto cleanup;

  return;

 cleanup:
  airplay_startup_cancel(rs);
}

static void
airplay_cb_startup_setpeers(struct evrtsp_request *req, void *arg)
{
  struct airplay_session *rs = arg;
  int ret;

  rs->reqs_in_flight--;

  if (!req)
    goto cleanup;

  if (req->response_code != RTSP_OK)
    {
      DPRINTF(E_LOG, L_RAOP, "SETPEERS request failed in session startup: %d %s\n", req->response_code, req->response_code_line);

      goto cleanup;
    }

  ret = airplay_check_cseq(rs, req);
  if (ret < 0)
    goto cleanup;

  airplay_set_volume_internal(rs, rs->volume, airplay_cb_startup_volume);

  return;

 cleanup:
  airplay_startup_cancel(rs);
}

static void
airplay_cb_startup_setup(struct evrtsp_request *req, void *arg)
{
  struct airplay_session *rs = arg;
  int ret;

  rs->reqs_in_flight--;

  if (!req)
    goto cleanup;

  if (req->response_code != RTSP_OK)
    {
      DPRINTF(E_LOG, L_RAOP, "SETUP request failed in session startup: %d %s\n", req->response_code, req->response_code_line);

      goto cleanup;
    }

  ret = airplay_check_cseq(rs, req);
  if (ret < 0)
    goto cleanup;

  uint8_t *data = evbuffer_pullup(req->input_buffer, -1);
  plist_t response;
  plist_from_bin((char *)data, evbuffer_get_length(req->input_buffer), &response);
  char *xml;
  uint32_t xml_len;
  plist_to_xml(response, &xml, &xml_len);

  DPRINTF(E_LOG, L_RAOP, "Response from %s to request is %s\n", rs->devname, xml);

  uint64_t uintval;
  plist_t tmp = plist_dict_get_item(response, "eventPort");
  if (tmp)
    {
      DPRINTF(E_LOG, L_RAOP, "Found event port\n");
      plist_get_uint_val(tmp, &uintval);
      rs->event_port = uintval;
    }

  tmp = plist_dict_get_item(response, "timingPort");
  if (tmp)
    {
      DPRINTF(E_LOG, L_RAOP, "Found timing port\n");
      plist_get_uint_val(tmp, &uintval);
      rs->timing_port = uintval;
    }

/*
  param = evrtsp_find_header(req->input_headers, "Session");
  if (!param)
    {
      DPRINTF(E_LOG, L_RAOP, "Missing Session header in SETUP reply\n");

      goto cleanup;
    }

  rs->session = strdup(param);

  param = evrtsp_find_header(req->input_headers, "Transport");
  if (!param)
    {
      DPRINTF(E_LOG, L_RAOP, "Missing Transport header in SETUP reply\n");

      goto cleanup;
    }

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
      DPRINTF(E_SPAM, L_RAOP, "token: %s\n", token);

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
*/

  if (rs->event_port == 0)
    {
      DPRINTF(E_LOG, L_RAOP, "SETUP reply is missing event port\n");

      goto cleanup;
    }

  ret = airplay_v2_event_open(rs);
  if (ret < 0)
    goto cleanup;

//  ret = airplay_send_req_record(rs, airplay_cb_startup_record, "startup_setup");
  ret = airplay_send_req_setpeers(rs, airplay_cb_startup_setpeers, "startup_setup");
  if (ret < 0)
    goto cleanup;

  return;

 cleanup:
  airplay_startup_cancel(rs);
}

/*
static void
airplay_cb_startup_announce(struct evrtsp_request *req, void *arg)
{
  struct airplay_session *rs = arg;
  int ret;

  rs->reqs_in_flight--;

  if (!req)
    goto cleanup;

  if (req->response_code != RTSP_OK)
    {
      DPRINTF(E_LOG, L_RAOP, "ANNOUNCE request failed in session startup: %d %s\n", req->response_code, req->response_code_line);

      goto cleanup;
    }

  ret = airplay_check_cseq(rs, req);
  if (ret < 0)
    goto cleanup;

  rs->state = RAOP_STATE_ANNOUNCE;

  // Send SETUP
  ret = airplay_send_req_setup(rs, airplay_cb_startup_setup, "startup_announce");
  if (ret < 0)
    goto cleanup;

  return;

 cleanup:
  airplay_startup_cancel(rs);
}
*/

static void
airplay_cb_startup_auth_setup(struct evrtsp_request *req, void *arg)
{
  struct airplay_session *rs = arg;
  int ret;

  rs->reqs_in_flight--;

  if (!req)
    goto cleanup;

  if (req->response_code != RTSP_OK)
    DPRINTF(E_WARN, L_RAOP, "Unexpected reply to auth-setup from '%s', proceeding anyway (%d %s)\n", rs->devname, req->response_code, req->response_code_line);

  // Send ANNOUNCE
  ret = airplay_send_req_setup(rs, airplay_cb_startup_setup, "startup_auth_setup");
  if (ret < 0)
    goto cleanup;

  return;

 cleanup:
  airplay_startup_cancel(rs);
}

static void
airplay_cb_startup_options(struct evrtsp_request *req, void *arg)
{
  struct airplay_session *rs = arg;
  struct output_device *device;
  const char *param;
  int ret;

  rs->reqs_in_flight--;

  if (!req || !req->response_code)
    {
      DPRINTF(E_LOG, L_RAOP, "No response from '%s' (%s) to OPTIONS request\n", rs->devname, rs->address);

      goto cleanup;
    }

  if ((req->response_code != RTSP_OK) && (req->response_code != RTSP_UNAUTHORIZED) && (req->response_code != RTSP_FORBIDDEN))
    {
      DPRINTF(E_LOG, L_RAOP, "OPTIONS request failed '%s' (%s): %d %s\n", rs->devname, rs->address, req->response_code, req->response_code_line);

      goto cleanup;
    }

  ret = airplay_check_cseq(rs, req);
  if (ret < 0)
    goto cleanup;

  if (req->response_code == RTSP_UNAUTHORIZED)
    {
      if (rs->req_has_auth)
	{
	  DPRINTF(E_LOG, L_RAOP, "Bad password for device '%s' (%s)\n", rs->devname, rs->address);

	  rs->state = RAOP_STATE_PASSWORD;
	  goto cleanup;
	}

      ret = airplay_parse_auth(rs, req);
      if (ret < 0)
	goto cleanup;

      ret = airplay_send_req_options(rs, airplay_cb_startup_options, "startup_options");
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_RAOP, "Could not re-run OPTIONS request with authentication for '%s' (%s)\n", rs->devname, rs->address);
	  goto cleanup;
	}

      return;
    }

  if (req->response_code == RTSP_FORBIDDEN)
    {
      device = outputs_device_get(rs->device_id);
      if (!device)
	goto cleanup;

      device->requires_auth = 1;

      ret = airplay_send_req_pin_start(rs, airplay_cb_pin_start, "startup_options");
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_RAOP, "Could not request PIN from '%s' (%s) for device verification\n", rs->devname, rs->address);
	  goto cleanup;
	}

      return;
    }

  rs->state = RAOP_STATE_OPTIONS;

  param = evrtsp_find_header(req->input_headers, "Public");
  if (param)
    rs->supports_post = (strstr(param, "POST") != NULL);
  else
    DPRINTF(E_DBG, L_RAOP, "Could not find 'Public' header in OPTIONS reply from '%s' (%s)\n", rs->devname, rs->address);

  if (rs->only_probe)
    {
      // Device probed successfully, tell our user
      airplay_status(rs);

      // We're not going further with this session
      session_cleanup(rs);
    }
  else if (rs->supports_post && rs->supports_auth_setup)
    {
      // AirPlay 2 devices require this step or the ANNOUNCE will get a 403
      ret = airplay_send_req_auth_setup(rs, airplay_cb_startup_auth_setup, "startup_options");
      if (ret < 0)
	goto cleanup;
    }
  else
    {
      ret = airplay_send_req_setup(rs, airplay_cb_startup_setup, "startup_options");
      if (ret < 0)
	goto cleanup;
    }

  return;

 cleanup:
  if (rs->only_probe)
    session_failure(rs);
  else
    airplay_startup_cancel(rs);
}


/* ------------------------ Pairing/device verification --------------------- */
/*                 e.g. for the ATV4 (read it from the bottom and up)         */

#ifdef RAOP_VERIFICATION
static int
airplay_pair_response_process(int step, struct evrtsp_request *req, struct airplay_session *rs)
{
  uint8_t *response;
  const char *errmsg;
  size_t len;
  int ret;

  rs->reqs_in_flight--;

  if (!req)
    {
      DPRINTF(E_LOG, L_RAOP, "Verification step %d to '%s' failed, empty callback\n", step, rs->devname);
      return -1;
    }

  if (req->response_code != RTSP_OK)
    {
      DPRINTF(E_LOG, L_RAOP, "Verification step %d to '%s' failed with error code %d: %s\n", step, rs->devname, req->response_code, req->response_code_line);
      return -1;
    }

  response = evbuffer_pullup(req->input_buffer, -1);
  len = evbuffer_get_length(req->input_buffer);

  switch (step)
    {
      case 1:
	ret = pair_setup_response1(rs->pair_setup_ctx, response, len);
	errmsg = pair_setup_errmsg(rs->pair_setup_ctx);
	break;
      case 2:
	ret = pair_setup_response2(rs->pair_setup_ctx, response, len);
	errmsg = pair_setup_errmsg(rs->pair_setup_ctx);
	break;
      case 3:
	ret = pair_setup_response3(rs->pair_setup_ctx, response, len);
	errmsg = pair_setup_errmsg(rs->pair_setup_ctx);
	break;
      case 4:
	ret = pair_verify_response1(rs->pair_verify_ctx, response, len);
	errmsg = pair_verify_errmsg(rs->pair_verify_ctx);
	break;
      case 5:
	ret = pair_verify_response2(rs->pair_verify_ctx, response, len);
	errmsg = pair_verify_errmsg(rs->pair_verify_ctx);
	break;
      default:
	ret = -1;
	errmsg = "Bug! Bad step number";
    }

  if (ret < 0)
    DPRINTF(E_LOG, L_RAOP, "Verification step %d response from '%s' error: %s\n", step, rs->devname, errmsg);

  return ret;
}

static int
airplay_pair_request_send(int step, struct airplay_session *rs, void (*cb)(struct evrtsp_request *, void *))
{
  struct evrtsp_request *req;
  uint8_t *body;
  uint32_t len;
  const char *errmsg;
  const char *url;
  const char *ctype;
  int ret;

  switch (step)
    {
      case 1:
	body    = pair_setup_request1(&len, rs->pair_setup_ctx);
	errmsg  = pair_setup_errmsg(rs->pair_setup_ctx);
	url     = "/pair-setup";
	ctype   = "application/octet-stream";
	break;
      case 2:
	body    = pair_setup_request2(&len, rs->pair_setup_ctx);
	errmsg  = pair_setup_errmsg(rs->pair_setup_ctx);
	url     = "/pair-setup";
	ctype   = "application/octet-stream";
	break;
      case 3:
	body    = pair_setup_request3(&len, rs->pair_setup_ctx);
	errmsg  = pair_setup_errmsg(rs->pair_setup_ctx);
	url     = "/pair-setup";
	ctype   = "application/octet-stream";
	break;
      case 4:
	body    = pair_verify_request1(&len, rs->pair_verify_ctx);
	errmsg  = pair_verify_errmsg(rs->pair_verify_ctx);
	url     = "/pair-verify";
	ctype   = "application/octet-stream";
	break;
      case 5:
	body    = pair_verify_request2(&len, rs->pair_verify_ctx);
	errmsg  = pair_verify_errmsg(rs->pair_verify_ctx);
	url     = "/pair-verify";
	ctype   = "application/octet-stream";
	break;
      default:
	body    = NULL;
	errmsg  = "Bug! Bad step number";
    }

  if (!body)
    {
      DPRINTF(E_LOG, L_RAOP, "Verification step %d request error: %s\n", step, errmsg);
      return -1;
    }

  req = evrtsp_request_new(cb, rs);
  if (!req)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not create RTSP request for verification step %d\n", step);
      return -1;
    }

  evbuffer_add(req->output_buffer, body, len);
  free(body);

  ret = airplay_add_headers(rs, req, EVRTSP_REQ_POST);
  if (ret < 0)
    {
      evrtsp_request_free(req);
      return -1;
    }

  evrtsp_add_header(req->output_headers, "Content-Type", ctype);
  evrtsp_add_header(req->output_headers, "X-Apple-HKP", "3"); // Required!!

  DPRINTF(E_INFO, L_RAOP, "Making verification request step %d to '%s'\n", step, rs->devname);

  ret = evrtsp_make_request(rs->ctrl, req, EVRTSP_REQ_POST, url);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Verification request step %d to '%s' failed\n", step, rs->devname);
      return -1;
    }

  rs->reqs_in_flight++;

  evrtsp_connection_set_closecb(rs->ctrl, NULL, NULL);

  return 0;
}

static void
airplay_cb_pair_verify_step2(struct evrtsp_request *req, void *arg)
{
  struct airplay_session *rs = arg;
  struct output_device *device;
  const uint8_t *shared_secret;
  int ret;

  ret = airplay_pair_response_process(5, req, rs);
  if (ret < 0)
    goto error;

  ret = pair_verify_result(&shared_secret, rs->pair_verify_ctx);
  if (ret < 0)
    goto error;

  rs->pair_cipher_ctx = pair_cipher_new(PAIR_HOMEKIT, shared_secret);
  if (!rs->pair_cipher_ctx)
    goto error;

  evrtsp_connection_set_ciphercb(rs->ctrl, airplay_rtsp_cipher, rs);

  DPRINTF(E_INFO, L_RAOP, "Verification of '%s' completed succesfully, now using encrypted mode\n", rs->devname);

  rs->state = RAOP_STATE_STARTUP;

  airplay_send_req_options(rs, airplay_cb_startup_options, "verify_step2");

  pair_verify_free(rs->pair_verify_ctx);
  return;

 error:
  pair_verify_free(rs->pair_verify_ctx);

  device = outputs_device_get(rs->device_id);
  if (!device)
    return;

  // Clear auth_key, the device did not accept it, or some other unexpected error
  free(device->auth_key);
  device->auth_key = NULL;

  rs->state = RAOP_STATE_PASSWORD;
  session_failure(rs);
}

static void
airplay_cb_pair_verify_step1(struct evrtsp_request *req, void *arg)
{
  struct airplay_session *rs = arg;
  struct output_device *device;
  int ret;

  ret = airplay_pair_response_process(4, req, rs);
  if (ret < 0)
    {
      device = outputs_device_get(rs->device_id);
      if (!device)
	goto error;

      // Clear auth_key, the device did not accept it
      free(device->auth_key);
      device->auth_key = NULL;
      goto error;
    }

  ret = airplay_pair_request_send(5, rs, airplay_cb_pair_verify_step2);
  if (ret < 0)
    goto error;

  return;

 error:
  pair_verify_free(rs->pair_verify_ctx);
  rs->pair_verify_ctx = NULL;

  rs->state = RAOP_STATE_PASSWORD;
  session_failure(rs);
}

static int
airplay_pair_verify(struct airplay_session *rs)
{
  struct output_device *device;
  int ret;

  device = outputs_device_get(rs->device_id);
  if (!device)
    goto error;

  CHECK_NULL(L_RAOP, rs->pair_verify_ctx = pair_verify_new(PAIR_HOMEKIT, device->auth_key, pair_device_id));

  ret = airplay_pair_request_send(4, rs, airplay_cb_pair_verify_step1);
  if (ret < 0)
    goto error;

  return 0;

 error:
  pair_verify_free(rs->pair_verify_ctx);
  rs->pair_verify_ctx = NULL;
  return -1;
}


static void
airplay_cb_pair_setup_step3(struct evrtsp_request *req, void *arg)
{
  struct airplay_session *rs = arg;
  struct output_device *device;
  const char *authorization_key;
  int ret;

  ret = airplay_pair_response_process(3, req, rs);
  if (ret < 0)
    goto out;

  ret = pair_setup_result(&authorization_key, rs->pair_setup_ctx);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Verification setup result error: %s\n", pair_setup_errmsg(rs->pair_setup_ctx));
      goto out;
    }

  DPRINTF(E_LOG, L_RAOP, "Verification setup stage complete, saving authorization key\n");

  device = outputs_device_get(rs->device_id);
  if (!device)
    goto out;

  free(device->auth_key);
  device->auth_key = strdup(authorization_key);

  // A blocking db call... :-~
  db_speaker_save(device);

  // No longer RAOP_STATE_PASSWORD
  rs->state = RAOP_STATE_STOPPED;

 out:
  pair_setup_free(rs->pair_setup_ctx);
  rs->pair_setup_ctx = NULL;

  // Callback to player with result
  airplay_status(rs);

  // We are telling the player that the device is now stopped, so we don't need
  // the session any more
  session_cleanup(rs);
}

static void
airplay_cb_pair_setup_step2(struct evrtsp_request *req, void *arg)
{
  struct airplay_session *rs = arg;
  int ret;

  ret = airplay_pair_response_process(2, req, rs);
  if (ret < 0)
    goto error;

  ret = airplay_pair_request_send(3, rs, airplay_cb_pair_setup_step3);
  if (ret < 0)
    goto error;

  return;

 error:
  pair_setup_free(rs->pair_setup_ctx);
  rs->pair_setup_ctx = NULL;
  session_failure(rs);
}

static void
airplay_cb_pair_setup_step1(struct evrtsp_request *req, void *arg)
{
  struct airplay_session *rs = arg;
  int ret;

  ret = airplay_pair_response_process(1, req, rs);
  if (ret < 0)
    goto error;

  ret = airplay_pair_request_send(2, rs, airplay_cb_pair_setup_step2);
  if (ret < 0)
    goto error;

  return;

 error:
  pair_setup_free(rs->pair_setup_ctx);
  rs->pair_setup_ctx = NULL;
  session_failure(rs);
}

static int
airplay_pair_setup(struct airplay_session *rs, const char *pin)
{
  int ret;

  rs->pair_setup_ctx = pair_setup_new(PAIR_HOMEKIT, pin, pair_device_id);
  if (!rs->pair_setup_ctx)
    {
      DPRINTF(E_LOG, L_RAOP, "Out of memory for verification setup context\n");
      return -1;
    }

  ret = airplay_pair_request_send(1, rs, airplay_cb_pair_setup_step1);
  if (ret < 0)
    goto error;

  rs->state = RAOP_STATE_PASSWORD;

  return 0;

 error:
  pair_setup_free(rs->pair_setup_ctx);
  rs->pair_setup_ctx = NULL;
  return -1;
}

static int
airplay_device_authorize(struct output_device *device, const char *pin, int callback_id)
{
  struct airplay_session *rs;
  int ret;

  // Make a session so we can communicate with the device
  rs = session_make(device, callback_id, true);
  if (!rs)
    return -1;

  ret = airplay_pair_setup(rs, pin);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not send verification setup request to '%s' (address %s)\n", device->name, rs->address);
      session_cleanup(rs);
      return -1;
    }

  return 1;
}

#else
static int
airplay_pair_verify(struct airplay_session *rs)
{
  DPRINTF(E_LOG, L_RAOP, "Device '%s' requires verification, but forked-daapd was built with --disable-verification\n", rs->devname);

  return -1;
}
#endif /* RAOP_VERIFICATION */


/* ---------------- Airplay devices discovery - mDNS callback --------------- */
/*                              Thread: main (mdns)                           */

static int
features_parse(struct keyval *features_kv, const char *fs, int part, const char *name)
{
  uint32_t features;
  int ret;
  int i;

  if (!fs)
    {
      DPRINTF(E_LOG, L_RAOP, "AirPlay '%s': no features field in TXT record!\n", name);
      return -1;
    }

  ret = safe_hextou32(fs, &features);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "AirPlay '%s': unexpected features field in TXT record!\n", name);
      return -1;
    }

  for (i = 0; i < ARRAY_SIZE(features_map); i++)
    {
      if ((part == 0 && features_map[i].bit >= 32) || (part == 1 && features_map[i].bit < 32))
	continue;

      if (features & (1 << (features_map[i].bit - 32 * part)))
        {
	  DPRINTF(E_DBG, L_RAOP, "Speaker '%s' supports feature %d: '%s'\n", name, features_map[i].bit, features_map[i].name);
          keyval_add(features_kv, features_map[i].name, "1");
        }
    }

  return 0;
}

/* Examples of txt content:
 * Airport Express 2:
     ["pk=7de...39" "gcgl=0" "gid=0fd...4" "pi=0fd...a4" "srcvers=366.0" "protovers=1.1" "serialNumber=C8...R" "manufacturer=Apple Inc." "model=AirPort10,115" "flags=0x4" "fv=p20.78100.3" "rsf=0x0" "features=0x445D0A00,0x1C340" "deviceid=74:1B:B2:D1:1A:B7" "acl=0"]
 * Apple TV 4:
     ["vv=2" "osvers=14.2" "srcvers=525.38.42" "pk=c4e...c88" "psi=67C...DBC" "pi=b0b...da0" "protovers=1.1" "model=AppleTV5,3" "gcgl=1" "igl=1" "gid=B...73" "flags=0x244" "features=0x5A7FDFD5,0x3C155FDE" "fex=1d9/Wt5fFTw" "deviceid=AA:BB:CC:DD:EE:FF" "btaddr=D0:00:44:66:BB:66" "acl=0"]
  * Roku
     ["pk=xxxxxxxxx” "gcgl=0" "gid=xxxxxxx” "psi=xxxxx” "pi=8A:71:CA:EF:xxxx" "srcvers=377.28.01" "protovers=1.1" "serialNumber=xxxxxxx” "manufacturer=Roku" "model=3810X" "flags=0x644" "at=0x3" "fv=p20.9.40.4190" "rsf=0x3" "features=0x7F8AD0,0x10BCF46" "deviceid=8A:71:CA:xxxxx” "acl=0"]
  * Samsung TV
     ["pk=7xxxxxxxxxx” "gcgl=0" "gid=xxxxxxxxxxx” "psi=xxxxxxx” "pi=4C:6F:64:xxxxxxx” "srcvers=377.17.24.6" "protovers=1.1" "serialNumber=xxxxxxx” "manufacturer=Samsung" "model=UNU7090" "flags=0x244" "fv=p20.0.1" "rsf=0x3" "features=0x7F8AD0,0x38BCB46" "deviceid=64:1C:AE:xxxxx” "acl=0"]
 */
static void
airplay_device_cb(const char *name, const char *type, const char *domain, const char *hostname, int family, const char *address, int port, struct keyval *txt)
{
  struct output_device *rd;
  struct airplay_extra *re;
  struct keyval features = { 0 };
  cfg_t *devcfg;
  cfg_opt_t *cfgopt;
  const char *p;
  char *s;
  char *ptr;
  uint64_t id;
  int ret;

  p = keyval_get(txt, "deviceid");
  if (!p)
    {
      DPRINTF(E_LOG, L_RAOP, "AirPlay device '%s' is missing a device ID\n", name);
      return;
    }

  // Convert AA:BB:CC:DD:EE:FF -> AABBCCDDEEFF -> uint64 id
  s = calloc(1, strlen(p) + 1);
  for (ptr = s; *p != '\0'; p++)
    {
      if (*p == ':')
	continue;

      *ptr = *p;
      ptr++;
    }

  ret = safe_hextou64(s, &id);
  free(s);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not extract AirPlay device ID ('%s')\n", name);
      return;
    }

  DPRINTF(E_DBG, L_RAOP, "Event for AirPlay device '%s' (port %d, id %" PRIx64 ")\n", name, port, id);

  devcfg = cfg_gettsec(cfg, "airplay", name);
  if (devcfg && cfg_getbool(devcfg, "exclude"))
    {
      DPRINTF(E_LOG, L_RAOP, "Excluding AirPlay device '%s' as set in config\n", name);
      return;
    }
  if (devcfg && cfg_getbool(devcfg, "permanent") && (port < 0))
    {
      DPRINTF(E_INFO, L_RAOP, "AirPlay device '%s' disappeared, but set as permanent in config\n", name);
      return;
    }

  CHECK_NULL(L_RAOP, rd = calloc(1, sizeof(struct output_device)));
  CHECK_NULL(L_RAOP, re = calloc(1, sizeof(struct airplay_extra)));

  rd->id = id;
  rd->name = strdup(name);
  rd->type = OUTPUT_TYPE_AIRPLAY;
  rd->type_name = outputs_name(rd->type);
  rd->extra_device_info = re;

  if (port < 0)
    {
      // Device stopped advertising
      switch (family)
	{
	  case AF_INET:
	    rd->v4_port = 1;
	    break;

	  case AF_INET6:
	    rd->v6_port = 1;
	    break;
	}

      ret = player_device_remove(rd);
      if (ret < 0)
	goto free_rd;

      return;
    }

  // Features, see features_map[]
  p = keyval_get(txt, "features");
  ret = features_parse(&features, p, 0, name);
  if (ret < 0)
    goto free_rd;

  p = strchr(p, ',');
  if (!p)
    goto free_rd;

  ret = features_parse(&features, p + 1, 1, name);
  if (ret < 0)
    goto free_rd;

  if (!keyval_get(&features, "SupportsAirPlayAudio"))
    goto free_rd;

// Disable artwork while debugging
//  if (keyval_get(&features, "MetadataFeatures_0"))
//    re->wanted_metadata |= RAOP_MD_WANTS_ARTWORK;
  if (keyval_get(&features, "MetadataFeatures_1"))
    re->wanted_metadata |= RAOP_MD_WANTS_PROGRESS;
  if (keyval_get(&features, "MetadataFeatures_2"))
    re->wanted_metadata |= RAOP_MD_WANTS_TEXT;
  if (keyval_get(&features, "Authentication_8"))
    re->supports_auth_setup = 1;
  if (keyval_get(&features, "SupportsHKPairingAndAccessControl"))
    rd->requires_auth = 1;

  keyval_clear(&features);

  // Only default audio quality supported so far
  rd->quality.sample_rate = RAOP_QUALITY_SAMPLE_RATE_DEFAULT;
  rd->quality.bits_per_sample = RAOP_QUALITY_BITS_PER_SAMPLE_DEFAULT;
  rd->quality.channels = RAOP_QUALITY_CHANNELS_DEFAULT;

  if (!quality_is_equal(&rd->quality, &airplay_quality_default))
    DPRINTF(E_LOG, L_RAOP, "Device '%s' requested non-default audio quality (%d/%d/%d)\n", rd->name, rd->quality.sample_rate, rd->quality.bits_per_sample, rd->quality.channels);

  // Device type
  re->devtype = RAOP_DEV_OTHER;
  p = keyval_get(txt, "model");

  if (!p)
    re->devtype = RAOP_DEV_APEX1_80211G; // First generation AirPort Express
  else if (strncmp(p, "AirPort4", strlen("AirPort4")) == 0)
    re->devtype = RAOP_DEV_APEX2_80211N; // Second generation
  else if (strncmp(p, "AirPort", strlen("AirPort")) == 0)
    re->devtype = RAOP_DEV_APEX3_80211N; // Third generation and newer
  else if (strncmp(p, "AppleTV5,3", strlen("AppleTV5,3")) == 0)
    re->devtype = RAOP_DEV_APPLETV4; // Stream to ATV with tvOS 10 needs to be kept alive
  else if (strncmp(p, "AppleTV", strlen("AppleTV")) == 0)
    re->devtype = RAOP_DEV_APPLETV;
  else if (strncmp(p, "AudioAccessory", strlen("AudioAccessory")) == 0)
    re->devtype = RAOP_DEV_HOMEPOD;
  else if (*p == '\0')
    DPRINTF(E_LOG, L_RAOP, "AirPlay device '%s': am has no value\n", name);

  // If the user didn't set any reconnect setting we enable for Apple TV and
  // HomePods due to https://github.com/ejurgensen/forked-daapd/issues/734
  cfgopt = devcfg ? cfg_getopt(devcfg, "reconnect") : NULL;
  if (cfgopt && cfgopt->nvalues == 1)
    rd->resurrect = cfg_opt_getnbool(cfgopt, 0);
  else
    rd->resurrect = (re->devtype == RAOP_DEV_APPLETV4) || (re->devtype == RAOP_DEV_HOMEPOD);

  switch (family)
    {
      case AF_INET:
	rd->v4_address = strdup(address);
	rd->v4_port = port;
	DPRINTF(E_INFO, L_RAOP, "Adding AirPlay device '%s': password: %u, verification: %u, encrypt: %u, authsetup: %u, metadata: %u, type %s, address %s:%d\n", 
	  name, rd->has_password, rd->requires_auth, re->encrypt, re->supports_auth_setup, re->wanted_metadata, airplay_devtype[re->devtype], address, port);
	break;

      case AF_INET6:
	rd->v6_address = strdup(address);
	rd->v6_port = port;
	DPRINTF(E_INFO, L_RAOP, "Adding AirPlay device '%s': password: %u, verification: %u, encrypt: %u, authsetup: %u, metadata: %u, type %s, address [%s]:%d\n", 
	  name, rd->has_password, rd->requires_auth, re->encrypt, re->supports_auth_setup, re->wanted_metadata, airplay_devtype[re->devtype], address, port);
	break;

      default:
	DPRINTF(E_LOG, L_RAOP, "Error: AirPlay device '%s' has neither ipv4 og ipv6 address\n", name);
	goto free_rd;
    }

  ret = player_device_add(rd);
  if (ret < 0)
    goto free_rd;

  return;

 free_rd:
  outputs_device_free(rd);
  keyval_clear(&features);
}


/* ---------------------------- Module definitions -------------------------- */
/*                                Thread: player                              */

static int
airplay_device_start_generic(struct output_device *device, int callback_id, bool only_probe)
{
  struct airplay_session *rs;
  int ret;

  /* Send an OPTIONS request to establish the connection. If device verification
   * is required we start with that. After that, we can determine our local
   * address and build our session URL for all subsequent requests.
   */

  rs = session_make(device, callback_id, only_probe);
  if (!rs)
    return -1;

  if (device->auth_key)
    ret = airplay_pair_verify(rs);
  else if (device->requires_auth)
    ret = airplay_send_req_pin_start(rs, airplay_cb_pin_start, "device_start");
  else
    ret = airplay_send_req_options(rs, airplay_cb_startup_options, "device_start");

  if (ret < 0)
    {
      DPRINTF(E_WARN, L_RAOP, "Could not send verification or OPTIONS request to '%s' (address %s)\n", device->name, rs->address);
      session_cleanup(rs);
      return -1;
    }

  return 1;
}

static int
airplay_device_probe(struct output_device *device, int callback_id)
{
  return airplay_device_start_generic(device, callback_id, 1);
}

static int
airplay_device_start(struct output_device *device, int callback_id)
{
  return airplay_device_start_generic(device, callback_id, 0);
}

static int
airplay_device_stop(struct output_device *device, int callback_id)
{
  struct airplay_session *rs = device->session;

  rs->callback_id = callback_id;

  session_teardown(rs, "device_stop");

  return 1;
}

static int
airplay_device_flush(struct output_device *device, int callback_id)
{
  struct airplay_session *rs = device->session;
  int ret;

  if (rs->state != RAOP_STATE_STREAMING)
    return 0; // No-op, nothing to flush

  ret = airplay_send_req_flush(rs, airplay_cb_flush, "flush");
  if (ret < 0)
    return -1;

  rs->callback_id = callback_id;

  return 1;
}

static void
airplay_device_cb_set(struct output_device *device, int callback_id)
{
  struct airplay_session *rs = device->session;

  rs->callback_id = callback_id;
}

static void
airplay_device_free_extra(struct output_device *device)
{
  struct airplay_extra *re = device->extra_device_info;

  free(re);
}

static void
airplay_write(struct output_buffer *obuf)
{
  struct airplay_master_session *rms;
  struct airplay_session *rs;
  int i;

  for (rms = airplay_master_sessions; rms; rms = rms->next)
    {
      for (i = 0; obuf->data[i].buffer; i++)
	{
	  if (!quality_is_equal(&obuf->data[i].quality, &rms->rtp_session->quality))
	    continue;

	  // Set rms->cur_stamp, which involves a calculation of which session
	  // rtptime corresponds to the pts we are given by the player.
	  timestamp_set(rms, obuf->pts);

	  // Sends sync packets to new sessions, and if it is sync time then also to old sessions
	  packets_sync_send(rms);

	  // TODO avoid this copy
	  evbuffer_add(rms->evbuf, obuf->data[i].buffer, obuf->data[i].bufsize);
	  rms->evbuf_samples += obuf->data[i].samples;

	  // Send as many packets as we have data for (one packet requires rawbuf_size bytes)
	  while (evbuffer_get_length(rms->evbuf) >= rms->rawbuf_size)
	    {
	      evbuffer_remove(rms->evbuf, rms->rawbuf, rms->rawbuf_size);
	      rms->evbuf_samples -= rms->samples_per_packet;

	      packets_send(rms);
	    }
	}
    }

  // Check for devices that have joined since last write (we have already sent them
  // initialization sync and rtp packets via packets_sync_send and packets_send)
  for (rs = airplay_sessions; rs; rs = rs->next)
    {
      if (rs->state != RAOP_STATE_CONNECTED)
	continue;

      // Start sending progress to keep ATV's alive
      if (!event_pending(keep_alive_timer, EV_TIMEOUT, NULL))
	evtimer_add(keep_alive_timer, &keep_alive_tv);

      rs->state = RAOP_STATE_STREAMING;
      // Make a cb?
    }
}

static int
airplay_init(void)
{
  char ebuf[64];
  char *ptr;
  gpg_error_t gc_err;
  int v6enabled;
  int family;
  int ret;

  timing_4svc.fd = -1;
  timing_4svc.port = 0;

  timing_6svc.fd = -1;
  timing_6svc.port = 0;

  control_4svc.fd = -1;
  control_4svc.port = 0;

  control_6svc.fd = -1;
  control_6svc.port = 0;

  // Generate AES key and IV
  gcry_randomize(airplay_aes_key, sizeof(airplay_aes_key), GCRY_STRONG_RANDOM);
  gcry_randomize(airplay_aes_iv, sizeof(airplay_aes_iv), GCRY_STRONG_RANDOM);

  // Setup AES
  gc_err = gcry_cipher_open(&airplay_aes_ctx, GCRY_CIPHER_AES, GCRY_CIPHER_MODE_CBC, 0);
  if (gc_err != GPG_ERR_NO_ERROR)
    {
      gpg_strerror_r(gc_err, ebuf, sizeof(ebuf));
      DPRINTF(E_LOG, L_RAOP, "Could not open AES cipher: %s\n", ebuf);

      return -1;
    }

  // Set key
  gc_err = gcry_cipher_setkey(airplay_aes_ctx, airplay_aes_key, sizeof(airplay_aes_key));
  if (gc_err != GPG_ERR_NO_ERROR)
    {
      gpg_strerror_r(gc_err, ebuf, sizeof(ebuf));
      DPRINTF(E_LOG, L_RAOP, "Could not set AES key: %s\n", ebuf);

      goto out_close_cipher;
    }

  // Prepare Base64-encoded key & IV for SDP
  airplay_aes_key_b64 = airplay_crypt_encrypt_aes_key_base64();
  if (!airplay_aes_key_b64)
    {
      DPRINTF(E_LOG, L_RAOP, "Couldn't encrypt and encode AES session key\n");

      goto out_close_cipher;
    }

  airplay_aes_iv_b64 = b64_encode(airplay_aes_iv, sizeof(airplay_aes_iv));
  if (!airplay_aes_iv_b64)
    {
      DPRINTF(E_LOG, L_RAOP, "Couldn't encode AES IV\n");

      goto out_free_b64_key;
    }

  // Remove base64 padding
  ptr = strchr(airplay_aes_key_b64, '=');
  if (ptr)
    *ptr = '\0';

  ptr = strchr(airplay_aes_iv_b64, '=');
  if (ptr)
    *ptr = '\0';

  CHECK_NULL(L_RAOP, keep_alive_timer = evtimer_new(evbase_player, airplay_keep_alive_timer_cb, NULL));

  v6enabled = cfg_getbool(cfg_getsec(cfg, "general"), "ipv6");

  ret = airplay_v2_timing_start(v6enabled);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "AirPlay time synchronization failed to start\n");

      goto out_free_timer;
    }

  ret = airplay_v2_control_start(v6enabled);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "AirPlay playback control failed to start\n");

      goto out_stop_timing;
    }

  if (v6enabled)
    v6enabled = !((timing_6svc.fd < 0) || (control_6svc.fd < 0));

  if (v6enabled)
    family = AF_UNSPEC;
  else
    family = AF_INET;

  ret = mdns_browse("_airplay._tcp", family, airplay_device_cb, MDNS_CONNECTION_TEST);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RAOP, "Could not add mDNS browser for AirPlay devices\n");

      goto out_stop_control;
    }

  return 0;

 out_stop_control:
  airplay_v2_control_stop();
 out_stop_timing:
  airplay_v2_timing_stop();
 out_free_timer:
  event_free(keep_alive_timer);
  free(airplay_aes_iv_b64);
 out_free_b64_key:
  free(airplay_aes_key_b64);
 out_close_cipher:
  gcry_cipher_close(airplay_aes_ctx);

  return -1;
}

static void
airplay_deinit(void)
{
  struct airplay_session *rs;

  for (rs = airplay_sessions; airplay_sessions; rs = airplay_sessions)
    {
      airplay_sessions = rs->next;

      session_free(rs);
    }

  airplay_v2_control_stop();
  airplay_v2_timing_stop();

  event_free(keep_alive_timer);

  gcry_cipher_close(airplay_aes_ctx);

  free(airplay_aes_key_b64);
  free(airplay_aes_iv_b64);
}

struct output_definition output_airplay =
{
  .name = "AirPlay 2",
  .type = OUTPUT_TYPE_AIRPLAY,
  .priority = 1,
  .disabled = 0,
  .init = airplay_init,
  .deinit = airplay_deinit,
  .device_start = airplay_device_start,
  .device_stop = airplay_device_stop,
  .device_flush = airplay_device_flush,
  .device_probe = airplay_device_probe,
  .device_cb_set = airplay_device_cb_set,
  .device_free_extra = airplay_device_free_extra,
  .device_volume_set = airplay_set_volume_one,
  .device_volume_to_pct = airplay_volume_to_pct,
  .write = airplay_write,
  .metadata_prepare = airplay_metadata_prepare,
  .metadata_send = airplay_metadata_send,
  .metadata_purge = airplay_metadata_purge,
#ifdef RAOP_VERIFICATION
  .device_authorize = airplay_device_authorize,
#endif
};