param([string]$VlcDir = '')
$ErrorActionPreference='Stop'
$root=Join-Path $env:LOCALAPPDATA 'VLC-XMEye-Control-Fix\backups'
$dirs=@(Get-ChildItem $root -Directory -ErrorAction Stop|Sort-Object Name -Descending)
$entry=$null
foreach($e in $dirs){
 $m=Join-Path $e.FullName 'manifest.txt'
 if(!(Test-Path $m)){continue}
 $data=Get-Content $m
 $found=$data|Where-Object{$_ -like 'VlcDir=*'}|Select-Object -First 1
 if(!$found){continue}
 $loc=$found.Substring(7)
 if(!$VlcDir -or $VlcDir -ieq $loc){$entry=$e;$VlcDir=$loc;break}
}
if(!$entry){throw 'No matching backup'}
$exe=Join-Path $VlcDir 'vlc.exe'
if(@(Get-CimInstance Win32_Process -Filter "name='vlc.exe'"|Where-Object{$_.ExecutablePath -ieq $exe}).Count){throw 'Close VLC before restoration'}
$target=Join-Path $VlcDir 'plugins\control\libxmeyecontrol_plugin.dll'
$data=Get-Content (Join-Path $entry.FullName 'manifest.txt')
if($data -contains 'Had=True'){Copy-Item (Join-Path $entry.FullName 'libxmeyecontrol_plugin.dll') $target -Force}
elseif(Test-Path $target){Remove-Item $target -Force}
& (Join-Path $VlcDir 'vlc-cache-gen.exe') (Join-Path $VlcDir 'plugins')
if($LASTEXITCODE -ne 0){throw 'Cache regeneration failed'}
Write-Output ('RESTORED='+$target)
