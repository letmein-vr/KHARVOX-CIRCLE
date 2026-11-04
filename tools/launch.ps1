[CmdletBinding()]
param(
    [ValidateSet('cinema','attach','off','diagnostic')][string]$Mode = 'cinema',
    [string]$GameDirectory,
    [ValidateSet('auto','vdxr','active','meta')][string]$Runtime = 'auto',
    [string]$RuntimeJson,
    [Alias('skip_logo_intros')][bool]$SkipLogoIntros = $true,
    [switch]$CaptureShaders,
    [int]$CaptureStartFrame = 2400,
    [int]$CaptureFrameCount = 2,
    [int]$MonitorSeconds = 0,
    [switch]$StopAfterMonitor,
    [switch]$ExperimentalStereo,
    [Alias('LiveStereo')][switch]$Stereo,
    [switch]$DiagnosticAER,
    [switch]$ExperimentalHandIK,
    [switch]$ExperimentalPunch,
    [switch]$ExperimentalTAA,
    [switch]$ExperimentalDLSS,
    [switch]$ExperimentalDLSSRR,
    [switch]$DiagnosticSyncEveryFrame,
    [switch]$DiagnosticFramegenGpu,
    [switch]$DiagnosticMotionCapture,
    [switch]$DiagnosticGpuBindings,
    [switch]$SimulatorDesktopInput,
    [switch]$DiagnosticTemporalImages,
    [ValidateRange(2,32)][int]$TemporalImagePairs = 2,
    [switch]$ExperimentalSecondaryCache,
    [switch]$DiagnosticDisableRayTracing,
    [switch]$DiagnosticGiHistory,
    [switch]$ExperimentalGiHistory,
    [switch]$ExperimentalNrdHistory,
    [ValidateSet('smooth','snap')][string]$TurnMode = 'smooth',
    [ValidateRange(10,90)][double]$SnapAngle = 45,
    [ValidateRange(10,360)][double]$SmoothTurnSpeed = 180,
    [ValidateRange(0.5,2.0)][double]$RenderScale = 1.0,
    [ValidateSet('immersive','phase5')][string]$VRProjection = 'immersive',
    [bool]$StereoCinematics = $false,
    [bool]$MonoVR = $false,
    [bool]$EnableLogging = $false,
    [bool]$OnlySubmitRealFrames = $false,
    [bool]$VrDlssFrameGeneration = $false,
    [bool]$VrFsrFrameGeneration = $false,
    [ValidateSet("auto","official","community")][string]$VrFgBackend="auto",
    [ValidateSet('auto','auto-fixed','2','3','4')][string]$VrFgMultiplier='2',
    [bool]$HandsOnly = $false,
    [bool]$LaserSight = $false,
    [bool]$PerformanceOverlay = $false,
    [ValidateSet('game','native','off','dlss-sr','dlss-rr','fsr')][string]$Upscaling = 'off',
    [string]$SettingsFile,
    [string]$LaunchCancelEvent,
    [string]$PackageRoot
)
$ErrorActionPreference = 'Stop'
# Microsoft Store / Game Pass installs. The folder is a registered package whose executable is
# encrypted on disk: started directly it only relaunches itself through GameLaunchHelper, without
# this process's environment or arguments. Package activation forwards arguments, and the game
# receives the user environment from the registry, so the launch environment is placed there for
# the moment of activation and the previous values are restored as soon as the game process exists.
function Resolve-CircleStorePackage([string]$GameDirectory) {
    $manifest = Join-Path $GameDirectory 'appxmanifest.xml'
    if (!(Test-Path -LiteralPath $manifest -PathType Leaf)) { throw 'Microsoft Store install without appxmanifest.xml.' }
    $identity = ([xml](Get-Content -LiteralPath $manifest -Raw)).Package.Identity
    $package = @(Get-AppxPackage -Name $identity.Name -ErrorAction SilentlyContinue | Where-Object { [string]$_.Version -eq $identity.Version })
    if ($package.Count -ne 1) { throw "Microsoft Store package $($identity.Name) $($identity.Version) is not registered for this user. Install or repair it in the Xbox app." }
    # The registered location reaches the install folder through junctions (WindowsApps -> drive WindowsApps -> folder).
    $wanted = [IO.Path]::GetFullPath($GameDirectory).TrimEnd('\'); $at = $package[0].InstallLocation
    for ($i = 0; $i -lt 4 -and $at; $i++) {
        if ([IO.Path]::GetFullPath($at).TrimEnd('\') -ieq $wanted) { return $package[0] }
        $item = Get-Item -LiteralPath $at -ErrorAction SilentlyContinue
        $at = if ($item -and $item.LinkType -eq 'Junction') { @($item.Target)[0] } else { $null }
    }
    throw "Microsoft Store package $($identity.Name) is registered at $($package[0].InstallLocation), not at $GameDirectory."
}
function Start-CircleStorePackage($Package, [string]$AppId, [string]$Arguments, [Collections.Specialized.StringDictionary]$Environment) {
    if (!('CircleStoreActivation' -as [type])) {
        Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices;
[ComImport, Guid("2e941141-7f97-4756-ba1d-9decde894a3d"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface ICircleApplicationActivationManager { int ActivateApplication(string appUserModelId, string arguments, int options, out uint processId); }
[ComImport, Guid("45BA127D-10A8-46EA-8AB7-56EA9078943C")] class CircleApplicationActivationManager {}
public static class CircleStoreActivation {
    public static uint Activate(string appUserModelId, string arguments) {
        var manager = (ICircleApplicationActivationManager)new CircleApplicationActivationManager(); uint pid;
        int hr = manager.ActivateApplication(appUserModelId, arguments, 0, out pid);
        if (hr != 0) throw new COMException("Package activation failed for " + appUserModelId, hr);
        return pid;
    }
}
'@
    }
    $prefix = '^(CIRCLE_|KHARVOX_CIRCLE_|ARGENT_)'
    $wanted = [Collections.Specialized.OrderedDictionary]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($name in @($Environment.Keys | Sort-Object)) { if ($name -match $prefix -or $name -in @('XR_RUNTIME_JSON','VK_ADD_IMPLICIT_LAYER_PATH')) { $wanted[$name.ToUpperInvariant()] = $Environment[$name] } }
    $restorePath = Join-Path $env:LOCALAPPDATA 'KHARVOX CIRCLE\store-launch-environment.json'
    $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Environment', $true)
    try {
        # A launch interrupted before its restore left its snapshot behind: put those values back first.
        if (Test-Path -LiteralPath $restorePath) { Restore-CircleUserEnvironment $key (Get-Content -LiteralPath $restorePath -Raw | ConvertFrom-Json); Remove-Item -LiteralPath $restorePath }
        $touched = @($key.GetValueNames() | Where-Object { $_ -match $prefix }) + @($wanted.Keys) | Sort-Object -Unique
        $snapshot = [ordered]@{}
        foreach ($name in $touched) { $snapshot[$name] = if ($null -ne $key.GetValue($name, $null, 'DoNotExpandEnvironmentNames')) { [ordered]@{ kind = [string]$key.GetValueKind($name); value = [string]$key.GetValue($name, $null, 'DoNotExpandEnvironmentNames') } } else { $null } }
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $restorePath) | Out-Null
        [pscustomobject]$snapshot | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath $restorePath -Encoding utf8
        try {
            foreach ($name in $touched) { if ($wanted.Contains($name)) { $key.SetValue($name, [string]$wanted[$name], [Microsoft.Win32.RegistryValueKind]::String) } elseif ($null -ne $snapshot[$name]) { $key.DeleteValue($name) } }
            $helper = [CircleStoreActivation]::Activate("$($Package.PackageFamilyName)!$AppId", $Arguments.Trim())
            $deadline = [DateTime]::UtcNow.AddSeconds(60); $game = $null
            while (!$game -and [DateTime]::UtcNow -lt $deadline) {
                $game = @(Get-CimInstance Win32_Process -Filter "Name='TheGreatCircle.exe' AND ParentProcessId=$helper" -ErrorAction SilentlyContinue)[0]
                if (!$game) { Start-Sleep -Milliseconds 200 }
            }
            if (!$game) { throw 'The Microsoft Store package started but TheGreatCircle.exe did not appear within 60 seconds.' }
            return Get-Process -Id $game.ProcessId
        } finally {
            Restore-CircleUserEnvironment $key ([pscustomobject]$snapshot)
            Remove-Item -LiteralPath $restorePath -ErrorAction SilentlyContinue
        }
    } finally { $key.Close() }
}
function Restore-CircleUserEnvironment([Microsoft.Win32.RegistryKey]$Key, $Snapshot) {
    foreach ($entry in $Snapshot.PSObject.Properties) {
        if ($null -eq $entry.Value) { if ($null -ne $Key.GetValue($entry.Name)) { $Key.DeleteValue($entry.Name) } }
        else { $Key.SetValue($entry.Name, [string]$entry.Value.value, [Microsoft.Win32.RegistryValueKind]$entry.Value.kind) }
    }
}
$root = if($PackageRoot){[IO.Path]::GetFullPath($PackageRoot)}else{Split-Path -Parent $PSScriptRoot}
$settings=$null
if (!$SettingsFile) {
    if(!(Get-Command Resolve-CircleSettingsPath -ErrorAction SilentlyContinue)){. (Join-Path $PSScriptRoot 'settings-path.ps1')}
    $SettingsFile=Resolve-CircleSettingsPath -Root $root
}
if (Test-Path -LiteralPath $SettingsFile) {
    $settings=Get-Content -LiteralPath $SettingsFile -Raw | ConvertFrom-Json
    if(!$PSBoundParameters.ContainsKey('VrFgBackend') -and $null -ne $settings.vr_fg_backend){
        if($settings.vr_fg_backend -notin @('auto','official','community')){throw 'vr_fg_backend must be auto, official or community.'}
        $VrFgBackend=[string]$settings.vr_fg_backend
    }
    if(!$PSBoundParameters.ContainsKey('VrFgMultiplier') -and $null -ne $settings.vr_fg_multiplier){
        if($settings.vr_fg_multiplier -is [bool] -or [string]$settings.vr_fg_multiplier -notin @('adaptive','auto','auto-fixed','2','2.2','2.4','2.6','2.8','3','3.2','3.4','3.6','3.8','4')){throw 'vr_fg_multiplier must be auto (Auto Seamless), auto-fixed (Auto), 2, 3 or 4 (known legacy fractional settings migrate to auto).'}
        $VrFgMultiplier=if([string]$settings.vr_fg_multiplier -in @('auto','auto-fixed','2','3','4')){[string]$settings.vr_fg_multiplier}else{'auto'}
    }
    if(!$PSBoundParameters.ContainsKey('VrFsrFrameGeneration') -and $null -ne $settings.vr_fsr_frame_generation){
        if($settings.vr_fsr_frame_generation -isnot [bool]){throw 'vr_fsr_frame_generation must be a JSON boolean.'}
        $VrFsrFrameGeneration=$settings.vr_fsr_frame_generation
    }
    if(!$PSBoundParameters.ContainsKey('VrDlssFrameGeneration') -and $null -ne $settings.vr_dlss_frame_generation){
        if($settings.vr_dlss_frame_generation -isnot [bool]){throw 'vr_dlss_frame_generation must be a JSON boolean.'}
        $VrDlssFrameGeneration=$settings.vr_dlss_frame_generation
    }
    if(!$PSBoundParameters.ContainsKey('OnlySubmitRealFrames') -and $null -ne $settings.vr_only_submit_real_frames){
        if($settings.vr_only_submit_real_frames -isnot [bool]){throw 'vr_only_submit_real_frames must be a JSON boolean.'}
        $OnlySubmitRealFrames=$settings.vr_only_submit_real_frames
    }
    if(!$PSBoundParameters.ContainsKey('MonoVR') -and $null -ne $settings.vr_mono){
        if($settings.vr_mono -isnot [bool]){throw 'vr_mono must be a JSON boolean.'}
        $MonoVR=$settings.vr_mono
    }
    if(!$PSBoundParameters.ContainsKey('EnableLogging') -and $null -ne $settings.enable_logging){
        if($settings.enable_logging -isnot [bool]){throw 'enable_logging must be a JSON boolean.'}
        $EnableLogging=$settings.enable_logging
    }
    if(!$PSBoundParameters.ContainsKey('GameDirectory') -and $settings.game_directory){$GameDirectory=[string]$settings.game_directory}
    if(!$PSBoundParameters.ContainsKey('RenderScale')) {$RenderScale=[double]$settings.vr_render_scale}
    if(!$PSBoundParameters.ContainsKey('StereoCinematics') -and $null -ne $settings.vr_stereo_cinematics){
        if($settings.vr_stereo_cinematics -isnot [bool]){throw 'vr_stereo_cinematics must be a JSON boolean.'}
        $StereoCinematics=$settings.vr_stereo_cinematics
    }
    if(!$PSBoundParameters.ContainsKey('Runtime')) {$Runtime=[string]$settings.openxr_runtime}
    if(!$PSBoundParameters.ContainsKey('PerformanceOverlay') -and $null -ne $settings.vr_performance_overlay){
        if($settings.vr_performance_overlay -isnot [bool]){throw 'vr_performance_overlay must be a JSON boolean.'}
        $PerformanceOverlay=$settings.vr_performance_overlay
    }
    if(!$PSBoundParameters.ContainsKey('HandsOnly') -and $null -ne $settings.vr_hands_only){
        if($settings.vr_hands_only -isnot [bool]){throw 'vr_hands_only must be a JSON boolean.'}
        $HandsOnly=$settings.vr_hands_only
    }
    if(!$PSBoundParameters.ContainsKey('LaserSight') -and $null -ne $settings.vr_laser_sight){
        if($settings.vr_laser_sight -isnot [bool]){throw 'vr_laser_sight must be a JSON boolean.'}
        $LaserSight=$settings.vr_laser_sight
    }
    if(!$PSBoundParameters.ContainsKey('Upscaling')) {$Upscaling=[string]$settings.upscaling}
    if(!$PSBoundParameters.ContainsKey('TurnMode') -and $settings.vr_turn_mode) {$TurnMode=[string]$settings.vr_turn_mode}
    if(!$PSBoundParameters.ContainsKey('SnapAngle') -and $null -ne $settings.vr_snap_angle) {$SnapAngle=[double]$settings.vr_snap_angle}
    if(!$PSBoundParameters.ContainsKey('SmoothTurnSpeed') -and $null -ne $settings.vr_smooth_turn_speed) {$SmoothTurnSpeed=[double]$settings.vr_smooth_turn_speed}
}
# Removed launcher settings never revive old projection/logo/beard preferences.
$SkipLogoIntros=$true
$bodyDeadzone=25.0; $bodySpeed=120.0; $bodyResidual=70.0
if($null -ne $settings.vr_body_yaw_deadzone){$bodyDeadzone=[double]$settings.vr_body_yaw_deadzone}
if($null -ne $settings.vr_body_yaw_follow_speed){$bodySpeed=[double]$settings.vr_body_yaw_follow_speed}
if($null -ne $settings.vr_body_yaw_max_residual){$bodyResidual=[double]$settings.vr_body_yaw_max_residual}
if([double]::IsNaN($bodyDeadzone) -or [double]::IsInfinity($bodyDeadzone) -or $bodyDeadzone -lt 0 -or $bodyDeadzone -gt 60){throw 'vr_body_yaw_deadzone must be 0 to 60 degrees.'}
if([double]::IsNaN($bodySpeed) -or [double]::IsInfinity($bodySpeed) -or $bodySpeed -lt 10 -or $bodySpeed -gt 360){throw 'vr_body_yaw_follow_speed must be 10 to 360 degrees per second.'}
if([double]::IsNaN($bodyResidual) -or [double]::IsInfinity($bodyResidual) -or $bodyResidual -lt $bodyDeadzone -or $bodyResidual -gt 120){throw 'vr_body_yaw_max_residual must be between the deadzone and 120 degrees.'}
if($TurnMode -notin @('smooth','snap')) {throw 'vr_turn_mode must be smooth or snap.'}
if(([double]::IsNaN($SnapAngle) -or [double]::IsInfinity($SnapAngle)) -or $SnapAngle -lt 10 -or $SnapAngle -gt 90) {throw 'vr_snap_angle must be 10 to 90 degrees.'}
if(([double]::IsNaN($SmoothTurnSpeed) -or [double]::IsInfinity($SmoothTurnSpeed)) -or $SmoothTurnSpeed -lt 10 -or $SmoothTurnSpeed -gt 360) {throw 'vr_smooth_turn_speed must be 10 to 360 degrees per second.'}
if([double]::IsNaN($RenderScale) -or [double]::IsInfinity($RenderScale) -or $RenderScale -lt 0.5 -or $RenderScale -gt 2.0) {throw 'vr_render_scale must be 0.5 to 2.0 (linear width/height scaling).'}
if($VRProjection -notin @('immersive','phase5')) {throw 'vr_projection must be immersive or phase5.'}
if($Runtime -notin @('auto','vdxr','active','meta')) {throw 'Invalid openxr_runtime.'}
if($Upscaling -notin @('game','native','off','dlss-sr','dlss-rr','fsr')) {throw 'upscaling must be off, native (TAA), dlss-sr, or fsr (legacy dlss-rr migrates to SR).'}
# Legacy game mode was forced to AA off in previous releases.
if($Upscaling -eq 'game'){$Upscaling='off'}
if ($Mode -eq 'diagnostic') {
    if(Get-Command Invoke-CircleXrDiagnostic -ErrorAction SilentlyContinue){Invoke-CircleXrDiagnostic -PackageRoot $root -Runtime $Runtime -RuntimeJson $RuntimeJson -EnableLogging $EnableLogging}
    else{& (Join-Path $PSScriptRoot 'test-xr.ps1') -Runtime $Runtime -RuntimeJson $RuntimeJson -EnableLogging $EnableLogging}
    return
}
if(!$GameDirectory){$GameDirectory='D:\Games\Jones'}
$GameDirectory=[IO.Path]::GetFullPath($GameDirectory)
$game = Join-Path $GameDirectory 'TheGreatCircle.exe'
if (!(Test-Path -LiteralPath $game -PathType Leaf)) {
    $candidates = @(Get-ChildItem -LiteralPath $GameDirectory -Filter '*.exe' -File | Where-Object { $_.VersionInfo.ProductName -match 'Indiana Jones' })
    if ($candidates.Count -ne 1) { throw 'Cannot unambiguously discover the Indiana Jones executable.' }
    $game = $candidates[0].FullName
}
if (Get-Process -Name ([IO.Path]::GetFileNameWithoutExtension($game)) -ErrorAction SilentlyContinue) { throw 'Indiana Jones is already running. Exit it normally before changing layer modes.' }
$run = Join-Path $root ('logs/' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + $Mode)
if($EnableLogging){New-Item -ItemType Directory -Force -Path $run | Out-Null}
$arguments = ''
$gogInfo = Get-ChildItem -LiteralPath $GameDirectory -Filter 'goggame-*.info' -File | Select-Object -First 1
if ($gogInfo) {
    $task = (Get-Content -LiteralPath $gogInfo.FullName -Raw | ConvertFrom-Json).playTasks | Where-Object isPrimary | Select-Object -First 1
    if ($task -and $task.path -eq [IO.Path]::GetFileName($game)) { $arguments = $task.arguments }
}
$catalog=if($script:CircleEmbeddedBuildCatalog){$script:CircleEmbeddedBuildCatalog | ConvertFrom-Json}else{Get-Content -LiteralPath (Join-Path $root 'profiles/motor-builds.json') -Raw | ConvertFrom-Json}
$storePackage=$null
if (Test-Path -LiteralPath (Join-Path $GameDirectory 'MicrosoftGame.config') -PathType Leaf) {
    # The encrypted executable's hash and version resource cannot be read: the registered package
    # identity selects the profile, and the native layer checks the exact code image in memory
    # before enabling any hook (an unmatched build stays an ordinary game).
    $storePackage=Resolve-CircleStorePackage -GameDirectory $GameDirectory
    $gameVersion=$null; $gameHash=$null
    $buildProfile=@($catalog.profiles | Where-Object { $_.store -eq 'gamepass' -and $_.packageName -eq $storePackage.Name -and $_.packageVersion -eq [string]$storePackage.Version })
    if($buildProfile.Count -eq 0){Write-Warning "Microsoft Store package version $($storePackage.Version) has no validated profile."}
} else {
    $gameVersion = (Get-Item -LiteralPath $game).VersionInfo.FileVersion
    $gameHash = (Get-FileHash -LiteralPath $game -Algorithm SHA256).Hash
    $buildProfile=@($catalog.profiles | Where-Object { $_.identity -ne 'code-image' -and $_.version -eq $gameVersion -and $_.sha256 -eq $gameHash })
}
$knownBuild=$buildProfile.Count -eq 1
$buildProfile=$(if($knownBuild){$buildProfile[0]}else{$null})
if(($ExperimentalStereo -or $Stereo -or $DiagnosticAER) -and (!$knownBuild -or $Mode -ne 'cinema')) {throw 'Stereo requires an explicitly verified Motor build (GOG, Steam or Game Pass) and cinema transport mode. Unknown updates need their own validated profile.'}
if($knownBuild){Write-Host "Verified game build: $($buildProfile.name)"}
if($knownBuild -and $Mode -eq 'cinema'){
    $arguments += ' +r_motionblur 0 +r_chromaticAberration 0'
    Write-Host 'VR comfort: motion blur and chromatic aberration forced off at startup and maintained by the native adapter.'
}
if(($Stereo -and ($DiagnosticAER -or $ExperimentalStereo)) -or ($DiagnosticAER -and $ExperimentalStereo)) {throw 'Choose one: Stereo, DiagnosticAER, or ExperimentalStereo.'}
if ($SkipLogoIntros -and !$knownBuild) {
    Write-Warning 'Unrecognized Motor build: logo skip and development hotkeys disabled; ordinary startup preserved.'
    $SkipLogoIntros = $false
}
if ($SkipLogoIntros) {
    # Existing Motor boot-video cvar. Does not delete assets or skip story cinematics.
    $arguments += ' +com_skipIntroVideo 1'
}
if($Stereo -and $VRProjection -eq 'immersive') {
    # Verified Motor r_fullscreen: 0 windowed, 1 exclusive, 2 borderless.
    # Its window-size handler deliberately ignores fullscreen; this small native
    # mirror enables resize while GetClientRect supplies runtime eye dimensions.
    $arguments += ' +r_fullscreen 0 +r_windowWidth 1280 +r_windowHeight 720'
}
# Explicit diagnostic shortcuts override the saved or command-line selection.
if(([int][bool]$ExperimentalTAA+[int][bool]$ExperimentalDLSS+[int][bool]$ExperimentalDLSSRR) -gt 1){throw 'Choose only one temporal test mode.'}
if(($ExperimentalTAA -or $ExperimentalDLSS -or $ExperimentalDLSSRR) -and (!$Stereo -or $VRProjection -ne 'immersive')){throw 'Temporal test modes require -Stereo with immersive projection.'}
if($ExperimentalTAA){$Upscaling='native'}
if($ExperimentalDLSS){$Upscaling='dlss-sr'}
if($ExperimentalDLSSRR){$Upscaling='dlss-rr'}
# Retain old settings/shortcut compatibility without ever enabling RR or PT.
if($Upscaling -eq 'dlss-rr'){Write-Warning 'Ray Reconstruction was retired; using DLSS Super Resolution.';$Upscaling='dlss-sr'}
$monoVrActive=$MonoVR -and $Stereo -and $Mode -eq 'cinema' -and $VRProjection -eq 'immersive'
$lowPerformanceActive=$false # Removed launcher feature; old saved flags never reactivate it.
if($Stereo) {
    # Mono permits the native in-game FG choice; stereo still requires FG off.
    if(!$monoVrActive){$arguments += ' +r_streamlineDLSSGMode 0'}
    if($VRProjection -ne 'immersive'){$Upscaling='off'}
    if($Upscaling -eq 'dlss-sr'){
        $arguments += ' +r_antialiasing 2 +r_TAASafeMode 0 +r_TAAAntiGhosting 0'
        $arguments += ' +rt_denoiserMode 1 +rt_pathtracingEnabled 0'
        Write-Host "Experimental per-eye Streamline $Upscaling enabled; native hardware/plugin checks remain active."
    }elseif($Upscaling -eq 'fsr'){
        $arguments += ' +r_antialiasing 3 +r_amdffxFGMode 0 +r_TAASafeMode 0 +r_TAAAntiGhosting 0 +rt_denoiserMode 1 +rt_pathtracingEnabled 0'
        Write-Host 'Experimental per-eye AMD FSR; quality comes from in-game FSR settings.'
    }elseif($Upscaling -eq 'native'){
        $arguments += ' +r_antialiasing 1 +r_TAASafeMode 0 +r_TAAAntiGhosting 0 +rt_denoiserMode 1 +rt_pathtracingEnabled 0'
        Write-Host 'Experimental per-eye native TAA with paired object motion enabled.'
    }else{
        $arguments += ' +r_antialiasing 0 +rt_denoiserMode 1 +rt_pathtracingEnabled 0'
        Write-Host 'Stereo release: forcing r_antialiasing 0 (TAA / upscaling disabled).'
    }
    # Motor equivalents of ARGENT's native hand-bob and camera-shake gates.
    $arguments += ' +g_setting_hands_bob 0 +view_skipShakes 1 +g_showcrosshair 0 +g_setting_motion_crosshair 0'
    $arguments += ' +r_skipFlares 1 +r_lensFlaresRatio 0'
}
# Use the installed storefront's recorded launch requirements. No executable patching or injection.
$start = [Diagnostics.ProcessStartInfo]::new()
$start.FileName = $game
$start.WorkingDirectory = $GameDirectory
if($DiagnosticSyncEveryFrame){$arguments += ' +r_syncEveryFrame 1'}
if($DiagnosticDisableRayTracing){
    if(!$knownBuild){throw 'Ray-tracing diagnostic requires a verified Motor build.'}
    $arguments += ' +r_enableRayTracing 0'
    Write-Warning 'Diagnostic only: requesting r_enableRayTracing 0. Native readback is required; no supported no-RT renderer is implied.'
}
$start.Arguments = $arguments
$start.UseShellExecute = $false
$start.CreateNoWindow = $true
if($knownBuild -and $buildProfile.store -eq 'steam'){
    # Ordinary Steam API context, inherited only by the licensed game process.
    # Steam/client ownership checks remain native; never write steam_appid.txt.
    $start.EnvironmentVariables['SteamAppId']=[string]$buildProfile.steamAppId
    $start.EnvironmentVariables['SteamGameId']=[string]$buildProfile.steamAppId
}else{
    $start.EnvironmentVariables.Remove('SteamAppId') | Out-Null
    $start.EnvironmentVariables.Remove('SteamGameId') | Out-Null
}
# Environment changes belong only to the child process. No registration or cleanup is needed.
@($start.EnvironmentVariables.Keys) | Where-Object { $_ -like 'CIRCLE_*' -or $_ -like 'KHARVOX_CIRCLE_*' -or $_ -like 'ARGENT_*' } | ForEach-Object { $start.EnvironmentVariables.Remove($_) | Out-Null }
$start.EnvironmentVariables['CIRCLE_ENABLE_LOGGING']=if($EnableLogging){'1'}else{'0'}
# Cheats are opt-in JSON settings, never inherited from the parent environment.
foreach($cheat in @('invulnerable','stealth','stamina','oxygen','ammo','damage')){
    $value=$settings.("cheat_"+$cheat)
    if($null -ne $value -and $value -isnot [bool]){throw "cheat_$cheat must be a JSON boolean."}
    $start.EnvironmentVariables[('CIRCLE_CHEAT_'+$cheat.ToUpperInvariant())]=if($value -eq $true){'1'}else{'0'}
}
$cheatMultiplier=99.0
if($null -ne $settings.cheat_damage_multiplier){
    $value=$settings.cheat_damage_multiplier
    if($value -is [bool] -or $value -is [string]){throw 'cheat_damage_multiplier must be a JSON number.'}
    $cheatMultiplier=[double]$value
    if([double]::IsNaN($cheatMultiplier) -or [double]::IsInfinity($cheatMultiplier) -or $cheatMultiplier -lt 1 -or $cheatMultiplier -gt 99 -or $cheatMultiplier -ne [Math]::Floor($cheatMultiplier)){throw 'cheat_damage_multiplier must be a whole number from 1 to 99.'}
}
$start.EnvironmentVariables['CIRCLE_CHEAT_DAMAGE_MULTIPLIER']=$cheatMultiplier.ToString([Globalization.CultureInfo]::InvariantCulture)
if($SimulatorDesktopInput){$start.EnvironmentVariables['CIRCLE_SIMULATOR_DESKTOP_INPUT']='1'}
$start.EnvironmentVariables.Remove('CIRCLE_EXPERIMENTAL_STEREO') | Out-Null
$start.EnvironmentVariables.Remove('CIRCLE_LIVE_STEREO') | Out-Null
$start.EnvironmentVariables.Remove('CIRCLE_DIAGNOSTIC_AER') | Out-Null
$start.EnvironmentVariables.Remove('CIRCLE_SEQUENTIAL_STEREO') | Out-Null
$start.EnvironmentVariables.Remove('CIRCLE_VR_PROJECTION') | Out-Null
$start.EnvironmentVariables['CIRCLE_VR_PROJECTION']=$VRProjection
$realFramesActive=$OnlySubmitRealFrames -and $Stereo -and $Mode -eq 'cinema' -and $VRProjection -eq 'immersive'
$start.EnvironmentVariables['CIRCLE_ONLY_SUBMIT_REAL_FRAMES']=if($realFramesActive){'1'}else{'0'}
$vrFsrFgActive=$VrFsrFrameGeneration -and $Upscaling -eq 'fsr' -and $Stereo -and !$monoVrActive -and $VRProjection -eq 'immersive' -and $Mode -eq 'cinema'
$vrFgActive=$VrDlssFrameGeneration -and $Upscaling -eq 'dlss-sr' -and $Stereo -and !$monoVrActive -and $VRProjection -eq 'immersive' -and $Mode -eq 'cinema'
$start.EnvironmentVariables['CIRCLE_VR_DLSS_FG']=if($vrFgActive){'1'}else{'0'}
$start.EnvironmentVariables['CIRCLE_VR_FG_AUTO']=if($vrFgActive -and $VrFgBackend -eq 'auto'){'1'}else{'0'}
$communityFgActive=$vrFgActive
$start.EnvironmentVariables['CIRCLE_VR_FG_COMMUNITY']=if($communityFgActive -or ($vrFgActive -and $VrFgBackend -eq 'auto')){'1'}else{'0'}
if($vrFsrFgActive -and $VrFgMultiplier -notin @('auto','auto-fixed','2')){$VrFgMultiplier='2'}
$start.EnvironmentVariables['CIRCLE_VR_FG_MULTIPLIER']=if($vrFgActive -or $vrFsrFgActive){[string]$VrFgMultiplier}else{'2'}
if(($vrFgActive -or $vrFsrFgActive) -and $VrFgMultiplier -in @('auto','auto-fixed','adaptive')){
    $start.EnvironmentVariables['KHARVOX_CIRCLE_GPU_TIMING']='sample'
}
$start.EnvironmentVariables.Remove('CIRCLE_VR_COMMUNITY_DIR') | Out-Null
if($communityFgActive -or ($vrFgActive -and $VrFgBackend -eq 'auto')){
    $communityDirectory=Join-Path $root 'runtimes/dlss-unlocked-0.4.0'
    if(Get-Command Install-CircleCommunityFramegen -ErrorAction SilentlyContinue){Install-CircleCommunityFramegen -Destination $communityDirectory -VerifyOnly -EnableLogging $EnableLogging}
    else{& (Join-Path $PSScriptRoot 'install-community-framegen.ps1') -Destination $communityDirectory -VerifyOnly -EnableLogging $EnableLogging}
    $start.EnvironmentVariables['CIRCLE_VR_COMMUNITY_DIR']=$communityDirectory
}

if($DiagnosticFramegenGpu){$start.EnvironmentVariables['CIRCLE_FG_DRED']='1'}
if($ExperimentalSecondaryCache){
    if(!$Stereo -or $monoVrActive -or $Mode -ne 'cinema'){throw 'Secondary-cache comparison requires stereo VR.'}
    $CaptureShaders=$true
    $CaptureFrameCount=0
}
$start.EnvironmentVariables['CIRCLE_SECONDARY_CACHE_FIX']=if($ExperimentalSecondaryCache){'1'}else{'0'}
if($DiagnosticTemporalImages){
    if(!$Stereo -or $monoVrActive -or $Upscaling -ne "dlss-sr"){throw "Temporal input capture requires Stereo VR and DLSS SR."}
    $DiagnosticGpuBindings=$true
}
$start.EnvironmentVariables["CIRCLE_TEMPORAL_IMAGE_AUDIT"]=if($DiagnosticTemporalImages){"1"}else{"0"}
$start.EnvironmentVariables['CIRCLE_TEMPORAL_IMAGE_PAIRS']=[string]$TemporalImagePairs
if($DiagnosticGpuBindings){
    $start.EnvironmentVariables['CIRCLE_GPU_BINDING_AUDIT']='1'
    $CaptureShaders=$true
    $CaptureFrameCount=0
    Write-Host 'GPU binding audit enabled; buffer copies require gpu-binding-audit.request in the run log folder.'
}
if($DiagnosticMotionCapture){
    if(!$Stereo -or $monoVrActive -or $Mode -ne 'cinema' -or $Upscaling -ne 'dlss-sr'){
        throw 'Native motion capture requires stereo immersive VR with DLSS SR.'
    }
    $start.EnvironmentVariables['KHARVOX_CIRCLE_CAPTURE_MOTION']=(Join-Path $run 'motion')+',120'
    Write-Host 'Diagnostic motion capture: up to 16 native L/R velocity pairs; on-demand captures remain available.'
}
if($VrDlssFrameGeneration){Write-Host ('Integrated VR DLSS FG requested; eligible mode: '+$vrFgActive+'. Requires active DLSS SR. Actual state is shown in the performance overlay. Native desktop FG is blocked. Real-only preference is preserved.')}
$start.EnvironmentVariables['CIRCLE_MONO_VR']=if($monoVrActive){'1'}else{'0'}
$start.EnvironmentVariables['CIRCLE_LOW_PERFORMANCE']=if($lowPerformanceActive){'1'}else{'0'}
$cinemaStereoActive=$StereoCinematics -and !$monoVrActive -and $Stereo -and $Mode -eq 'cinema' -and $VRProjection -eq 'immersive'
$performanceOverlayActive=$PerformanceOverlay -and $Stereo -and $Mode -eq 'cinema'
$start.EnvironmentVariables['CIRCLE_PERFORMANCE_OVERLAY']=$(if($performanceOverlayActive){'1'}else{'0'})
$handsOnlyActive=$HandsOnly -and $Stereo -and $Mode -eq 'cinema'
$start.EnvironmentVariables['CIRCLE_HANDS_ONLY']=$(if($handsOnlyActive){'1'}else{'0'})
$laserSightActive=$LaserSight -and $Stereo -and $Mode -eq 'cinema'
$start.EnvironmentVariables['CIRCLE_LASER_SIGHT']=$(if($laserSightActive){'1'}else{'0'})
if($Stereo){Write-Host ('Player body: '+$(if($handsOnlyActive){'Hands Only (normal gameplay)'}else{'full body'}))}
$start.EnvironmentVariables['CIRCLE_STEREO_CINEMATICS']=$(if($cinemaStereoActive){'1'}else{'0'})
if($Stereo){Write-Host ('Cutscene screen: '+$(if($cinemaStereoActive){'experimental stereo'}else{'2D (same image in both eyes)'})+'; item inspection always 2D.')}
$start.EnvironmentVariables.Remove('CIRCLE_HAND_IK') | Out-Null
$handSmoothing=20.0
if($null -ne $settings.vr_hand_smoothing_ms){$handSmoothing=[double]$settings.vr_hand_smoothing_ms}
if([double]::IsNaN($handSmoothing) -or [double]::IsInfinity($handSmoothing) -or $handSmoothing -lt 0 -or $handSmoothing -gt 60){throw 'vr_hand_smoothing_ms must be 0 to 60.'}
if($vrFgActive -or $vrFsrFgActive){$handSmoothing=0.0;Write-Host 'VR frame generation: hand smoothing disabled (0 ms).'}
$start.EnvironmentVariables['CIRCLE_HAND_SMOOTHING_MS']=$handSmoothing.ToString([Globalization.CultureInfo]::InvariantCulture)
$start.EnvironmentVariables.Remove('CIRCLE_PHYSICAL_PUNCH') | Out-Null
if($ExperimentalPunch -and $ExperimentalHandIK -and $Stereo){$start.EnvironmentVariables['CIRCLE_PHYSICAL_PUNCH']='1';Write-Host 'Experimental unarmed gesture-to-native punches enabled; native hit-path validation pending.'}
if($ExperimentalHandIK -and $Stereo){$start.EnvironmentVariables['CIRCLE_HAND_IK']='1';Write-Host 'Experimental native hand IK enabled; headset validation pending.'}
$start.EnvironmentVariables['CIRCLE_INPUT']=$(if($Stereo){'1'}else{'0'})
$start.EnvironmentVariables['CIRCLE_VERBOSE_DIAGNOSTICS']=$(if($EnableLogging -and $settings.vr_verbose_diagnostics -eq $true){'1'}else{'0'})
$start.EnvironmentVariables['CIRCLE_INPUT_DEBUG']=$(if($EnableLogging -and $settings.vr_input_debug -eq $true){'1'}else{'0'})
$start.EnvironmentVariables.Remove('CIRCLE_DISABLE_STRAND_HAIR') | Out-Null
$start.EnvironmentVariables['CIRCLE_DISABLE_FACIAL_HAIR']='0'
$pauseWindow=60
if($null -ne $settings.vr_pause_chord_window_ms){$pauseWindow=[int]$settings.vr_pause_chord_window_ms}
if($pauseWindow -lt 50 -or $pauseWindow -gt 300){throw 'vr_pause_chord_window_ms must be 50 to 300.'}
$start.EnvironmentVariables['CIRCLE_PAUSE_WINDOW_MS']=[string]$pauseWindow
$start.EnvironmentVariables.Remove('CIRCLE_ROOMSCALE') | Out-Null
$start.EnvironmentVariables.Remove('CIRCLE_BODY_NATIVE_TURN') | Out-Null
$start.EnvironmentVariables['CIRCLE_TURN_MODE']=$TurnMode
$start.EnvironmentVariables['CIRCLE_SNAP_ANGLE']=$SnapAngle.ToString([Globalization.CultureInfo]::InvariantCulture)
$start.EnvironmentVariables['CIRCLE_TURN_SPEED']=$SmoothTurnSpeed.ToString([Globalization.CultureInfo]::InvariantCulture)
$start.EnvironmentVariables['CIRCLE_BODY_DEADZONE']=$bodyDeadzone.ToString([Globalization.CultureInfo]::InvariantCulture)
$start.EnvironmentVariables['CIRCLE_BODY_SPEED']=$bodySpeed.ToString([Globalization.CultureInfo]::InvariantCulture)
$start.EnvironmentVariables['CIRCLE_BODY_RESIDUAL']=$bodyResidual.ToString([Globalization.CultureInfo]::InvariantCulture)
$start.EnvironmentVariables['CIRCLE_BODY_FOLLOW']=$(if($null -eq $settings.vr_body_yaw_follow -or $settings.vr_body_yaw_follow -eq $true){'1'}else{'0'})
$start.EnvironmentVariables['CIRCLE_ROOMSCALE_DEBUG']=$(if($EnableLogging -and $settings.vr_roomscale_debug -eq $true){'1'}else{'0'})
$roomscaleStart=.015; $roomscaleStop=.006
if($null -ne $settings.vr_roomscale_start_threshold_m){$roomscaleStart=[double]$settings.vr_roomscale_start_threshold_m}
if($null -ne $settings.vr_roomscale_stop_threshold_m){$roomscaleStop=[double]$settings.vr_roomscale_stop_threshold_m}
if([double]::IsNaN($roomscaleStart) -or $roomscaleStart -lt .005 -or $roomscaleStart -gt .1 -or [double]::IsNaN($roomscaleStop) -or $roomscaleStop -lt 0 -or $roomscaleStop -gt $roomscaleStart*.9){throw 'Invalid roomscale thresholds: start 0.005–0.1m, stop 0–90% of start.'}
$start.EnvironmentVariables['CIRCLE_ROOMSCALE_START']=$roomscaleStart.ToString([Globalization.CultureInfo]::InvariantCulture)
$start.EnvironmentVariables['CIRCLE_ROOMSCALE_STOP']=$roomscaleStop.ToString([Globalization.CultureInfo]::InvariantCulture)
if($settings.vr_movement_direction -in @('head','offhand')) {$start.EnvironmentVariables['CIRCLE_MOVE_DIRECTION']=[string]$settings.vr_movement_direction}
else {$start.EnvironmentVariables.Remove('CIRCLE_MOVE_DIRECTION') | Out-Null}
if($Stereo -and $VRProjection -eq 'immersive' -and $settings.vr_roomscale_enabled -eq $true -and $settings.vr_roomscale_collision -eq $true) {
    $start.EnvironmentVariables['CIRCLE_ROOMSCALE']='1'
    # Native stick turning is independent of physical body yaw follow.

}
if($Stereo -and $VRProjection -eq 'immersive'){$start.EnvironmentVariables['CIRCLE_BODY_NATIVE_TURN']='1'}
if($Stereo) {
    $start.EnvironmentVariables['CIRCLE_SEQUENTIAL_STEREO']='1'
    if($monoVrActive){Write-Host 'Mono VR: one centred native render shared by both eyes; no right-eye render. Cinema is 2D.'}
    else{Write-Host 'Motor native sequential stereo: gameplay L/R from one frame; cinema for menus and unowned views.'}
}
$start.EnvironmentVariables['CIRCLE_UPSCALING']=$Upscaling
$start.EnvironmentVariables['CIRCLE_VR_FSR_FG']=if($vrFsrFgActive){'1'}else{'0'}
if($Stereo -and $Upscaling -eq 'dlss-sr'){$start.EnvironmentVariables['CIRCLE_EXPERIMENTAL_DLSS']='1'}
else {$start.EnvironmentVariables.Remove('CIRCLE_EXPERIMENTAL_DLSS') | Out-Null}
# RR is retired; no launch mode sets CIRCLE_EXPERIMENTAL_RR.
if($DiagnosticAER) {$start.EnvironmentVariables['CIRCLE_DIAGNOSTIC_AER']='1'; Write-Warning 'Diagnostic AER: eyes use DIFFERENT simulation frames. This is not synchronized VR.'}
if($DiagnosticGiHistory){$start.EnvironmentVariables['CIRCLE_GI_HISTORY_PROBE']='1'}
# r61: headset-verified final-gather history isolation is standard stereo behavior.
if($Stereo -and $Mode -ne 'off'){$start.EnvironmentVariables['CIRCLE_GI_HISTORY']='1'}
# Retain the test switch as a diagnostic alias; normal launches need no opt-in.
if($ExperimentalGiHistory){$start.EnvironmentVariables['CIRCLE_GI_HISTORY_PROBE']='1'}
if($ExperimentalNrdHistory){$start.EnvironmentVariables['CIRCLE_NRD_HISTORY']='1'}
if($ExperimentalStereo) {$start.EnvironmentVariables['CIRCLE_EXPERIMENTAL_STEREO']='1'}
$selectedRuntime = $null
if ($Mode -ne 'off') {
    if (!(Test-Path -LiteralPath (Join-Path $root 'bin/KharvoxCircle.dll'))) { throw 'Build first: .\tools\build.ps1' }
    $vdxr = 'C:\Program Files\Virtual Desktop Streamer\OpenXR\virtualdesktop-openxr.json'
    $meta = 'C:\Program Files\Oculus\Support\oculus-runtime\oculus_openxr_64.json'
    $active = (Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Khronos\OpenXR\1' -ErrorAction SilentlyContinue).ActiveRuntime
    if ($RuntimeJson) { $selectedRuntime = (Resolve-Path -LiteralPath $RuntimeJson).Path }
    elseif ($Runtime -eq 'meta') { $selectedRuntime = $meta }
    elseif ($Runtime -eq 'vdxr') { $selectedRuntime = $vdxr }
    else { $selectedRuntime = $active }
    if ($selectedRuntime) {
        if (!(Test-Path -LiteralPath $selectedRuntime -PathType Leaf)) { throw "Runtime manifest missing: $selectedRuntime" }
        $start.EnvironmentVariables['XR_RUNTIME_JSON'] = $selectedRuntime
    }
    $extraPath = $start.EnvironmentVariables['VK_ADD_IMPLICIT_LAYER_PATH']
    $start.EnvironmentVariables['VK_ADD_IMPLICIT_LAYER_PATH'] = (Join-Path $root 'bin') + $(if ($extraPath) { ';' + $extraPath } else { '' })
    $start.EnvironmentVariables['KHARVOX_CIRCLE_ENABLE_LAYER'] = '1'
    if($EnableLogging){$start.EnvironmentVariables['KHARVOX_CIRCLE_LOG_DIR'] = $run}
    $start.EnvironmentVariables['KHARVOX_CIRCLE_MODE'] = $Mode
    $start.EnvironmentVariables['KHARVOX_CIRCLE_PACING'] = '0'
    $start.EnvironmentVariables['KHARVOX_CIRCLE_VIRTUAL_CLIENT'] = '0'
    $start.EnvironmentVariables['KHARVOX_CIRCLE_UI_LAYER'] = $(if($Stereo){'1'}else{'0'})
    $start.EnvironmentVariables['KHARVOX_CIRCLE_UI_WASH'] = '0'
    $start.EnvironmentVariables['KHARVOX_CIRCLE_UI_RETICLE'] = '0' 
    $start.EnvironmentVariables['KHARVOX_CIRCLE_STEREO'] = '0'
    $start.EnvironmentVariables['KHARVOX_CIRCLE_CONTROLLERS'] = '0'
    $start.EnvironmentVariables['KHARVOX_CIRCLE_KEEP_ACTIVE'] = '0'
    if($Stereo -and $VRProjection -eq 'immersive') {
        $start.EnvironmentVariables['KHARVOX_CIRCLE_RENDER_SIZE']='auto'
        $start.EnvironmentVariables['KHARVOX_CIRCLE_RENDER_SCALE']=$RenderScale.ToString([Globalization.CultureInfo]::InvariantCulture)
        Write-Host "Immersive projection; runtime eye recommendation x $RenderScale (linear). Selected AA/upscaling: $Upscaling."
    }
    # Enable a development key only when all current game config files were readable
    # and none binds it. Missing config/build validation disables the keys.
    $hotkeyMask = 0
    $bindingDirectory = Join-Path $env:USERPROFILE 'Saved Games\MachineGames\TheGreatCircle\base'
    $bindingFiles = @(Get-ChildItem -LiteralPath $bindingDirectory -Filter '*.cfg' -File -ErrorAction SilentlyContinue)
    $bindingChecks = @()
    if ($knownBuild -and $bindingFiles.Count -gt 0) {
        try {
            $bindingLines = @($bindingFiles | ForEach-Object { Get-Content -LiteralPath $_.FullName -ErrorAction Stop })
            foreach ($keyNumber in 6..9) {
                $occupied = @($bindingLines | Where-Object { $_ -match ('^\s*bind\s+"?F' + $keyNumber + '"?\s+"?[^"\s]') }).Count -gt 0
                if (!$occupied) { $hotkeyMask = $hotkeyMask -bor (1 -shl ($keyNumber-6)) }
                $bindingChecks += [ordered]@{ key="F$keyNumber"; enabled=!$occupied; reason=$(if ($occupied) {'existing game binding'} else {'unbound in checked config'}) }
            }
        } catch { $hotkeyMask=0; $bindingChecks=@([ordered]@{reason='config unreadable; development keys disabled'}) }
    }
    $start.EnvironmentVariables['KHARVOX_CIRCLE_HOTKEY_MASK'] = [string]$hotkeyMask
    if ($CaptureShaders -and $EnableLogging) {
        $capture = Join-Path $run 'shaders'
        New-Item -ItemType Directory -Force -Path $capture | Out-Null
        $start.EnvironmentVariables['KHARVOX_CIRCLE_DUMP_SHADERS'] = $capture
        $start.EnvironmentVariables['KHARVOX_CIRCLE_DUMP_SKIP_FRAMES'] = [string]$CaptureStartFrame
        $start.EnvironmentVariables['KHARVOX_CIRCLE_DUMP_FRAMES'] = [string]$CaptureFrameCount
    }
}
$record = [ordered]@{
    experimental_secondary_cache = [bool]$ExperimentalSecondaryCache
    diagnostic_temporal_images = [bool]$DiagnosticTemporalImages
    temporal_image_pairs = $TemporalImagePairs
    diagnostic_gpu_bindings = [bool]$DiagnosticGpuBindings
    simulator_desktop_input = [bool]$SimulatorDesktopInput
    diagnostic_motion_capture = [bool]$DiagnosticMotionCapture
    started = (Get-Date).ToString('o'); game = $game; version = (Get-Item -LiteralPath $game).VersionInfo.FileVersion
    mode = $Mode; runtime = $selectedRuntime; arguments = $arguments; logDirectory = $(if($EnableLogging){$run}else{$null}); enable_logging = $EnableLogging
    gameExecutableSha256 = $gameHash; skipLogoIntros = $SkipLogoIntros; knownMotorBuild = $knownBuild
    buildProfile = $(if($knownBuild){$buildProfile.id}else{'unknown'}); storefront = $(if($knownBuild){$buildProfile.store}else{'unknown'})
    experimentalStereo = [bool]$ExperimentalStereo
    sequentialStereoRequested = [bool]$Stereo
    vr_only_submit_real_frames = [bool]$realFramesActive
    vr_fg_backend_requested = $VrFgBackend
    vr_fg_multiplier_requested = $VrFgMultiplier
    vr_hand_smoothing_ms_effective = $handSmoothing
    vr_fg_community_active = [bool]$communityFgActive
    vr_fsr_frame_generation_requested = [bool]$VrFsrFrameGeneration
    vr_fsr_frame_generation_active = [bool]$vrFsrFgActive
    vr_dlss_frame_generation_requested = [bool]$VrDlssFrameGeneration
    vr_dlss_frame_generation = [bool]$vrFgActive
    diagnostic_framegen_gpu = [bool]$DiagnosticFramegenGpu
    vr_mono = [bool]$monoVrActive
    vr_low_performance = [bool]$lowPerformanceActive
    diagnosticAER = [bool]$DiagnosticAER
    vr_projection = $VRProjection; vr_render_scale = $RenderScale; upscaling=$Upscaling; settingsFile=$SettingsFile
    vr_stereo_cinematics = [bool]$cinemaStereoActive
    vr_performance_overlay = [bool]$performanceOverlayActive
    vr_hands_only = [bool]$handsOnlyActive
    vr_laser_sight = [bool]$laserSightActive
    vr_turn_mode=$TurnMode; vr_snap_angle=$SnapAngle; vr_smooth_turn_speed=$SmoothTurnSpeed
    hotkeyMask = $hotkeyMask; bindingChecks = $bindingChecks
    status = 'starting'; pid = $null
}
$cancelEvent=$null
try {
    if($LaunchCancelEvent){$cancelEvent=[Threading.EventWaitHandle]::OpenExisting($LaunchCancelEvent)}
    if($Mode -eq 'cinema'){
        if(!(Get-Command Invoke-CircleVrIntro -ErrorAction SilentlyContinue)){. (Join-Path $PSScriptRoot 'vr-intro.ps1')}
        Invoke-CircleVrIntro -Root $root -SettingsFile $SettingsFile -RuntimeJson $start.EnvironmentVariables['XR_RUNTIME_JSON'] -EnableLogging $EnableLogging -CancelEvent $cancelEvent
    }
    if($cancelEvent -and $cancelEvent.WaitOne(0)){throw 'Game launch cancelled.'}
    if($storePackage){
        # Unrecognized package versions launch too (mod features stay off natively), like unknown file builds.
        $appId=if($knownBuild){[string]$buildProfile.appId}else{'Game'}
        $process = Start-CircleStorePackage -Package $storePackage -AppId $appId -Arguments $arguments -Environment $start.EnvironmentVariables
    } else { $process = [Diagnostics.Process]::Start($start) }
} finally {if($cancelEvent){$cancelEvent.Dispose()}}
$record.pid = $process.Id
$record.status = 'launched'
if($EnableLogging){$record | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $run 'launch.json') -Encoding utf8}
Write-Host "Game PID $($process.Id), mode $Mode, runtime $selectedRuntime"
if($EnableLogging){Write-Host "Logs: $run"}else{Write-Host "Logging disabled."}
if ($MonitorSeconds -gt 0) {
    $deadline = [DateTime]::UtcNow.AddSeconds($MonitorSeconds)
    while (!$process.HasExited -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 500; $process.Refresh() }
    $record.layerLoaded = Test-Path -LiteralPath (Join-Path $run 'LAYER_LOADED')
    $record.aliveAfterMonitor = !$process.HasExited
    $record.exitCode = $(if ($process.HasExited) { $process.ExitCode } else { $null })
    if ($EnableLogging -and !$process.HasExited) {
        $record.windowTitle = $process.MainWindowTitle
        try { $process.Modules | Select-Object ModuleName,FileName | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $run 'modules.json') -Encoding utf8 } catch { $record.moduleInspectionError = $_.Exception.Message }
    }
    $record.status = 'monitored'
    if($EnableLogging){$record | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $run 'launch.json') -Encoding utf8}
    Write-Host "Layer loaded: $($record.layerLoaded); alive: $($record.aliveAfterMonitor); exit: $($record.exitCode)"
    if ($StopAfterMonitor -and !$process.HasExited) {
        $closed = $process.CloseMainWindow()
        $record.closeRequested = $closed
        if ($closed) { $record.closedNormally = $process.WaitForExit(10000) }
        if($EnableLogging){$record | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $run 'launch.json') -Encoding utf8}
    }
}

