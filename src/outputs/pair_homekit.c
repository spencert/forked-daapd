/*
 * Adaption of ap2-sender by ViktoriiaKh:
 *    <https://github.com/ViktoriiaKh/ap2-sender>
 *
 * To test, it is useful to try with this receiver:
 *    <https://github.com/Argyropus/airplay2-receiver>
 *
 * The Secure Remote Password 6a implementation is adapted from:
 *  - Tom Cocagne
 *    <https://github.com/cocagne/csrp>
 *
 * TLV helpers are adapted from ESP homekit:
 *    <https://github.com/maximkulkin/esp-homekit>
 *
 * The MIT License (MIT)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is furnished to do
 * so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include <assert.h>

#include <sodium.h>

#include "pair-internal.h"


/* ----------------------------- DEFINES ETC ------------------------------- */

#define USERNAME "Pair-Setup"
#define AUTHTAG_LENGTH 16
#define NONCE_LENGTH 12 // 96 bits according to chacha poly1305
#define REQUEST_BUFSIZE 4096
#define ENCRYPTED_LEN_MAX 0x400

enum pair_keys
{
  PAIR_SETUP_MSG01 = 0,
  PAIR_SETUP_MSG02,
  PAIR_SETUP_MSG03,
  PAIR_SETUP_MSG04,
  PAIR_SETUP_MSG05,
  PAIR_SETUP_MSG06,
  PAIR_SETUP_SIGN,
  PAIR_VERIFY_MSG01,
  PAIR_VERIFY_MSG02,
  PAIR_VERIFY_MSG03,
  PAIR_VERIFY_MSG04,
  PAIR_CONTROL_WRITE,
  PAIR_CONTROL_READ,
};

struct pair_keys_map
{
  uint8_t state;
  const char *salt;
  const char *info;
  const char nonce[8];
};

struct pair_keys_map pair_keys_map[] =
{
  // Used for /pair-setup
  { 0x01, NULL, NULL, "" },
  { 0x02, NULL, NULL, "" },
  { 0x03, NULL, NULL, "" },
  { 0x04, NULL, NULL, "" },
  { 0x05, "Pair-Setup-Encrypt-Salt", "Pair-Setup-Encrypt-Info", "PS-Msg05" },
  { 0x06, "Pair-Setup-Encrypt-Salt", "Pair-Setup-Encrypt-Info", "PS-Msg06" },
  { 0, "Pair-Setup-Controller-Sign-Salt", "Pair-Setup-Controller-Sign-Info", "" },

  // Used for /pair-verify
  { 0x01, NULL, NULL, "" },
  { 0x02, "Pair-Verify-Encrypt-Salt", "Pair-Verify-Encrypt-Info", "PV-Msg02" },
  { 0x03, "Pair-Verify-Encrypt-Salt", "Pair-Verify-Encrypt-Info", "PV-Msg03" },
  { 0x04, NULL, NULL, "" },

  // Encryption/decryption
  { 0, "Control-Salt", "Control-Write-Encryption-Key", "" },
  { 0, "Control-Salt", "Control-Read-Encryption-Key", "" },
};

enum pair_method {
  PairingMethodPairSetup          = 0x00,
  PairingMethodPairSetupWithAuth  = 0x01,
  PairingMethodPairVerify         = 0x02,
  PairingMethodAddPairing         = 0x03,
  PairingMethodRemovePairing      = 0x04,
  PairingMethodListPairings       = 0x05
};


/* ---------------------------------- SRP ----------------------------------- */

typedef enum
{
  SRP_NG_2048,
  SRP_NG_3072,
  SRP_NG_CUSTOM
} SRP_NGType;

typedef struct
{
  bnum N;
  bnum g;
} NGConstant;

struct SRPUser
{
  enum hash_alg     alg;
  NGConstant        *ng;

  bnum a;
  bnum A;
  bnum S;

  const unsigned char *bytes_A;
  int           authenticated;

  char          *username;
  unsigned char *password;
  int           password_len;

  unsigned char M           [SHA512_DIGEST_LENGTH];
  unsigned char H_AMK       [SHA512_DIGEST_LENGTH];
  unsigned char session_key [SHA512_DIGEST_LENGTH];
  int           session_key_len;
};

struct NGHex
{
  const char *n_hex;
  const char *g_hex;
};

// These constants here were pulled from Appendix A of RFC 5054
static struct NGHex global_Ng_constants[] =
{
  { /* 2048 */
    "AC6BDB41324A9A9BF166DE5E1389582FAF72B6651987EE07FC3192943DB56050A37329CBB4"
    "A099ED8193E0757767A13DD52312AB4B03310DCD7F48A9DA04FD50E8083969EDB767B0CF60"
    "95179A163AB3661A05FBD5FAAAE82918A9962F0B93B855F97993EC975EEAA80D740ADBF4FF"
    "747359D041D5C33EA71D281E446B14773BCA97B43A23FB801676BD207A436C6481F1D2B907"
    "8717461A5B9D32E688F87748544523B524B0D57D5EA77A2775D2ECFA032CFBDBF52FB37861"
    "60279004E57AE6AF874E7303CE53299CCC041C7BC308D82A5698F3A8D0C38271AE35F8E9DB"
    "FBB694B5C803D89F7AE435DE236D525F54759B65E372FCD68EF20FA7111F9E4AFF73",
    "2"
  },
  { /* 3072 */
    "FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E088A67CC74020BBEA63B"
    "139B22514A08798E3404DDEF9519B3CD3A431B302B0A6DF25F14374FE1356D6D51C245E485"
    "B576625E7EC6F44C42E9A637ED6B0BFF5CB6F406B7EDEE386BFB5A899FA5AE9F24117C4B1F"
    "E649286651ECE45B3DC2007CB8A163BF0598DA48361C55D39A69163FA8FD24CF5F83655D23"
    "DCA3AD961C62F356208552BB9ED529077096966D670C354E4ABC9804F1746C08CA18217C32"
    "905E462E36CE3BE39E772C180E86039B2783A2EC07A28FB5C55DF06F4C52C9DE2BCBF69558"
    "17183995497CEA956AE515D2261898FA051015728E5A8AAAC42DAD33170D04507A33A85521"
    "ABDF1CBA64ECFB850458DBEF0A8AEA71575D060C7DB3970F85A6E1E4C7ABF5AE8CDB0933D7"
    "1E8C94E04A25619DCEE3D2261AD2EE6BF12FFA06D98A0864D87602733EC86A64521F2B1817"
    "7B200CBBE117577A615D6C770988C0BAD946E208E24FA074E5AB3143DB5BFCE0FD108E4B82"
    "D120A93AD2CAFFFFFFFFFFFFFFFF",
    "5"
  },
  {0,0} /* null sentinel */
};


static NGConstant *
new_ng(SRP_NGType ng_type, const char *n_hex, const char *g_hex)
{
  NGConstant *ng = calloc(1, sizeof(NGConstant));

  if ( ng_type != SRP_NG_CUSTOM )
    {
      n_hex = global_Ng_constants[ ng_type ].n_hex;
      g_hex = global_Ng_constants[ ng_type ].g_hex;
    }

  bnum_hex2bn(ng->N, n_hex);
  bnum_hex2bn(ng->g, g_hex);

  return ng;
}

static void
free_ng(NGConstant * ng)
{
  if (!ng)
    return;

  bnum_free(ng->N);
  bnum_free(ng->g);
  free(ng);
}

