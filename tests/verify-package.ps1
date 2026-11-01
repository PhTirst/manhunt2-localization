param(
    [Parameter(Mandatory)][string]$Archive,
    [Parameter(Mandatory)][string]$TestRoot
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archivePath = (Resolve-Path -LiteralPath $Archive).Path
$checksum = [IO.File]::ReadAllText($archivePath + '.sha256').Trim().Split(' ')[0]
if ($checksum -notmatch '^[a-fA-F0-9]{64}$' -or (Get-FileHash -LiteralPath $archivePath).Hash -ne $checksum) {
    throw 'Package SHA-256 mismatch'
}
$prefix = [IO.Path]::GetFileNameWithoutExtension($archivePath) + '/'
$expected = @(
    'mh2loc.exe', 'project.json', 'README.md', 'README.zh-CN.md', 'LICENSE', 'NOTICE.md', 'CHANGELOG.md',
    '00_capture_originals.bat', '01_build.bat', '02_install.bat', '03_restore.bat', '04_validate_json.bat',
    'docs/FORMAT.md', 'docs/SAFETY.md', 'docs/VERIFICATION.md', 'THIRD_PARTY/zlib-ng-LICENSE.md',
    'JSON/.gitkeep', 'TTF/font1/.gitkeep', 'TTF/font2/.gitkeep'
)
$directories = @('', 'docs/', 'THIRD_PARTY/', 'JSON/', 'TTF/', 'TTF/font1/', 'TTF/font2/')
$found = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$zip = [IO.Compression.ZipFile]::OpenRead($archivePath)
try {
    foreach ($entry in $zip.Entries) {
        if (!$entry.FullName.StartsWith($prefix, [StringComparison]::Ordinal)) {throw "Unexpected package root: $($entry.FullName)"}
        $relative = $entry.FullName.Substring($prefix.Length)
        if ($entry.FullName.EndsWith('/')) {
            if ($directories -cnotcontains $relative) {throw "Unexpected package directory: $relative"}
        } else {
            if ($expected -cnotcontains $relative -or !$found.Add($relative)) {throw "Unexpected/duplicate package file: $relative"}
        }
    }
    foreach ($relative in $expected) {if (!$found.Contains($relative)) {throw "Missing package file: $relative"}}
} finally {$zip.Dispose()}

$extract = Join-Path ([IO.Path]::GetFullPath($TestRoot)) ([Guid]::NewGuid().ToString('N'))
if (Test-Path -LiteralPath $extract) {throw 'Package test directory must be new'}
Expand-Archive -LiteralPath $archivePath -DestinationPath $extract
$toolRoot = Join-Path $extract $prefix.TrimEnd('/')
& (Join-Path $PSScriptRoot 'integration.ps1') -Executable (Join-Path $toolRoot 'mh2loc.exe') `
    -Distribution $toolRoot -TestRoot (Join-Path $extract 'smoke-tests')
Write-Output "Package whitelist, checksum and extracted BAT tests passed: $archivePath"
