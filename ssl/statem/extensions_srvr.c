/*
 * Copyright 2016-2025 The OpenSSL Project Authors. All Rights Reserved.
 *
 * Licensed under the Apache License 2.0 (the "License").  You may not use
 * this file except in compliance with the License.  You can obtain a copy
 * in the file LICENSE in the source distribution or at
 * https://www.openssl.org/source/license.html
 */

#include <openssl/ocsp.h>
#include "../ssl_local.h"
#include "statem_local.h"
#include "internal/cryptlib.h"
#include "internal/ssl_unwrap.h"

#define COOKIE_STATE_FORMAT_VERSION     1

/*
 * 2 bytes for packet length, 2 bytes for format version, 2 bytes for
 * protocol version, 2 bytes for group id, 2 bytes for cipher id, 1 byte for
 * key_share present flag, 8 bytes for timestamp, 2 bytes for the hashlen,
 * EVP_MAX_MD_SIZE for transcript hash, 1 byte for app cookie length, app cookie
 * length bytes, SHA256_DIGEST_LENGTH bytes for the HMAC of the whole thing.
 */
#define MAX_COOKIE_SIZE (2 + 2 + 2 + 2 + 2 + 1 + 8 + 2 + EVP_MAX_MD_SIZE + 1 \
                         + SSL_COOKIE_LENGTH + SHA256_DIGEST_LENGTH)

/*
 * Message header + 2 bytes for protocol version + number of random bytes +
 * + 1 byte for legacy session id length + number of bytes in legacy session id
 * + 2 bytes for ciphersuite + 1 byte for legacy compression
 * + 2 bytes for extension block length + 6 bytes for key_share extension
 * + 4 bytes for cookie extension header + the number of bytes in the cookie
 */
#define MAX_HRR_SIZE    (SSL3_HM_HEADER_LENGTH + 2 + SSL3_RANDOM_SIZE + 1 \
                         + SSL_MAX_SSL_SESSION_ID_LENGTH + 2 + 1 + 2 + 6 + 4 \
                         + MAX_COOKIE_SIZE)

/*
 * Parse the client's renegotiation binding and abort if it's not right
 */
int tls_parse_ctos_renegotiate(SSL_CONNECTION *s, PACKET *pkt,
                               unsigned int context,
                               X509 *x, size_t chainidx)
{
    unsigned int ilen;
    const unsigned char *data;
    int ok;

    /* Parse the length byte */
    if (!PACKET_get_1(pkt, &ilen)
        || !PACKET_get_bytes(pkt, &data, ilen)) {
        SSLfatal(s, SSL_AD_DECODE_ERROR, SSL_R_RENEGOTIATION_ENCODING_ERR);
        return 0;
    }

    /* Check that the extension matches */
    if (ilen != s->s3.previous_client_finished_len) {
        SSLfatal(s, SSL_AD_HANDSHAKE_FAILURE, SSL_R_RENEGOTIATION_MISMATCH);
        return 0;
    }

    ok = memcmp(data, s->s3.previous_client_finished,
                    s->s3.previous_client_finished_len);
#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
    if (ok) {
        if ((data[0] ^ s->s3.previous_client_finished[0]) != 0xFF) {
            ok = 0;
        }
    }
#endif
    if (ok) {
        SSLfatal(s, SSL_AD_HANDSHAKE_FAILURE, SSL_R_RENEGOTIATION_MISMATCH);
        return 0;
    }

    s->s3.send_connection_binding = 1;

    return 1;
}

/*-
 * The servername extension is treated as follows:
 *
 * - Only the hostname type is supported with a maximum length of 255.
 * - The servername is rejected if too long or if it contains zeros,
 *   in which case an fatal alert is generated.
 * - The servername field is maintained together with the session cache.
 * - When a session is resumed, the servername call back invoked in order
 *   to allow the application to position itself to the right context.
 * - The servername is acknowledged if it is new for a session or when
 *   it is identical to a previously used for the same session.
 *   Applications can control the behaviour.  They can at any time
 *   set a 'desirable' servername for a new SSL object. This can be the
 *   case for example with HTTPS when a Host: header field is received and
 *   a renegotiation is requested. In this case, a possible servername
 *   presented in the new client hello is only acknowledged if it matches
 *   the value of the Host: field.
 * - Applications must  use SSL_OP_NO_SESSION_RESUMPTION_ON_RENEGOTIATION
 *   if they provide for changing an explicit servername context for the
 *   session, i.e. when the session has been established with a servername
 *   extension.
 * - On session reconnect, the servername extension may be absent.
 */
int tls_parse_ctos_server_name(SSL_CONNECTION *s, PACKET *pkt,
                               unsigned int context, X509 *x, size_t chainidx)
{
    unsigned int servname_type;
    PACKET sni, hostname;

    if (!PACKET_as_length_prefixed_2(pkt, &sni)
        /* ServerNameList must be at least 1 byte long. */
        || PACKET_remaining(&sni) == 0) {
        SSLfatal(s, SSL_AD_DECODE_ERROR, SSL_R_BAD_EXTENSION);
        return 0;
    }

    /*
     * Although the intent was for server_name to be extensible, RFC 4366
     * was not clear about it; and so OpenSSL among other implementations,
     * always and only allows a 'host_name' name types.
     * RFC 6066 corrected the mistake but adding new name types
     * is nevertheless no longer feasible, so act as if no other
     * SNI types can exist, to simplify parsing.
     *
     * Also note that the RFC permits only one SNI value per type,
     * i.e., we can only have a single hostname.
     */
    if (!PACKET_get_1(&sni, &servname_type)
        || servname_type != TLSEXT_NAMETYPE_host_name
        || !PACKET_as_length_prefixed_2(&sni, &hostname)) {
        SSLfatal(s, SSL_AD_DECODE_ERROR, SSL_R_BAD_EXTENSION);
        return 0;
    }

    /*
     * In TLSv1.2 and below the SNI is associated with the session. In TLSv1.3
     * we always use the SNI value from the handshake.
     */
    if (!s->hit || SSL_CONNECTION_IS_TLS13(s)) {
        if (PACKET_remaining(&hostname) > TLSEXT_MAXLEN_host_name) {
            SSLfatal(s, SSL_AD_UNRECOGNIZED_NAME, SSL_R_BAD_EXTENSION);
            return 0;
        }

        if (PACKET_contains_zero_byte(&hostname)) {
            SSLfatal(s, SSL_AD_UNRECOGNIZED_NAME, SSL_R_BAD_EXTENSION);
            return 0;
        }

        /*
         * Store the requested SNI in the SSL as temporary storage.
         * If we accept it, it will get stored in the SSL_SESSION as well.
         */
        OPENSSL_free(s->ext.hostname);
        s->ext.hostname = NULL;
        if (!PACKET_strndup(&hostname, &s->ext.hostname)) {
            SSLfatal(s, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
            return 0;
        }

        s->servername_done = 1;
    } else {
        /*
         * In TLSv1.2 and below we should check if the SNI is consistent between
         * the initial handshake and the resumption. In TLSv1.3 SNI is not
         * associated with the session.
         */
        s->servername_done = (s->session->ext.hostname != NULL)
            && PACKET_equal(&hostname, s->session->ext.hostname,
                            strlen(s->session->ext.hostname));
    }

    return 1;
}

int tls_parse_ctos_maxfragmentlen(SSL_CONNECTION *s, PACKET *pkt,
                                  unsigned int context,
                                  X509 *x, size_t chainidx)
{
    unsigned int value;

    if (PACKET_remaining(pkt) != 1 || !PACKET_get_1(pkt, &value)) {
        SSLfatal(s, SSL_AD_DECODE_ERROR, SSL_R_BAD_EXTENSION);
        return 0;
    }

    /* Received |value| should be a valid max-fragment-length code. */
    if (!IS_MAX_FRAGMENT_LENGTH_EXT_VALID(value)) {
        SSLfatal(s, SSL_AD_ILLEGAL_PARAMETER,
                 SSL_R_SSL3_EXT_INVALID_MAX_FRAGMENT_LENGTH);
        return 0;
    }

    /*
     * When doing a full handshake or a renegotiation max_fragment_len_mode will
     * be TLSEXT_max_fragment_length_UNSPECIFIED
     *
     * In case of a resumption max_fragment_len_mode will be one of
     *      TLSEXT_max_fragment_length_DISABLED, TLSEXT_max_fragment_length_512,
     *      TLSEXT_max_fragment_length_1024, TLSEXT_max_fragment_length_2048.
     *      TLSEXT_max_fragment_length_4096
     *
     * RFC 6066: The negotiated length applies for the duration of the session
     * including session resumptions.
     *
     * So we only set the value in case it is unspecified.
     */
    if (s->session->ext.max_fragment_len_mode == TLSEXT_max_fragment_length_UNSPECIFIED)
        /*
         * Store it in session, so it'll become binding for us
         * and we'll include it in a next Server Hello.
         */
        s->session->ext.max_fragment_len_mode = value;

    return 1;
}

#ifndef OPENSSL_NO_SRP
int tls_parse_ctos_srp(SSL_CONNECTION *s, PACKET *pkt, unsigned int context,
                       X509 *x, size_t chainidx)
{
    PACKET srp_I;

    if (!PACKET_as_length_prefixed_1(pkt, &srp_I)
            || PACKET_contains_zero_byte(&srp_I)) {
        SSLfatal(s, SSL_AD_DECODE_ERROR, SSL_R_BAD_EXTENSION);
        return 0;
    }

    if (!PACKET_strndup(&srp_I, &s->srp_ctx.login)) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
        return 0;
    }

    return 1;
}
#endif

int tls_parse_ctos_ec_pt_formats(SSL_CONNECTION *s, PACKET *pkt,
                                 unsigned int context,
                                 X509 *x, size_t chainidx)
{
    PACKET ec_point_format_list;

    if (!PACKET_as_length_prefixed_1(pkt, &ec_point_format_list)
        || PACKET_remaining(&ec_point_format_list) == 0) {
        SSLfatal(s, SSL_AD_DECODE_ERROR, SSL_R_BAD_EXTENSION);
        return 0;
    }

    if (!s->hit) {
        if (!PACKET_memdup(&ec_point_format_list,
                           &s->ext.peer_ecpointformats,
                           &s->ext.peer_ecpointformats_len)) {
            SSLfatal(s, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
            return 0;
        }
    }

    return 1;
}

int tls_parse_ctos_session_ticket(SSL_CONNECTION *s, PACKET *pkt,
                                  unsigned int context,
                                  X509 *x, size_t chainidx)
{
    if (s->ext.session_ticket_cb &&
            !s->ext.session_ticket_cb(SSL_CONNECTION_GET_USER_SSL(s),
                                      PACKET_data(pkt), (int)PACKET_remaining(pkt),
                                      s->ext.session_ticket_cb_arg)) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
        return 0;
    }

    return 1;
}

int tls_parse_ctos_sig_algs_cert(SSL_CONNECTION *s, PACKET *pkt,
                                 ossl_unused unsigned int context,
                                 ossl_unused X509 *x,
                                 ossl_unused size_t chainidx)
{
    PACKET supported_sig_algs;

    if (!PACKET_as_length_prefixed_2(pkt, &supported_sig_algs)
            || PACKET_remaining(&supported_sig_algs) == 0) {
        SSLfatal(s, SSL_AD_DECODE_ERROR, SSL_R_BAD_EXTENSION);
        return 0;
    }

    /*
     * We use this routine on both clients and servers, and when clients
     * get asked for PHA we need to always save the sigalgs regardless
     * of whether it was a resumption or not.
     */
    if ((!s->server || (s->server && !s->hit))
            && !tls1_save_sigalgs(s, &supported_sig_algs, 1)) {
        SSLfatal(s, SSL_AD_DECODE_ERROR, SSL_R_BAD_EXTENSION);
        return 0;
    }

    return 1;
}

