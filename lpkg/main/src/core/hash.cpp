#include "hash.hpp"
#include "exception.hpp"
#include "localization.hpp"

#include <openssl/evp.h>

#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <vector>
#include <array>

namespace fs = std::filesystem;

// EVP_MD_CTX 的自定义删除器
struct EvpMdCtxDeleter {
    void operator()(EVP_MD_CTX* ctx) const {
        if (ctx) {
            EVP_MD_CTX_free(ctx);
        }
    }
};

using EvpMdCtxPtr = std::unique_ptr<EVP_MD_CTX, EvpMdCtxDeleter>;

/**
 * 计算文件的 SHA-256 哈希值
 * 使用 OpenSSL EVP 接口进行哈希计算，支持大文件流式读取
 * @param file_path 目标文件路径
 * @return 小写十六进制字符串表示的 SHA-256 哈希值
 * @throws LpkgException 文件无法打开或哈希计算失败时抛出
 */
std::string calculate_sha256(const fs::path& file_path) {
    std::ifstream file(file_path, std::ios::binary);
    if (!file) {
        throw LpkgException(string_format("error.open_file_failed", file_path.string()));
    }

    EvpMdCtxPtr md_ctx(EVP_MD_CTX_new());
    if (!md_ctx) {
        throw LpkgException(get_string("error.openssl_ctx_failed"));
    }

    if (EVP_DigestInit_ex(md_ctx.get(), EVP_sha256(), nullptr) != 1) {
        throw LpkgException(get_string("error.openssl_init_failed"));
    }

    std::vector<char> buffer(1024 * 1024);
    while (file.read(buffer.data(), buffer.size()) || file.gcount() > 0) {
        if (EVP_DigestUpdate(md_ctx.get(), buffer.data(), file.gcount()) != 1) {
            throw LpkgException(get_string("error.openssl_update_failed"));
        }
    }

    std::array<unsigned char, EVP_MAX_MD_SIZE> hash{};
    unsigned int hash_len;
    if (EVP_DigestFinal_ex(md_ctx.get(), hash.data(), &hash_len) != 1) {
        throw LpkgException(get_string("error.openssl_final_failed"));
    }

    std::stringstream ss;
    for (unsigned int i = 0; i < hash_len; ++i) {
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
    }

    return ss.str();
}
