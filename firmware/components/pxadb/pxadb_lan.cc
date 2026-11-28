#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
#include "pxadb_lan.h"
#include "pxadb/pxadb_service.h"

#include <esp_netif.h>
#include <esp_random.h>
#include <esp_srp.h>
#include <esp_timer.h>
#include <lwip/sockets.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/private/bignum.h>
#include <mbedtls/ssl.h>
#include <mbedtls/ssl_ciphersuites.h>
#include <psa/crypto.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace pxadb {
namespace {
constexpr char kPort[] = "47012";
constexpr unsigned char kIdentity[] = "pxadb";
char srp_user[] = "PXADB LAN v2";
constexpr char kPairMagic[] = "PXA6";
constexpr size_t kPinDigits = 6;
constexpr size_t kSrpPublicBytes = 384;
constexpr size_t kSaltBytes = 16;
constexpr size_t kProofBytes = 64;
constexpr unsigned kMaxFailedPairings = 5;
// RFC 5054 Appendix A, the 3072-bit group used by esp_srp (generator 5).
constexpr char kSrpPrime[] =
    "FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E088A67CC74020B"
    "BEA63B139B22514A08798E3404DDEF9519B3CD3A431B302B0A6DF25F14374FE135"
    "6D6D51C245E485B576625E7EC6F44C42E9A637ED6B0BFF5CB6F406B7EDEE386BF"
    "B5A899FA5AE9F24117C4B1FE649286651ECE45B3DC2007CB8A163BF0598DA4836"
    "1C55D39A69163FA8FD24CF5F83655D23DCA3AD961C62F356208552BB9ED529077"
    "096966D670C354E4ABC9804F1746C08CA18217C32905E462E36CE3BE39E772C18"
    "0E86039B2783A2EC07A28FB5C55DF06F4C52C9DE2BCBF6955817183995497CEA"
    "956AE515D2261898FA051015728E5A8AAAC42DAD33170D04507A33A85521ABDF1"
    "CBA64ECFB850458DBEF0A8AEA71575D060C7DB3970F85A6E1E4C7ABF5AE8CDB09"
    "33D71E8C94E04A25619DCEE3D2261AD2EE6BF12FFA06D98A0864D87602733EC8"
    "6A64521F2B18177B200CBBE117577A615D6C770988C0BAD946E208E24FA074E5"
    "AB3143DB5BFCE0FD108E4B82D120A93AD2CAFFFFFFFFFFFFFFFF";
static_assert(sizeof(kSrpPrime) - 1 == 2 * kSrpPublicBytes);
constexpr int kCiphers[] = {MBEDTLS_TLS_PSK_WITH_AES_128_GCM_SHA256, 0};
constexpr int64_t kHandshakeTimeoutUs = 8'000'000;
constexpr int64_t kNetworkCheckUs = 500'000;

enum class PairStep { client_hello, server_hello, client_proof, server_proof, tls };

std::mutex mutex;
mbedtls_net_context listener{-1}, peer{-1};
mbedtls_ssl_config config{};
mbedtls_ssl_context tls{};
esp_netif_ip_info_t bound{};
std::array<char, kPinDigits + 1> pin{};
std::array<uint8_t, kSaltBytes> salt{};
std::array<uint8_t, kSrpPublicBytes> verifier{};
std::array<uint8_t, kSrpPublicBytes + kSaltBytes> pair_buffer{};
esp_srp_handle_t* srp;
char* session_key;
PairStep pair_step;
size_t pair_offset;
unsigned pair_failures;
int64_t handshake_deadline;
int64_t network_check;
bool enabled;
bool authenticated;
bool disconnected;

bool WifiAddress(esp_netif_ip_info_t* output) {
    auto* wifi = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    return wifi && esp_netif_is_netif_up(wifi) &&
           esp_netif_get_ip_info(wifi, output) == ESP_OK &&
           output->ip.addr && output->netmask.addr;
}

bool CreateVerifier() {
    constexpr uint32_t space = 1'000'000;
    uint32_t value;
    do { value = esp_random(); } while (value >= UINT32_MAX - UINT32_MAX % space);
    std::snprintf(pin.data(), pin.size(), "%06lu", static_cast<unsigned long>(value % space));
    esp_fill_random(salt.data(), salt.size());

    char password[sizeof(srp_user) + kPinDigits + 1];
    std::snprintf(password, sizeof(password), "%s:%s", srp_user, pin.data());
    std::array<uint8_t, kSaltBytes + kProofBytes> hash_input{};
    size_t written = 0;
    const auto hashed = psa_hash_compute(PSA_ALG_SHA_512,
        reinterpret_cast<const uint8_t*>(password), sizeof(password) - 1,
        hash_input.data() + kSaltBytes, kProofBytes, &written);
    mbedtls_platform_zeroize(password, sizeof(password));
    if (hashed != PSA_SUCCESS || written != kProofBytes) return false;
    std::memcpy(hash_input.data(), salt.data(), salt.size());
    std::array<uint8_t, kProofBytes> digest{};
    const auto result = psa_hash_compute(PSA_ALG_SHA_512, hash_input.data(), hash_input.size(),
        digest.data(), digest.size(), &written);
    mbedtls_platform_zeroize(hash_input.data(), hash_input.size());

    mbedtls_mpi modulus, generator, exponent, output;
    mbedtls_mpi_init(&modulus);
    mbedtls_mpi_init(&generator);
    mbedtls_mpi_init(&exponent);
    mbedtls_mpi_init(&output);
    const bool ok = result == PSA_SUCCESS && written == digest.size() &&
        mbedtls_mpi_read_string(&modulus, 16, kSrpPrime) == 0 &&
        mbedtls_mpi_lset(&generator, 5) == 0 &&
        mbedtls_mpi_read_binary(&exponent, digest.data(), digest.size()) == 0 &&
        mbedtls_mpi_exp_mod(&output, &generator, &exponent, &modulus, nullptr) == 0 &&
        mbedtls_mpi_write_binary(&output, verifier.data(), verifier.size()) == 0;
    mbedtls_platform_zeroize(digest.data(), digest.size());
    mbedtls_mpi_free(&output);
    mbedtls_mpi_free(&exponent);
    mbedtls_mpi_free(&generator);
    mbedtls_mpi_free(&modulus);
    return ok;
}

void ClosePeer() {
    disconnected |= authenticated;
    authenticated = false;
    mbedtls_ssl_free(&tls);
    mbedtls_ssl_config_free(&config);
    mbedtls_net_free(&peer);
    if (srp) esp_srp_free(srp);
    srp = nullptr;
    session_key = nullptr;
    pair_step = PairStep::client_hello;
    pair_offset = 0;
}

void CloseListener() {
    ClosePeer();
    mbedtls_net_free(&listener);
    mbedtls_platform_zeroize(pin.data(), pin.size());
    mbedtls_platform_zeroize(verifier.data(), verifier.size());
    enabled = false;
}

bool RejectPairing() {
    if (++pair_failures >= kMaxFailedPairings) CloseListener();
    else ClosePeer();
    return false;
}

bool RemoteIsLocal() {
    sockaddr_in address{};
    socklen_t length = sizeof(address);
    return getpeername(peer.fd, reinterpret_cast<sockaddr*>(&address), &length) == 0 &&
           address.sin_family == AF_INET &&
           !((address.sin_addr.s_addr ^ bound.ip.addr) & bound.netmask.addr);
}

bool MakeServerHello() {
    if (std::memcmp(pair_buffer.data(), kPairMagic, sizeof(kPairMagic) - 1)) return false;
    srp = esp_srp_init(ESP_NG_3072);
    if (!srp || esp_srp_set_salt_verifier(srp, reinterpret_cast<const char*>(salt.data()), salt.size(),
            reinterpret_cast<const char*>(verifier.data()), verifier.size()) != ESP_OK) return false;
    char* public_key = nullptr;
    int public_bytes = 0;
    if (esp_srp_srv_pubkey_from_salt_verifier(srp, &public_key, &public_bytes) != ESP_OK || !public_key ||
        public_bytes <= 0 || public_bytes > int(kSrpPublicBytes)) return false;

    // ESP-IDF hashes the minimal encoding for the two SRP proofs.
    auto* first = pair_buffer.data() + sizeof(kPairMagic) - 1;
    auto* end = first + kSrpPublicBytes;
    while (first < end && *first == 0) ++first;
    uint16_t session_bytes = 0;
    if (first == end || esp_srp_get_session_key(srp, reinterpret_cast<char*>(first), end - first,
            &session_key, &session_bytes) != ESP_OK || session_bytes != kProofBytes) return false;
    std::memcpy(pair_buffer.data(), salt.data(), salt.size());
    auto* start = pair_buffer.data() + kSaltBytes;
    const auto pad = kSrpPublicBytes - public_bytes;
    std::memset(start, 0, pad);
    std::memcpy(start + pad, public_key, public_bytes);
    return true;
}

bool PrepareTls() {
    std::array<char, kProofBytes> proof{};
    if (esp_srp_exchange_proofs(srp, srp_user, sizeof(srp_user) - 1,
            reinterpret_cast<char*>(pair_buffer.data()), proof.data()) != ESP_OK) return false;
    std::array<uint8_t, 32> key{};
    size_t written = 0;
    const auto hashed = psa_hash_compute(PSA_ALG_SHA_256, reinterpret_cast<const uint8_t*>(session_key),
        kProofBytes, key.data(), key.size(), &written);
    esp_srp_free(srp);
    srp = nullptr;
    session_key = nullptr;
    mbedtls_ssl_config_init(&config);
    bool ok = hashed == PSA_SUCCESS && written == key.size() &&
        mbedtls_ssl_config_defaults(&config, MBEDTLS_SSL_IS_SERVER,
            MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) == 0;
    if (ok) {
        mbedtls_ssl_conf_min_tls_version(&config, MBEDTLS_SSL_VERSION_TLS1_2);
        mbedtls_ssl_conf_max_tls_version(&config, MBEDTLS_SSL_VERSION_TLS1_2);
        mbedtls_ssl_conf_ciphersuites(&config, kCiphers);
        ok = mbedtls_ssl_conf_psk(&config, key.data(), key.size(), kIdentity, sizeof(kIdentity) - 1) == 0 &&
             mbedtls_ssl_setup(&tls, &config) == 0;
    }
    mbedtls_platform_zeroize(key.data(), key.size());
    if (!ok) return false;
    std::memcpy(pair_buffer.data(), proof.data(), proof.size());
    mbedtls_ssl_set_bio(&tls, &peer, mbedtls_net_send, mbedtls_net_recv, nullptr);
    return true;
}

bool ContinueHandshake(int64_t now) {
    if (peer.fd < 0) {
        if (mbedtls_net_accept(&listener, &peer, nullptr, 0, nullptr) != 0) return false;
        if (!RemoteIsLocal() || mbedtls_net_set_nonblock(&peer) != 0) {
            ClosePeer();
            return false;
        }
        handshake_deadline = now + kHandshakeTimeoutUs;
    }
    if (now >= handshake_deadline) return RejectPairing();

    if (pair_step != PairStep::tls) {
        const bool sending = pair_step == PairStep::server_hello || pair_step == PairStep::server_proof;
        const size_t length = pair_step == PairStep::client_hello ? sizeof(kPairMagic) - 1 + kSrpPublicBytes :
            pair_step == PairStep::server_hello ? kSaltBytes + kSrpPublicBytes : kProofBytes;
        auto* cursor = pair_buffer.data() + pair_offset;
        const auto remaining = length - pair_offset;
        const int count = sending ? mbedtls_net_send(&peer, cursor, remaining) :
                                    mbedtls_net_recv(&peer, cursor, remaining);
        if (count == MBEDTLS_ERR_SSL_WANT_READ || count == MBEDTLS_ERR_SSL_WANT_WRITE) return false;
        if (count <= 0) return RejectPairing();
        pair_offset += count;
        if (pair_offset != length) return false;
        pair_offset = 0;
        if (pair_step == PairStep::client_hello) {
            if (!MakeServerHello()) return RejectPairing();
            pair_step = PairStep::server_hello;
        } else if (pair_step == PairStep::server_hello) {
            pair_step = PairStep::client_proof;
        } else if (pair_step == PairStep::client_proof) {
            if (!PrepareTls()) return RejectPairing();
            pair_step = PairStep::server_proof;
        } else {
            pair_step = PairStep::tls;
        }
        return false;
    }
    const int result = mbedtls_ssl_handshake(&tls);
    if (result == 0) {
        authenticated = true;
        pair_failures = 0;
    } else if (result != MBEDTLS_ERR_SSL_WANT_READ && result != MBEDTLS_ERR_SSL_WANT_WRITE) {
        return RejectPairing();
    }
    return authenticated;
}
}  // namespace

