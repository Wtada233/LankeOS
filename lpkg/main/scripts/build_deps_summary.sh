#!/bin/bash
# build_deps_summary.sh — 分析 LankeBUILD.json 的 build_deps 并与 Arch makedepends 对比
#   AI 模式：设置 DEEPSEEK_API_KEY 环境变量自动填写 build_deps
#   手动模式：无 API key 时输出对比信息供参考
#
# 用法:
#   DEEPSEEK_API_KEY=sk-xxx ./build_deps_summary.sh . .           # AI 自动填写（完整扫描）
#   DEEPSEEK_API_KEY=sk-xxx ./build_deps_summary.sh . /tmp/test   # AI：用 pkgs/ 做 context，写 /tmp/test/
#   ./build_deps_summary.sh .                                      # 手动模式，当前目录

set -euo pipefail

# ── 颜色定义 ──
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
RED='\033[0;31m'
BOLD='\033[1m'
NC='\033[0m'

# ── 检查依赖 ──
for cmd in jq curl; do
    if ! command -v "$cmd" &>/dev/null; then
        echo -e "${RED}[ERROR]${NC} 需要 $cmd 但未安装" >&2
        exit 1
    fi
done

# ── 配置 ──
BATCH_SIZE=12
DEEPSEEK_API_KEY="${DEEPSEEK_API_KEY:-}"
AI_DATA_DIR=""
if [ -n "$DEEPSEEK_API_KEY" ]; then
    AI_DATA_DIR=$(mktemp -d)
    trap "rm -rf $AI_DATA_DIR" EXIT
fi