int tls_parse_ctos_sig_algs(SSL_CONNECTION *s, PACKET *pkt,
                            unsigned int context, X509 *x, size_t chainidx)
{
    PACKET supported_sig_algs;

    if (!PACKET_as_length_prefixed_2(pkt, &supported_sig_algs)
            || PACKET_remaining(&supported_sig_algs) == 0) {
        SSLfatal(s, SSL_AD_DECODE_ERROR, SSL_R_BAD_EXTENSION);
        return 0;
    }

    /*
     * We use this routine on both clients and servers, and when clients
     * get asked for PHA we need to always save the sigalgs regardless
     * of whether it was a resumption or not.
     */
    if ((!s->server || (s->server && !s->hit))
            && !tls1_save_sigalgs(s, &supported_sig_algs, 0)) {
        SSLfatal(s, SSL_AD_DECODE_ERROR, SSL_R_BAD_EXTENSION);
        return 0;
    }

    return 1;
}

#ifndef OPENSSL_NO_OCSP
int tls_parse_ctos_status_request(SSL_CONNECTION *s, PACKET *pkt,
                                  unsigned int context,
                                  X509 *x, size_t chainidx)
{
    PACKET responder_id_list, exts;

    /* We ignore this in a resumption handshake */
    if (s->hit)
        return 1;

    /* Not defined if we get one of these in a client Certificate */
    if (x != NULL)
        return 1;

    if (!PACKET_get_1(pkt, (unsigned int *)&s->ext.status_type)) {
        SSLfatal(s, SSL_AD_DECODE_ERROR, SSL_R_BAD_EXTENSION);
        return 0;
    }

    if (s->ext.status_type != TLSEXT_STATUSTYPE_ocsp) {
        /*
         * We don't know what to do with any other type so ignore it.
         */
        s->ext.status_type = TLSEXT_STATUSTYPE_nothing;
        return 1;
    }

    if (!PACKET_get_length_prefixed_2 (pkt, &responder_id_list)) {
        SSLfatal(s, SSL_AD_DECODE_ERROR, SSL_R_BAD_EXTENSION);
        return 0;
    }

    /*
     * We remove any OCSP_RESPIDs from a previous handshake
     * to prevent unbounded memory growth - CVE-2016-6304
     */
    sk_OCSP_RESPID_pop_free(s->ext.ocsp.ids, OCSP_RESPID_free);
    if (PACKET_remaining(&responder_id_list) > 0) {
        s->ext.ocsp.ids = sk_OCSP_RESPID_new_null();
        if (s->ext.ocsp.ids == NULL) {
            SSLfatal(s, SSL_AD_INTERNAL_ERROR, ERR_R_CRYPTO_LIB);
            return 0;
        }
    } else {
        s->ext.ocsp.ids = NULL;
    }

    while (PACKET_remaining(&responder_id_list) > 0) {
        OCSP_RESPID *id;
        PACKET responder_id;
        const unsigned char *id_data;

        if (!PACKET_get_length_prefixed_2(&responder_id_list, &responder_id)
                || PACKET_remaining(&responder_id) == 0) {
            SSLfatal(s, SSL_AD_DECODE_ERROR, SSL_R_BAD_EXTENSION);
            return 0;
        }

        id_data = PACKET_data(&responder_id);
        id = d2i_OCSP_RESPID(NULL, &id_data,
                             (int)PACKET_remaining(&responder_id));
        if (id == NULL) {
            SSLfatal(s, SSL_AD_DECODE_ERROR, SSL_R_BAD_EXTENSION);
            return 0;
        }

        if (id_data != PACKET_end(&responder_id)) {
            OCSP_RESPID_free(id);
            SSLfatal(s, SSL_AD_DECODE_ERROR, SSL_R_BAD_EXTENSION);

            return 0;
        }

        if (!sk_OCSP_RESPID_push(s->ext.ocsp.ids, id)) {
            OCSP_RESPID_free(id);
            SSLfatal(s, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);

            return 0;
        }
    }

    /* Read in request_extensions */
    if (!PACKET_as_length_prefixed_2(pkt, &exts)) {
        SSLfatal(s, SSL_AD_DECODE_ERROR, SSL_R_BAD_EXTENSION);
        return 0;
    }

    if (PACKET_remaining(&exts) > 0) {
        const unsigned char *ext_data = PACKET_data(&exts);

        sk_X509_EXTENSION_pop_free(s->ext.ocsp.exts,
                                   X509_EXTENSION_free);
        s->ext.ocsp.exts =
            d2i_X509_EXTENSIONS(NULL, &ext_data, (int)PACKET_remaining(&exts));
        if (s->ext.ocsp.exts == NULL || ext_data != PACKET_end(&exts)) {
            SSLfatal(s, SSL_AD_DECODE_ERROR, SSL_R_BAD_EXTENSION);
            return 0;
        }
    }

    return 1;
}
#endif

#ifndef OPENSSL_NO_NEXTPROTONEG
int tls_parse_ctos_npn(SSL_CONNECTION *s, PACKET *pkt, unsigned int context,
                       X509 *x, size_t chainidx)
{
    /*
     * We shouldn't accept this extension on a
     * renegotiation.
     */
    if (SSL_IS_FIRST_HANDSHAKE(s))
        s->s3.npn_seen = 1;

    return 1;
}
#endif

/*
 * Save the ALPN extension in a ClientHello.|pkt| holds the contents of the ALPN
 * extension, not including type and length. Returns: 1 on success, 0 on error.
 */
int tls_parse_ctos_alpn(SSL_CONNECTION *s, PACKET *pkt, unsigned int context,
                        X509 *x, size_t chainidx)
{
    PACKET protocol_list, save_protocol_list, protocol;

    if (!SSL_IS_FIRST_HANDSHAKE(s))
        return 1;

    if (!PACKET_as_length_prefixed_2(pkt, &protocol_list)
        || PACKET_remaining(&protocol_list) < 2) {
        SSLfatal(s, SSL_AD_DECODE_ERROR, SSL_R_BAD_EXTENSION);
        return 0;
    }

    save_protocol_list = protocol_list;
    do {
        /* Protocol names can't be empty. */
        if (!PACKET_get_length_prefixed_1(&protocol_list, &protocol)
                || PACKET_remaining(&protocol) == 0) {
            SSLfatal(s, SSL_AD_DECODE_ERROR, SSL_R_BAD_EXTENSION);
            return 0;
        }
    } while (PACKET_remaining(&protocol_list) != 0);

    OPENSSL_free(s->s3.alpn_proposed);
    s->s3.alpn_proposed = NULL;
    s->s3.alpn_proposed_len = 0;
    if (!PACKET_memdup(&save_protocol_list,
                       &s->s3.alpn_proposed, &s->s3.alpn_proposed_len)) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
        return 0;
    }

    return 1;
}

#ifndef OPENSSL_NO_SRTP
int tls_parse_ctos_use_srtp(SSL_CONNECTION *s, PACKET *pkt,
                            unsigned int context, X509 *x, size_t chainidx)
{
    STACK_OF(SRTP_PROTECTION_PROFILE) *srvr;
    unsigned int ct, mki_len, id;
    int i, srtp_pref;
    PACKET subpkt;
    SSL *ssl = SSL_CONNECTION_GET_SSL(s);

    /* Ignore this if we have no SRTP profiles */
    if (SSL_get_srtp_profiles(ssl) == NULL)
        return 1;

    /* Pull off the length of the cipher suite list  and check it is even */
    if (!PACKET_get_net_2(pkt, &ct) || (ct & 1) != 0
            || !PACKET_get_sub_packet(pkt, &subpkt, ct)) {
        SSLfatal(s, SSL_AD_DECODE_ERROR,
               SSL_R_BAD_SRTP_PROTECTION_PROFILE_LIST);
        return 0;
    }

    srvr = SSL_get_srtp_profiles(ssl);
    s->srtp_profile = NULL;
    /* Search all profiles for a match initially */
    srtp_pref = sk_SRTP_PROTECTION_PROFILE_num(srvr);

    while (PACKET_remaining(&subpkt)) {
        if (!PACKET_get_net_2(&subpkt, &id)) {
            SSLfatal(s, SSL_AD_DECODE_ERROR,
                     SSL_R_BAD_SRTP_PROTECTION_PROFILE_LIST);
            return 0;
        }

        /*
         * Only look for match in profiles of higher preference than
         * current match.
         * If no profiles have been have been configured then this
         * does nothing.
         */
        for (i = 0; i < srtp_pref; i++) {
            SRTP_PROTECTION_PROFILE *sprof =
                sk_SRTP_PROTECTION_PROFILE_value(srvr, i);

            if (sprof->id == id) {
                s->srtp_profile = sprof;
                srtp_pref = i;
                break;
            }
        }
    }

    /* Now extract the MKI value as a sanity check, but discard it for now */
    if (!PACKET_get_1(pkt, &mki_len)) {
        SSLfatal(s, SSL_AD_DECODE_ERROR,
                 SSL_R_BAD_SRTP_PROTECTION_PROFILE_LIST);
        return 0;
    }

    if (!PACKET_forward(pkt, mki_len)
        || PACKET_remaining(pkt)) {
        SSLfatal(s, SSL_AD_DECODE_ERROR, SSL_R_BAD_SRTP_MKI_VALUE);
        return 0;
    }

    return 1;
}
#endif

int tls_parse_ctos_etm(SSL_CONNECTION *s, PACKET *pkt, unsigned int context,
                       X509 *x, size_t chainidx)
{
    if (!(s->options & SSL_OP_NO_ENCRYPT_THEN_MAC))
        s->ext.use_etm = 1;

    return 1;
}

/*
 * Process a psk_kex_modes extension received in the ClientHello. |pkt| contains
 * the raw PACKET data for the extension. Returns 1 on success or 0 on failure.
 */
int tls_parse_ctos_psk_kex_modes(SSL_CONNECTION *s, PACKET *pkt,
                                 unsigned int context,
                                 X509 *x, size_t chainidx)
{
#ifndef OPENSSL_NO_TLS1_3
    PACKET psk_kex_modes;
    unsigned int mode;

    if (!PACKET_as_length_prefixed_1(pkt, &psk_kex_modes)
            || PACKET_remaining(&psk_kex_modes) == 0) {
        SSLfatal(s, SSL_AD_DECODE_ERROR, SSL_R_BAD_EXTENSION);
        return 0;
    }

    while (PACKET_get_1(&psk_kex_modes, &mode)) {
        if (mode == TLSEXT_KEX_MODE_KE_DHE)
            s->ext.psk_kex_mode |= TLSEXT_KEX_MODE_FLAG_KE_DHE;
        else if (mode == TLSEXT_KEX_MODE_KE
                && (s->options & SSL_OP_ALLOW_NO_DHE_KEX) != 0)
            s->ext.psk_kex_mode |= TLSEXT_KEX_MODE_FLAG_KE;
    }

    if (((s->ext.psk_kex_mode & TLSEXT_KEX_MODE_FLAG_KE) != 0)
            && (s->options & SSL_OP_PREFER_NO_DHE_KEX) != 0) {

        /*
         * If NO_DHE is supported and preferred, then we only remember this
         * mode. DHE PSK will not be used for sure, because in any case where
         * it would be supported (i.e. if a key share is present), NO_DHE would
         * be supported as well. As the latter is preferred it would be
         * chosen. By removing DHE PSK here, we don't have to deal with the
         * SSL_OP_PREFER_NO_DHE_KEX option in any other place.
         */
        s->ext.psk_kex_mode = TLSEXT_KEX_MODE_FLAG_KE;
    }

#endif

    return 1;
}

/*
 * Use function tls_parse_ctos_key_share with helper functions extract_keyshares,
 * check_overlap and tls_accept_ksgroup to parse the key_share extension(s)
 * received in the ClientHello and to select the group used of the key exchange
 */

#ifndef OPENSSL_NO_TLS1_3
/*
 * Accept a key share group by setting the related variables in s->s3 and
 * by generating a pubkey for this group
 */
