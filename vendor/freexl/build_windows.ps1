param(
    [string]$OutputDirectory=(Join-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) 'build/freexl-runtime'),
    [string]$CompilerSetup=$env:PALEO_MSVC_SETUP,
    [string]$IconvDll='iconv-2.dll',
    [string]$CharsetDll='',
    [switch]$Trace
)
$ErrorActionPreference='Stop'
if($CompilerSetup){
    $setupPath=(Resolve-Path -LiteralPath $CompilerSetup).Path
    $lines=& cmd.exe /d /c "`"$setupPath`" >nul && set"
    if($LASTEXITCODE -ne 0){throw 'MSVC setup failed'}
    foreach($line in $lines){if($line -match '^([^=]+)=(.*)$'){[Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process')}}
}
foreach($tool in @('cl.exe','lib.exe','link.exe')){
    if(-not (Get-Command $tool -ErrorAction SilentlyContinue)){throw "Missing $tool. Use an x64 MSVC developer shell or pass -CompilerSetup <setup.bat>."}
}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$out=(Resolve-Path -LiteralPath $OutputDirectory).Path
$source=Join-Path $PSScriptRoot 'freexl-2.0.0'
$windows=Join-Path $PSScriptRoot 'windows'
foreach($name in @($IconvDll,$CharsetDll)){
    if($name -and $name -notmatch '^[A-Za-z0-9_.-]+\.dll$'){throw 'Runtime DLL names must be plain basenames.'}
}
$iconvExports=@("LIBRARY $IconvDll",'EXPORTS','libiconv','libiconv_close','libiconv_open')
if(-not $CharsetDll){$iconvExports+='locale_charset'}
$iconvDef=Join-Path $out 'iconv.def'
Set-Content -LiteralPath $iconvDef -Value $iconvExports -Encoding ascii
& lib.exe /nologo /machine:x64 "/def:$iconvDef" "/out:$out/iconv.lib"
if($LASTEXITCODE -ne 0){throw 'iconv import library failed'}
$charsetLibraries=@()
if($CharsetDll){
    $charsetDef=Join-Path $out 'charset.def'
    Set-Content -LiteralPath $charsetDef -Value @("LIBRARY $CharsetDll",'EXPORTS','locale_charset') -Encoding ascii
    & lib.exe /nologo /machine:x64 "/def:$charsetDef" "/out:$out/charset.lib"
    if($LASTEXITCODE -ne 0){throw 'charset import library failed'}
    $charsetLibraries+="$out/charset.lib"
}
$arguments=@('/nologo','/O2','/MD','/W3','/D_CRT_SECURE_NO_WARNINGS','/DDLL_EXPORT','/DOMIT_XMLDOC',"/I$source","/I$source/headers","/I$windows",'/c',"$source/src/freexl.c","/Fo$out/freexl.obj")
if($Trace){$arguments+='/DPALEO_FREEXL_TRACE'}
& cl.exe @arguments
if($LASTEXITCODE -ne 0){throw 'FreeXL compilation failed'}
& link.exe /nologo /dll "/out:$out/freexl.dll" "/implib:$out/freexl.lib" "$out/freexl.obj" "$out/iconv.lib" @charsetLibraries
if($LASTEXITCODE -ne 0){throw 'FreeXL link failed'}
Write-Output "Built $out/freexl.dll (XLS BIFF; XLSX uses the application's separate parser)."
Write-Output "At runtime, $IconvDll $CharsetDll must be accessible beside the application or on PATH."