static bnum
calculate_x(enum hash_alg alg, const bnum salt, const char *username, const unsigned char *password, int password_len)
{
  unsigned char ucp_hash[SHA512_DIGEST_LENGTH];
  HashCTX       ctx;

  hash_init( alg, &ctx );
  hash_update( alg, &ctx, username, strlen(username) );
  hash_update( alg, &ctx, ":", 1 );
  hash_update( alg, &ctx, password, password_len );
  hash_final( alg, &ctx, ucp_hash );

  return H_ns( alg, salt, ucp_hash, hash_length(alg) );
}

static void
calculate_M(enum hash_alg alg, NGConstant *ng, unsigned char *dest, const char *I, const bnum s,
            const bnum A, const bnum B, const unsigned char *K, int K_len)
{
  unsigned char H_N[ SHA512_DIGEST_LENGTH ];
  unsigned char H_g[ SHA512_DIGEST_LENGTH ];
  unsigned char H_I[ SHA512_DIGEST_LENGTH ];
  unsigned char H_xor[ SHA512_DIGEST_LENGTH ];
  HashCTX       ctx;
  int           i = 0;
  int           hash_len = hash_length(alg);

  hash_num( alg, ng->N, H_N );
  hash_num( alg, ng->g, H_g );

  hash(alg, (const unsigned char *)I, strlen(I), H_I);

  for (i=0; i < hash_len; i++ )
    H_xor[i] = H_N[i] ^ H_g[i];
    
  hash_init( alg, &ctx );

  hash_update( alg, &ctx, H_xor, hash_len );
  hash_update( alg, &ctx, H_I,   hash_len );
  update_hash_n( alg, &ctx, s );
  update_hash_n( alg, &ctx, A );
  update_hash_n( alg, &ctx, B );
  hash_update( alg, &ctx, K, K_len );

  hash_final( alg, &ctx, dest );
}

static void
calculate_H_AMK(enum hash_alg alg, unsigned char *dest, const bnum A, const unsigned char * M, const unsigned char * K, int K_len)
{
  HashCTX ctx;

  hash_init( alg, &ctx );

  update_hash_n( alg, &ctx, A );
  hash_update( alg, &ctx, M, hash_length(alg) );
  hash_update( alg, &ctx, K, K_len );

  hash_final( alg, &ctx, dest );
}

static struct SRPUser *
srp_user_new(enum hash_alg alg, SRP_NGType ng_type, const char *username,
             const unsigned char *bytes_password, int len_password,
             const char *n_hex, const char *g_hex)
{
  struct SRPUser  *usr  = calloc(1, sizeof(struct SRPUser));
  int              ulen = strlen(username) + 1;

  if (!usr)
    goto err_exit;

  usr->alg = alg;
  usr->ng  = new_ng( ng_type, n_hex, g_hex );

  bnum_new(usr->a);
  bnum_new(usr->A);
  bnum_new(usr->S);

  if (!usr->ng || !usr->a || !usr->A || !usr->S)
    goto err_exit;

  usr->username     = malloc(ulen);
  usr->password     = malloc(len_password);
  usr->password_len = len_password;

  if (!usr->username || !usr->password)
    goto err_exit;

  memcpy(usr->username, username,       ulen);
  memcpy(usr->password, bytes_password, len_password);

  usr->authenticated = 0;
  usr->bytes_A = 0;

  return usr;

 err_exit:
  if (!usr)
    return NULL;

  bnum_free(usr->a);
  bnum_free(usr->A);
  bnum_free(usr->S);

  free(usr->username);
  if (usr->password)
    {
      memset(usr->password, 0, usr->password_len);
      free(usr->password);
    }
  free(usr);

  return NULL;
}

static void
srp_user_free(struct SRPUser *usr)
{
  if(!usr)
    return;

  bnum_free(usr->a);
  bnum_free(usr->A);
  bnum_free(usr->S);

  free_ng(usr->ng);

  memset(usr->password, 0, usr->password_len);

  free(usr->username);
  free(usr->password);
  free((char *)usr->bytes_A);

  memset(usr, 0, sizeof(*usr));
  free(usr);
}

static int
srp_user_is_authenticated(struct SRPUser *usr)
{
  return usr->authenticated;
}

static const unsigned char *
srp_user_get_session_key(struct SRPUser *usr, int *key_length)
{
  if (key_length)
    *key_length = usr->session_key_len;
  return usr->session_key;
}

/* Output: username, bytes_A, len_A */
static void
srp_user_start_authentication(struct SRPUser *usr, const char **username,
                              const unsigned char **bytes_A, int *len_A)
{
  bnum_random(usr->a, 256);
//  BN_hex2bn(&(usr->a), "D929DFB605687233C9E9030C2280156D03BDB9FDCF3CCE3BC27D9CCFCB5FF6A1");

  bnum_modexp(usr->A, usr->ng->g, usr->a, usr->ng->N);
    
  *len_A   = bnum_num_bytes(usr->A);
  *bytes_A = malloc(*len_A);

  if (!*bytes_A)
    {
      *len_A = 0;
      *bytes_A = 0;
      *username = 0;
      return;
    }

  bnum_bn2bin(usr->A, (unsigned char *) *bytes_A, *len_A);

  usr->bytes_A = *bytes_A;
  *username = usr->username;
}

/* Output: bytes_M. Buffer length is SHA512_DIGEST_LENGTH */
static void
srp_user_process_challenge(struct SRPUser *usr, const unsigned char *bytes_s, int len_s,
                           const unsigned char *bytes_B, int len_B,
                           const unsigned char **bytes_M, int *len_M )
{
  bnum s, B, k, v;
  bnum tmp1, tmp2, tmp3;
  bnum u, x;

  *len_M = 0;
  *bytes_M = 0;

  bnum_bin2bn(s, bytes_s, len_s);
  bnum_bin2bn(B, bytes_B, len_B);
  k    = H_nn_pad(usr->alg, usr->ng->N, usr->ng->g);

  bnum_new(v);
  bnum_new(tmp1);
  bnum_new(tmp2);
  bnum_new(tmp3);

  if (!s || !B || !k || !v || !tmp1 || !tmp2 || !tmp3)
    goto cleanup1;

  u = H_nn_pad(usr->alg, usr->A, B);
  x = calculate_x(usr->alg, s, usr->username, usr->password, usr->password_len);
  if (!u || !x)
    goto cleanup2;

  // SRP-6a safety check
  if (!bnum_is_zero(B) && !bnum_is_zero(u))
    {
      bnum_modexp(v, usr->ng->g, x, usr->ng->N);

      // S = (B - k*(g^x)) ^ (a + ux)
      bnum_mul(tmp1, u, x);
      bnum_add(tmp2, usr->a, tmp1);        // tmp2 = (a + ux)
      bnum_modexp(tmp1, usr->ng->g, x, usr->ng->N);
      bnum_mul(tmp3, k, tmp1);             // tmp3 = k*(g^x)
      bnum_sub(tmp1, B, tmp3);             // tmp1 = (B - K*(g^x))
      bnum_modexp(usr->S, tmp1, tmp2, usr->ng->N);

      hash_num(usr->alg, usr->S, usr->session_key);
      usr->session_key_len = hash_length(usr->alg);

      calculate_M(usr->alg, usr->ng, usr->M, usr->username, s, usr->A, B, usr->session_key, usr->session_key_len);
      calculate_H_AMK(usr->alg, usr->H_AMK, usr->A, usr->M, usr->session_key, usr->session_key_len);

      *bytes_M = usr->M;
      if (len_M)
        *len_M = hash_length(usr->alg);
    }
  else
    {
      *bytes_M = NULL;
      if (len_M)
        *len_M   = 0;
    }

 cleanup2:
  bnum_free(x);
  bnum_free(u);
 cleanup1:
  bnum_free(tmp3);
  bnum_free(tmp2);
  bnum_free(tmp1);
  bnum_free(v);
  bnum_free(k);
  bnum_free(B);
  bnum_free(s);
}