static int tls_accept_ksgroup(SSL_CONNECTION *s, uint16_t ksgroup, PACKET *encoded_pubkey)
{
    /* Accept the key share group */
    s->s3.group_id = ksgroup;
    s->s3.group_id_candidate = ksgroup;
    /* Cache the selected group ID in the SSL_SESSION */
    s->session->kex_group = ksgroup;
    if ((s->s3.peer_tmp = ssl_generate_param_group(s, ksgroup)) == NULL) {
        SSLfatal(s,
                 SSL_AD_INTERNAL_ERROR,
                 SSL_R_UNABLE_TO_FIND_ECDH_PARAMETERS);
        return 0;
    }
    if (tls13_set_encoded_pub_key(s->s3.peer_tmp,
                                  PACKET_data(encoded_pubkey),
                                  PACKET_remaining(encoded_pubkey)) <= 0) {
        SSLfatal(s, SSL_AD_ILLEGAL_PARAMETER, SSL_R_BAD_ECPOINT);
        return 0;
    }
    return 1;
}

# define GROUPLIST_INCREMENT 32 /* Memory allocation chunk size (nominally 64 Bytes chunks) */

typedef enum KS_EXTRACTION_RESULT {
    EXTRACTION_FAILURE,
    EXTRACTION_SUCCESS,
    EXTRACTION_SUCCESS_HRR
} KS_EXTRACTION_RESULT;

static KS_EXTRACTION_RESULT extract_keyshares(SSL_CONNECTION *s, PACKET *key_share_list,
                                              const uint16_t *clntgroups, size_t clnt_num_groups,
                                              const uint16_t *srvrgroups, size_t srvr_num_groups,
                                              uint16_t **keyshares_arr, PACKET **encoded_pubkey_arr,
                                              size_t *keyshares_cnt, size_t *keyshares_max)
{
    PACKET encoded_pubkey;
    size_t key_share_pos = 0;
    size_t previous_key_share_pos = 0;
    unsigned int group_id = 0;

    /* Prepare memory to hold the extracted key share groups and related pubkeys */
    *keyshares_arr = OPENSSL_malloc(*keyshares_max * sizeof(**keyshares_arr));
    if (*keyshares_arr == NULL) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
        goto failure;
    }
    *encoded_pubkey_arr = OPENSSL_malloc(*keyshares_max * sizeof(**encoded_pubkey_arr));
    if (*encoded_pubkey_arr == NULL) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
        goto failure;
    }

    while (PACKET_remaining(key_share_list) > 0) {
        /* Get the group_id for the current share and its encoded_pubkey */
        if (!PACKET_get_net_2(key_share_list, &group_id)
                || !PACKET_get_length_prefixed_2(key_share_list, &encoded_pubkey)
                || PACKET_remaining(&encoded_pubkey) == 0) {
            SSLfatal(s, SSL_AD_DECODE_ERROR, SSL_R_LENGTH_MISMATCH);
            goto failure;
        }

        /*
         * If we sent an HRR then the key_share sent back MUST be for the group
         * we requested, and must be the only key_share sent.
         */
        if (s->s3.group_id != 0
                && (group_id != s->s3.group_id
                    || PACKET_remaining(key_share_list) != 0)) {
            SSLfatal(s, SSL_AD_ILLEGAL_PARAMETER, SSL_R_BAD_KEY_SHARE);
            goto failure;
        }

        /*
         * Check if this share is in supported_groups sent from client
         * RFC 8446 also mandates that clients send keyshares in the same
         * order as listed in the supported groups extension, but its not
         * required that the server check that, and some clients violate this
         * so instead of failing the connection when that occurs, log a trace
         * message indicating the client discrepancy.
         */
        if (!check_in_list(s, group_id, clntgroups, clnt_num_groups, 0, &key_share_pos)) {
            SSLfatal(s, SSL_AD_ILLEGAL_PARAMETER, SSL_R_BAD_KEY_SHARE);
            goto failure;
        }

        if (key_share_pos < previous_key_share_pos)
            OSSL_TRACE1(TLS, "key share group id %d is out of RFC 8446 order\n", group_id);

        previous_key_share_pos = key_share_pos;

        if (s->s3.group_id != 0) {
            /*
             * We have sent a HRR, and the key share we got back is
             * the one we expected and is the only key share and is
             * in the list of supported_groups (checked
             * above already), hence we accept this key share group
             */
            if (!tls_accept_ksgroup(s, s->s3.group_id, &encoded_pubkey))
                goto failure; /* SSLfatal already called */
            /* We have selected a key share group via HRR, hence we're done here */
            return EXTRACTION_SUCCESS_HRR;
        }

        /*
         * We tolerate but ignore a group id that we don't think is
         * suitable for TLSv1.3 or which is not supported by the server
         */
        if (!check_in_list(s, group_id, srvrgroups, srvr_num_groups, 1, NULL)
                || !tls_group_allowed(s, group_id, SSL_SECOP_CURVE_SUPPORTED)
                || !tls_valid_group(s, group_id, TLS1_3_VERSION, TLS1_3_VERSION,
                                    0, NULL)) {
            /* Share not suitable or not supported, check next share */
            continue;
        }

        /* Memorize this key share group ID and its encoded point */
        (*keyshares_arr)[*keyshares_cnt] = group_id;
        (*encoded_pubkey_arr)[(*keyshares_cnt)++] = encoded_pubkey;

        /*
         * Memory management (remark: While limiting the client to only allow
         * a maximum of OPENSSL_CLIENT_MAX_KEY_SHARES to be sent, the server can
         * handle any number of key shares)
         */
        if (*keyshares_cnt == *keyshares_max) {
            PACKET *tmp_pkt;
            uint16_t *tmp =
                OPENSSL_realloc(*keyshares_arr,
                                (*keyshares_max + GROUPLIST_INCREMENT) * sizeof(**keyshares_arr));

            if (tmp == NULL)
                goto failure;
            *keyshares_arr = tmp;
            tmp_pkt =
                OPENSSL_realloc(*encoded_pubkey_arr,
                                (*keyshares_max + GROUPLIST_INCREMENT) *
                                sizeof(**encoded_pubkey_arr));
            if (tmp_pkt == NULL)
                goto failure;
            *encoded_pubkey_arr = tmp_pkt;
            *keyshares_max += GROUPLIST_INCREMENT;
        }

    }

    return EXTRACTION_SUCCESS;

failure:
    /* Fatal error -> free any allocated memory and return 0 */
    OPENSSL_free(*keyshares_arr);
    OPENSSL_free(*encoded_pubkey_arr);
    return EXTRACTION_FAILURE;
}
#endif

/*
 * For each group in the priority list of groups, check if that group is
 * also present in the secondary list; if so, select the first overlap and
 * assign to selected_group and also set the related index in the candidate group list,
 * or set selected_group to 0 if no overlap
 */
#ifndef OPENSSL_NO_TLS1_3
static void check_overlap(SSL_CONNECTION *s,
                          const uint16_t *prio_groups, size_t prio_num_groups,
                          const uint16_t *candidate_groups, size_t candidate_num_groups,
                          int *prio_group_idx, int *candidate_group_idx,
                          uint16_t *selected_group)
{
    uint16_t current_group;
    size_t group_idx = prio_num_groups;
    size_t new_group_idx = 0;

    *candidate_group_idx = 0;
    *prio_group_idx = 0;
    *selected_group = 0;

    for (current_group = 0; current_group < candidate_num_groups; current_group++) {
        if (!check_in_list(s, candidate_groups[current_group], prio_groups,
                           prio_num_groups, 1, &new_group_idx)
            || !tls_group_allowed(s, candidate_groups[current_group],
                                  SSL_SECOP_CURVE_SUPPORTED)
            || !tls_valid_group(s, candidate_groups[current_group], TLS1_3_VERSION,
                                TLS1_3_VERSION, 0, NULL))
            /* No overlap or group not suitable, check next group */
            continue;

        /*
         * is the found new_group_idx earlier in the priority list than
         * initial or last group_idx?
         */
        if (new_group_idx < group_idx) {
            group_idx = new_group_idx;
            *candidate_group_idx = current_group;
            *prio_group_idx = (int)group_idx;
            *selected_group = prio_groups[group_idx];
        }
    }
}
#endif

int tls_parse_ctos_key_share(SSL_CONNECTION *s, PACKET *pkt,
                             unsigned int context, X509 *x, size_t chainidx)
{
#ifndef OPENSSL_NO_TLS1_3
    PACKET key_share_list;
    const uint16_t *clntgroups, *srvrgroups;
    const size_t *srvrtuples;
    uint16_t *first_group_in_tuple;
    size_t clnt_num_groups, srvr_num_groups, srvr_num_tuples;
    PACKET *encoded_pubkey_arr = NULL;
    uint16_t *keyshares_arr = NULL;
    size_t keyshares_cnt = 0;
    size_t keyshares_max = GROUPLIST_INCREMENT;
    /* We conservatively assume that we did not find a suitable group */
    uint16_t group_id_candidate = 0;
    KS_EXTRACTION_RESULT ks_extraction_result;
    size_t current_tuple;
    int ret = 0;

    s->s3.group_id_candidate = 0;
    if (s->hit && (s->ext.psk_kex_mode & TLSEXT_KEX_MODE_FLAG_KE_DHE) == 0)
        return 1;

    /* Sanity check */
    if (s->s3.peer_tmp != NULL) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
        return 0;
    }

    if (!PACKET_as_length_prefixed_2(pkt, &key_share_list)) {
        SSLfatal(s, SSL_AD_DECODE_ERROR, SSL_R_LENGTH_MISMATCH);
        return 0;
    }

    /* Get list of server supported groups and the group tuples */
    tls1_get_supported_groups(s, &srvrgroups, &srvr_num_groups);
    tls1_get_group_tuples(s, &srvrtuples, &srvr_num_tuples);
    /* Get the clients list of supported groups. */
    tls1_get_peer_groups(s, &clntgroups, &clnt_num_groups);

    if (clnt_num_groups == 0) {
        /*
         * This can only happen if the supported_groups extension was not sent,
         * because we verify that the length is non-zero when we process that
         * extension.
         */
        SSLfatal(s, SSL_AD_MISSING_EXTENSION,
                 SSL_R_MISSING_SUPPORTED_GROUPS_EXTENSION);
        return 0;
    }

    if (s->s3.group_id != 0 && PACKET_remaining(&key_share_list) == 0) {
        /*
         * If we set a group_id already, then we must have sent an HRR
         * requesting a new key_share. If we haven't got one then that is an
         * error
         */
        SSLfatal(s, SSL_AD_ILLEGAL_PARAMETER, SSL_R_BAD_KEY_SHARE);
        return 0;
    }

    /* We parse the key share extension and memorize the entries (after some checks) */
    ks_extraction_result = extract_keyshares(s,
                                             &key_share_list,
                                             clntgroups, clnt_num_groups,
                                             srvrgroups, srvr_num_groups,
                                             &keyshares_arr, &encoded_pubkey_arr,
                                             &keyshares_cnt, &keyshares_max);

    if (ks_extraction_result == EXTRACTION_FAILURE) /* Fatal error during tests */
        return 0; /* Memory already freed and SSLfatal already called */
    if (ks_extraction_result == EXTRACTION_SUCCESS_HRR) /* Successful HRR */
        goto end;

    /*
     * We now have the folowing lists available to make a decision for
     * which group the server should use for key exchange :
     * From client: clntgroups[clnt_num_groups],
     *              keyshares_arr[keyshares_cnt], encoded_pubkey_arr[keyshares_cnt]
     * From server: srvrgroups[srvr_num_groups], srvrtuples[srvr_num_tuples]
     *
     * Group selection algorithm:
     *    For all tuples do:
     *      key share group(s) overlapping with current tuple?
     *         --> Yes: accept group_id for SH
     *        --> No: is any of the client supported_groups overlapping with current tuple?
     *            --> Yes: memorize group_id for HRR, break
     *             --> No: continue to check next tuple
     *
     * Remark: Selection priority different for client- or server-preference
     */
    first_group_in_tuple = (uint16_t *)srvrgroups;
    for (current_tuple = 0; current_tuple < srvr_num_tuples; current_tuple++) {
        size_t number_of_groups_in_tuple = srvrtuples[current_tuple];
        int prio_group_idx = 0, candidate_group_idx = 0;

        /* Server or client preference ? */
        if (s->options & SSL_OP_SERVER_PREFERENCE) {
            /* Server preference */
            /* Is there overlap with a key share group?  */
            check_overlap(s,
                          first_group_in_tuple, number_of_groups_in_tuple,
                          keyshares_arr, keyshares_cnt,
                          &prio_group_idx, &candidate_group_idx,
                          &group_id_candidate);
            if (group_id_candidate > 0) { /* Overlap found -> accept the key share group */
                if (!tls_accept_ksgroup(s, group_id_candidate,
                                        &encoded_pubkey_arr[candidate_group_idx]))
                    goto err; /* SSLfatal already called */
                /* We have all info for a SH, hence we're done here */
                goto end;
            } else {
                /*
                 * There's no overlap with a key share, but is there at least a client
                 * supported_group overlapping with the current tuple?
                 */
                check_overlap(s,
                              first_group_in_tuple, number_of_groups_in_tuple,
                              clntgroups, clnt_num_groups,
                              &prio_group_idx, &candidate_group_idx,
                              &group_id_candidate);
                if (group_id_candidate > 0) {
                    /*
                     * We did not have a key share overlap, but at least the supported
                     * groups overlap hence we can stop searching
                     * (and report group_id_candidate 'upward' for HRR)
                     */
                    s->s3.group_id_candidate = group_id_candidate;
                    goto end;
                } else {
                    /*
                     * Neither key share nor supported_groups overlap current
                     * tuple, hence we try the next tuple
                     */
                    first_group_in_tuple = &first_group_in_tuple[number_of_groups_in_tuple];
                    continue;
                }
            }

        } else { /* We have client preference */
            check_overlap(s,
                          keyshares_arr, keyshares_cnt,
                          first_group_in_tuple, number_of_groups_in_tuple,
                          &prio_group_idx, &candidate_group_idx,
                          &group_id_candidate);
            if (group_id_candidate > 0) {
                if (!tls_accept_ksgroup(s, group_id_candidate, &encoded_pubkey_arr[prio_group_idx]))
                    goto err;
                goto end;
            } else {
                check_overlap(s,
                              clntgroups, clnt_num_groups,
                              first_group_in_tuple, number_of_groups_in_tuple,
                              &prio_group_idx, &candidate_group_idx,
                              &group_id_candidate);
                if (group_id_candidate > 0) {
                    s->s3.group_id_candidate = group_id_candidate;
                    goto end;
                } else {
                    first_group_in_tuple = &first_group_in_tuple[number_of_groups_in_tuple];
                    continue;
                }
            }
        }
    }

