/* The public camera demo's HTTPS sender, compiled unmodified from the
 * reference clone (freenove-wrover-cam/main/chirp_send.c), so its own
 * set_auth_headers() can be asked which X-Chirp-Nonce and X-Chirp-Signature
 * it would send. Everything ESP-IDF is a stand-in (stub/, stub_idf.c). */
#include <string.h>

#include "chirp_send.c" /* static functions included on purpose */

/* Headers the stand-in esp_http_client_set_header() captured (stub_idf.c). */
void stub_headers_clear(void);

/* Run the demo's set_auth_headers() for a POST (with tag and body) or a
 * GET (the downlink challenge) as a Signed device with this key and id. */
int cam_oracle(int post, const uint8_t key[32], const char *hwid, const char *tag,
               const uint8_t *body, size_t len)
{
    memset(&s_config, 0, sizeof s_config);
    strncpy(s_config.hwid, hwid, sizeof s_config.hwid - 1);
    strcpy(s_config.cred, "YOUR_DEVICE_TOKEN");
    s_config.cred_kind = CHIRP_CRED_TOKEN;
    memcpy(s_config.key, key, 32);
    s_config.has_key = true;
    stub_headers_clear();
    return set_auth_headers((esp_http_client_handle_t)0, post ? HTTP_METHOD_POST : HTTP_METHOD_GET, tag,
                            body, len);
}
