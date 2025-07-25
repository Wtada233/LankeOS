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
        throw LpkgException(string_format("error.create_file_failed", file_path));
    }

    EvpMdCtxPtr md_ctx(EVP_MD_CTX_new());
    if (!md_ctx) {
        throw std::runtime_error("Failed to create EVP_MD_CTX");
    }

    if (EVP_DigestInit_ex(md_ctx.get(), EVP_sha256(), NULL) != 1) {
        throw std::runtime_error("Failed to initialize SHA256 digest");
    }

    char buffer[8192];
    while (file.read(buffer, sizeof(buffer))) {
        if (EVP_DigestUpdate(md_ctx.get(), buffer, file.gcount()) != 1) {
            throw std::runtime_error("Failed to update SHA256 digest");
        }
    }
    if (file.gcount() > 0) { // Handle the last chunk
        if (EVP_DigestUpdate(md_ctx.get(), buffer, file.gcount()) != 1) {
            throw std::runtime_error("Failed to update SHA256 digest");
        }
    }

    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len;
    if (EVP_DigestFinal_ex(md_ctx.get(), hash, &hash_len) != 1) {
        throw std::runtime_error("Failed to finalize SHA256 digest");
    }

    std::stringstream ss;
    for (unsigned int i = 0; i < hash_len; ++i) {
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
    }

    return ss.str();
}