end:
    ret = 1;

err:
    OPENSSL_free(keyshares_arr);
    OPENSSL_free(encoded_pubkey_arr);
    return ret;

#endif

    return 1;
}

int tls_parse_ctos_cookie(SSL_CONNECTION *s, PACKET *pkt, unsigned int context,
                          X509 *x, size_t chainidx)
{
#ifndef OPENSSL_NO_TLS1_3
    unsigned int format, version, key_share, group_id;
    EVP_MD_CTX *hctx;
    EVP_PKEY *pkey;
    PACKET cookie, raw, chhash, appcookie;
    WPACKET hrrpkt;
    const unsigned char *data, *mdin, *ciphdata;
    unsigned char hmac[SHA256_DIGEST_LENGTH];
    unsigned char hrr[MAX_HRR_SIZE];
    size_t rawlen, hmaclen, hrrlen, ciphlen;
    uint64_t tm, now;
    SSL *ssl = SSL_CONNECTION_GET_SSL(s);
    SSL_CTX *sctx = SSL_CONNECTION_GET_CTX(s);

    /* Ignore any cookie if we're not set up to verify it */
    if (sctx->verify_stateless_cookie_cb == NULL
            || (s->s3.flags & TLS1_FLAGS_STATELESS) == 0)
        return 1;

    if (!PACKET_as_length_prefixed_2(pkt, &cookie)) {
        SSLfatal(s, SSL_AD_DECODE_ERROR, SSL_R_LENGTH_MISMATCH);
        return 0;
    }

    raw = cookie;
    data = PACKET_data(&raw);
    rawlen = PACKET_remaining(&raw);
    if (rawlen < SHA256_DIGEST_LENGTH
            || !PACKET_forward(&raw, rawlen - SHA256_DIGEST_LENGTH)) {
        SSLfatal(s, SSL_AD_DECODE_ERROR, SSL_R_LENGTH_MISMATCH);
        return 0;
    }
    mdin = PACKET_data(&raw);

    /* Verify the HMAC of the cookie */
    hctx = EVP_MD_CTX_create();
    pkey = EVP_PKEY_new_raw_private_key_ex(sctx->libctx, "HMAC",
                                           sctx->propq,
                                           s->session_ctx->ext.cookie_hmac_key,
                                           sizeof(s->session_ctx->ext.cookie_hmac_key));
    if (hctx == NULL || pkey == NULL) {
        EVP_MD_CTX_free(hctx);
        EVP_PKEY_free(pkey);
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, ERR_R_EVP_LIB);
        return 0;
    }

    hmaclen = SHA256_DIGEST_LENGTH;
    if (EVP_DigestSignInit_ex(hctx, NULL, "SHA2-256", sctx->libctx,
                              sctx->propq, pkey, NULL) <= 0
            || EVP_DigestSign(hctx, hmac, &hmaclen, data,
                              rawlen - SHA256_DIGEST_LENGTH) <= 0
            || hmaclen != SHA256_DIGEST_LENGTH) {
        EVP_MD_CTX_free(hctx);
        EVP_PKEY_free(pkey);
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
        return 0;
    }

    EVP_MD_CTX_free(hctx);
    EVP_PKEY_free(pkey);

    if (CRYPTO_memcmp(hmac, mdin, SHA256_DIGEST_LENGTH) != 0) {
        SSLfatal(s, SSL_AD_ILLEGAL_PARAMETER, SSL_R_COOKIE_MISMATCH);
        return 0;
    }

    if (!PACKET_get_net_2(&cookie, &format)) {
        SSLfatal(s, SSL_AD_DECODE_ERROR, SSL_R_LENGTH_MISMATCH);
        return 0;
    }
    /* Check the cookie format is something we recognise. Ignore it if not */
    if (format != COOKIE_STATE_FORMAT_VERSION)
        return 1;

    /*
     * The rest of these checks really shouldn't fail since we have verified the
     * HMAC above.
     */

    /* Check the version number is sane */
    if (!PACKET_get_net_2(&cookie, &version)) {
        SSLfatal(s, SSL_AD_DECODE_ERROR, SSL_R_LENGTH_MISMATCH);
        return 0;
    }
    if (version != TLS1_3_VERSION) {
        SSLfatal(s, SSL_AD_ILLEGAL_PARAMETER,
                 SSL_R_BAD_PROTOCOL_VERSION_NUMBER);
        return 0;
    }

    if (!PACKET_get_net_2(&cookie, &group_id)) {
        SSLfatal(s, SSL_AD_DECODE_ERROR, SSL_R_LENGTH_MISMATCH);
        return 0;
    }

    ciphdata = PACKET_data(&cookie);
    if (!PACKET_forward(&cookie, 2)) {
        SSLfatal(s, SSL_AD_DECODE_ERROR, SSL_R_LENGTH_MISMATCH);
        return 0;
    }
    if (group_id != s->s3.group_id
            || s->s3.tmp.new_cipher
               != ssl_get_cipher_by_char(s, ciphdata, 0)) {
        /*
         * We chose a different cipher or group id this time around to what is
         * in the cookie. Something must have changed.
         */
        SSLfatal(s, SSL_AD_ILLEGAL_PARAMETER, SSL_R_BAD_CIPHER);
        return 0;
    }

    if (!PACKET_get_1(&cookie, &key_share)
            || !PACKET_get_net_8(&cookie, &tm)
            || !PACKET_get_length_prefixed_2(&cookie, &chhash)
            || !PACKET_get_length_prefixed_1(&cookie, &appcookie)
            || PACKET_remaining(&cookie) != SHA256_DIGEST_LENGTH) {
        SSLfatal(s, SSL_AD_DECODE_ERROR, SSL_R_LENGTH_MISMATCH);
        return 0;
    }

    /* We tolerate a cookie age of up to 10 minutes (= 60 * 10 seconds) */
    now = time(NULL);
    if (tm > now || (now - tm) > 600) {
        /* Cookie is stale. Ignore it */
        return 1;
    }

    /* Verify the app cookie */
    if (sctx->verify_stateless_cookie_cb(SSL_CONNECTION_GET_USER_SSL(s),
                                         PACKET_data(&appcookie),
                                         PACKET_remaining(&appcookie)) == 0) {
        SSLfatal(s, SSL_AD_ILLEGAL_PARAMETER, SSL_R_COOKIE_MISMATCH);
        return 0;
    }

    /*
     * Reconstruct the HRR that we would have sent in response to the original
     * ClientHello so we can add it to the transcript hash.
     * Note: This won't work with custom HRR extensions
     */
    if (!WPACKET_init_static_len(&hrrpkt, hrr, sizeof(hrr), 0)) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
        return 0;
    }
    if (!WPACKET_put_bytes_u8(&hrrpkt, SSL3_MT_SERVER_HELLO)
            || !WPACKET_start_sub_packet_u24(&hrrpkt)
            || !WPACKET_put_bytes_u16(&hrrpkt, TLS1_2_VERSION)
            || !WPACKET_memcpy(&hrrpkt, hrrrandom, SSL3_RANDOM_SIZE)
            || !WPACKET_sub_memcpy_u8(&hrrpkt, s->tmp_session_id,
                                      s->tmp_session_id_len)
            || !ssl->method->put_cipher_by_char(s->s3.tmp.new_cipher, &hrrpkt,
                                                &ciphlen)
            || !WPACKET_put_bytes_u8(&hrrpkt, 0)
            || !WPACKET_start_sub_packet_u16(&hrrpkt)) {
        WPACKET_cleanup(&hrrpkt);
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
        return 0;
    }
    if (!WPACKET_put_bytes_u16(&hrrpkt, TLSEXT_TYPE_supported_versions)
            || !WPACKET_start_sub_packet_u16(&hrrpkt)
            || !WPACKET_put_bytes_u16(&hrrpkt, s->version)
            || !WPACKET_close(&hrrpkt)) {
        WPACKET_cleanup(&hrrpkt);
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
        return 0;
    }
    if (key_share) {
        if (!WPACKET_put_bytes_u16(&hrrpkt, TLSEXT_TYPE_key_share)
                || !WPACKET_start_sub_packet_u16(&hrrpkt)
                || !WPACKET_put_bytes_u16(&hrrpkt, s->s3.group_id)
                || !WPACKET_close(&hrrpkt)) {
            WPACKET_cleanup(&hrrpkt);
            SSLfatal(s, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
            return 0;
        }
    }
    if (!WPACKET_put_bytes_u16(&hrrpkt, TLSEXT_TYPE_cookie)
            || !WPACKET_start_sub_packet_u16(&hrrpkt)
            || !WPACKET_sub_memcpy_u16(&hrrpkt, data, rawlen)
            || !WPACKET_close(&hrrpkt) /* cookie extension */
            || !WPACKET_close(&hrrpkt) /* extension block */
            || !WPACKET_close(&hrrpkt) /* message */
            || !WPACKET_get_total_written(&hrrpkt, &hrrlen)
            || !WPACKET_finish(&hrrpkt)) {
        WPACKET_cleanup(&hrrpkt);
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
        return 0;
    }

    /* Reconstruct the transcript hash */
    if (!create_synthetic_message_hash(s, PACKET_data(&chhash),
                                       PACKET_remaining(&chhash), hrr,
                                       hrrlen)) {
        /* SSLfatal() already called */
        return 0;
    }

    /* Act as if this ClientHello came after a HelloRetryRequest */
    s->hello_retry_request = SSL_HRR_PENDING;

    s->ext.cookieok = 1;
#endif

    return 1;
}