static void
srp_user_verify_session(struct SRPUser *usr, const unsigned char *bytes_HAMK)
{
  if (memcmp(usr->H_AMK, bytes_HAMK, hash_length(usr->alg)) == 0)
    usr->authenticated = 1;
}


/* ---------------------------------- TLV ----------------------------------- */

#define TLV_ERROR_MEMORY -1
#define TLV_ERROR_INSUFFICIENT_SIZE -2

typedef enum {
    TLVType_Method = 0,        // (integer) Method to use for pairing. See PairMethod
    TLVType_Identifier = 1,    // (UTF-8) Identifier for authentication
    TLVType_Salt = 2,          // (bytes) 16+ bytes of random salt
    TLVType_PublicKey = 3,     // (bytes) Curve25519, SRP public key or signed Ed25519 key
    TLVType_Proof = 4,         // (bytes) Ed25519 or SRP proof
    TLVType_EncryptedData = 5, // (bytes) Encrypted data with auth tag at end
    TLVType_State = 6,         // (integer) State of the pairing process. 1=M1, 2=M2, etc.
    TLVType_Error = 7,         // (integer) Error code. Must only be present if error code is
                               // not 0. See TLVError
    TLVType_RetryDelay = 8,    // (integer) Seconds to delay until retrying a setup code
    TLVType_Certificate = 9,   // (bytes) X.509 Certificate
    TLVType_Signature = 10,    // (bytes) Ed25519
    TLVType_Permissions = 11,  // (integer) Bit value describing permissions of the controller
                               // being added.
                               // None (0x00): Regular user
                               // Bit 1 (0x01): Admin that is able to add and remove
                               // pairings against the accessory
    TLVType_FragmentData = 13, // (bytes) Non-last fragment of data. If length is 0,
                               // it's an ACK.
    TLVType_FragmentLast = 14, // (bytes) Last fragment of data
    TLVType_Separator = 0xff,
} TLVType;


typedef enum {
  TLVMethod_PairSetup = 1,
  TLVMethod_PairVerify = 2,
  TLVMethod_AddPairing = 3,
  TLVMethod_RemovePairing = 4,
  TLVMethod_ListPairings = 5,
} TLVMethod;


typedef enum {
  TLVError_Unknown = 1,         // Generic error to handle unexpected errors
  TLVError_Authentication = 2,  // Setup code or signature verification failed
  TLVError_Backoff = 3,         // Client must look at the retry delay TLV item and
                                // wait that many seconds before retrying
  TLVError_MaxPeers = 4,        // Server cannot accept any more pairings
  TLVError_MaxTries = 5,        // Server reached its maximum number of
                                // authentication attempts
  TLVError_Unavailable = 6,     // Server pairing method is unavailable
  TLVError_Busy = 7,            // Server is busy and cannot accept a pairing
                                // request at this time
} TLVError;

typedef struct _tlv {
    struct _tlv *next;
    uint8_t type;
    uint8_t *value;
    size_t size;
} tlv_t;


typedef struct {
    tlv_t *head;
} tlv_values_t;


static tlv_values_t *
tlv_new() {
    tlv_values_t *values = malloc(sizeof(tlv_values_t));
    if (!values)
        return NULL;

    values->head = NULL;
    return values;
}

static void
tlv_free(tlv_values_t *values) {
    tlv_t *t = values->head;
    while (t) {
        tlv_t *t2 = t;
        t = t->next;
        if (t2->value)
            free(t2->value);
        free(t2);
    }
    free(values);
}

static int
tlv_add_value_(tlv_values_t *values, uint8_t type, uint8_t *value, size_t size) {
    tlv_t *tlv = malloc(sizeof(tlv_t));
    if (!tlv) {
        return TLV_ERROR_MEMORY;
    }
    tlv->type = type;
    tlv->size = size;
    tlv->value = value;
    tlv->next = NULL;

    if (!values->head) {
        values->head = tlv;
    } else {
        tlv_t *t = values->head;
        while (t->next) {
            t = t->next;
        }
        t->next = tlv;
    }

    return 0;
}

static int
tlv_add_value(tlv_values_t *values, uint8_t type, const uint8_t *value, size_t size) {
    uint8_t *data = NULL;
    int ret;
    if (size) {
        data = malloc(size);
        if (!data) {
            return TLV_ERROR_MEMORY;
        }
        memcpy(data, value, size);
    }
    ret = tlv_add_value_(values, type, data, size);
    if (ret < 0)
        free(data);
    return ret;
}

static tlv_t *
tlv_get_value(const tlv_values_t *values, uint8_t type) {
    tlv_t *t = values->head;
    while (t) {
        if (t->type == type)
            return t;
        t = t->next;
    }
    return NULL;
}

static int
tlv_format(const tlv_values_t *values, uint8_t *buffer, size_t *size) {
    size_t required_size = 0;
    tlv_t *t = values->head;
    while (t) {
        required_size += t->size + 2 * ((t->size + 254) / 255);
        t = t->next;
    }

    if (*size < required_size) {
        *size = required_size;
        return TLV_ERROR_INSUFFICIENT_SIZE;
    }

    *size = required_size;

    t = values->head;
    while (t) {
        uint8_t *data = t->value;
        if (!t->size) {
            buffer[0] = t->type;
            buffer[1] = 0;
            buffer += 2;
            t = t->next;
            continue;
        }

        size_t remaining = t->size;

        while (remaining) {
            buffer[0] = t->type;
            size_t chunk_size = (remaining > 255) ? 255 : remaining;
            buffer[1] = chunk_size;
            memcpy(&buffer[2], data, chunk_size);
            remaining -= chunk_size;
            buffer += chunk_size + 2;
            data += chunk_size;
        }

        t = t->next;
    }

    return 0;
}

static int
tlv_parse(const uint8_t *buffer, size_t length, tlv_values_t *values) {
    size_t i = 0;
    int ret;
    while (i < length) {
        uint8_t type = buffer[i];
        size_t size = 0;
        uint8_t *data = NULL;

        // scan TLVs to accumulate total size of subsequent TLVs with same type (chunked data)
        size_t j = i;
        while (j < length && buffer[j] == type && buffer[j+1] == 255) {
            size_t chunk_size = buffer[j+1];
            size += chunk_size;
            j += chunk_size + 2;
        }
        if (j < length && buffer[j] == type) {
            size_t chunk_size = buffer[j+1];
            size += chunk_size;
        }

        // allocate memory to hold all pieces of chunked data and copy data there
        if (size != 0) {
            data = malloc(size);
            if (!data)
                return TLV_ERROR_MEMORY;

            uint8_t *p = data;

            size_t remaining = size;
            while (remaining) {
                size_t chunk_size = buffer[i+1];
                memcpy(p, &buffer[i+2], chunk_size);
                p += chunk_size;
                i += chunk_size + 2;
                remaining -= chunk_size;
            }
        }

        ret = tlv_add_value_(values, type, data, size);
        if (ret < 0) {
            free(data);
            return ret;
        }
    }

    return 0;
}