bool SetLanEnabled(bool desired) {
    std::lock_guard guard(mutex);
    if (desired && !IsRunning()) return false;
    if (enabled == desired) return true;
    if (!desired) {
        CloseListener();
        return true;
    }
    if (!WifiAddress(&bound) || psa_crypto_init() != PSA_SUCCESS) return false;
    char address[16];
    std::snprintf(address, sizeof(address), IPSTR, IP2STR(&bound.ip));
    if (!CreateVerifier() ||
        mbedtls_net_bind(&listener, address, kPort, MBEDTLS_NET_PROTO_TCP) != 0 ||
        mbedtls_net_set_nonblock(&listener) != 0) {
        CloseListener();
        return false;
    }
    network_check = esp_timer_get_time() + kNetworkCheckUs;
    pair_failures = 0;
    enabled = true;
    return true;
}

bool IsLanEnabled() {
    std::lock_guard guard(mutex);
    return enabled;
}

bool ReadLanEndpoint(char* address, size_t address_size, char* pairing, size_t pairing_size) {
    std::lock_guard guard(mutex);
    if (!enabled || !address || address_size < 22 || !pairing || pairing_size < pin.size()) return false;
    std::snprintf(address, address_size, IPSTR ":%s", IP2STR(&bound.ip), kPort);
    std::memcpy(pairing, pin.data(), pin.size());
    return true;
}

