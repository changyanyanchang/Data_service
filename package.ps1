param(
    [string]$BuildDirectory=(Join-Path $PSScriptRoot 'build'),
    [string]$OutputDirectory=(Join-Path $PSScriptRoot 'dist/PaleoDataService'),
    [string]$ExampleBuildDirectory=(Join-Path $PSScriptRoot 'build-examples'),
    [string]$Configuration='Release',
    [string]$QtRoot=$env:QT_ROOT,
    [string]$DependencyRoot=$env:PALEO_DEPENDENCY_ROOT,
    [string[]]$RuntimeDirectories=@(),
    [string]$CompilerSetup=$env:PALEO_MSVC_SETUP,
    [string]$Dumpbin='dumpbin.exe'
)
$ErrorActionPreference='Stop'
if($CompilerSetup){
    $setupPath=(Resolve-Path -LiteralPath $CompilerSetup).Path
    $lines=& cmd.exe /d /c "`"$setupPath`" >nul && set"
    if($LASTEXITCODE -ne 0){throw 'Compiler environment initialization failed'}
    foreach($line in $lines){if($line -match '^([^=]+)=(.*)$'){[Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process')}}
}
if(-not $QtRoot){
    $qtTool=Get-Command windeployqt.exe -ErrorAction SilentlyContinue
    if(-not $qtTool){throw 'Pass -QtRoot <Qt installation> or put windeployqt.exe on PATH.'}
    $QtRoot=Split-Path (Split-Path $qtTool.Source -Parent) -Parent
}
$deploy=Join-Path $QtRoot 'bin/windeployqt.exe'
if(-not (Test-Path -LiteralPath $deploy)){throw "Qt deployment tool not found: $deploy"}
if(-not (Get-Command $Dumpbin -ErrorAction SilentlyContinue)){throw 'Use an MSVC developer shell, -CompilerSetup, or -Dumpbin <path>.'}
$binaryDirectory=$BuildDirectory
if(-not (Test-Path -LiteralPath (Join-Path $binaryDirectory 'paleo_data_demo.exe'))){$binaryDirectory=Join-Path $BuildDirectory $Configuration}
if(-not (Test-Path -LiteralPath (Join-Path $binaryDirectory 'paleo_data_demo.exe'))){throw 'Build paleo_data_demo before packaging.'}
if(Test-Path -LiteralPath $OutputDirectory){throw 'Choose a new OutputDirectory; existing package contents are never merged.'}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$OutputDirectory=(Resolve-Path -LiteralPath $OutputDirectory).Path
$searchDirs=@($OutputDirectory,(Join-Path $QtRoot 'bin'))+$RuntimeDirectories
if($DependencyRoot){$searchDirs+=(Join-Path $DependencyRoot 'bin')}
$systemRoot=[IO.Path]::GetFullPath($env:WINDIR).TrimEnd('\')
$searchDirs+=($env:PATH -split ';' | Where-Object {$_ -and (Test-Path -LiteralPath $_ -PathType Container) -and -not ([IO.Path]::GetFullPath($_).TrimEnd('\').StartsWith($systemRoot,[StringComparison]::OrdinalIgnoreCase))})
$env:PATH=($searchDirs -join ';')+';'+$env:PATH
foreach($name in @('paleo_data_demo.exe','paleo_data_cli.exe','facies_feature_example.exe','service_tests.exe','aux_parser_tests.exe','text_parser_tests.exe','seismic_service_tests.exe','seismic_hd_tests.exe','timeslice_batch_tests.exe','timeslice_benchmark.exe','create_api_demo_project.exe','api_demo_tests.exe','spreadsheet_xml_tests.exe','core_asset_tests.exe','format_compatibility_tests.exe','image_preview_tests.exe','segyio_access_tests.exe','seismic_processing_tests.exe','seismic_ui_tests.exe','segyio_bounds_tests.exe','segyio_acceptance.exe','import_performance_tests.exe','file_digest_tests.exe')){
    $file=Join-Path $binaryDirectory $name
    if(Test-Path -LiteralPath $file){Copy-Item -LiteralPath $file -Destination $OutputDirectory}
}
$exampleDirectory=$ExampleBuildDirectory
if(Test-Path -LiteralPath (Join-Path $exampleDirectory $Configuration)){$exampleDirectory=Join-Path $exampleDirectory $Configuration}
if(Test-Path -LiteralPath $exampleDirectory){Get-ChildItem -LiteralPath $exampleDirectory -Filter 'example_*.exe' -File | ForEach-Object {Copy-Item -LiteralPath $_.FullName -Destination $OutputDirectory}}
$deployMode=if($Configuration -eq 'Debug'){'--debug'}else{'--release'}
& $deploy $deployMode --compiler-runtime --no-translations --no-opengl-sw --dir $OutputDirectory (Join-Path $OutputDirectory 'paleo_data_demo.exe')
if($LASTEXITCODE -ne 0){throw 'Qt runtime deployment failed'}
$offscreen=Join-Path $QtRoot 'plugins/platforms/qoffscreen.dll'
if(Test-Path -LiteralPath $offscreen){Copy-Item -LiteralPath $offscreen -Destination (Join-Path $OutputDirectory 'platforms/qoffscreen.dll') -Force}
$freexl=Join-Path $binaryDirectory 'paleo_freexl.dll'
if(-not (Test-Path -LiteralPath $freexl)){ $freexl=Join-Path $binaryDirectory 'freexl.dll' }
if(-not (Test-Path -LiteralPath $freexl)){throw 'Build the maintained FreeXL runtime first (build.ps1 includes it by default on Windows).'}
Copy-Item -LiteralPath $freexl -Destination $OutputDirectory
# Qt's Windows ICU build imports the OS's unsuffixed ICU API. A standalone ICU
# distribution on PATH can use the same icuuc.dll filename but only export names
# such as ucnv_open_78. Copying it beside the EXE shadows System32 and prevents
# startup (0xc0000139). Keep Windows ICU in System32, and verify each consumer;
# reject a plugin needing another ICU ABI instead of silently mixing the two.
function Assert-WindowsIcuImports([string]$Binary,[string]$Dll,[string]$SystemDll){
    if(-not (Test-Path -LiteralPath $SystemDll)){
        throw "Windows ICU dependency is unavailable: $Dll. This Qt runtime requires Windows with the system ICU API."
    }
    $symbols=[System.Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    $exportLines=& $Dumpbin /nologo /exports $SystemDll
    if($LASTEXITCODE -ne 0){throw "Cannot inspect Windows ICU exports: $SystemDll"}
    foreach($entry in $exportLines){
        # Include forwarders (System32/icuuc.dll forwards its exports to icu.dll).
        if($entry -match '^\s+\d+\s+[0-9A-F]+\s+(?:[0-9A-F]+\s+)?(\S+)'){
            [void]$symbols.Add($Matches[1])
        }
    }
    $importLines=& $Dumpbin /nologo /imports $Binary
    if($LASTEXITCODE -ne 0){throw "Cannot inspect ICU imports: $Binary"}
    $selected=$false
    foreach($entry in $importLines){
        if($entry -match '^    (\S+\.dll)\s*$'){$selected=$Matches[1] -ieq $Dll}
        elseif($entry -match '^  Summary'){$selected=$false}
        elseif($selected -and $entry -match '^\s{8,}[0-9A-F]+\s+(\S+)\s*$'){
            $symbol=$Matches[1]
            if(-not $symbols.Contains($symbol)){
                throw "Incompatible ICU dependency: $Binary imports $Dll!$symbol, which Windows ICU does not export. Use libraries built for the same ICU ABI; do not bundle a same-name DLL over Windows ICU."
            }
        }
    }
}
# Inspect each PE import, copying required non-system dependencies from explicit roots/PATH.
$queue=[System.Collections.Generic.Queue[string]]::new()
Get-ChildItem -LiteralPath $OutputDirectory -File -Recurse | Where-Object { $_.Extension -in @('.dll','.exe') } | ForEach-Object {$queue.Enqueue($_.FullName)}
$visited=[System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
while($queue.Count){
    $binary=$queue.Dequeue();if(-not $visited.Add($binary)){continue}
    $imports=& $Dumpbin /nologo /dependents $binary
    if($LASTEXITCODE -ne 0){throw "Cannot inspect dependencies: $binary"}
    foreach($line in $imports){
        if($line -notmatch '^\s+([A-Za-z0-9_.-]+\.dll)\s*$'){continue}
        $dll=$Matches[1];$located=$null
        if($dll.ToLowerInvariant() -in @('icu.dll','icuuc.dll','icuin.dll')){
            Assert-WindowsIcuImports -Binary $binary -Dll $dll -SystemDll (Join-Path $env:WINDIR ('System32/'+$dll))
            if(Test-Path -LiteralPath (Join-Path $OutputDirectory $dll)){
                throw "The deployment step bundled $dll, which would shadow Windows ICU. Use the system-ICU Qt build without a local replacement."
            }
            continue
        }
        foreach($dir in $searchDirs){$candidate=Join-Path $dir $dll;if(Test-Path -LiteralPath $candidate){$located=$candidate;break}}
        if($located){
            $target=Join-Path $OutputDirectory $dll
            if([IO.Path]::GetFullPath($located) -ne [IO.Path]::GetFullPath($target)){Copy-Item -LiteralPath $located -Destination $target -Force}
            $queue.Enqueue($target)
        }elseif($dll -notlike 'api-ms-*' -and $dll -notlike 'ext-ms-*' -and -not (Test-Path -LiteralPath (Join-Path $env:WINDIR ('System32/'+$dll)))){
            throw "Dependency not found: $dll. Add its directory with -RuntimeDirectories."
        }
    }
}
@('[Paths]','Prefix=.','Plugins=.','Translations=translations') | Set-Content -LiteralPath (Join-Path $OutputDirectory 'qt.conf') -Encoding utf8
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'README.md') -Destination (Join-Path $OutputDirectory '使用说明.md')
$docsOutput=Join-Path $OutputDirectory 'docs';New-Item -ItemType Directory -Path $docsOutput | Out-Null
foreach($document in @('api-demo.md','cpp-reading-guide.md','segyio-guide.md','hd-seismic.md','time-slice-performance.md','import-performance.md','portable-testing.md')){
    $file=Join-Path $PSScriptRoot ('docs/'+$document)
    if(Test-Path -LiteralPath $file){Copy-Item -LiteralPath $file -Destination $docsOutput}
}
$examplesOutput=Join-Path $OutputDirectory 'examples';New-Item -ItemType Directory -Path $examplesOutput | Out-Null
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'examples/getting_started') -Destination $examplesOutput -Recurse
foreach($notice in @('sbm/README.md','sbm/PATCHES.md','segyio/License.md','segyio/PATCHES.md','glm/copying.txt','freexl/NOTICE.txt','freexl/freexl-2.0.0/COPYING','freexl/LGPL-2.1.txt','paleo_io/NOTICE.txt')){
    $destination=Join-Path $OutputDirectory ('third-party-notices/'+$notice)
    New-Item -ItemType Directory -Path (Split-Path $destination -Parent) -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot ('vendor/'+$notice)) -Destination $destination
}
# Corresponding maintained LGPL source is shipped with the runtime.
$thirdParty=Join-Path $OutputDirectory 'third-party-source';New-Item -ItemType Directory -Path $thirdParty | Out-Null
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'vendor/freexl') -Destination $thirdParty -Recurse
Write-Output "Runtime package ready: $OutputDirectory"