/* -------------------------------- HELPERS --------------------------------- */

#ifdef DEBUG_PAIR
static void
tlv_debug(const tlv_values_t *values)
{
  printf("Received TLV values\n");
  for (tlv_t *t=values->head; t; t=t->next)
    {
      printf("Type %d value (%zu bytes): \n", t->type, t->size);
      hexdump("", t->value, t->size);
    }
}
#endif

static tlv_values_t *
response_process(const uint8_t *data, uint32_t data_len, const char **errmsg)
{
  tlv_values_t *response;
  tlv_t *error;
  int ret;

  response = tlv_new();
  if (!response)
    {
      *errmsg = "Out of memory\n";
      return NULL;
    }

  ret = tlv_parse(data, data_len, response);
  if (ret < 0)
    {
      *errmsg = "Could not parse TLV\n";
      goto error;
    }

#ifdef DEBUG_PAIR
  tlv_debug(response);
#endif

  error = tlv_get_value(response, TLVType_Error);
  if (error)
    {
      if (error->value[0] == TLVError_Authentication)
	*errmsg = "Device returned an authtication failure";
      else if (error->value[0] == TLVError_Backoff)
	*errmsg = "Device told us to back off pairing attempts\n";
      else if (error->value[0] == TLVError_MaxPeers)
	*errmsg = "Max peers trying to connect to device\n";
      else if (error->value[0] == TLVError_MaxTries)
	*errmsg = "Max pairing attemps reached\n";
      else if (error->value[0] == TLVError_Unavailable)
	*errmsg = "Device is unuavailble at this time\n";
      else
	*errmsg = "Device is busy/returned unknown error\n";

      goto error;
    }

  return response;

 error:
  tlv_free(response);
  return NULL;
}

/* Executes SHA512 RFC 5869 extract + expand, writing a derived key to okm

   hkdfExtract(SHA512, salt, salt_len, ikm, ikm_len, prk);
   hkdfExpand(SHA512, prk, SHA512_LEN, info, info_len, okm, okm_len);
*/
static int
hkdf_extract_expand(uint8_t *okm, size_t okm_len, const uint8_t *ikm, size_t ikm_len, enum pair_keys pair_key)
{
#ifdef CONFIG_OPENSSL
#include <openssl/kdf.h>
  EVP_PKEY_CTX *pctx;

  if (okm_len > SHA512_DIGEST_LENGTH)
    return -1;
  if (! (pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, NULL)))
    return -1;
  if (EVP_PKEY_derive_init(pctx) <= 0)
    goto error;
  if (EVP_PKEY_CTX_set_hkdf_md(pctx, EVP_sha512()) <= 0)
    goto error;
  if (EVP_PKEY_CTX_set1_hkdf_salt(pctx, (const unsigned char *)pair_keys_map[pair_key].salt, strlen(pair_keys_map[pair_key].salt)) <= 0)
    goto error;
  if (EVP_PKEY_CTX_set1_hkdf_key(pctx, ikm, ikm_len) <= 0)
    goto error;
  if (EVP_PKEY_CTX_add1_hkdf_info(pctx, (const unsigned char *)pair_keys_map[pair_key].info, strlen(pair_keys_map[pair_key].info)) <= 0)
    goto error;
  if (EVP_PKEY_derive(pctx, okm, &okm_len) <= 0)
    goto error;

  EVP_PKEY_CTX_free(pctx);
  return 0;

 error:
  EVP_PKEY_CTX_free(pctx);
  return -1;
#elif CONFIG_GCRYPT
  uint8_t prk[SHA512_DIGEST_LENGTH];
  gcry_md_hd_t hmac_handle;

  if (okm_len > SHA512_DIGEST_LENGTH)
    return -1; // Below calculation not valid if output is larger than hash size
  if (gcry_md_open(&hmac_handle, GCRY_MD_SHA512, GCRY_MD_FLAG_HMAC) != GPG_ERR_NO_ERROR)
    return -1;
  if (gcry_md_setkey(hmac_handle, (const unsigned char *)pair_keys_map[pair_key].salt, strlen(pair_keys_map[pair_key].salt)) != GPG_ERR_NO_ERROR)
    goto error;
  gcry_md_write(hmac_handle, ikm, ikm_len);
  memcpy(prk, gcry_md_read(hmac_handle, 0), sizeof(prk));

  gcry_md_reset(hmac_handle);

  if (gcry_md_setkey(hmac_handle, prk, sizeof(prk)) != GPG_ERR_NO_ERROR)
    goto error;
  gcry_md_write(hmac_handle, (const unsigned char *)pair_keys_map[pair_key].info, strlen(pair_keys_map[pair_key].info));
  gcry_md_putc(hmac_handle, 1);

  memcpy(okm, gcry_md_read(hmac_handle, 0), okm_len);

  gcry_md_close(hmac_handle);
  return 0;

 error:
  gcry_md_close(hmac_handle);
  return -1;
#else
  return -1;
#endif
}

static int
encrypt_chacha(uint8_t *cipher, uint8_t *plain, size_t plain_len, const uint8_t *key, size_t key_len, const void *ad, size_t ad_len, uint8_t *tag, size_t tag_len, const uint8_t nonce[NONCE_LENGTH])
{
#ifdef CONFIG_OPENSSL
  EVP_CIPHER_CTX *ctx;
  int len;

  if (! (ctx = EVP_CIPHER_CTX_new()))
    return -1;

  if (EVP_EncryptInit_ex(ctx, EVP_chacha20_poly1305(), NULL, key, nonce) != 1)
    goto error;

  if (EVP_CIPHER_CTX_set_padding(ctx, 0) != 1) // Maybe not necessary
    goto error;

  if (EVP_EncryptUpdate(ctx, NULL, &len, ad, ad_len) != 1)
    goto error;

  if (EVP_EncryptUpdate(ctx, cipher, &len, plain, plain_len) != 1)
    goto error;

  assert(len == plain_len);

  if (EVP_EncryptFinal_ex(ctx, NULL, &len) != 1)
    goto error;

  if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, tag_len, tag) != 1)
    goto error;

  EVP_CIPHER_CTX_free(ctx);
  return 0;

 error:
  EVP_CIPHER_CTX_free(ctx);
  return -1;
#elif CONFIG_GCRYPT
  gcry_cipher_hd_t hd;

  if (gcry_cipher_open(&hd, GCRY_CIPHER_CHACHA20, GCRY_CIPHER_MODE_POLY1305, 0) != GPG_ERR_NO_ERROR)
    return -1;

  if (gcry_cipher_setkey(hd, key, key_len) != GPG_ERR_NO_ERROR)
    goto error;

  if (gcry_cipher_setiv(hd, nonce, NONCE_LENGTH) != GPG_ERR_NO_ERROR)
    goto error;

  if (gcry_cipher_authenticate(hd, ad, ad_len) != GPG_ERR_NO_ERROR)
    goto error;

  if (gcry_cipher_encrypt(hd, cipher, plain_len, plain, plain_len) != GPG_ERR_NO_ERROR)
    goto error;

  if (gcry_cipher_gettag(hd, tag, tag_len) != GPG_ERR_NO_ERROR)
    goto error;

  gcry_cipher_close(hd);
  return 0;

 error:
  gcry_cipher_close(hd);
  return -1;
#else
  return -1;
#endif
}