int ReadLan(uint8_t* destination, size_t size) {
    std::lock_guard guard(mutex);
    const auto now = esp_timer_get_time();
    if (enabled && now >= network_check) {
        network_check = now + kNetworkCheckUs;
        esp_netif_ip_info_t current{};
        if (!WifiAddress(&current) || current.ip.addr != bound.ip.addr ||
            current.netmask.addr != bound.netmask.addr) CloseListener();
    }
    if (disconnected) {
        disconnected = false;
        return -2;
    }
    if (!enabled || (!authenticated && !ContinueHandshake(now))) return 0;
    const int count = mbedtls_ssl_read(&tls, destination, size);
    if (count > 0) return count;
    if (count == MBEDTLS_ERR_SSL_WANT_READ || count == MBEDTLS_ERR_SSL_WANT_WRITE) return 0;
    ClosePeer();
    disconnected = false;
    return -2;
}

int WriteLan(const uint8_t* source, size_t size) {
    std::lock_guard guard(mutex);
    if (!enabled || !authenticated) return -1;
    const int count = mbedtls_ssl_write(&tls, source, size);
    if (count == MBEDTLS_ERR_SSL_WANT_READ || count == MBEDTLS_ERR_SSL_WANT_WRITE) return 0;
    if (count <= 0) ClosePeer();
    return count;
}

}  // namespace pxadb
