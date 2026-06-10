#!/usr/bin/env bash
# build_manual.sh - docs/manual/*.md 를 단일 자기완결형 manual.html 로 빌드
#
# 사용: bash docs/build_manual.sh
# 환경변수 PANDOC 로 pandoc 경로 지정 가능.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PANDOC="${PANDOC:-pandoc}"
OUT="$HERE/manual.html"

"$PANDOC" "$HERE"/manual/*.md \
  --metadata-file="$HERE/meta.yaml" \
  --standalone \
  --toc --toc-depth=3 \
  --number-sections \
  --css="$HERE/assets/manual.css" \
  --embed-resources \
  --syntax-highlighting=tango \
  -o "$OUT"

echo "생성 완료: $OUT"
