param([Parameter(Mandatory)][string]$Executable, [Parameter(Mandatory)][string]$Distribution, [Parameter(Mandatory)][string]$TestRoot)
$ErrorActionPreference = 'Stop'
$root = Join-Path $TestRoot ([Guid]::NewGuid().ToString('N') + ' path !')
New-Item -ItemType Directory -Path $root -Force | Out-Null
Copy-Item -LiteralPath $Executable -Destination (Join-Path $root 'mh2loc.exe')
Copy-Item -Path (Join-Path $Distribution '*') -Destination $root -Recurse -Force
New-Item -ItemType Directory -Path (Join-Path $root 'JSON') -Force | Out-Null
[IO.File]::WriteAllText((Join-Path $root 'JSON\example.json'), '[{"Key":"KEY","Extra":"0x1234","en_US":"Hello","fr_FR":"Bonjour"}]', [Text.UTF8Encoding]::new($false))
$config = Get-Content -LiteralPath (Join-Path $root 'project.json') -Raw | ConvertFrom-Json
$config.language = 'fr_FR'
[IO.File]::WriteAllText((Join-Path $root 'project.json'), ($config | ConvertTo-Json -Depth 10), [Text.UTF8Encoding]::new($false))
$env:MH2LOC_NO_PAUSE = '1'
$script:invocations = 0
function Invoke-Batch([string]$Name, [bool]$Success) {
    $script:invocations++
    $stdout = Join-Path $root "$script:invocations.stdout"
    $stderr = Join-Path $root "$script:invocations.stderr"
    $command = '/d /s /c ""' + (Join-Path $root $Name) + '""'
    $process = Start-Process -FilePath $env:ComSpec -ArgumentList $command -WorkingDirectory $env:TEMP `
        -WindowStyle Hidden -Wait -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    $output = [IO.File]::ReadAllText($stdout, [Text.Encoding]::UTF8) + [IO.File]::ReadAllText($stderr, [Text.Encoding]::UTF8)
    if (($process.ExitCode -eq 0) -ne $Success) {throw "Unexpected exit $($process.ExitCode) from $Name`: $output"}
    Write-Output $output
}
Push-Location $env:TEMP
try {
    & (Join-Path $root 'mh2loc.exe') --version
    if($LASTEXITCODE -ne 0){throw 'Version failed'}
    & (Join-Path $root 'mh2loc.exe') --help
    if($LASTEXITCODE -ne 0){throw 'Help failed'}
    Invoke-Batch '04_validate_json.bat' $true
    [IO.File]::WriteAllText((Join-Path $root 'JSON\example.json'), "[`n{`"Key`":`"BAD`",`"fr_FR`":`"x`",}`n]", [Text.UTF8Encoding]::new($false))
    Invoke-Batch '04_validate_json.bat' $false
    foreach($bat in @('00_capture_originals.bat','01_build.bat','02_install.bat','03_restore.bat')) {
        Invoke-Batch $bat $false
    }
    if(Test-Path -LiteralPath (Join-Path $root 'dist')){throw 'Failed build created a partial dist'}
} finally {Pop-Location; Remove-Item Env:MH2LOC_NO_PAUSE -ErrorAction SilentlyContinue}
Write-Output 'CLI and double-click BAT workflow passed'