int tls_parse_ctos_supported_groups(SSL_CONNECTION *s, PACKET *pkt,
                                    unsigned int context,
                                    X509 *x, size_t chainidx)
{
    PACKET supported_groups_list;

    /* Each group is 2 bytes and we must have at least 1. */
    if (!PACKET_as_length_prefixed_2(pkt, &supported_groups_list)
            || PACKET_remaining(&supported_groups_list) == 0
            || (PACKET_remaining(&supported_groups_list) % 2) != 0) {
        SSLfatal(s, SSL_AD_DECODE_ERROR, SSL_R_BAD_EXTENSION);
        return 0;
    }

    if (!s->hit || SSL_CONNECTION_IS_TLS13(s)) {
        OPENSSL_free(s->ext.peer_supportedgroups);
        s->ext.peer_supportedgroups = NULL;
        s->ext.peer_supportedgroups_len = 0;
        if (!tls1_save_u16(&supported_groups_list,
                           &s->ext.peer_supportedgroups,
                           &s->ext.peer_supportedgroups_len)) {
            SSLfatal(s, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
            return 0;
        }
    }

    return 1;
}

int tls_parse_ctos_ems(SSL_CONNECTION *s, PACKET *pkt, unsigned int context,
                       X509 *x, size_t chainidx)
{
    /* The extension must always be empty */
    if (PACKET_remaining(pkt) != 0) {
        SSLfatal(s, SSL_AD_DECODE_ERROR, SSL_R_BAD_EXTENSION);
        return 0;
    }

    if (s->options & SSL_OP_NO_EXTENDED_MASTER_SECRET)
        return 1;

    s->s3.flags |= TLS1_FLAGS_RECEIVED_EXTMS;

    return 1;
}


int tls_parse_ctos_early_data(SSL_CONNECTION *s, PACKET *pkt, unsigned int context,
                              X509 *x, size_t chainidx)
{
    if (PACKET_remaining(pkt) != 0) {
        SSLfatal(s, SSL_AD_DECODE_ERROR, SSL_R_BAD_EXTENSION);
        return 0;
    }

    if (s->hello_retry_request != SSL_HRR_NONE) {
        SSLfatal(s, SSL_AD_ILLEGAL_PARAMETER, SSL_R_BAD_EXTENSION);
        return 0;
    }

    return 1;
}

static SSL_TICKET_STATUS tls_get_stateful_ticket(SSL_CONNECTION *s, PACKET *tick,
                                                 SSL_SESSION **sess)
{
    SSL_SESSION *tmpsess = NULL;

    s->ext.ticket_expected = 1;

    switch (PACKET_remaining(tick)) {
        case 0:
            return SSL_TICKET_EMPTY;

        case SSL_MAX_SSL_SESSION_ID_LENGTH:
            break;

        default:
            return SSL_TICKET_NO_DECRYPT;
    }

    tmpsess = lookup_sess_in_cache(s, PACKET_data(tick),
                                   SSL_MAX_SSL_SESSION_ID_LENGTH);

    if (tmpsess == NULL)
        return SSL_TICKET_NO_DECRYPT;

    *sess = tmpsess;
    return SSL_TICKET_SUCCESS;
}

int tls_parse_ctos_psk(SSL_CONNECTION *s, PACKET *pkt, unsigned int context,
                       X509 *x, size_t chainidx)
{
    PACKET identities, binders, binder;
    size_t binderoffset;
    int hashsize;
    SSL_SESSION *sess = NULL;
    unsigned int id, i, ext = 0;
    const EVP_MD *md = NULL;
    SSL_CTX *sctx = SSL_CONNECTION_GET_CTX(s);
    SSL *ussl = SSL_CONNECTION_GET_USER_SSL(s);

    /*
     * If we have no PSK kex mode that we recognise then we can't resume so
     * ignore this extension
     */
    if ((s->ext.psk_kex_mode
            & (TLSEXT_KEX_MODE_FLAG_KE | TLSEXT_KEX_MODE_FLAG_KE_DHE)) == 0)
        return 1;

    if (!PACKET_get_length_prefixed_2(pkt, &identities)) {
        SSLfatal(s, SSL_AD_DECODE_ERROR, SSL_R_BAD_EXTENSION);
        return 0;
    }

    s->ext.ticket_expected = 0;
    for (id = 0; PACKET_remaining(&identities) != 0; id++) {
        PACKET identity;
        unsigned long ticket_agel;
        size_t idlen;

        if (!PACKET_get_length_prefixed_2(&identities, &identity)
                || !PACKET_get_net_4(&identities, &ticket_agel)) {
            SSLfatal(s, SSL_AD_DECODE_ERROR, SSL_R_BAD_EXTENSION);
            return 0;
        }

        idlen = PACKET_remaining(&identity);
        if (s->psk_find_session_cb != NULL
                && !s->psk_find_session_cb(ussl, PACKET_data(&identity), idlen,
                                           &sess)) {
            SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_R_BAD_EXTENSION);
            return 0;
        }

#ifndef OPENSSL_NO_PSK
        if (sess == NULL
                && s->psk_server_callback != NULL
                && idlen <= PSK_MAX_IDENTITY_LEN) {
            char *pskid = NULL;
            unsigned char pskdata[PSK_MAX_PSK_LEN];
            unsigned int pskdatalen;

            if (!PACKET_strndup(&identity, &pskid)) {
                SSLfatal(s, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
                return 0;
            }
            pskdatalen = s->psk_server_callback(ussl, pskid, pskdata,
                                                sizeof(pskdata));
            OPENSSL_free(pskid);
            if (pskdatalen > PSK_MAX_PSK_LEN) {
                SSLfatal(s, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
                return 0;
            } else if (pskdatalen > 0) {
                const SSL_CIPHER *cipher;
                const unsigned char tls13_aes128gcmsha256_id[] = { 0x13, 0x01 };

                /*
                 * We found a PSK using an old style callback. We don't know
                 * the digest so we default to SHA256 as per the TLSv1.3 spec
                 */
                cipher = SSL_CIPHER_find(SSL_CONNECTION_GET_SSL(s),
                                         tls13_aes128gcmsha256_id);
                if (cipher == NULL) {
                    OPENSSL_cleanse(pskdata, pskdatalen);
                    SSLfatal(s, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
                    return 0;
                }

                sess = SSL_SESSION_new();
                if (sess == NULL
                        || !SSL_SESSION_set1_master_key(sess, pskdata,
                                                        pskdatalen)
                        || !SSL_SESSION_set_cipher(sess, cipher)
                        || !SSL_SESSION_set_protocol_version(sess,
                                                             TLS1_3_VERSION)) {
                    OPENSSL_cleanse(pskdata, pskdatalen);
                    SSLfatal(s, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
                    goto err;
                }
                OPENSSL_cleanse(pskdata, pskdatalen);
            }
        }
#endif /* OPENSSL_NO_PSK */

        if (sess != NULL) {
            /* We found a PSK */
            SSL_SESSION *sesstmp = ssl_session_dup(sess, 0);

            if (sesstmp == NULL) {
                SSLfatal(s, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
                goto err;
            }
            SSL_SESSION_free(sess);
            sess = sesstmp;

            /*
             * We've just been told to use this session for this context so
             * make sure the sid_ctx matches up.
             */
            memcpy(sess->sid_ctx, s->sid_ctx, s->sid_ctx_length);
            sess->sid_ctx_length = s->sid_ctx_length;
            ext = 1;
            if (id == 0)
                s->ext.early_data_ok = 1;
            s->ext.ticket_expected = 1;
        } else {
            OSSL_TIME t, age, expire;
            int ret;

            /*
             * If we are using anti-replay protection then we behave as if
             * SSL_OP_NO_TICKET is set - we are caching tickets anyway so there
             * is no point in using full stateless tickets.
             */
            if ((s->options & SSL_OP_NO_TICKET) != 0
                    || (s->max_early_data > 0
                        && (s->options & SSL_OP_NO_ANTI_REPLAY) == 0))
                ret = tls_get_stateful_ticket(s, &identity, &sess);
            else
                ret = tls_decrypt_ticket(s, PACKET_data(&identity),
                                         PACKET_remaining(&identity), NULL, 0,
                                         &sess);

            if (ret == SSL_TICKET_EMPTY) {
                SSLfatal(s, SSL_AD_DECODE_ERROR, SSL_R_BAD_EXTENSION);
                return 0;
            }

            if (ret == SSL_TICKET_FATAL_ERR_MALLOC
                    || ret == SSL_TICKET_FATAL_ERR_OTHER) {
                SSLfatal(s, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
                return 0;
            }
            if (ret == SSL_TICKET_NONE || ret == SSL_TICKET_NO_DECRYPT)
                continue;

            /* Check for replay */
            if (s->max_early_data > 0
                    && (s->options & SSL_OP_NO_ANTI_REPLAY) == 0
                    && !SSL_CTX_remove_session(s->session_ctx, sess)) {
                SSL_SESSION_free(sess);
                sess = NULL;
                continue;
            }

            age = ossl_time_subtract(ossl_ms2time(ticket_agel),
                                     ossl_ms2time(sess->ext.tick_age_add));
            t = ossl_time_subtract(ossl_time_now(), sess->time);

            /*
             * Although internally we use OSS_TIME which has ns granularity,
             * when SSL_SESSION structures are serialised/deserialised we use
             * second granularity for the sess->time field. Therefore it could
             * appear that the client's ticket age is longer than ours (our
             * ticket age calculation should always be slightly longer than the
             * client's due to the network latency). Therefore we add 1000ms to
             * our age calculation to adjust for rounding errors.
             */
            expire = ossl_time_add(t, ossl_ms2time(1000));

            if (id == 0
                    && ossl_time_compare(sess->timeout, t) >= 0
                    && ossl_time_compare(age, expire) <= 0
                    && ossl_time_compare(ossl_time_add(age, TICKET_AGE_ALLOWANCE),
                                         expire) >= 0) {
                /*
                 * Ticket age is within tolerance and not expired. We allow it
                 * for early data
                 */
                s->ext.early_data_ok = 1;
            }
        }

        md = ssl_md(sctx, sess->cipher->algorithm2);
        if (md == NULL) {
            SSLfatal(s, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
            goto err;
        }
        if (!EVP_MD_is_a(md,
                EVP_MD_get0_name(ssl_md(sctx,
                                        s->s3.tmp.new_cipher->algorithm2)))) {
            /* The ciphersuite is not compatible with this session. */
            SSL_SESSION_free(sess);
            sess = NULL;
            s->ext.early_data_ok = 0;
            s->ext.ticket_expected = 0;
            continue;
        }
        break;
    }

    if (sess == NULL)
        return 1;

    binderoffset = PACKET_data(pkt) - (const unsigned char *)s->init_buf->data;
    hashsize = EVP_MD_get_size(md);
    if (hashsize <= 0)
        goto err;

    if (!PACKET_get_length_prefixed_2(pkt, &binders)) {
        SSLfatal(s, SSL_AD_DECODE_ERROR, SSL_R_BAD_EXTENSION);
        goto err;
    }

    for (i = 0; i <= id; i++) {
        if (!PACKET_get_length_prefixed_1(&binders, &binder)) {
            SSLfatal(s, SSL_AD_DECODE_ERROR, SSL_R_BAD_EXTENSION);
            goto err;
        }
    }

    if (PACKET_remaining(&binder) != (size_t)hashsize) {
        SSLfatal(s, SSL_AD_DECODE_ERROR, SSL_R_BAD_EXTENSION);
        goto err;
    }
    if (tls_psk_do_binder(s, md, (const unsigned char *)s->init_buf->data,
                          binderoffset, PACKET_data(&binder), NULL, sess, 0,
                          ext) != 1) {
        /* SSLfatal() already called */
        goto err;
    }

    s->ext.tick_identity = id;

    SSL_SESSION_free(s->session);
    s->session = sess;
    return 1;
err:
    SSL_SESSION_free(sess);
    return 0;
}

int tls_parse_ctos_post_handshake_auth(SSL_CONNECTION *s, PACKET *pkt,
                                       ossl_unused unsigned int context,
                                       ossl_unused X509 *x,
                                       ossl_unused size_t chainidx)
{
    if (PACKET_remaining(pkt) != 0) {
        SSLfatal(s, SSL_AD_DECODE_ERROR,
                 SSL_R_POST_HANDSHAKE_AUTH_ENCODING_ERR);
        return 0;
    }

    s->post_handshake_auth = SSL_PHA_EXT_RECEIVED;

    return 1;
}

/*
 * Add the server's renegotiation binding
 */
EXT_RETURN tls_construct_stoc_renegotiate(SSL_CONNECTION *s, WPACKET *pkt,
                                          unsigned int context, X509 *x,
                                          size_t chainidx)
{
    if (!s->s3.send_connection_binding)
        return EXT_RETURN_NOT_SENT;

    /* Still add this even if SSL_OP_NO_RENEGOTIATION is set */
    if (!WPACKET_put_bytes_u16(pkt, TLSEXT_TYPE_renegotiate)
            || !WPACKET_start_sub_packet_u16(pkt)
            || !WPACKET_start_sub_packet_u8(pkt)
            || !WPACKET_memcpy(pkt, s->s3.previous_client_finished,
                               s->s3.previous_client_finished_len)
            || !WPACKET_memcpy(pkt, s->s3.previous_server_finished,
                               s->s3.previous_server_finished_len)
            || !WPACKET_close(pkt)
            || !WPACKET_close(pkt)) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
        return EXT_RETURN_FAIL;
    }

    return EXT_RETURN_SENT;
}

EXT_RETURN tls_construct_stoc_server_name(SSL_CONNECTION *s, WPACKET *pkt,
                                          unsigned int context, X509 *x,
                                          size_t chainidx)
{
    if (s->servername_done != 1)
        return EXT_RETURN_NOT_SENT;

    /*
     * Prior to TLSv1.3 we ignore any SNI in the current handshake if resuming.
     * We just use the servername from the initial handshake.
     */
    if (s->hit && !SSL_CONNECTION_IS_TLS13(s))
        return EXT_RETURN_NOT_SENT;

    if (!WPACKET_put_bytes_u16(pkt, TLSEXT_TYPE_server_name)
            || !WPACKET_put_bytes_u16(pkt, 0)) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
        return EXT_RETURN_FAIL;
    }

    return EXT_RETURN_SENT;
}

/* Add/include the server's max fragment len extension into ServerHello */
EXT_RETURN tls_construct_stoc_maxfragmentlen(SSL_CONNECTION *s, WPACKET *pkt,
                                             unsigned int context, X509 *x,
                                             size_t chainidx)
{
    if (!USE_MAX_FRAGMENT_LENGTH_EXT(s->session))
        return EXT_RETURN_NOT_SENT;

    /*-
     * 4 bytes for this extension type and extension length
     * 1 byte for the Max Fragment Length code value.
     */
    if (!WPACKET_put_bytes_u16(pkt, TLSEXT_TYPE_max_fragment_length)
        || !WPACKET_start_sub_packet_u16(pkt)
        || !WPACKET_put_bytes_u8(pkt, s->session->ext.max_fragment_len_mode)
        || !WPACKET_close(pkt)) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
        return EXT_RETURN_FAIL;
    }

    return EXT_RETURN_SENT;
}

EXT_RETURN tls_construct_stoc_ec_pt_formats(SSL_CONNECTION *s, WPACKET *pkt,
                                            unsigned int context, X509 *x,
                                            size_t chainidx)
{
    unsigned long alg_k = s->s3.tmp.new_cipher->algorithm_mkey;
    unsigned long alg_a = s->s3.tmp.new_cipher->algorithm_auth;
    int using_ecc = ((alg_k & SSL_kECDHE) || (alg_a & SSL_aECDSA))
                    && (s->ext.peer_ecpointformats != NULL);
    const unsigned char *plist;
    size_t plistlen;

    if (!using_ecc)
        return EXT_RETURN_NOT_SENT;

    tls1_get_formatlist(s, &plist, &plistlen);
    if (!WPACKET_put_bytes_u16(pkt, TLSEXT_TYPE_ec_point_formats)
            || !WPACKET_start_sub_packet_u16(pkt)
            || !WPACKET_sub_memcpy_u8(pkt, plist, plistlen)
            || !WPACKET_close(pkt)) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
        return EXT_RETURN_FAIL;
    }

    return EXT_RETURN_SENT;
}

EXT_RETURN tls_construct_stoc_supported_groups(SSL_CONNECTION *s, WPACKET *pkt,
                                               unsigned int context, X509 *x,
                                               size_t chainidx)
{
    const uint16_t *groups;
    size_t numgroups, i, first = 1;
    int version;

    /* s->s3.group_id is non zero if we accepted a key_share */
    if (s->s3.group_id == 0)
        return EXT_RETURN_NOT_SENT;

    /* Get our list of supported groups */
    tls1_get_supported_groups(s, &groups, &numgroups);
    if (numgroups == 0) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
        return EXT_RETURN_FAIL;
    }

    /* Copy group ID if supported */
    version = SSL_version(SSL_CONNECTION_GET_SSL(s));
    for (i = 0; i < numgroups; i++) {
        uint16_t group = groups[i];

        if (tls_valid_group(s, group, version, version, 0, NULL)
                && tls_group_allowed(s, group, SSL_SECOP_CURVE_SUPPORTED)) {
            if (first) {
                /*
                 * Check if the client is already using our preferred group. If
                 * so we don't need to add this extension
                 */
                if (s->s3.group_id == group)
                    return EXT_RETURN_NOT_SENT;

                /* Add extension header */
                if (!WPACKET_put_bytes_u16(pkt, TLSEXT_TYPE_supported_groups)
                           /* Sub-packet for supported_groups extension */
                        || !WPACKET_start_sub_packet_u16(pkt)
                        || !WPACKET_start_sub_packet_u16(pkt)) {
                    SSLfatal(s, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
                    return EXT_RETURN_FAIL;
                }

                first = 0;
            }
            if (!WPACKET_put_bytes_u16(pkt, group)) {
                    SSLfatal(s, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
                    return EXT_RETURN_FAIL;
                }
        }
    }

    if (!WPACKET_close(pkt) || !WPACKET_close(pkt)) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
        return EXT_RETURN_FAIL;
    }

    return EXT_RETURN_SENT;
}

EXT_RETURN tls_construct_stoc_session_ticket(SSL_CONNECTION *s, WPACKET *pkt,
                                             unsigned int context, X509 *x,
                                             size_t chainidx)
{
    if (!s->ext.ticket_expected || !tls_use_ticket(s)) {
        s->ext.ticket_expected = 0;
        return EXT_RETURN_NOT_SENT;
    }

    if (!WPACKET_put_bytes_u16(pkt, TLSEXT_TYPE_session_ticket)
            || !WPACKET_put_bytes_u16(pkt, 0)) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
        return EXT_RETURN_FAIL;
    }

    return EXT_RETURN_SENT;
}

#ifndef OPENSSL_NO_OCSP
EXT_RETURN tls_construct_stoc_status_request(SSL_CONNECTION *s, WPACKET *pkt,
                                             unsigned int context, X509 *x,
                                             size_t chainidx)
{
    /* We don't currently support this extension inside a CertificateRequest */
    if (context == SSL_EXT_TLS1_3_CERTIFICATE_REQUEST)
        return EXT_RETURN_NOT_SENT;

    if (!s->ext.status_expected)
        return EXT_RETURN_NOT_SENT;

    if (!WPACKET_put_bytes_u16(pkt, TLSEXT_TYPE_status_request)
            || !WPACKET_start_sub_packet_u16(pkt)) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
        return EXT_RETURN_FAIL;
    }

    /*
     * In TLSv1.3 we include the certificate status itself. In <= TLSv1.2 we
     * send back an empty extension, with the certificate status appearing as a
     * separate message
     */
    if (SSL_CONNECTION_IS_TLS13(s)
        && !tls_construct_cert_status_body(s, chainidx, pkt)) {
        /* SSLfatal() already called */
        return EXT_RETURN_FAIL;
    }
    if (!WPACKET_close(pkt)) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
        return EXT_RETURN_FAIL;
    }

    return EXT_RETURN_SENT;
}
#endif

