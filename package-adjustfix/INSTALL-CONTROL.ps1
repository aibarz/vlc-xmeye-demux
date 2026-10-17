param([string]$VlcDir = '')
$ErrorActionPreference='Stop'
$source=Join-Path $PSScriptRoot 'libxmeyecontrol_plugin.dll'
$hashFile=Join-Path $PSScriptRoot 'SHA256SUMS.txt'
if(!(Test-Path $source) -or !(Test-Path $hashFile)){throw 'Candidate DLL/checksum missing'}
$hash=([string](Get-Content $hashFile|Select-Object -First 1)).Substring(0,64).ToUpperInvariant()
if($hash -notmatch '^[A-F0-9]{64}$' -or (Get-FileHash $source).Hash -ne $hash){throw 'Invalid release hash'}
if(!$VlcDir){
 foreach($loc in @('D:\Program Files\VideoLAN.VLC','C:\Program Files\VideoLAN\VLC','D:\Workspace\vlc-3.0.24-xmeye-runtime')){
  if(Test-Path (Join-Path $loc 'vlc.exe')){$VlcDir=$loc;break}
 }
}
$exe=Join-Path $VlcDir 'vlc.exe'
$target=Join-Path $VlcDir 'plugins\control\libxmeyecontrol_plugin.dll'
$cache=Join-Path $VlcDir 'vlc-cache-gen.exe'
if(!(Test-Path $exe) -or !(Test-Path $cache)){throw 'VLC path invalid'}
if((Get-Item $exe).VersionInfo.FileVersion -notlike '3.0.24*'){throw 'VLC version must be 3.0.24'}
if(@(Get-CimInstance Win32_Process -Filter "name='vlc.exe'"|Where-Object {$_.ExecutablePath -ieq $exe}).Count){throw 'Close target VLC before replacing DLL'}
$backup=Join-Path (Join-Path $env:LOCALAPPDATA 'VLC-XMEye-Control-Fix\backups') (Get-Date -Format 'yyyyMMdd-HHmmss-fff')
New-Item $backup -ItemType Directory -Force |Out-Null
$had=Test-Path $target
if($had){Copy-Item $target (Join-Path $backup 'libxmeyecontrol_plugin.dll') -Force}
@("VlcDir=$VlcDir","Had=$had")| Set-Content (Join-Path $backup 'manifest.txt') -Encoding UTF8
try {
 Copy-Item $source $target -Force
 if((Get-FileHash $target).Hash -ne $hash){throw 'Installed checksum mismatch'}
 & $cache (Join-Path $VlcDir 'plugins')
 if($LASTEXITCODE -ne 0){throw 'VLC cache generation failed'}
 Write-Output ('INSTALLED='+$target)
 Write-Output ('BACKUP='+$backup)
} catch {
 if($had){Copy-Item (Join-Path $backup 'libxmeyecontrol_plugin.dll') $target -Force}
 elseif(Test-Path $target){Remove-Item $target -Force}
 & $cache (Join-Path $VlcDir 'plugins')
 throw
}
