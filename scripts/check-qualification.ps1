param(
  [Parameter(Mandatory = $true)]
  [string]$Record,
  [switch]$RequireApproval
)

$ErrorActionPreference = "Stop"
$path = (Resolve-Path -LiteralPath $Record).Path
$record = Get-Content -Raw -LiteralPath $path | ConvertFrom-Json
if ($record.schema -ne 1 -or $record.release -ne "QLIC 1.0.1" -or
    $record.source_revision_kind -ne "qlic-source-tree-sha256-v1" -or
    $record.source_revision -notmatch '^[0-9a-fA-F]{64}$') {
  throw "Qualification record identity is invalid: $path"
}
$requiredApplications = @(
  "Explorer", "Windows Photos", "Adobe Photoshop",
  "Adobe Lightroom Classic", "Affinity Photo"
)
foreach ($name in $requiredApplications) {
  $matches = @($record.windows.applications | Where-Object { $_.name -eq $name })
  if ($matches.Count -ne 1) {
    throw "Qualification record must contain exactly one $name result."
  }
  $entry = $matches[0]
  if ($entry.status -notin @("pass", "fail", "pending") -or
      ($entry.status -eq "pass" -and
       (!$entry.version -or @($entry.evidence).Count -eq 0))) {
    throw "Qualification entry is incomplete: $name"
  }
  foreach ($evidence in @($entry.evidence)) {
    if (!$evidence.path -or $evidence.sha256 -notmatch '^[0-9a-fA-F]{64}$') {
      throw "Qualification evidence is malformed for $name."
    }
    $evidencePath = if ([IO.Path]::IsPathRooted($evidence.path)) {
      $evidence.path
    } else {
      Join-Path (Split-Path -Parent $path) $evidence.path
    }
    if (!(Test-Path -LiteralPath $evidencePath -PathType Leaf)) {
      throw "Qualification evidence is missing: $evidencePath"
    }
    $actual = (Get-FileHash -LiteralPath $evidencePath -Algorithm SHA256).Hash
    if ($actual -ne $evidence.sha256) {
      throw "Qualification evidence hash mismatch: $evidencePath"
    }
  }
}
$requiredWorkflows = @(
  "rgba8_straight", "rgba16_straight", "rgba16_premultiplied_tiff",
  "hdr10_pq_bt2020", "icc_color_managed", "exif_xmp_iptc_jumb",
  "orientation_and_resolution", "wic_install_upgrade_repair_uninstall"
)
foreach ($name in $requiredWorkflows) {
  if ($record.workflows.$name -notin @("pass", "fail", "pending")) {
    throw "Qualification workflow is missing or invalid: $name"
  }
}
if ($RequireApproval) {
  $badApplications = @($record.windows.applications |
    Where-Object { $_.status -ne "pass" })
  $badWorkflows = @($requiredWorkflows |
    Where-Object { $record.workflows.$_ -ne "pass" })
  if ($badApplications.Count -or $badWorkflows.Count -or
      !$record.approved -or !$record.approved_by -or
      $record.approved_at_utc -notmatch '^\d{4}-\d{2}-\d{2}T') {
    throw "Qualification record is not approved for a production release."
  }
}
Write-Host "QLIC qualification record validated: $path"