#ifndef OPENSSL_NO_NEXTPROTONEG
EXT_RETURN tls_construct_stoc_next_proto_neg(SSL_CONNECTION *s, WPACKET *pkt,
                                             unsigned int context, X509 *x,
                                             size_t chainidx)
{
    const unsigned char *npa;
    unsigned int npalen;
    int ret;
    int npn_seen = s->s3.npn_seen;
    SSL_CTX *sctx = SSL_CONNECTION_GET_CTX(s);

    s->s3.npn_seen = 0;
    if (!npn_seen || sctx->ext.npn_advertised_cb == NULL)
        return EXT_RETURN_NOT_SENT;

    ret = sctx->ext.npn_advertised_cb(SSL_CONNECTION_GET_USER_SSL(s), &npa,
                                      &npalen, sctx->ext.npn_advertised_cb_arg);
    if (ret == SSL_TLSEXT_ERR_OK) {
        if (!WPACKET_put_bytes_u16(pkt, TLSEXT_TYPE_next_proto_neg)
                || !WPACKET_sub_memcpy_u16(pkt, npa, npalen)) {
            SSLfatal(s, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
            return EXT_RETURN_FAIL;
        }
        s->s3.npn_seen = 1;
        return EXT_RETURN_SENT;
    }

    return EXT_RETURN_NOT_SENT;
}
#endif

EXT_RETURN tls_construct_stoc_alpn(SSL_CONNECTION *s, WPACKET *pkt, unsigned int context,
                                   X509 *x, size_t chainidx)
{
    if (s->s3.alpn_selected == NULL)
        return EXT_RETURN_NOT_SENT;

    if (!WPACKET_put_bytes_u16(pkt,
                TLSEXT_TYPE_application_layer_protocol_negotiation)
            || !WPACKET_start_sub_packet_u16(pkt)
            || !WPACKET_start_sub_packet_u16(pkt)
            || !WPACKET_sub_memcpy_u8(pkt, s->s3.alpn_selected,
                                      s->s3.alpn_selected_len)
            || !WPACKET_close(pkt)
            || !WPACKET_close(pkt)) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
        return EXT_RETURN_FAIL;
    }

    return EXT_RETURN_SENT;
}

#ifndef OPENSSL_NO_SRTP
EXT_RETURN tls_construct_stoc_use_srtp(SSL_CONNECTION *s, WPACKET *pkt,
                                       unsigned int context, X509 *x,
                                       size_t chainidx)
{
    if (s->srtp_profile == NULL)
        return EXT_RETURN_NOT_SENT;

    if (!WPACKET_put_bytes_u16(pkt, TLSEXT_TYPE_use_srtp)
            || !WPACKET_start_sub_packet_u16(pkt)
            || !WPACKET_put_bytes_u16(pkt, 2)
            || !WPACKET_put_bytes_u16(pkt, s->srtp_profile->id)
            || !WPACKET_put_bytes_u8(pkt, 0)
            || !WPACKET_close(pkt)) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
        return EXT_RETURN_FAIL;
    }

    return EXT_RETURN_SENT;
}
#endif

