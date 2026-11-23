#!/bin/bash
# 刷新参考项目克隆并打印快照，用于维护 docs/reference-forks.md 的跟踪表。
#
# 用法：
#   tools/refresh-reference-forks.sh            # 只 fetch + 打印快照
#   tools/refresh-reference-forks.sh --log      # 额外打印各仓库上次快照之后的提交标题
#
# 参考源根目录可用 REFERENCE_ROOT 覆盖，默认取工作区内的 reference/。

set -u

REFERENCE_ROOT="${REFERENCE_ROOT:-/Volumes/XWorkspaces/Workspaces/GitResources/opensource/reference}"
REPOS=(
  NootedRed-vithalchowdary
  NootedRed-weachy
  NootedRed-steve12311
  NootedRed-Kingtous
  NootedRed-xiaot-evo
)

WANT_LOG=0
[ "${1:-}" = "--log" ] && WANT_LOG=1

# GitHub 直连偶发失败，走本地代理；如未启动代理可注释掉这一段。
export https_proxy="${https_proxy:-http://127.0.0.1:7890}"
export http_proxy="${http_proxy:-http://127.0.0.1:7890}"
export all_proxy="${all_proxy:-socks5://127.0.0.1:7890}"

printf '%-46s %-9s %-8s %-22s %s\n' "克隆目录" "分支" "提交数" "HEAD" "最近提交日期"
for entry in "${REPOS[@]}"; do
  dir="$entry"
  path="$REFERENCE_ROOT/$dir"
  if [ ! -d "$path/.git" ]; then
    printf '%-46s %s\n' "$dir" "缺失（先 clone）"
    continue
  fi
  ( cd "$path" || exit 1
    git fetch --all --tags --quiet 2>/dev/null || echo "  ! fetch 失败，可能是代理未开"
    ref="origin/$(git symbolic-ref --short refs/remotes/origin/HEAD 2>/dev/null | sed 's#origin/##' || echo master)"
    printf '%-46s %-9s %-8s %-22s %s\n' \
      "$dir" "${ref#origin/}" "$(git rev-list --count "$ref" 2>/dev/null)" \
      "$(git log -1 --format=%h "$ref" 2>/dev/null)" \
      "$(git log -1 --format=%ad --date=short "$ref" 2>/dev/null)"
    if [ "$WANT_LOG" = "1" ]; then
      git log --no-merges --format='    - %h %ad %s' --date=short "HEAD..$ref" 2>/dev/null | head -30
    fi
  )
done

cat <<'EOF'

提示：
  - 分析完新提交后，把结论落进 docs/reference-forks.md 的「跟踪表」「决策明细」「更新日志」三张表。
  - 参考源只读，不向其写回；采纳项先进实验分支，实机验证后再进主线并打 tag。
EOF
