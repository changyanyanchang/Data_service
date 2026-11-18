param(
    [string]$BuildDirectory=(Join-Path $PSScriptRoot 'build'),
    [string]$PackageName='PaleoDataService-Windows-x64',
    [string]$ExampleBuildDirectory=(Join-Path $PSScriptRoot 'build-examples'),
    [string]$Configuration='Release',
    [string]$QtRoot=$env:QT_ROOT,
    [string]$DependencyRoot=$env:PALEO_DEPENDENCY_ROOT,
    [string[]]$RuntimeDirectories=@(),
    [string]$CompilerSetup=$env:PALEO_MSVC_SETUP,
    [string]$Dumpbin='dumpbin.exe',
    [string]$CMake='cmake'
)
$ErrorActionPreference='Stop'
if($PackageName -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]+$'){throw 'Use a simple package directory name.'}
$packageRoot=Join-Path $PSScriptRoot ('dist/'+$PackageName)
if(Test-Path -LiteralPath $packageRoot){throw 'Package directory already exists; choose another PackageName.'}
$appDirectory=Join-Path $packageRoot 'app'
& (Join-Path $PSScriptRoot 'package.ps1') -BuildDirectory $BuildDirectory -OutputDirectory $appDirectory -ExampleBuildDirectory $ExampleBuildDirectory -Configuration $Configuration -QtRoot $QtRoot -DependencyRoot $DependencyRoot -RuntimeDirectories $RuntimeDirectories -CompilerSetup $CompilerSetup -Dumpbin $Dumpbin
foreach($folder in @('docs','logs')){New-Item -ItemType Directory -Path (Join-Path $packageRoot $folder) | Out-Null}
Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot 'packaging/portable') -File | ForEach-Object {
    $content=[IO.File]::ReadAllText($_.FullName).Replace("`r`n","`n").Replace("`n","`r`n")
    [IO.File]::WriteAllText((Join-Path $packageRoot $_.Name),$content,[Text.UTF8Encoding]::new($false))
}
foreach($document in @('requirements.md','api-demo.md','cpp-reading-guide.md','portable-testing.md','segyio-guide.md','hd-seismic.md','time-slice-performance.md','import-performance.md','data-model.html')){
    $file=Join-Path $PSScriptRoot ('docs/'+$document)
    if(Test-Path -LiteralPath $file){Copy-Item -LiteralPath $file -Destination (Join-Path $packageRoot 'docs')}
}
$readme=Join-Path $PSScriptRoot 'docs/portable-testing.md'
if(Test-Path -LiteralPath $readme){Copy-Item -LiteralPath $readme -Destination (Join-Path $packageRoot '先读我.txt')}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'include') -Destination $packageRoot -Recurse
# Install headers/libraries from this exact build, rather than copying a possibly stale SDK.
& $CMake --install $BuildDirectory --config $Configuration --prefix (Join-Path $packageRoot 'sdk')
if($LASTEXITCODE -ne 0){throw 'SDK installation failed'}
$sourceDirectory=Join-Path $packageRoot 'source';New-Item -ItemType Directory -Path $sourceDirectory | Out-Null
foreach($folder in @('include','src','demo','tests','tools','vendor','cmake','examples','packaging')){
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $folder) -Destination $sourceDirectory -Recurse
}
foreach($file in @('CMakeLists.txt','README.md','AGENTS.md','build.ps1','build-examples.ps1','package.ps1','package-portable.ps1')){
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $file) -Destination $sourceDirectory
}
New-Item -ItemType Directory -Path (Join-Path $sourceDirectory 'docs') | Out-Null
Get-ChildItem -LiteralPath (Join-Path $packageRoot 'docs') -File | ForEach-Object {Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $sourceDirectory 'docs')}
$freexl=Join-Path $appDirectory 'paleo_freexl.dll'
if(-not (Test-Path -LiteralPath $freexl)){ $freexl=Join-Path $appDirectory 'freexl.dll' }
$manifest=[ordered]@{
    name=$PackageName;version='0.1.0';architecture='Windows x64';packagedAt=(Get-Date).ToString('o')
    seismicEngine='equinor/segyio v2.0.0-alpha.1 with documented local patches'
    seismicSourcePolicy='External read-only; never copy amplitude volumes'
    entry='启动程序.cmd';syntheticEntry='打开合成示例.cmd';checkEntry='运行自检.cmd'
    syntheticProject='Generated on demand; no dataset is bundled'
    realData='Not included'
    sourceModelSha256=(Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'include/paleo_data/model.h') -Algorithm SHA256).Hash
    executableSha256=(Get-FileHash -LiteralPath (Join-Path $appDirectory 'paleo_data_demo.exe') -Algorithm SHA256).Hash
    freexlSha256=(Get-FileHash -LiteralPath $freexl -Algorithm SHA256).Hash
}
$manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $packageRoot 'package-info.json') -Encoding utf8
Write-Output "Portable folder prepared: $packageRoot"
Write-Output 'Run the included self-check before creating a delivery ZIP. This script does not add datasets.'