static int
decrypt_chacha(uint8_t *plain, uint8_t *cipher, size_t cipher_len, const uint8_t *key, size_t key_len, const void *ad, size_t ad_len, uint8_t *tag, size_t tag_len, const uint8_t nonce[NONCE_LENGTH])
{
#ifdef CONFIG_OPENSSL
  EVP_CIPHER_CTX *ctx;
  int len;

  if (! (ctx = EVP_CIPHER_CTX_new()))
    return -1;

  if (EVP_DecryptInit_ex(ctx, EVP_chacha20_poly1305(), NULL, key, nonce) != 1)
    goto error;

  if (EVP_CIPHER_CTX_set_padding(ctx, 0) != 1) // Maybe not necessary
    goto error;

  if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, tag_len, tag) != 1)
    goto error;

  if (EVP_DecryptUpdate(ctx, NULL, &len, ad, ad_len) != 1)
    goto error;

  if (EVP_DecryptUpdate(ctx, plain, &len, cipher, cipher_len) != 1)
    goto error;

  assert(len == cipher_len);

  if (EVP_DecryptFinal_ex(ctx, NULL, &len) != 1)
    goto error;

  EVP_CIPHER_CTX_free(ctx);
  return 0;

 error:
  EVP_CIPHER_CTX_free(ctx);
  return -1;
#elif CONFIG_GCRYPT
  gcry_cipher_hd_t hd;

  if (gcry_cipher_open(&hd, GCRY_CIPHER_CHACHA20, GCRY_CIPHER_MODE_POLY1305, 0) != GPG_ERR_NO_ERROR)
    return -1;

  if (gcry_cipher_setkey(hd, key, key_len) != GPG_ERR_NO_ERROR)
    goto error;

  if (gcry_cipher_setiv(hd, nonce, NONCE_LENGTH) != GPG_ERR_NO_ERROR)
    goto error;

  if (gcry_cipher_authenticate(hd, ad, ad_len) != GPG_ERR_NO_ERROR)
    goto error;

  if (gcry_cipher_decrypt(hd, plain, cipher_len, cipher, cipher_len) != GPG_ERR_NO_ERROR)
    goto error;

  if (gcry_cipher_checktag(hd, tag, tag_len) != GPG_ERR_NO_ERROR)
    goto error;

  gcry_cipher_close(hd);
  return 0;

 error:
  gcry_cipher_close(hd);
  return -1;
#else
  return -1;
#endif
}

static int
create_and_sign_device_info(uint8_t *data, size_t *data_len, const char *device_id, uint8_t *device_pk, size_t device_pk_len, uint8_t *pk, size_t pk_len, uint8_t *sk)
{
  tlv_values_t *tlv;
  uint8_t *device_info;
  uint32_t device_info_len;
  size_t device_id_len;
  uint8_t signature[crypto_sign_BYTES];
  int ret;

  device_id_len = strlen(device_id);

  device_info_len = device_pk_len + device_id_len + pk_len;
  device_info = malloc(device_info_len);

  memcpy(device_info, device_pk, device_pk_len);
  memcpy(device_info + device_pk_len, device_id, device_id_len);
  memcpy(device_info + device_pk_len + device_id_len, pk, pk_len);

  crypto_sign_detached(signature, NULL, device_info, device_info_len, sk);
  free(device_info);

  tlv = tlv_new();
  tlv_add_value(tlv, TLVType_Identifier, (unsigned char *)device_id, device_id_len);
  tlv_add_value(tlv, TLVType_Signature, signature, sizeof(signature));

  ret = tlv_format(tlv, data, data_len);

  tlv_free(tlv);
  return ret;
}


/* -------------------------- IMPLEMENTATION -------------------------------- */

static struct pair_setup_context *
pair_setup_new(int type, const char *pin, const char *device_id)
{
  struct pair_setup_context *sctx;

  if (sodium_init() == -1)
    return NULL;

  if (!pin || strlen(pin) < 4)
    return NULL;

  if (device_id && strlen(device_id) != 16)
    return NULL;

  sctx = calloc(1, sizeof(struct pair_setup_context));
  if (!sctx)
    return NULL;

  sctx->type = type;

  memcpy(sctx->pin, pin, sizeof(sctx->pin));

  if (device_id)
    memcpy(sctx->device_id, device_id, strlen(device_id));

  return sctx;
}

static void
pair_setup_free(struct pair_setup_context *sctx)
{
  if (!sctx)
    return;

  srp_user_free(sctx->user);

  free(sctx->pkB);
  free(sctx->M2);
  free(sctx->salt);
  free(sctx->epk);
  free(sctx->authtag);

  free(sctx);
}

static uint8_t *
pair_setup_request1(uint32_t *len, struct pair_setup_context *sctx)
{
  tlv_values_t *request;
  uint8_t *data;
  size_t data_len;
  uint8_t method;
  int endian_test = 1;
  int ret;

  data_len = REQUEST_BUFSIZE;
  data = malloc(data_len);
  request = tlv_new();

  // Test here instead of setup_new() so we can give an error message
  if(*(char *)&endian_test != 1)
    {
      sctx->errmsg = "Setup request 1: No support for big endian architechture";
      goto error;
    }

  sctx->user = srp_user_new(HASH_SHA512, SRP_NG_3072, USERNAME, (unsigned char *)sctx->pin, sizeof(sctx->pin), 0, 0);
  if (!sctx->user)
    {
      sctx->errmsg = "Setup request 1: Create SRP user failed";
      goto error;
    }

  method = PairingMethodPairSetup;
  tlv_add_value(request, TLVType_State, &pair_keys_map[PAIR_SETUP_MSG01].state, sizeof(pair_keys_map[PAIR_SETUP_MSG01].state));
  tlv_add_value(request, TLVType_Method, &method, sizeof(method));

  ret = tlv_format(request, data, &data_len);
  if (ret < 0)
    {
      sctx->errmsg = "Setup request 1: tlv_format returned an error";
      goto error;
    }

  *len = data_len;

  tlv_free(request);
  return data;

 error:
  tlv_free(request);
  free(data);
  return NULL;
}

static uint8_t *
pair_setup_request2(uint32_t *len, struct pair_setup_context *sctx)
{
  tlv_values_t *request;
  uint8_t *data;
  size_t data_len;
  const char *auth_username = NULL;
  int ret;

  data_len = REQUEST_BUFSIZE;
  data = malloc(data_len);
  request = tlv_new();

  // Calculate A
  srp_user_start_authentication(sctx->user, &auth_username, &sctx->pkA, &sctx->pkA_len);

  // Calculate M1 (client proof)
  srp_user_process_challenge(sctx->user, (const unsigned char *)sctx->salt, sctx->salt_len, (const unsigned char *)sctx->pkB, sctx->pkB_len, &sctx->M1, &sctx->M1_len);

  tlv_add_value(request, TLVType_State, &pair_keys_map[PAIR_SETUP_MSG03].state, sizeof(pair_keys_map[PAIR_SETUP_MSG03].state));
  tlv_add_value(request, TLVType_PublicKey, sctx->pkA, sctx->pkA_len);
  tlv_add_value(request, TLVType_Proof, sctx->M1, sctx->M1_len);

  ret = tlv_format(request, data, &data_len);
  if (ret < 0)
    {
      sctx->errmsg = "Setup request 2: tlv_format returned an error";
      goto error;
    }

  *len = data_len;

  tlv_free(request);
  return data;

 error:
  tlv_free(request);
  free(data);
  return NULL;
}

