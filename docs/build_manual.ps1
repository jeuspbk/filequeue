# build_manual.ps1 - docs/manual/*.md 를 단일 자기완결형 manual.html 로 빌드
#
# 사용: pwsh docs/build_manual.ps1
# 환경변수 PANDOC 로 pandoc 경로 지정 가능.
$ErrorActionPreference = 'Stop'

$here   = Split-Path -Parent $MyInvocation.MyCommand.Path
$pandoc = if ($env:PANDOC) { $env:PANDOC } else { 'pandoc' }
$out    = Join-Path $here 'manual.html'

$inputs = Get-ChildItem (Join-Path $here 'manual/*.md') |
          Sort-Object Name | ForEach-Object { $_.FullName }

& $pandoc $inputs `
  --metadata-file="$here/meta.yaml" `
  --standalone `
  --toc --toc-depth=3 `
  --number-sections `
  --css="$here/assets/manual.css" `
  --embed-resources `
  --syntax-highlighting=tango `
  -o $out

Write-Host "생성 완료: $out"
