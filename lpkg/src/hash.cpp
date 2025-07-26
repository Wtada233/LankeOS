#include "hash.hpp"
#include "exception.hpp"
#include "localization.hpp"
#include <openssl/evp.h>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <memory>

// Custom deleter for EVP_MD_CTX
struct EvpMdCtxDeleter {
    void operator()(EVP_MD_CTX* ctx) const {
        if (ctx) {
            EVP_MD_CTX_free(ctx);
        }
    }
};

using EvpMdCtxPtr = std::unique_ptr<EVP_MD_CTX, EvpMdCtxDeleter>;

std::string calculate_sha256(const std::string& file_path) {
    std::ifstream file(file_path, std::ios::binary);
    if (!file) {
        throw LpkgException(string_format("error.open_file_failed", file_path));
    }

    EvpMdCtxPtr md_ctx(EVP_MD_CTX_new());
    if (!md_ctx) {
        throw LpkgException(get_string("error.openssl_ctx_failed"));
    }

    if (EVP_DigestInit_ex(md_ctx.get(), EVP_sha256(), NULL) != 1) {
        throw LpkgException(get_string("error.openssl_init_failed"));
    }

    char buffer[8192];
    while (file.read(buffer, sizeof(buffer))) {
        if (EVP_DigestUpdate(md_ctx.get(), buffer, file.gcount()) != 1) {
            throw LpkgException(get_string("error.openssl_update_failed"));
        }
    }
    if (file.gcount() > 0) { // Handle the last chunk
        if (EVP_DigestUpdate(md_ctx.get(), buffer, file.gcount()) != 1) {
            throw LpkgException(get_string("error.openssl_update_failed"));
        }
    }

    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len;
    if (EVP_DigestFinal_ex(md_ctx.get(), hash, &hash_len) != 1) {
        throw LpkgException(get_string("error.openssl_final_failed"));
    }

    std::stringstream ss;
    for (unsigned int i = 0; i < hash_len; ++i) {
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
    }

    return ss.str();
}