static uint8_t *
pair_setup_request3(uint32_t *len, struct pair_setup_context *sctx)
{
  tlv_values_t *request;
  uint8_t *data;
  size_t data_len;
  const unsigned char *session_key;
  int session_key_len;
  uint8_t device_x[32];
  uint8_t nonce[NONCE_LENGTH] = { 0 };
  uint8_t tag[AUTHTAG_LENGTH];
  uint8_t derived_key[32];
  tlv_values_t *append;
  size_t append_len;
  uint8_t *encrypted_data = NULL;
  size_t encrypted_data_len;
  int ret;

  data_len = REQUEST_BUFSIZE;
  data = malloc(data_len);
  request = tlv_new();

  session_key = srp_user_get_session_key(sctx->user, &session_key_len);
  if (!session_key)
    {
      sctx->errmsg = "Setup request 3: No valid session key";
      goto error;
    }

  ret = hkdf_extract_expand(device_x, sizeof(device_x), session_key, session_key_len, PAIR_SETUP_SIGN);
  if (ret < 0)
    {
      sctx->errmsg = "Setup request 3: hkdf error getting device_x";
      goto error;
    }

  crypto_sign_keypair(sctx->public_key, sctx->private_key);

  ret = create_and_sign_device_info(data, &data_len, sctx->device_id, device_x, sizeof(device_x), sctx->public_key, sizeof(sctx->public_key), sctx->private_key);
  if (ret < 0)
    {
      sctx->errmsg = "Setup request 3: error creating signed device info";
      goto error;
    }

  ret = hkdf_extract_expand(derived_key, sizeof(derived_key), session_key, 64, PAIR_SETUP_MSG05); // TODO is session_key_len always 64?
  if (ret < 0)
    {
      sctx->errmsg = "Setup request 3: hkdf error getting derived_key";
      goto error;
    }

  // Append TLV-encoded public key to *data, which already has identifier and signature
  append = tlv_new();
  append_len = REQUEST_BUFSIZE - data_len;
  tlv_add_value(append, TLVType_PublicKey, sctx->public_key, sizeof(sctx->public_key));
  ret = tlv_format(append, data + data_len, &append_len);
  tlv_free(append);
  if (ret < 0)
    {
      sctx->errmsg = "Setup request 3: error appending public key to TLV";
      goto error;
    }
  data_len += append_len;

  memcpy(nonce + 4, pair_keys_map[PAIR_SETUP_MSG05].nonce, NONCE_LENGTH - 4);

  encrypted_data_len = data_len + sizeof(tag); // Space for ciphered payload and authtag
  encrypted_data = malloc(encrypted_data_len);

  ret = encrypt_chacha(encrypted_data, data, data_len, derived_key, sizeof(derived_key), NULL, 0, tag, sizeof(tag), nonce);
  if (ret < 0)
    {
      sctx->errmsg = "Setup request 3: Could not encrypt";
      goto error;
    }

  memcpy(encrypted_data + data_len, tag, sizeof(tag));

  tlv_add_value(request, TLVType_State, &pair_keys_map[PAIR_SETUP_MSG05].state, sizeof(pair_keys_map[PAIR_SETUP_MSG05].state));
  tlv_add_value(request, TLVType_EncryptedData, encrypted_data, encrypted_data_len);

  data_len = REQUEST_BUFSIZE; // Re-using *data, so pass original length to tlv_format
  ret = tlv_format(request, data, &data_len);
  if (ret < 0)
    {
      sctx->errmsg = "Setup request 3: error appending public key to TLV";
      goto error;
    }

  *len = data_len;

  free(encrypted_data);
  tlv_free(request);
  return data;

 error:
  free(encrypted_data);
  tlv_free(request);
  free(data);
  return NULL;
}

static int
pair_setup_response1(struct pair_setup_context *sctx, const uint8_t *data, uint32_t data_len)
{
  tlv_values_t *response;
  tlv_t *pk;
  tlv_t *salt;

  response = response_process(data, data_len, &sctx->errmsg);
  if (!response)
    {
      sctx->errmsg = "Setup response 1: Could not parse TLV";
      return -1;
    }

  pk = tlv_get_value(response, TLVType_PublicKey);
  salt = tlv_get_value(response, TLVType_Salt);
  if (!pk || !salt)
    {
      sctx->errmsg = "Setup response 1: Missing or invalid pk/salt";
      goto error;
    }

  sctx->pkB_len = pk->size; // 384
  sctx->pkB = malloc(sctx->pkB_len);
  memcpy(sctx->pkB, pk->value, sctx->pkB_len);

  sctx->salt_len = salt->size; // 16
  sctx->salt = malloc(sctx->salt_len);
  memcpy(sctx->salt, salt->value, sctx->salt_len);

  tlv_free(response);
  return 0;

 error:
  tlv_free(response);
  return -1;
}

static int
pair_setup_response2(struct pair_setup_context *sctx, const uint8_t *data, uint32_t data_len)
{
  tlv_values_t *response;
  tlv_t *proof;

  response = response_process(data, data_len, &sctx->errmsg);
  if (!response)
    {
      sctx->errmsg = "Setup response 2: Could not parse TLV";
      return -1;
    }

  proof = tlv_get_value(response, TLVType_Proof);
  if (!proof)
    {
      sctx->errmsg = "Setup response 2: Missing proof";
      goto error;
    }

  sctx->M2_len = proof->size; // 64
  sctx->M2 = malloc(sctx->M2_len);
  memcpy(sctx->M2, proof->value, sctx->M2_len);

  // Check M2
  srp_user_verify_session(sctx->user, (const unsigned char *)sctx->M2);
  if (!srp_user_is_authenticated(sctx->user))
    {
      sctx->errmsg = "Setup response 2: Server authentication failed";
      goto error;
    }

  tlv_free(response);
  return 0;

 error:
  tlv_free(response);
  return -1;
}

static int
pair_setup_response3(struct pair_setup_context *sctx, const uint8_t *data, uint32_t data_len)
{
  tlv_values_t *response;
  tlv_t *encrypted_data;
  uint8_t nonce[NONCE_LENGTH] = { 0 };
  uint8_t tag[AUTHTAG_LENGTH];
  uint8_t derived_key[32];
  size_t encrypted_len;
  uint8_t *decrypted_data = NULL;
  const uint8_t *session_key;
  int session_key_len;
  int ret;

  response = response_process(data, data_len, &sctx->errmsg);
  if (!response)
    {
      sctx->errmsg = "Setup response 3: Could not parse TLV";
      return -1;
    }

  encrypted_data = tlv_get_value(response, TLVType_EncryptedData);
  if (!encrypted_data)
    {
      sctx->errmsg = "Setup response 3: Missing encrypted_data";
      goto error;
    }

  session_key = srp_user_get_session_key(sctx->user, &session_key_len);
  if (!session_key)
    {
      sctx->errmsg = "Setup response 3: No valid session key";
      goto error;
    }

  ret = hkdf_extract_expand(derived_key, sizeof(derived_key), session_key, 64, PAIR_SETUP_MSG06); // TODO is session_key_len always 64?
  if (ret < 0)
    {
      sctx->errmsg = "Setup response 3: hkdf error getting derived_key";
      goto error;
    }

  // encrypted_data->value consists of the encrypted payload + the auth tag
  if (encrypted_data->size < AUTHTAG_LENGTH)
    {
      sctx->errmsg = "Setup response 3: Invalid encrypted data";
      goto error;
    }

  encrypted_len = encrypted_data->size - AUTHTAG_LENGTH;
  memcpy(tag, encrypted_data->value + encrypted_len, AUTHTAG_LENGTH);
  memcpy(nonce + 4, pair_keys_map[PAIR_SETUP_MSG06].nonce, NONCE_LENGTH - 4);

  decrypted_data = malloc(encrypted_len);

  ret = decrypt_chacha(decrypted_data, encrypted_data->value, encrypted_len, derived_key, sizeof(derived_key), NULL, 0, tag, sizeof(tag), nonce);
  if (ret < 0)
    {
      sctx->errmsg = "Setup response 3: Decryption error";
      goto error;
    }

  tlv_free(response);
  response = response_process(decrypted_data, encrypted_len, &sctx->errmsg);
  if (!response)
    {
      sctx->errmsg = "Setup response 3: Could not parse decrypted TLV";
      goto error;
    }

  // TODO check identifier and signature - we get an identifier (36), a public key (32) and a signature (64)

  free(decrypted_data);
  tlv_free(response);
  return 0;

 error:
  free(decrypted_data);
  tlv_free(response);
  return -1;
}

