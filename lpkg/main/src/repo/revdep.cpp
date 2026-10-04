#include "revdep.hpp"

#include "repository.hpp"

std::unordered_map<std::string, std::unordered_set<std::string>> build_reverse_dependency_map(
    const Repository& repo, RevdepEdges edges)
{
    std::unordered_map<std::string, std::unordered_set<std::string>> rev;

    // 第一步：SONAME → 提供它的包名集合。
    // **每个包只看最新版**（版本列表升序 ⇒ `back()` 最新）：旧版本提供的 SONAME 不再参与，
    // 与"取最新版建图"这条既有语义一致（`LatestVersionIsByVersionOrderNotFileOrder` 钉着它）。
    std::unordered_map<std::string, std::unordered_set<std::string>> soname_provider;
    for (const auto& [name, versions] : repo.packages()) {
        if (versions.empty()) continue;
        for (const auto& prov : versions.back().provides_soname) {  // 8.0.0：SONAME 走新字段
            soname_provider[prov].insert(name);
        }
    }

    // 第二步：逐个包（最新版）连边。
    for (const auto& [name, versions] : repo.packages()) {
        if (versions.empty()) continue;
        const PackageInfo& latest = versions.back();

        if (edges == RevdepEdges::DepsAndSoname) {
            for (const auto& dep : latest.dependencies) {
                // 自环排除在此处（构建期），不在查询期再 skip 一次 —— 见头文件的说明。
                if (dep.name != name) rev[dep.name].insert(name);
            }
        }

        for (const auto& so : latest.needed_so) {
            auto it = soname_provider.find(so);
            if (it == soname_provider.end()) continue;
            // **多提供者全连**：同一个 SONAME 由 N 个包提供时，N 个包都是它的反向依赖。
            // 只连第一个会让另外 N-1 个在 `depend remove` / `abibreak` 里凭空消失。
            for (const auto& prov : it->second) {
                if (prov != name) rev[prov].insert(name);
            }
        }
    }

    return rev;
}