EXT_RETURN tls_construct_stoc_etm(SSL_CONNECTION *s, WPACKET *pkt,
                                  unsigned int context,
                                  X509 *x, size_t chainidx)
{
    if (!s->ext.use_etm)
        return EXT_RETURN_NOT_SENT;

    /*
     * Don't use encrypt_then_mac if AEAD or RC4 might want to disable
     * for other cases too.
     */
    if (s->s3.tmp.new_cipher->algorithm_mac == SSL_AEAD
        || s->s3.tmp.new_cipher->algorithm_enc == SSL_RC4
        || s->s3.tmp.new_cipher->algorithm_enc == SSL_eGOST2814789CNT
        || s->s3.tmp.new_cipher->algorithm_enc == SSL_eGOST2814789CNT12
        || s->s3.tmp.new_cipher->algorithm_enc == SSL_MAGMA
        || s->s3.tmp.new_cipher->algorithm_enc == SSL_KUZNYECHIK) {
        s->ext.use_etm = 0;
        return EXT_RETURN_NOT_SENT;
    }

    if (!WPACKET_put_bytes_u16(pkt, TLSEXT_TYPE_encrypt_then_mac)
            || !WPACKET_put_bytes_u16(pkt, 0)) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
        return EXT_RETURN_FAIL;
    }

    return EXT_RETURN_SENT;
}

EXT_RETURN tls_construct_stoc_ems(SSL_CONNECTION *s, WPACKET *pkt,
                                  unsigned int context,
                                  X509 *x, size_t chainidx)
{
    if ((s->s3.flags & TLS1_FLAGS_RECEIVED_EXTMS) == 0)
        return EXT_RETURN_NOT_SENT;

    if (!WPACKET_put_bytes_u16(pkt, TLSEXT_TYPE_extended_master_secret)
            || !WPACKET_put_bytes_u16(pkt, 0)) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
        return EXT_RETURN_FAIL;
    }

    return EXT_RETURN_SENT;
}

EXT_RETURN tls_construct_stoc_supported_versions(SSL_CONNECTION *s, WPACKET *pkt,
                                                 unsigned int context, X509 *x,
                                                 size_t chainidx)
{
    if (!ossl_assert(SSL_CONNECTION_IS_TLS13(s))) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
        return EXT_RETURN_FAIL;
    }

    if (!WPACKET_put_bytes_u16(pkt, TLSEXT_TYPE_supported_versions)
            || !WPACKET_start_sub_packet_u16(pkt)
            || !WPACKET_put_bytes_u16(pkt, s->version)
            || !WPACKET_close(pkt)) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
        return EXT_RETURN_FAIL;
    }

    return EXT_RETURN_SENT;
}

EXT_RETURN tls_construct_stoc_key_share(SSL_CONNECTION *s, WPACKET *pkt,
                                        unsigned int context, X509 *x,
                                        size_t chainidx)
{
#ifndef OPENSSL_NO_TLS1_3
    unsigned char *encoded_pubkey;
    size_t encoded_pubkey_len = 0;
    EVP_PKEY *ckey = s->s3.peer_tmp, *skey = NULL;
    const TLS_GROUP_INFO *ginf = NULL;

    if (s->hello_retry_request == SSL_HRR_PENDING) {
        if (ckey != NULL) {
            /* Original key_share was acceptable so don't ask for another one */
            return EXT_RETURN_NOT_SENT;
        }
        if (!WPACKET_put_bytes_u16(pkt, TLSEXT_TYPE_key_share)
                || !WPACKET_start_sub_packet_u16(pkt)
                || !WPACKET_put_bytes_u16(pkt, s->s3.group_id)
                || !WPACKET_close(pkt)) {
            SSLfatal(s, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
            return EXT_RETURN_FAIL;
        }

        return EXT_RETURN_SENT;
    }

    if (ckey == NULL) {
        /* No key_share received from client - must be resuming */
        if (!s->hit || !tls13_generate_handshake_secret(s, NULL, 0)) {
            SSLfatal(s, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
            return EXT_RETURN_FAIL;
        }
        return EXT_RETURN_NOT_SENT;
    }

    if (s->hit && (s->ext.psk_kex_mode & TLSEXT_KEX_MODE_FLAG_KE_DHE) == 0) {
        /*
         * PSK ('hit') and explicitly not doing DHE. If the client sent the
         * DHE option, we take it by default, except if non-DHE would be
         * preferred by config, but this case would have been handled in
         * tls_parse_ctos_psk_kex_modes().
         */
        return EXT_RETURN_NOT_SENT;
    }

    if (!WPACKET_put_bytes_u16(pkt, TLSEXT_TYPE_key_share)
            || !WPACKET_start_sub_packet_u16(pkt)
            || !WPACKET_put_bytes_u16(pkt, s->s3.group_id)) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
        return EXT_RETURN_FAIL;
    }

    if ((ginf = tls1_group_id_lookup(SSL_CONNECTION_GET_CTX(s),
                                     s->s3.group_id)) == NULL) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
        return EXT_RETURN_FAIL;
    }

    if (!ginf->is_kem) {
        /* Regular KEX */
        skey = ssl_generate_pkey(s, ckey);
        if (skey == NULL) {
            SSLfatal(s, SSL_AD_INTERNAL_ERROR, ERR_R_SSL_LIB);
            return EXT_RETURN_FAIL;
        }

        /* Generate encoding of server key */
        encoded_pubkey_len = EVP_PKEY_get1_encoded_public_key(skey, &encoded_pubkey);
        if (encoded_pubkey_len == 0) {
            SSLfatal(s, SSL_AD_INTERNAL_ERROR, ERR_R_EC_LIB);
            EVP_PKEY_free(skey);
            return EXT_RETURN_FAIL;
        }

        if (!WPACKET_sub_memcpy_u16(pkt, encoded_pubkey, encoded_pubkey_len)
                || !WPACKET_close(pkt)) {
            SSLfatal(s, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
            EVP_PKEY_free(skey);
            OPENSSL_free(encoded_pubkey);
            return EXT_RETURN_FAIL;
        }
        OPENSSL_free(encoded_pubkey);

        /*
         * This causes the crypto state to be updated based on the derived keys
         */
        s->s3.tmp.pkey = skey;
        if (ssl_derive(s, skey, ckey, 1) == 0) {
            /* SSLfatal() already called */
            return EXT_RETURN_FAIL;
        }
    } else {
        /* KEM mode */
        unsigned char *ct = NULL;
        size_t ctlen = 0;

        /*
         * This does not update the crypto state.
         *
         * The generated pms is stored in `s->s3.tmp.pms` to be later used via
         * ssl_gensecret().
         */
        if (ssl_encapsulate(s, ckey, &ct, &ctlen, 0) == 0) {
            /* SSLfatal() already called */
            return EXT_RETURN_FAIL;
        }

        if (ctlen == 0) {
            SSLfatal(s, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
            OPENSSL_free(ct);
            return EXT_RETURN_FAIL;
        }

        if (!WPACKET_sub_memcpy_u16(pkt, ct, ctlen)
                || !WPACKET_close(pkt)) {
            SSLfatal(s, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
            OPENSSL_free(ct);
            return EXT_RETURN_FAIL;
        }
        OPENSSL_free(ct);

        /*
         * This causes the crypto state to be updated based on the generated pms
         */
        if (ssl_gensecret(s, s->s3.tmp.pms, s->s3.tmp.pmslen) == 0) {
            /* SSLfatal() already called */
            return EXT_RETURN_FAIL;
        }
    }
    s->s3.did_kex = 1;
    return EXT_RETURN_SENT;
#else
    return EXT_RETURN_FAIL;
#endif
}

EXT_RETURN tls_construct_stoc_cookie(SSL_CONNECTION *s, WPACKET *pkt,
                                     unsigned int context,
                                     X509 *x, size_t chainidx)
{
#ifndef OPENSSL_NO_TLS1_3
    unsigned char *hashval1, *hashval2, *appcookie1, *appcookie2, *cookie;
    unsigned char *hmac, *hmac2;
    size_t startlen, ciphlen, totcookielen, hashlen, hmaclen, appcookielen;
    EVP_MD_CTX *hctx;
    EVP_PKEY *pkey;
    int ret = EXT_RETURN_FAIL;
    SSL_CTX *sctx = SSL_CONNECTION_GET_CTX(s);
    SSL *ssl = SSL_CONNECTION_GET_SSL(s);
    SSL *ussl = SSL_CONNECTION_GET_USER_SSL(s);

    if ((s->s3.flags & TLS1_FLAGS_STATELESS) == 0)
        return EXT_RETURN_NOT_SENT;

    if (sctx->gen_stateless_cookie_cb == NULL) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_R_NO_COOKIE_CALLBACK_SET);
        return EXT_RETURN_FAIL;
    }

    if (!WPACKET_put_bytes_u16(pkt, TLSEXT_TYPE_cookie)
            || !WPACKET_start_sub_packet_u16(pkt)
            || !WPACKET_start_sub_packet_u16(pkt)
            || !WPACKET_get_total_written(pkt, &startlen)
            || !WPACKET_reserve_bytes(pkt, MAX_COOKIE_SIZE, &cookie)
            || !WPACKET_put_bytes_u16(pkt, COOKIE_STATE_FORMAT_VERSION)
            || !WPACKET_put_bytes_u16(pkt, TLS1_3_VERSION)
            || !WPACKET_put_bytes_u16(pkt, s->s3.group_id)
            || !ssl->method->put_cipher_by_char(s->s3.tmp.new_cipher, pkt,
                                                &ciphlen)
               /* Is there a key_share extension present in this HRR? */
            || !WPACKET_put_bytes_u8(pkt, s->s3.peer_tmp == NULL)
            || !WPACKET_put_bytes_u64(pkt, time(NULL))
            || !WPACKET_start_sub_packet_u16(pkt)
            || !WPACKET_reserve_bytes(pkt, EVP_MAX_MD_SIZE, &hashval1)) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
        return EXT_RETURN_FAIL;
    }

    /*
     * Get the hash of the initial ClientHello. ssl_handshake_hash() operates
     * on raw buffers, so we first reserve sufficient bytes (above) and then
     * subsequently allocate them (below)
     */
    if (!ssl3_digest_cached_records(s, 0)
            || !ssl_handshake_hash(s, hashval1, EVP_MAX_MD_SIZE, &hashlen)) {
        /* SSLfatal() already called */
        return EXT_RETURN_FAIL;
    }

    if (!WPACKET_allocate_bytes(pkt, hashlen, &hashval2)
            || !ossl_assert(hashval1 == hashval2)
            || !WPACKET_close(pkt)
            || !WPACKET_start_sub_packet_u8(pkt)
            || !WPACKET_reserve_bytes(pkt, SSL_COOKIE_LENGTH, &appcookie1)) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
        return EXT_RETURN_FAIL;
    }

    /* Generate the application cookie */
    if (sctx->gen_stateless_cookie_cb(ussl, appcookie1,
                                      &appcookielen) == 0) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_R_COOKIE_GEN_CALLBACK_FAILURE);
        return EXT_RETURN_FAIL;
    }

    if (!WPACKET_allocate_bytes(pkt, appcookielen, &appcookie2)
            || !ossl_assert(appcookie1 == appcookie2)
            || !WPACKET_close(pkt)
            || !WPACKET_get_total_written(pkt, &totcookielen)
            || !WPACKET_reserve_bytes(pkt, SHA256_DIGEST_LENGTH, &hmac)) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
        return EXT_RETURN_FAIL;
    }
    hmaclen = SHA256_DIGEST_LENGTH;

    totcookielen -= startlen;
    if (!ossl_assert(totcookielen <= MAX_COOKIE_SIZE - SHA256_DIGEST_LENGTH)) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
        return EXT_RETURN_FAIL;
    }

    /* HMAC the cookie */
    hctx = EVP_MD_CTX_create();
    pkey = EVP_PKEY_new_raw_private_key_ex(sctx->libctx, "HMAC",
                                           sctx->propq,
                                           s->session_ctx->ext.cookie_hmac_key,
                                           sizeof(s->session_ctx->ext.cookie_hmac_key));
    if (hctx == NULL || pkey == NULL) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, ERR_R_EVP_LIB);
        goto err;
    }

    if (EVP_DigestSignInit_ex(hctx, NULL, "SHA2-256", sctx->libctx,
                              sctx->propq, pkey, NULL) <= 0
            || EVP_DigestSign(hctx, hmac, &hmaclen, cookie,
                              totcookielen) <= 0) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
        goto err;
    }

    if (!ossl_assert(totcookielen + hmaclen <= MAX_COOKIE_SIZE)) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
        goto err;
    }

    if (!WPACKET_allocate_bytes(pkt, hmaclen, &hmac2)
            || !ossl_assert(hmac == hmac2)
            || !ossl_assert(cookie == hmac - totcookielen)
            || !WPACKET_close(pkt)
            || !WPACKET_close(pkt)) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
        goto err;
    }

    ret = EXT_RETURN_SENT;

 err:
    EVP_MD_CTX_free(hctx);
    EVP_PKEY_free(pkey);
    return ret;
