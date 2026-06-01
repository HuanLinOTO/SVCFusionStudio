param(
  [string] $RepoId = "SVCFusion/things",
  [string] $ModelsDir = "Resources/models",
  [string[]] $DeletePatterns = @(
    "SVCFusionStudio-Models.7z",
    "hifisampler-models.7z"
  ),
  [int] $NumWorkers = 8,
  [switch] $KeepLegacyArchives
)

$ErrorActionPreference = "Stop"

if (-not (Get-Command uvx -ErrorAction SilentlyContinue)) {
  throw "uvx is not available in PATH"
}

$resolvedModelsDir = Resolve-Path -LiteralPath $ModelsDir -ErrorAction Stop

if (-not (Test-Path -LiteralPath $resolvedModelsDir -PathType Container)) {
  throw "Models directory not found: $ModelsDir"
}

Write-Host "Uploading model tree from $resolvedModelsDir to $RepoId"

& uvx hf repos create $RepoId --type model --exist-ok | Out-Host
if ($LASTEXITCODE -ne 0) {
  throw "Failed to ensure Hugging Face repo exists: $RepoId"
}

& uvx hf upload-large-folder $RepoId $resolvedModelsDir --repo-type model --num-workers $NumWorkers | Out-Host
if ($LASTEXITCODE -ne 0) {
  throw "Failed to upload models to $RepoId"
}

if (-not $KeepLegacyArchives -and $DeletePatterns.Count -gt 0) {
  Write-Host "Deleting legacy archive files from $RepoId"
  & uvx hf repos delete-files $RepoId @DeletePatterns --type model --commit-message "Remove legacy model archives" | Out-Host
  if ($LASTEXITCODE -ne 0) {
    throw "Failed to delete legacy archive files from $RepoId"
  }
}

Write-Host "Finished syncing models to $RepoId"