# ── 参数 ──
# 参数 1：input_pkgname_scandir — 包名扫描目录（显示列表 + AI context）
# 参数 2：gen_target_dir       — 实际处理目录（查找 LankeBUILD.json 并写入，可选，默认同参数 1）
if [ $# -ge 2 ]; then
    PKG_SCAN_DIR="$1"
    TARGET_DIR="$2"
elif [ $# -eq 1 ]; then
    PKG_SCAN_DIR="$1"
    TARGET_DIR="$1"
else
    PKG_SCAN_DIR="."
    TARGET_DIR="."
fi

# ── 在目标目录中递归查找 LankeBUILD.json ──
mapfile -t JSON_FILES < <(find "$TARGET_DIR" -name 'LankeBUILD.json' -type f 2>/dev/null | sort -t/ -k1,1)
if [ ${#JSON_FILES[@]} -eq 0 ]; then
    echo -e "${RED}未找到任何 LankeBUILD.json 文件${NC}" >&2
    exit 1
fi

# ── 收集包信息 ──
declare -a PKG_NAMES          # AI context（来自 scan_dir）
declare -a PKG_HAS_BUILD_DEPS # 来自 target_dir（需与 TARGET_NAMES 对齐）
declare -a PKG_DIRS
declare -a TARGET_NAMES       # 实际处理的包（来自 target_dir）
declare -a TARGET_DIRS        # 对应的目录路径

# ── 预扫所有包名（给 AI 提供完整 context）──
# 从 input_pkgname_scandir 获取完整包列表，不受处理范围影响
for json_path in $(find "$PKG_SCAN_DIR" -name 'LankeBUILD.json' -type f 2>/dev/null | sort); do
    name=$(jq -r '.name // empty' "$json_path")
    [ -z "$name" ] && continue
    PKG_NAMES+=("$name")
done

# 用于 AI 批处理
declare -a BATCH_NAMES
declare -a BATCH_DIRS
declare -a BATCH_JSONS
declare -i BATCH_COUNT=0

# ── 查询 Arch makedepends ──
fetch_arch_makedeps() {
    local pkgname="$1"
    local pkgbase="" found=false
    local pkgname_deps="" src_deps="" base_deps="" check_deps=""
    local http_code curl_tmp
    local max_api_retries=10

    # ── 辅助：带状态码的 curl ──
    curl_with_status() {
        local url="$1" out_var="$2" code_var="$3"
        local tmpf
        tmpf=$(mktemp)
        local code
        code=$(curl -s --connect-timeout 5 -w "%{http_code}" -o "$tmpf" "$url" 2>/dev/null || echo "000")
        printf -v "$out_var" "$(cat "$tmpf")"
        printf -v "$code_var" "$code"
        rm -f "$tmpf"
    }

    # ── 辅助：写入不可用占位符 ──
    write_unavailable() {
        if [ -n "$AI_DATA_DIR" ]; then
            echo "(API不可用，仅依据deps和构建命令推断)" > "$AI_DATA_DIR/$pkgname.deps" 2>/dev/null
        fi
    }

    # ── 1) JSON API 搜索（带重试）──
    local json_data=""
    local retry
    for ((retry=0; retry<max_api_retries; retry++)); do
        [ "$retry" -gt 0 ] && sleep 0.5
        curl_with_status "https://archlinux.org/packages/search/json/?name=$pkgname" json_data http_code
        if [ "$http_code" = "404" ]; then
            write_unavailable
            return  # 404 静默跳过
        elif [ "$http_code" = "200" ] && [ -n "$json_data" ]; then
            break
        fi
        # 其他错误继续重试
    done

    if [ -z "$json_data" ]; then
        echo "  (Arch 查询失败，已重试 ${max_api_retries} 次)"
        write_unavailable
        return
    fi

    local results_count
    results_count=$(echo "$json_data" | jq '.results | length' 2>/dev/null || echo 0)
    if [ "$results_count" -eq 0 ]; then
        write_unavailable
        return  # 未找到包，不报错
    fi

    pkgbase=$(echo "$json_data" | jq -r '.results[0].pkgbase // empty' 2>/dev/null)
    echo "  pkgbase: $pkgbase"

    pkgname_deps=$(echo "$json_data" | jq -r '.results[0].makedepends[]? // empty' 2>/dev/null)
    found=true

    # ── 2) 如果 pkgbase != pkgname，额外获取 pkgbase 的 makedepends（带重试）──
    if [ -n "$pkgbase" ] && [ "$pkgbase" != "$pkgname" ]; then
        local base_json=""
        for ((retry=0; retry<max_api_retries; retry++)); do
            [ "$retry" -gt 0 ] && sleep 0.5
            curl_with_status "https://archlinux.org/packages/search/json/?name=$pkgbase" base_json http_code
            [ "$http_code" = "200" ] && [ -n "$base_json" ] && break
        done
        if [ -n "$base_json" ]; then
            base_deps=$(echo "$base_json" | jq -r '.results[0].makedepends[]? // empty' 2>/dev/null)
        fi
    fi

    # ── 3) .SRCINFO（pkgbase 级别，带重试）──
    if [ -n "$pkgbase" ]; then
        local srcinfo=""
        for ((retry=0; retry<max_api_retries; retry++)); do
            [ "$retry" -gt 0 ] && sleep 0.5
            curl_with_status \
                "https://gitlab.archlinux.org/archlinux/packaging/packages/$pkgbase/-/raw/main/.SRCINFO?inline=false" \
                srcinfo http_code
            [ "$http_code" = "200" ] && [ -n "$srcinfo" ] && break
        done
        if [ -n "$srcinfo" ]; then
            src_deps=$(echo "$srcinfo" | \
                grep -E '^[[:space:]]*makedepends[[:space:]]*=' | \
                sed 's/^[[:space:]]*makedepends[[:space:]]*=[[:space:]]*//;s/^"//;s/"$//' | \
                sort -u)

            check_deps=$(echo "$srcinfo" | \
                grep -E '^[[:space:]]*checkdepends[[:space:]]*=' | \
                sed 's/^[[:space:]]*checkdepends[[:space:]]*=[[:space:]]*//;s/^"//;s/"$//' | \
                sort -u)
        fi
    fi

    # ── 4) 显示 + 保存 ──
    if [ "$pkgbase" = "$pkgname" ]; then
        local all_deps
        all_deps=$( { echo "$pkgname_deps"; echo "$check_deps"; } | grep -v '^$' | sort -u )
        local all_count
        all_count=$(echo "$all_deps" | grep -c . || true)
        echo "  ┌─ makedepends (${all_count} 个) ────────────────────"
        if [ "$all_count" -gt 0 ]; then
            echo "$all_deps" | sed 's/^/  │ • /'
        else
            echo "  │ (空)"
        fi
        echo "  └──────────────────────────────────────────────"
        # 保存合并结果给 AI
        if [ -n "$AI_DATA_DIR" ]; then
            echo "$all_deps" | paste -sd ',' - > "$AI_DATA_DIR/$pkgname.deps" 2>/dev/null
        fi
    else
        local base_combined
        base_combined=$( { echo "$src_deps"; echo "$base_deps"; } | grep -v '^$' | sort -u )
        local base_count
        base_count=$(echo "$base_combined" | grep -c . || true)

        echo "  ┌─ $pkgname (pkgname) makedepends ────────────"
        if [ -n "$pkgname_deps" ]; then
            echo "$pkgname_deps" | sed 's/^/  │ • /'
        else
            echo "  │ (空)"
        fi
        echo "  └──────────────────────────────────────────────"

        echo "  ┌─ $pkgbase (pkgbase) makedepends ────────────"
        if [ "$base_count" -gt 0 ]; then
            echo "$base_combined" | sed 's/^/  │ • /'
        else
            echo "  │ (空)"
        fi
        echo "  └──────────────────────────────────────────────"

        local check_count
        check_count=$(echo "$check_deps" | grep -c . || true)
        if [ "$check_count" -gt 0 ]; then
            echo "  ┌─ checkdepends ─────────────────────────────"
            echo "$check_deps" | sed 's/^/  │ • /'
            echo "  └──────────────────────────────────────────────"
        fi

        local merged
        merged=$( { echo "$pkgname_deps"; echo "$base_combined"; echo "$check_deps"; } | grep -v '^$' | sort -u)
        local merged_count
        merged_count=$(echo "$merged" | grep -c . || true)
        echo "  ┌─ 合并 (${merged_count} 个, 去重) ─────────────────"
        if [ "$merged_count" -gt 0 ]; then
            echo "$merged" | sed 's/^/  │ • /'
        else
            echo "  │ (空)"
        fi
        echo "  └──────────────────────────────────────────────"
        # 保存合并结果给 AI
        if [ -n "$AI_DATA_DIR" ]; then
            echo "$merged" | paste -sd ',' - > "$AI_DATA_DIR/$pkgname.deps" 2>/dev/null
        fi
    fi

    $found || echo "  (无 makedepends 信息)"
}

# ── 提取构建命令摘要（供 AI prompt 使用）──
extract_build_cmds() {
    local lankebuild="$1"
    if grep -qE '^    \.\/configure |^    cmake |^    meson |^    cargo |^    go build ' \
        "$lankebuild" 2>/dev/null; then
        awk '
            /^    \.\/configure / || /^    cmake / || /^    meson / || /^    cargo / || /^    go build / {
                line = $0
                gsub(/\${[^}]+}/, "{VAR}", line)
                printf "▶ %s", line
                if ($0 ~ /\\$/) { show = 1; next }
                show = 0; printf "; "; next
            }
            show && /^[[:space:]]/ {
                line = $0
                gsub(/\${[^}]+}/, "{VAR}", line)
                printf " %s", line
                if ($0 !~ /\\$/) { show = 0; printf "; " }
            }
        ' "$lankebuild" | sed 's/; $//'
    else
        echo "(custom build script)"
    fi
}

# ── AI 批处理（12 包一批，自动重试缺失包）──
process_ai_batch() {
    local -n _names=$1 _dirs=$2 _jsons=$3
    local batch_num=$4
    local count=${#_names[@]}

    [ "$count" -eq 0 ] && return

    echo -e "${CYAN}${BOLD}"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo "  AI 分析 Batch #${batch_num} (${count} 个包)"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo -e "${NC}"

    # ── 基础 prompt（规则 + 完整包列表）──
    local base_prompt="你是 LankeOS 构建依赖分析专家。根据以下信息，输出每个包的**完整构建依赖**（build_deps）。

build_deps 的构成公式：**运行时 deps + 构建工具 + 映射后的 Arch makedepends**

规则：
1. 只输出下表中存在的包名
2. 删掉 LankeBUILD 里通过 --disable-* 或 -D*=false 禁用的依赖
3. python-* 包删掉（由 pip 管理），但 python 本体保留
4. ❗**运行时 deps 全部必须加入 build_deps，一个也不能删**。这些包 100% 在 LankeOS 仓库中，且提供了编译所需的头文件或库。
5. 📌根据 LankeBUILD 脚本添加构建工具：编译器（如果有编译动作）、pkgconf（如果用到了 pkg-config）、meson/cmake/ninja（如果用了对应构建系统）等。不要假设任何编译器是"系统自带"的。
6. Arch 的 makedepends 是重要参考，结合 LankeBUILD 脚本交叉核对
7. ❗如果 Arch 的某个依赖在可用包列表中不存在，先查找是否有其他包提供同等功能：
   例如：clang → llvm（llvm 提供 clang 编译器）、glib2-devel → glib。
   确实找不到替代品才删掉。

输出格式（每行一个包，只输出以下格式，不要任何其他文字）：
包名:dep1,dep2,dep3

---

LankeOS 可用包列表（共 ${#PKG_NAMES[@]} 个，只有这里列出的包才能作为依赖引用）：
$(for ((_i=0; _i<${#PKG_NAMES[@]}; _i+=10)); do echo "  ${PKG_NAMES[@]:_i:10}"; done)

---

"

    # ── 构建某几个包的详情段 ──
    build_prompt_details() {
        local -n _pkgs=$1
        local out="" pkg idx dir cmds deps
        for pkg in "${_pkgs[@]}"; do
            idx=-1
            for ((i=0; i<count; i++)); do
                [ "${_names[$i]}" = "$pkg" ] && { idx=$i; break; }
            done
            [ "$idx" -eq -1 ] && continue

            dir="${_dirs[$idx]}"
            cmds=""; deps=""; rtime_deps=""
            [ -f "$dir/LankeBUILD" ] && cmds=$(extract_build_cmds "$dir/LankeBUILD")
            if [ -n "$AI_DATA_DIR" ] && [ -f "$AI_DATA_DIR/$pkg.deps" ]; then
                deps=$(cat "$AI_DATA_DIR/$pkg.deps")
            else
                deps="(unknown)"
            fi
            # 读取运行时 deps（辅助 AI 反推编译依赖）
            if [ -f "$dir/LankeBUILD.json" ]; then
                rtime_deps=$(jq -r '.deps // [] | join(",")' "$dir/LankeBUILD.json" 2>/dev/null)
                [ -z "$rtime_deps" ] && rtime_deps="(none)"
            fi
            out+="包名: $pkg
构建命令: $cmds
Arch makedepends: $deps
当前 deps（运行时依赖）: $rtime_deps

"
        done
        echo "$out"
    }

    local max_retries=3
    local retry=0
    local missing=("${_names[@]}")

    while [ "$retry" -lt "$max_retries" ] && [ ${#missing[@]} -gt 0 ]; do
        local prompt="$base_prompt"
        prompt+="$(build_prompt_details missing)"
        prompt+="---\n\n输出："

        [ "$retry" -gt 0 ] && echo -e "  ${YELLOW}重试 #${retry}（缺失 ${#missing[@]} 个包）${NC}"

        # ── 调用 DeepSeek API ──
        local response
        response=$(curl -s https://api.deepseek.com/chat/completions \
            -H "Content-Type: application/json" \
            -H "Authorization: Bearer $DEEPSEEK_API_KEY" \
            -d "$(jq -n -R --arg prompt "$prompt" '{
                model: "deepseek-v4-flash",
                thinking: {type: "enabled"},
                reasoning_effort: "high",
                messages: [{role: "user", content: $prompt}],
                stream: false,
                temperature: 0.1
            }')") || {
            echo -e "  ${RED}curl 请求失败${NC}"
            echo "$response" | head -c 500 >&2
            echo >&2
            retry=$((retry + 1)); sleep 2; continue
        }

        echo "DEBUG: $response" | head -c 300 >&2
        echo >&2

        local content
        content=$(echo "$response" | jq -r '.choices[0].message.content // empty' 2>/dev/null)
        if [ -z "$content" ]; then
            echo -e "  ${RED}API 原始响应:${NC}" >&2
            echo "$response" | head -c 1000 >&2
            echo >&2
            retry=$((retry + 1)); sleep 2; continue
        fi

	        # ── Phase 1：解析 → 保存到临时目录 ──
	        local batch_tmpdir
	        batch_tmpdir=$(mktemp -d)
	        local parsed_count=0

	        while IFS= read -r line; do
	            [ -z "$line" ] && continue
	            [[ "$line" == '```'* ]] && continue
	            [[ "$line" != *":"* ]] && continue

	            local pkg="${line%%:*}" deps_list="${line#*:}"
	            pkg="${pkg#"${pkg%%[![:space:]]*}"}"
	            pkg="${pkg%"${pkg##*[![:space:]]}"}"

	            # 只在 missing 列表中的才处理
	            local is_missing=false
	            for m in "${missing[@]}"; do
	                [ "$m" = "$pkg" ] && { is_missing=true; break; }
	            done
	            $is_missing || continue

	            IFS=',' read -ra dep_arr <<< "$deps_list"
	            local json_deps="[" sep=""
	            local dep
	            for dep in "${dep_arr[@]}"; do
	                dep="${dep#"${dep%%[![:space:]]*}"}"
	                dep="${dep%"${dep##*[![:space:]]}"}"
	                [ -z "$dep" ] && continue
	                json_deps+="${sep}\"$dep\""
	                sep=", "
	            done
	            json_deps+="]"

	            # 写入临时文件（而非直接改 LankeBUILD.json）
	            echo "$json_deps" > "$batch_tmpdir/$pkg.deps"
	            parsed_count=$((parsed_count + 1))
	        done <<< "$content"

	        # ── Phase 2：验证全覆盖 ──
	        local all_covered=true
	        for m in "${missing[@]}"; do
	            if [ ! -f "$batch_tmpdir/$m.deps" ]; then
	                all_covered=false
	                break
	            fi
	        done

	        # ── Phase 3：全覆盖才写入，否则重试 ──
	        if $all_covered; then
	            for m in "${missing[@]}"; do
	                local json_deps
	                json_deps=$(cat "$batch_tmpdir/$m.deps")
	                # 找到此包在批次中的索引
	                local found_idx=-1
	                for ((i=0; i<count; i++)); do
	                    [ "${_names[$i]}" = "$m" ] && { found_idx=$i; break; }
	                done
	                if [ "$found_idx" -ne -1 ]; then
	                    local tmpf
	                    tmpf=$(mktemp)
	                    jq --arg deps "$json_deps" '.build_deps = ($deps | fromjson)' \
	                        "${_jsons[$found_idx]}" > "$tmpf" && mv "$tmpf" "${_jsons[$found_idx]}"
	                    rm -f "$tmpf"
	                    echo -e "  ${GREEN}✓${NC} $m → $(echo "$json_deps" | tr -d '\n')"
	                fi
	            done
	            missing=()   # 全部清除
	        else
	            echo -e "  ${YELLOW}部分缺失（解析到 ${parsed_count}/${#missing[@]} 个），将重试${NC}"
	        fi
	        rm -rf "$batch_tmpdir"

	        retry=$((retry + 1))
	        [ ${#missing[@]} -gt 0 ] && [ "$retry" -lt "$max_retries" ] && sleep 1
    done

    [ ${#missing[@]} -gt 0 ] && echo -e "  ${YELLOW}⚠ 未能覆盖: ${missing[*]}${NC}"
    echo
    return 0
}

# ════════════════════════════════════════════════════════════════
# 主循环（批次处理）
# ════════════════════════════════════════════════════════════════

echo -e "${CYAN}${BOLD}════════════════════════════════════════════════════════════${NC}"
echo -e "${CYAN}${BOLD}  构建依赖分析 (Build Deps Summary)${NC}"
echo -e "${CYAN}${BOLD}════════════════════════════════════════════════════════════${NC}"
echo

BATCH_COUNT=0

for json_path in "${JSON_FILES[@]}"; do
    dir=$(dirname "$json_path")
    name=$(jq -r '.name // empty' "$json_path")
    [ -z "$name" ] && continue

    # 收集到批次数组
    BATCH_NAMES+=("$name")
    BATCH_DIRS+=("$dir")
    BATCH_JSONS+=("$json_path")

    echo -e "${GREEN}${BOLD}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo -e "${GREEN}${BOLD}  包: $name${NC}"
    echo -e "${GREEN}  目录: $(realpath "$dir" 2>/dev/null || echo "$dir")${NC}"
    echo -e "${GREEN}${BOLD}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo

    TARGET_NAMES+=("$name")
    TARGET_DIRS+=("$dir")
    PKG_DIRS+=("$dir")       # 保持与 TARGET_NAMES 对齐，供待处理列表使用

    # ── 1. build_deps ──
    build_deps=$(jq -r '.build_deps // []' "$json_path" 2>/dev/null)
    deps_count=$(echo "$build_deps" | jq 'length' 2>/dev/null || echo 0)

    if [ "$deps_count" -eq 0 ] || [ "$build_deps" = "[]" ]; then
        echo -e "▶ ${YELLOW}build_deps: (空)${NC}"
        PKG_HAS_BUILD_DEPS+=("no")
    else
        echo -e "▶ ${GREEN}build_deps${NC}:"
        echo "$build_deps" | jq -r '.[]' | sed 's/^/  • /'
        PKG_HAS_BUILD_DEPS+=("yes")
    fi
    echo

    # ── 1.1 deps（运行时依赖，供 AI 反推编译依赖参考）──
    pkg_deps=$(jq -r '.deps // []' "$json_path" 2>/dev/null)
    pkg_deps_count=$(echo "$pkg_deps" | jq 'length' 2>/dev/null || echo 0)
    if [ "$pkg_deps_count" -gt 0 ] && [ "$pkg_deps" != "[]" ]; then
        echo -e "▶ ${CYAN}deps${NC} (运行时依赖):"
        echo "$pkg_deps" | jq -r '.[]' | sed 's/^/  • /'
        echo
    fi

    # ── 2. LankeBUILD 文件内容 ──
    if [ -f "$dir/LankeBUILD" ]; then
        echo -e "▶ ${CYAN}LankeBUILD${NC} (${dir}/LankeBUILD):"
        echo "  ┌─────────────────────────────────────────────"
        awk '
            /^lankebuild_(prepare|build|package|check)\b/ { printf "  │ %s\n", $0 }
        ' "$dir/LankeBUILD"
        echo "  │"
        # 提取标准构建命令（含多行 \ 续行），匹配不到则输出完整 LankeBUILD
        if grep -qE '^    \.\/configure |^    cmake |^    meson |^    cargo |^    go build ' \
            "$dir/LankeBUILD" 2>/dev/null; then
            awk '
                /^    \.\/configure / || /^    cmake / || /^    meson / || /^    cargo / || /^    go build / {
                    printf "  │ ▶ %s\n", $0;
                    if ($0 ~ /\\$/) { show = 1 } else { show = 0 }
                    next
                }
                show && /^[[:space:]]/ {
                    printf "  │    %s\n", $0;
                    if ($0 !~ /\\$/) { show = 0 }
                }
            ' "$dir/LankeBUILD"
        else
            sed 's/^/  │ /' "$dir/LankeBUILD"
        fi
        echo "  └─────────────────────────────────────────────"
    else
        echo -e "▶ ${RED}LankeBUILD: (文件不存在)${NC}"
    fi
    echo

    # ── 3. Arch makedepends ──
    echo -e "▶ ${CYAN}Arch makedepends${NC} (查询: $name)..."
    fetch_arch_makedeps "$name" || true
    echo

    echo -e "${YELLOW}────────────────────────────────────────────────────────────${NC}"
    echo

    # ── 批次满，触发 AI 处理 ──
    if [ ${#BATCH_NAMES[@]} -ge $BATCH_SIZE ] && [ -n "$DEEPSEEK_API_KEY" ]; then
        BATCH_COUNT=$((BATCH_COUNT + 1))
        process_ai_batch BATCH_NAMES BATCH_DIRS BATCH_JSONS "$BATCH_COUNT" || true
        BATCH_NAMES=()
        BATCH_DIRS=()
        BATCH_JSONS=()
    fi
done

# ── 处理最后一批 ──
if [ ${#BATCH_NAMES[@]} -gt 0 ] && [ -n "$DEEPSEEK_API_KEY" ]; then
    BATCH_COUNT=$((BATCH_COUNT + 1))
    process_ai_batch BATCH_NAMES BATCH_DIRS BATCH_JSONS "$BATCH_COUNT" || true
    BATCH_NAMES=()
    BATCH_DIRS=()
    BATCH_JSONS=()
fi

# ════════════════════════════════════════════════════════════════
# 汇总（完整包列表）
# ════════════════════════════════════════════════════════════════

echo -e "${CYAN}${BOLD}════════════════════════════════════════════════════════════${NC}"
echo -e "${CYAN}${BOLD}  包列表 (共 ${#TARGET_NAMES[@]} 个)${NC}"
echo -e "${CYAN}${BOLD}════════════════════════════════════════════════════════════${NC}"
echo
for ((i=0; i<${#TARGET_NAMES[@]}; i+=12)); do
    echo "  ${TARGET_NAMES[@]:i:12}"
done
echo

# ── 待处理列表 ──
echo -e "${YELLOW}待填写 build_deps 的包:${NC}"
missing_count=0
for i in "${!TARGET_NAMES[@]}"; do
    if [ "${PKG_HAS_BUILD_DEPS[$i]}" = "no" ]; then
        echo "  • ${TARGET_NAMES[$i]}  (${TARGET_DIRS[$i]}/LankeBUILD.json)"
        missing_count=$((missing_count + 1))
    fi
done
if [ "$missing_count" -eq 0 ]; then
    echo "  (全部已填写 ✓)"
elif [ -z "$DEEPSEEK_API_KEY" ]; then
    echo
    echo -e "  设置 ${CYAN}DEEPSEEK_API_KEY${NC} 环境变量可自动填写 build_deps"
fi
echo

# ── 提示（仅手动模式）──
if [ -z "$DEEPSEEK_API_KEY" ]; then
    echo -e "${YELLOW}${BOLD}════════════════════════════════════════════════════════════${NC}"
    echo -e "${YELLOW}${BOLD}  操作提示${NC}"
    echo -e "${YELLOW}${BOLD}════════════════════════════════════════════════════════════${NC}"
    echo
    echo -e "  根据 ${CYAN}Arch makedepends${NC} 填写 LankeBUILD.json 的 ${GREEN}build_deps${NC} 字段:"
    echo
    echo "  1. 删掉 LankeOS 没有的依赖"
    echo "     (Arch 有但 LankeOS 仓库未打包的)"
    echo
    echo "  2. 删掉 LankeBUILD 里已关闭/条件编译禁用的依赖"
    echo "     (例如: --disable-foo, -Dfoo=disabled)"
    echo
    echo "  3. Python 系列的依赖不用管"
    echo
    echo "  4. 手动填写 LankeBUILD.json:"
    echo '     "build_deps": ["dep1", "dep2", ...]'
    echo
    echo "  批量编辑待处理包:"
    for i in "${!TARGET_NAMES[@]}"; do
        if [ "${PKG_HAS_BUILD_DEPS[$i]}" = "no" ]; then
            echo "    vim ${TARGET_DIRS[$i]}/LankeBUILD.json"
        fi
    done
    echo
fi