static uint8_t *
pair_verify_request1(uint32_t *len, struct pair_verify_context *vctx)
{
  const uint8_t basepoint[32] = {9};
  tlv_values_t *request;
  uint8_t *data;
  size_t data_len;
  int ret;

  data_len = REQUEST_BUFSIZE;
  data = malloc(data_len);
  request = tlv_new();

  ret = crypto_scalarmult(vctx->client_eph_public_key, vctx->client_eph_private_key, basepoint);
  if (ret < 0)
    {
      vctx->errmsg = "Verify request 1: Curve 25519 returned an error";
      goto error;
    }

  tlv_add_value(request, TLVType_State, &pair_keys_map[PAIR_VERIFY_MSG01].state, sizeof(pair_keys_map[PAIR_VERIFY_MSG01].state));
  tlv_add_value(request, TLVType_PublicKey, vctx->client_eph_public_key, sizeof(vctx->client_eph_public_key));

  ret = tlv_format(request, data, &data_len);
  if (ret < 0)
    {
      vctx->errmsg = "Verify request 1: tlv_format returned an error";
      goto error;
    }

  *len = data_len;

  tlv_free(request);
  return data;

 error:
  tlv_free(request);
  free(data);
  return NULL;
}

static uint8_t *
pair_verify_request2(uint32_t *len, struct pair_verify_context *vctx)
{
  tlv_values_t *request;
  uint8_t *data;
  size_t data_len;
  uint8_t nonce[NONCE_LENGTH] = { 0 };
  uint8_t tag[AUTHTAG_LENGTH];
  uint8_t derived_key[32];
  uint8_t *encrypted_data = NULL;
  size_t encrypted_data_len;
  int ret;

  data_len = REQUEST_BUFSIZE;
  data = malloc(data_len);
  request = tlv_new();

  ret = create_and_sign_device_info(data, &data_len, vctx->device_id, vctx->client_eph_public_key, sizeof(vctx->client_eph_public_key),
                                    vctx->server_eph_public_key, sizeof(vctx->server_eph_public_key), vctx->client_private_key);
  if (ret < 0)
    {
      vctx->errmsg = "Verify request 2: error creating signed device info";
      goto error;
    }

  ret = hkdf_extract_expand(derived_key, sizeof(derived_key), vctx->shared_secret, sizeof(vctx->shared_secret), PAIR_VERIFY_MSG03);
  if (ret < 0)
    {
      vctx->errmsg = "Verify request 2: hkdf error getting derived_key";
      goto error;
    }

  memcpy(nonce + 4, pair_keys_map[PAIR_VERIFY_MSG03].nonce, NONCE_LENGTH - 4);

  encrypted_data_len = data_len + sizeof(tag); // Space for ciphered payload and authtag
  encrypted_data = malloc(encrypted_data_len);

  ret = encrypt_chacha(encrypted_data, data, data_len, derived_key, sizeof(derived_key), NULL, 0, tag, sizeof(tag), nonce);
  if (ret < 0)
    {
      vctx->errmsg = "Verify request 2: Could not encrypt";
      goto error;
    }

  memcpy(encrypted_data + data_len, tag, sizeof(tag));

  tlv_add_value(request, TLVType_State, &pair_keys_map[PAIR_VERIFY_MSG03].state, sizeof(pair_keys_map[PAIR_VERIFY_MSG03].state));
  tlv_add_value(request, TLVType_EncryptedData, encrypted_data, encrypted_data_len);

  data_len = REQUEST_BUFSIZE; // Re-using *data, so pass original length to tlv_format
  ret = tlv_format(request, data, &data_len);
  if (ret < 0)
    {
      vctx->errmsg = "Verify request 2: tlv_format returned an error";
      goto error;
    }

  *len = data_len;

  free(encrypted_data);
  tlv_free(request);
  return data;

 error:
  free(encrypted_data);
  tlv_free(request);
  free(data);
  return NULL;
}

static int
pair_verify_response1(struct pair_verify_context *vctx, const uint8_t *data, uint32_t data_len)
{
  tlv_values_t *response;
  tlv_t *encrypted_data;
  tlv_t *public_key;
  uint8_t nonce[NONCE_LENGTH] = { 0 };
  uint8_t tag[AUTHTAG_LENGTH];
  uint8_t derived_key[32];
  size_t encrypted_len;
  uint8_t *decrypted_data = NULL;
  int ret;

  response = response_process(data, data_len, &vctx->errmsg);
  if (!response)
    {
      vctx->errmsg = "Verify response 1: Could not parse TLV";
      return -1;
    }

  encrypted_data = tlv_get_value(response, TLVType_EncryptedData);
  if (!encrypted_data)
    {
      vctx->errmsg = "Verify response 1: Missing encrypted_data";
      goto error;
    }

  public_key = tlv_get_value(response, TLVType_PublicKey);
  if (!public_key || public_key->size != sizeof(vctx->server_eph_public_key))
    {
      vctx->errmsg = "Verify response 1: Missing or invalid public_key";
      goto error;
    }

  memcpy(vctx->server_eph_public_key, public_key->value, sizeof(vctx->server_eph_public_key));
  ret = crypto_scalarmult(vctx->shared_secret, vctx->client_eph_private_key, vctx->server_eph_public_key);
  if (ret < 0)
    {
      vctx->errmsg = "Verify response 1: Curve 25519 returned an error";
      goto error;
    }

  ret = hkdf_extract_expand(derived_key, sizeof(derived_key), vctx->shared_secret, sizeof(vctx->shared_secret), PAIR_VERIFY_MSG02);
  if (ret < 0)
    {
      vctx->errmsg = "Verify response 1: hkdf error getting derived_key";
      goto error;
    }

  // encrypted_data->value consists of the encrypted payload + the auth tag
  if (encrypted_data->size < AUTHTAG_LENGTH)
    {
      vctx->errmsg = "Verify response 1: Invalid encrypted data";
      goto error;
    }

  encrypted_len = encrypted_data->size - AUTHTAG_LENGTH;
  memcpy(tag, encrypted_data->value + encrypted_len, AUTHTAG_LENGTH);
  memcpy(nonce + 4, pair_keys_map[PAIR_VERIFY_MSG02].nonce, NONCE_LENGTH - 4);

  decrypted_data = malloc(encrypted_len);

  ret = decrypt_chacha(decrypted_data, encrypted_data->value, encrypted_len, derived_key, sizeof(derived_key), NULL, 0, tag, sizeof(tag), nonce);
  if (ret < 0)
    {
      vctx->errmsg = "Verify response 1: Decryption error";
      goto error;
    }

  tlv_free(response);
  response = response_process(decrypted_data, encrypted_len, &vctx->errmsg);
  if (!response)
    {
      vctx->errmsg = "Verify response 1: Could not parse decrypted TLV";
      goto error;
    }

  // TODO check identifier and signature

  free(decrypted_data);
  tlv_free(response);
  return 0;

 error:
  free(decrypted_data);
  tlv_free(response);
  return -1;
}