#else
    return EXT_RETURN_FAIL;
#endif
}

EXT_RETURN tls_construct_stoc_cryptopro_bug(SSL_CONNECTION *s, WPACKET *pkt,
                                            unsigned int context, X509 *x,
                                            size_t chainidx)
{
    const unsigned char cryptopro_ext[36] = {
        0xfd, 0xe8,         /* 65000 */
        0x00, 0x20,         /* 32 bytes length */
        0x30, 0x1e, 0x30, 0x08, 0x06, 0x06, 0x2a, 0x85,
        0x03, 0x02, 0x02, 0x09, 0x30, 0x08, 0x06, 0x06,
        0x2a, 0x85, 0x03, 0x02, 0x02, 0x16, 0x30, 0x08,
        0x06, 0x06, 0x2a, 0x85, 0x03, 0x02, 0x02, 0x17
    };

    if (((s->s3.tmp.new_cipher->id & 0xFFFF) != 0x80
         && (s->s3.tmp.new_cipher->id & 0xFFFF) != 0x81)
            || (SSL_get_options(SSL_CONNECTION_GET_SSL(s))
                & SSL_OP_CRYPTOPRO_TLSEXT_BUG) == 0)
        return EXT_RETURN_NOT_SENT;

    if (!WPACKET_memcpy(pkt, cryptopro_ext, sizeof(cryptopro_ext))) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
        return EXT_RETURN_FAIL;
    }

    return EXT_RETURN_SENT;
}

EXT_RETURN tls_construct_stoc_early_data(SSL_CONNECTION *s, WPACKET *pkt,
                                         unsigned int context, X509 *x,
                                         size_t chainidx)
{
    if (context == SSL_EXT_TLS1_3_NEW_SESSION_TICKET) {
        if (s->max_early_data == 0)
            return EXT_RETURN_NOT_SENT;

        if (!WPACKET_put_bytes_u16(pkt, TLSEXT_TYPE_early_data)
                || !WPACKET_start_sub_packet_u16(pkt)
                || !WPACKET_put_bytes_u32(pkt, s->max_early_data)
                || !WPACKET_close(pkt)) {
            SSLfatal(s, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
            return EXT_RETURN_FAIL;
        }

        return EXT_RETURN_SENT;
    }

    if (s->ext.early_data != SSL_EARLY_DATA_ACCEPTED)
        return EXT_RETURN_NOT_SENT;

    if (!WPACKET_put_bytes_u16(pkt, TLSEXT_TYPE_early_data)
            || !WPACKET_start_sub_packet_u16(pkt)
            || !WPACKET_close(pkt)) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
        return EXT_RETURN_FAIL;
    }

    return EXT_RETURN_SENT;
}

EXT_RETURN tls_construct_stoc_psk(SSL_CONNECTION *s, WPACKET *pkt,
                                  unsigned int context,
                                  X509 *x, size_t chainidx)
{
    if (!s->hit)
        return EXT_RETURN_NOT_SENT;

    if (!WPACKET_put_bytes_u16(pkt, TLSEXT_TYPE_psk)
            || !WPACKET_start_sub_packet_u16(pkt)
            || !WPACKET_put_bytes_u16(pkt, s->ext.tick_identity)
            || !WPACKET_close(pkt)) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
        return EXT_RETURN_FAIL;
    }

    return EXT_RETURN_SENT;
}

EXT_RETURN tls_construct_stoc_client_cert_type(SSL_CONNECTION *sc, WPACKET *pkt,
                                               unsigned int context,
                                               X509 *x, size_t chainidx)
{
    if (sc->ext.client_cert_type_ctos == OSSL_CERT_TYPE_CTOS_ERROR
        && (send_certificate_request(sc)
            || sc->post_handshake_auth == SSL_PHA_EXT_RECEIVED)) {
        /* Did not receive an acceptable cert type - and doing client auth */
        SSLfatal(sc, SSL_AD_UNSUPPORTED_CERTIFICATE, SSL_R_BAD_EXTENSION);
        return EXT_RETURN_FAIL;
    }

    if (sc->ext.client_cert_type == TLSEXT_cert_type_x509) {
        sc->ext.client_cert_type_ctos = OSSL_CERT_TYPE_CTOS_NONE;
        return EXT_RETURN_NOT_SENT;
    }

    /*
     * Note: only supposed to send this if we are going to do a cert request,
     * but TLSv1.3 could do a PHA request if the client supports it
     */
    if ((!send_certificate_request(sc) && sc->post_handshake_auth != SSL_PHA_EXT_RECEIVED)
            || sc->ext.client_cert_type_ctos != OSSL_CERT_TYPE_CTOS_GOOD
            || sc->client_cert_type == NULL) {
        /* if we don't send it, reset to TLSEXT_cert_type_x509 */
        sc->ext.client_cert_type_ctos = OSSL_CERT_TYPE_CTOS_NONE;
        sc->ext.client_cert_type = TLSEXT_cert_type_x509;
        return EXT_RETURN_NOT_SENT;
    }

    if (!WPACKET_put_bytes_u16(pkt, TLSEXT_TYPE_client_cert_type)
            || !WPACKET_start_sub_packet_u16(pkt)
            || !WPACKET_put_bytes_u8(pkt, sc->ext.client_cert_type)
            || !WPACKET_close(pkt)) {
        SSLfatal(sc, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
        return EXT_RETURN_FAIL;
    }
    return EXT_RETURN_SENT;
}

/* One of |pref|, |other| is configured and the values are sanitized */
static int reconcile_cert_type(const unsigned char *pref, size_t pref_len,
                               const unsigned char *other, size_t other_len,
                               uint8_t *chosen_cert_type)
{
    size_t i;

    for (i = 0; i < pref_len; i++) {
        if (memchr(other, pref[i], other_len) != NULL) {
            *chosen_cert_type = pref[i];
            return OSSL_CERT_TYPE_CTOS_GOOD;
        }
    }
    return OSSL_CERT_TYPE_CTOS_ERROR;
}

int tls_parse_ctos_client_cert_type(SSL_CONNECTION *sc, PACKET *pkt,
                                    unsigned int context,
                                    X509 *x, size_t chainidx)
{
    PACKET supported_cert_types;
    const unsigned char *data;
    size_t len;

    /* Ignore the extension */
    if (sc->client_cert_type == NULL) {
        sc->ext.client_cert_type_ctos = OSSL_CERT_TYPE_CTOS_NONE;
        sc->ext.client_cert_type = TLSEXT_cert_type_x509;
        return 1;
    }

    if (!PACKET_as_length_prefixed_1(pkt, &supported_cert_types)) {
        sc->ext.client_cert_type_ctos = OSSL_CERT_TYPE_CTOS_ERROR;
        SSLfatal(sc, SSL_AD_DECODE_ERROR, SSL_R_BAD_EXTENSION);
        return 0;
    }
    if ((len = PACKET_remaining(&supported_cert_types)) == 0) {
        sc->ext.client_cert_type_ctos = OSSL_CERT_TYPE_CTOS_ERROR;
        SSLfatal(sc, SSL_AD_DECODE_ERROR, SSL_R_BAD_EXTENSION);
        return 0;
    }
    if (!PACKET_get_bytes(&supported_cert_types, &data, len)) {
        sc->ext.client_cert_type_ctos = OSSL_CERT_TYPE_CTOS_ERROR;
        SSLfatal(sc, SSL_AD_DECODE_ERROR, SSL_R_BAD_EXTENSION);
        return 0;
    }
    /* client_cert_type: client (peer) has priority */
    sc->ext.client_cert_type_ctos = reconcile_cert_type(data, len,
                                                        sc->client_cert_type, sc->client_cert_type_len,
                                                        &sc->ext.client_cert_type);

    /* Ignore the error until sending - so we can check cert auth*/
    return 1;
}

EXT_RETURN tls_construct_stoc_server_cert_type(SSL_CONNECTION *sc, WPACKET *pkt,
                                               unsigned int context,
                                               X509 *x, size_t chainidx)
{
    if (sc->ext.server_cert_type == TLSEXT_cert_type_x509) {
        sc->ext.server_cert_type_ctos = OSSL_CERT_TYPE_CTOS_NONE;
        return EXT_RETURN_NOT_SENT;
    }
    if (sc->ext.server_cert_type_ctos != OSSL_CERT_TYPE_CTOS_GOOD
            || sc->server_cert_type == NULL) {
        /* if we don't send it, reset to TLSEXT_cert_type_x509 */
        sc->ext.server_cert_type_ctos = OSSL_CERT_TYPE_CTOS_NONE;
        sc->ext.server_cert_type = TLSEXT_cert_type_x509;
        return EXT_RETURN_NOT_SENT;
    }

    if (!WPACKET_put_bytes_u16(pkt, TLSEXT_TYPE_server_cert_type)
            || !WPACKET_start_sub_packet_u16(pkt)
            || !WPACKET_put_bytes_u8(pkt, sc->ext.server_cert_type)
            || !WPACKET_close(pkt)) {
        SSLfatal(sc, SSL_AD_INTERNAL_ERROR, ERR_R_INTERNAL_ERROR);
        return EXT_RETURN_FAIL;
    }
    return EXT_RETURN_SENT;
}

int tls_parse_ctos_server_cert_type(SSL_CONNECTION *sc, PACKET *pkt,
                                    unsigned int context,
                                    X509 *x, size_t chainidx)
{
    PACKET supported_cert_types;
    const unsigned char *data;
    size_t len;

    /* Ignore the extension */
    if (sc->server_cert_type == NULL) {
        sc->ext.server_cert_type_ctos = OSSL_CERT_TYPE_CTOS_NONE;
        sc->ext.server_cert_type = TLSEXT_cert_type_x509;
        return 1;
    }

    if (!PACKET_as_length_prefixed_1(pkt, &supported_cert_types)) {
        SSLfatal(sc, SSL_AD_DECODE_ERROR, SSL_R_BAD_EXTENSION);
        return 0;
    }

    if ((len = PACKET_remaining(&supported_cert_types)) == 0) {
        SSLfatal(sc, SSL_AD_DECODE_ERROR, SSL_R_BAD_EXTENSION);
        return 0;
    }
    if (!PACKET_get_bytes(&supported_cert_types, &data, len)) {
        SSLfatal(sc, SSL_AD_DECODE_ERROR, SSL_R_BAD_EXTENSION);
        return 0;
    }
    /* server_cert_type: server (this) has priority */
    sc->ext.server_cert_type_ctos = reconcile_cert_type(sc->server_cert_type, sc->server_cert_type_len,
                                                        data, len,
                                                        &sc->ext.server_cert_type);
    if (sc->ext.server_cert_type_ctos == OSSL_CERT_TYPE_CTOS_GOOD)
        return 1;

    /* Did not receive an acceptable cert type */
    SSLfatal(sc, SSL_AD_UNSUPPORTED_CERTIFICATE, SSL_R_BAD_EXTENSION);
    return 0;
}
