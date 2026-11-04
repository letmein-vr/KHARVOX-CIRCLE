$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$catalogPath=Join-Path $root 'profiles/motor-builds.json'
$catalog=Get-Content -LiteralPath $catalogPath -Raw | ConvertFrom-Json
$header=Get-Content -LiteralPath (Join-Path $root 'src/motor/build_profiles.generated.hpp') -Raw
$hasher=[Security.Cryptography.SHA256]::Create()
try {$hash=([BitConverter]::ToString($hasher.ComputeHash([IO.File]::ReadAllBytes($catalogPath)))).Replace('-','')} finally {$hasher.Dispose()}
if($header -notmatch "(?i)Catalog SHA256: $hash") {throw 'Build catalog changed: run tools/generate-build-profiles.py before building.'}
$registered=@{}
$steam=@{}
$gamepass=@{}
$gamepassSize=[Convert]::ToInt64((@($catalog.profiles | Where-Object id -eq 'GamePass170')[0].imageSize).Substring(2),16)
foreach($entry in $catalog.addresses.PSObject.Properties){
    $key=[Convert]::ToInt64($entry.Name.Substring(2),16)
    $target=[Convert]::ToInt64($entry.Value.steam.Substring(2),16)
    if($key -lt 0x400000 -or $key -ge 0x746e000 -or $target -lt 0x400000 -or $target -ge 0x74ee000){throw "Out-of-image address: $($entry.Name)"}
    if($steam.ContainsKey($target)){throw "Duplicate Steam address: $($entry.Value.steam)"}
    $registered[$key]=$true; $steam[$target]=$true
    foreach($field in @('gogBytes','steamBytes')){if($entry.Value.$field -notmatch '^[a-fA-F0-9]{64}$'){throw "Missing fingerprint: $($entry.Name) $field"}}
    # Game Pass: null = not present in that build. Every hook site needs an address and its own fingerprint.
    if($null -eq $entry.Value.gamepass){if($entry.Value.code){throw "Missing Game Pass address: $($entry.Name)"}}
    else{
        $gp=[Convert]::ToInt64($entry.Value.gamepass.Substring(2),16)
        if($gp -lt 0x400000 -or $gp -ge $gamepassSize){throw "Out-of-image Game Pass address: $($entry.Name)"}
        if($gamepass.ContainsKey($gp)){throw "Duplicate Game Pass address: $($entry.Value.gamepass)"}
        $gamepass[$gp]=$true
        if($entry.Value.code -and $entry.Value.gamepassBytes -notmatch '^[a-fA-F0-9]{64}$'){throw "Missing fingerprint: $($entry.Name) gamepassBytes"}
    }
}
$files=@(Get-ChildItem -LiteralPath (Join-Path $root 'src/motor') -File | Where-Object {$_.Extension -in '.cpp','.hpp','.inl' -and $_.Name -notlike '*_test.cpp' -and $_.Name -notlike 'build_*'})
$files+=Get-Item -LiteralPath (Join-Path $root 'src/circlevr/src/vkcore/ui_engine.cpp')
foreach($file in $files){
    foreach($line in Get-Content -LiteralPath $file.FullName){
        if($line.TrimStart().StartsWith('//')){continue}
        foreach($literal in [regex]::Matches($line,'0x[0-9a-fA-F]+')){
            $value=[Convert]::ToInt64($literal.Value.Substring(2),16)
            # Render-state structure member, not an image RVA. The checked
            # C1E8C6/C35AE6 instruction stores options+15 at RBX+4d4265 in both builds.
            if($file.Name -eq 'sequential.cpp' -and $value -eq 0x4d4250 -and $line -match 'r\.rbx\+0x4d4250'){continue}
            if($value -ge 0x400000 -and $value -lt 0x746e000 -and !$registered.ContainsKey($value)){throw "Unregistered Motor address $($literal.Value) in $($file.Name); validate every supported build."}
        }
        if($line -match '\b(?:base|base_|g_base)\s*\+\s*0x[0-9a-fA-F]{6,}') {throw "Raw executable address in $($file.Name): use build::rva()."}
    }
}
Write-Output "Build catalog synchronized; $($registered.Count) addresses covered for GOG and Steam; $($gamepass.Count) for Game Pass."