static int
pair_verify_response2(struct pair_verify_context *vctx, const uint8_t *data, uint32_t data_len)
{
  // TODO actually check response
  return 0;
}

static void
pair_cipher_free(struct pair_cipher_context *cctx)
{
  if (!cctx)
    return;

  free(cctx);
}

static struct pair_cipher_context *
pair_cipher_new(int type, const uint8_t shared_secret[32])
{
  struct pair_cipher_context *cctx;
  int ret;

  cctx = calloc(1, sizeof(struct pair_cipher_context));
  if (!cctx)
    goto error;

  cctx->type = type;

  ret = hkdf_extract_expand(cctx->encryption_key, sizeof(cctx->encryption_key), shared_secret, 32, PAIR_CONTROL_WRITE);
  if (ret < 0)
    goto error;

  ret = hkdf_extract_expand(cctx->decryption_key, sizeof(cctx->decryption_key), shared_secret, 32, PAIR_CONTROL_READ);
  if (ret < 0)
    goto error;

  return cctx;

 error:
  pair_cipher_free(cctx);
  return NULL;
}

static int
pair_encrypt(uint8_t **ciphertext, size_t *ciphertext_len, uint8_t *plaintext, size_t plaintext_len, struct pair_cipher_context *cctx)
{
  uint8_t nonce[NONCE_LENGTH] = { 0 };
  uint8_t tag[AUTHTAG_LENGTH];
  uint8_t *plain_block;
  uint8_t *cipher_block;
  uint16_t block_len;
  int nblocks;
  int ret;
  int i;

  if (plaintext_len == 0 || !plaintext)
    return -1;

  // Encryption is done in blocks, where each block consists of a short, the
  // encrypted data and an auth tag. The short is the size of the encrypted
  // data. The encrypted data in the block cannot exceed ENCRYPTED_LEN_MAX.
  nblocks = 1 + ((plaintext_len - 1) / ENCRYPTED_LEN_MAX); // Ceiling of division

  *ciphertext_len = nblocks * (sizeof(block_len) + AUTHTAG_LENGTH) + plaintext_len;
  *ciphertext = malloc(*ciphertext_len);

  for (i = 0, plain_block = plaintext, cipher_block = *ciphertext; i < nblocks; i++)
    {
      // If it is the last block we will encrypt only the remaining data
      block_len = (i + 1 == nblocks) ? (plaintext + plaintext_len - plain_block) : ENCRYPTED_LEN_MAX;

      memcpy(nonce + 4, &(cctx->encryption_counter), sizeof(cctx->encryption_counter));// TODO BE or LE?

      // Write the ciphered block
      memcpy(cipher_block, &block_len, sizeof(block_len)); // TODO BE or LE?
      ret = encrypt_chacha(cipher_block + sizeof(block_len), plain_block, block_len, cctx->encryption_key, sizeof(cctx->encryption_key), &block_len, sizeof(block_len), tag, sizeof(tag), nonce);
      if (ret < 0)
	{
	  cctx->errmsg = "Encryption with chacha poly1305 failed";
	  free(*ciphertext);
	  return -1;
	}
      memcpy(cipher_block + sizeof(block_len) + block_len, tag, AUTHTAG_LENGTH);

      plain_block += block_len;
      cipher_block += block_len + sizeof(block_len) + AUTHTAG_LENGTH;
      cctx->encryption_counter++;
    }

  assert(plain_block == plaintext + plaintext_len);
  assert(cipher_block == *ciphertext + *ciphertext_len);

#ifdef DEBUG_PAIR
  hexdump("Encrypted:\n", *ciphertext, *ciphertext_len);
#endif

  return 0;
}

static int
pair_decrypt(uint8_t **plaintext, size_t *plaintext_len, uint8_t *ciphertext, size_t ciphertext_len, struct pair_cipher_context *cctx)
{
  uint8_t nonce[NONCE_LENGTH] = { 0 };
  uint8_t tag[AUTHTAG_LENGTH];
  uint8_t *plain_block;
  uint8_t *cipher_block;
  uint16_t block_len;
  int ret;

  if (ciphertext_len < sizeof(block_len) || !ciphertext)
    return -1;

  // This will allocate more than we need. Since we don't know the number of
  // blocks in the ciphertext yet we can't calculate the exact required length.
  *plaintext = malloc(ciphertext_len);

  for (plain_block = *plaintext, cipher_block = ciphertext; cipher_block < ciphertext + ciphertext_len; )
    {
      memcpy(&block_len, cipher_block, sizeof(block_len)); // TODO BE or LE?
      if (cipher_block + block_len + sizeof(block_len) + AUTHTAG_LENGTH > ciphertext + ciphertext_len)
	{
	  cctx->errmsg = "Corrupt block length in encrypted data";
	  free(*plaintext);
	  return -1; // Corrupt block_len, stop before we read over the end
	}

      memcpy(tag, cipher_block + sizeof(block_len) + block_len, sizeof(tag));
      memcpy(nonce + 4, &(cctx->decryption_counter), sizeof(cctx->decryption_counter));// TODO BE or LE?

      ret = decrypt_chacha(plain_block, cipher_block + sizeof(block_len), block_len, cctx->decryption_key, sizeof(cctx->decryption_key), &block_len, sizeof(block_len), tag, sizeof(tag), nonce);
      if (ret < 0)
	{
	  cctx->errmsg = "Decryption with chacha poly1305 failed";
	  free(*plaintext);
	  return -1;
	}

      plain_block += block_len;
      cipher_block += block_len + sizeof(block_len) + AUTHTAG_LENGTH;
      cctx->decryption_counter++;
    }

  assert(plain_block < *plaintext + ciphertext_len);
  assert(cipher_block == ciphertext + ciphertext_len);

  *plaintext_len = plain_block - *plaintext;

#ifdef DEBUG_PAIR
  hexdump("Decrypted:\n", *plaintext, *plaintext_len);
#endif

  return 0;
}

struct pair_definition pair_homekit =
{
  .pair_setup_new = pair_setup_new,
  .pair_setup_free = pair_setup_free,

  .pair_setup_request1 = pair_setup_request1,
  .pair_setup_request2 = pair_setup_request2,
  .pair_setup_request3 = pair_setup_request3,

  .pair_setup_response1 = pair_setup_response1,
  .pair_setup_response2 = pair_setup_response2,
  .pair_setup_response3 = pair_setup_response3,

  .pair_verify_request1 = pair_verify_request1,
  .pair_verify_request2 = pair_verify_request2,

  .pair_verify_response1 = pair_verify_response1,
  .pair_verify_response2 = pair_verify_response2,

  .pair_cipher_new = pair_cipher_new,
  .pair_cipher_free = pair_cipher_free,

  .pair_encrypt = pair_encrypt,
  .pair_decrypt = pair_decrypt,
};
