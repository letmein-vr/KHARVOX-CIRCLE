using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.Runtime.InteropServices;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Threading;
using System.Web.Script.Serialization;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;

namespace Kharvox.Circle {
sealed class Choice {
    public string Key {get;private set;}
    public string Label {get;private set;}
    public Choice(string key,string label){Key=key;Label=label;}
    public override string ToString(){return Label;}
}
sealed class Option {
    public string Key, Label, Tip;
    public object Default;
    public string[] Choices;
    public double Min, Max;
    public FrameworkElement Control;
    public Option(string key, string label, object value, string tip, double min=0, double max=0, params string[] choices) {
        Key=key; Label=label; Default=value; Tip=tip; Min=min; Max=max; Choices=choices;
    }
}
// DXGI hardware inventory is a UI hint. The renderer resolves the actual Vulkan device.
sealed class GpuInfo {
    public string Name="GPU detection unavailable";
    public uint Vendor;
    public bool Mixed;
    public ulong? DedicatedVramBytes;
    public bool LowVram {get{return DedicatedVramBytes.HasValue && DedicatedVramBytes.Value<=10UL*1024*1024*1024;}}
    public const string LowVramWarning="Attention RAMlet: You need more than 10 gigabytes of VRAM for this game. Feel free to try but don't expect much.";
    public bool Amd {get{return Vendor==0x1002&&!Mixed;}}
    public bool Triple {get{return Mixed||(Vendor==0x10de&&(Name.Contains("RTX 20")||Name.Contains("RTX 30")||Name.Contains("RTX 40")||Name.Contains("RTX 50")));}}
    public string Backend(string multiplier) {
        if(Amd)return "AMD FidelityFX - 2x with adaptive pacing";
        if(Mixed||Vendor==0)return "Automatic - resolved on the game's GPU";
        if(Vendor!=0x10de)return "Automatic - runtime capability check";
        bool community=Name.Contains("RTX 20")||Name.Contains("RTX 30")||Name.Contains("RTX 40");
        return community?"Community DLSS (experimental)":"Native NVIDIA DLSS";
    }
    [StructLayout(LayoutKind.Sequential,CharSet=CharSet.Unicode)]
    struct Desc {
        [MarshalAs(UnmanagedType.ByValTStr,SizeConst=128)] public string Description;
        public uint VendorId,DeviceId,SubSysId,Revision;
        public UIntPtr DedicatedVideoMemory,DedicatedSystemMemory,SharedSystemMemory;
        public uint LuidLow;public int LuidHigh;public uint Flags;
    }
    [DllImport("dxgi.dll",ExactSpelling=true)] static extern int CreateDXGIFactory1(ref Guid iid,out IntPtr factory);
    [UnmanagedFunctionPointer(CallingConvention.StdCall)] delegate int EnumAdapter(IntPtr self,uint index,out IntPtr adapter);
    [UnmanagedFunctionPointer(CallingConvention.StdCall)] delegate int GetDesc(IntPtr self,out Desc desc);
    static T Method<T>(IntPtr pointer,int slot) where T:class {return Marshal.GetDelegateForFunctionPointer(Marshal.ReadIntPtr(Marshal.ReadIntPtr(pointer),slot*IntPtr.Size),typeof(T)) as T;}
    public static GpuInfo Detect() {
        IntPtr factory=IntPtr.Zero;
        try {
            var iid=new Guid("770aae78-f26f-4dba-a829-253c83d1b387");
            if(CreateDXGIFactory1(ref iid,out factory)<0)return new GpuInfo();
            var list=new List<Tuple<Desc,ulong>>();
            for(uint i=0;i<32;i++) {
                IntPtr adapter;
                if(Method<EnumAdapter>(factory,12)(factory,i,out adapter)<0)break;
                try {Desc d;if(Method<GetDesc>(adapter,10)(adapter,out d)>=0&&(d.Flags&2)==0)list.Add(Tuple.Create(d,d.DedicatedVideoMemory.ToUInt64()));}
                finally {Marshal.Release(adapter);}
            }
            if(list.Count==0)return new GpuInfo();
            var selected=list.OrderByDescending(x=>x.Item2).First().Item1;
            return new GpuInfo {Name=selected.Description,Vendor=selected.VendorId,DedicatedVramBytes=selected.DedicatedVideoMemory.ToUInt64(),Mixed=list.Any(x=>x.Item1.VendorId==0x10de)&&list.Any(x=>x.Item1.VendorId==0x1002)};
        } catch {return new GpuInfo();}
        finally {if(factory!=IntPtr.Zero)Marshal.Release(factory);}
    }
}
static class SettingsStore {
    public static string SharedPath {get{return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),"KHARVOX CIRCLE","circle-settings.json");}}
    public static void Verify() {
        string temp=Path.Combine(Path.GetTempPath(),"CircleSettingsTest-"+Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(temp);
        try {
            string a=Path.Combine(temp,"KHARVOX-CIRCLE-old"),b=Path.Combine(temp,"KHARVOX-CIRCLE-new"),shared=Path.Combine(temp,"user","circle-settings.json");
            Directory.CreateDirectory(a);Directory.CreateDirectory(b);
            string original="{\"vr_render_scale\":1.25,\"future_key\":{\"keep\":true}}";
            File.WriteAllText(Path.Combine(a,"circle-settings.json"),original);
            File.WriteAllText(Path.Combine(a,"circle-settings.json.bak"),"{}");
            File.WriteAllText(Path.Combine(b,"circle-settings.defaults.json"),"{}");
            Resolve(b,shared);
            if(File.ReadAllText(shared)!=original)throw new Exception("Previous release migration failed");
            File.WriteAllText(Path.Combine(b,"circle-settings.json"),"{}");Resolve(b,shared);
            if(File.ReadAllText(shared)!=original)throw new Exception("Release defaults overwrote shared settings");
            File.WriteAllText(shared,"{broken");Resolve(a,shared);
            if(File.ReadAllText(shared)!="{broken")throw new Exception("Invalid shared data silently replaced");
            string invalid=Path.Combine(temp,"invalid");Directory.CreateDirectory(invalid);File.WriteAllText(Path.Combine(invalid,"circle-settings.json"),"[]");
            string absent=Path.Combine(temp,"absent.json");bool rejected=false;
            try {Resolve(invalid,absent);}catch(InvalidDataException){rejected=true;}
            if(!rejected||File.Exists(absent))throw new Exception("Invalid migration accepted");
        } finally {Directory.Delete(temp,true);}
    }
    public static string Resolve(string root,string shared) {
        if(File.Exists(shared))return shared;
        string source=Path.Combine(root,"circle-settings.json");
        if(!File.Exists(source)) {
            // Older sibling packages may contain the user's last saved settings.
            // A .bak identifies launcher-saved settings over untouched package defaults.
            var parent=Directory.GetParent(root.TrimEnd(Path.DirectorySeparatorChar));
            var candidates=new List<string>();
            if(parent!=null)try {
                foreach(var directory in Directory.GetDirectories(parent.FullName,"KHARVOX-CIRCLE*")) {
                    var candidate=Path.Combine(directory,"circle-settings.json");
                    if(File.Exists(candidate))candidates.Add(candidate);
                }
            }catch(IOException){}catch(UnauthorizedAccessException){}
            source=candidates.OrderByDescending(x=>File.Exists(x+".bak")).ThenByDescending(x=>File.GetLastWriteTimeUtc(x)).ThenBy(x=>x,StringComparer.OrdinalIgnoreCase).FirstOrDefault();
        }
        if(source==null||!File.Exists(source))source=Path.Combine(root,"circle-settings.defaults.json");
        string text=File.Exists(source)?File.ReadAllText(source):EmbeddedResources.Text("defaults.json");
        if((new JavaScriptSerializer().DeserializeObject(text) as Dictionary<string,object>)==null)throw new InvalidDataException("Settings must contain a JSON object: "+source);
        Directory.CreateDirectory(Path.GetDirectoryName(shared));
        var temporary=shared+"."+Guid.NewGuid().ToString("N")+".tmp";
        try {
            File.WriteAllText(temporary,text,new UTF8Encoding(false));
            try {File.Move(temporary,shared);}catch(IOException){if(!File.Exists(shared))throw;}
        }finally{if(File.Exists(temporary))File.Delete(temporary);}
        return shared;
    }
}
sealed class Launcher : Window {
    readonly string root;
    readonly string settingsPath;
    readonly GpuInfo gpu;
    readonly JavaScriptSerializer json=new JavaScriptSerializer();
    Dictionary<string,object> settings;
    // Fixed launcher defaults, matching the confirmed configuration before cleanup.
    static readonly Dictionary<string,object> FixedDefaults=new Dictionary<string,object> {
        {"vr_only_submit_real_frames",true},
        {"vr_roomscale_enabled",true},
        {"vr_roomscale_collision",true},
        {"vr_roomscale_start_threshold_m",.015},
        {"vr_roomscale_stop_threshold_m",.006},
        {"vr_body_yaw_follow",true},
        {"vr_body_yaw_deadzone",25},
        {"vr_body_yaw_follow_speed",120},
        {"vr_body_yaw_max_residual",70},
        {"openxr_runtime","auto"},
        {"runtime_json",""},
        {"launch_mode","stereo"},
        {"experimental_hand_ik",true},
        {"experimental_punch",true},
        {"vr_hand_smoothing_ms",20},
        {"vr_pause_chord_window_ms",60}
    };
    readonly List<Option> options=new List<Option>();
    readonly List<Button> tabs=new List<Button>();
    readonly List<ScrollViewer> pages=new List<ScrollViewer>();
    readonly Brush accent=new SolidColorBrush(Color.FromRgb(255,61,12));
    readonly Brush muted=new SolidColorBrush(Color.FromRgb(155,155,160));
    ComboBox gamePath;
    TextBlock status;
    Button launch, diagnose;
    bool busy, loading=true;
    EventWaitHandle launchCancellation;
    bool closeAfterLaunch;
    string savedSelection;
    readonly DispatcherTimer autoSaveTimer=new DispatcherTimer {Interval=TimeSpan.FromMilliseconds(350)};
    int activeTab;
    readonly bool preview;
    public Launcher(string directory, bool isPreview, GpuInfo detectedGpu=null) {
        root=directory; preview=isPreview;gpu=detectedGpu??GpuInfo.Detect();
        settingsPath=preview?Path.Combine(root,"circle-settings.json"):SettingsStore.Resolve(root,SettingsStore.SharedPath);
        Title="KHARVOX: CIRCLE Launcher \u2014 Beta 0.2"; Width=916; Height=682; MinWidth=840; MinHeight=640;
        Background=Brushes.Black; Foreground=Brushes.White; FontFamily=new FontFamily("Segoe UI"); FontSize=14;
        WindowStartupLocation=WindowStartupLocation.CenterScreen;
        var readPath=File.Exists(SettingsPath)?SettingsPath:Path.Combine(root,"circle-settings.defaults.json");
        settings=json.Deserialize<Dictionary<string,object>>(File.Exists(readPath)?File.ReadAllText(readPath):EmbeddedResources.Text("defaults.json"));
        if(settings==null) throw new InvalidDataException("circle-settings.json must contain an object.");
        foreach(string key in new[]{"vr_intro_started","disable_vr_intro"})
            if(settings.ContainsKey(key)&&!(settings[key] is bool))throw new InvalidDataException(key+" must be a JSON boolean.");
        if(!settings.ContainsKey("vr_disable_facial_hair") && settings.ContainsKey("vr_disable_strand_hair")) settings["vr_disable_facial_hair"]=settings["vr_disable_strand_hair"];
        Resources.Add(typeof(Button),ButtonStyle());
        Resources.Add(typeof(CheckBox),CheckStyle());
        Resources.Add(typeof(TextBox),TextStyle());
        Resources.Add(typeof(ComboBox),ComboStyle());
        var layout=new Grid {Margin=new Thickness(42,18,42,24)};
        foreach(var h in new[]{116.0,48,46,1,8,52,32}) layout.RowDefinitions.Add(new RowDefinition {Height=h==1?new GridLength(1,GridUnitType.Star):new GridLength(h)});
        Content=layout;
        var source=EmbeddedResources.Image("banner.png");
        // Same central banner crop used by Argent. The supplied PNG stays unchanged.
        var banner=new Image {Source=new CroppedBitmap(source,new Int32Rect(0,source.PixelHeight*18/100,source.PixelWidth,source.PixelHeight*64/100)),Stretch=Stretch.Uniform,VerticalAlignment=VerticalAlignment.Top};
        layout.Children.Add(banner);
        var pathRow=new Grid(); pathRow.ColumnDefinitions.Add(new ColumnDefinition {Width=new GridLength(92)}); pathRow.ColumnDefinitions.Add(new ColumnDefinition()); pathRow.ColumnDefinitions.Add(new ColumnDefinition {Width=new GridLength(108)});
        pathRow.Children.Add(new TextBlock {Text="Game folder",VerticalAlignment=VerticalAlignment.Center,Foreground=muted});
        gamePath=new ComboBox {IsEditable=true,Text=Value("game_directory","D:\\Games\\Jones").ToString(),Margin=new Thickness(0,5,12,5),VerticalContentAlignment=VerticalAlignment.Center,ToolTip="Select an installed GOG/Steam/Game Pass copy or browse to another installation (Game Pass: the folder that holds MicrosoftGame.config). Every executable is checked against its own build profile."};
        var installations=InstalledGames(gamePath.Text).ToArray();
        foreach(var path in installations)gamePath.Items.Add(path);
        if(installations.Length>0&&!installations.Contains(gamePath.Text,StringComparer.OrdinalIgnoreCase))gamePath.Text=installations[0];
        Grid.SetColumn(gamePath,1); pathRow.Children.Add(gamePath);
        var browse=Button("Browse",(s,e)=>Browse()); Grid.SetColumn(browse,2); pathRow.Children.Add(browse); Grid.SetRow(pathRow,1); layout.Children.Add(pathRow);
        var strip=new UniformGridCompat(6); Grid.SetRow(strip,2); layout.Children.Add(strip);
        var host=new Grid(); Grid.SetRow(host,3); layout.Children.Add(host);
        string[] names={"Rendering","Movement","VR Options","Development","Instructions","Cheats"};
        for(int i=0;i<names.Length;i++) {
            int index=i; var b=Button(names[i],(s,e)=>ShowTab(index)); b.Style=TabStyle(); tabs.Add(b); strip.Children.Add(b);
            var scroll=new ScrollViewer {VerticalScrollBarVisibility=ScrollBarVisibility.Auto,HorizontalScrollBarVisibility=ScrollBarVisibility.Disabled,Margin=new Thickness(12,14,12,8)};
            var panel=new StackPanel(); scroll.Content=panel; pages.Add(scroll); host.Children.Add(scroll);
        }
        Add(0,new Option("upscaling","Anti-aliasing / Upscaling","off","DLSS SR and AMD FSR use the in-game quality setting. VR Frame Generation requires Stereo VR and disables Mono VR. AMD FSR + VR FG uses FidelityFX optical flow and interpolation with adaptive pacing. Experimental; restart to apply.",0,0,gpu.Amd?new[]{"off","native","fsr","fsr-fg"}:new[]{"off","native","dlss-sr","dlss-sr-fg","fsr","fsr-fg"}));

        Add(0,new Option("vr_fg_multiplier","VR FG multiplier","auto-fixed","Auto Seamless prioritizes real frames and fills gaps continuously, including between 1x and 2x, up to 4x. Auto starts at 2x and switches only between 2x, 3x and 4x: sustained major drops trigger an increase; sustained headroom allows a verified decrease. Short dips are ignored. RTX 20/30/40 use Community DLSS; RTX 50 uses NVIDIA DLSS. AMD is limited to 2x (Auto Seamless: Off/2x).",0,0,gpu.Triple?new[]{"auto","auto-fixed","2","3","4"}:new[]{"auto","auto-fixed","2"}));
        Add(5,new Option("cheat_invulnerable","Invulnerable",false,"Blocks calculated damage to the local player, including ordinary fall/combat damage. Scripted deaths are not overridden."));
        Add(5,new Option("cheat_stealth","Undetectable / Stealth",false,"Filters the player from AI sight targets and player-originated hearing events. Existing alerts and scripted encounters may remain. Experimental."));
        Add(5,new Option("cheat_stamina","Infinite stamina",false,"Refills the local player's base stamina after its native update; does not alter enemy stamina."));
        Add(5,new Option("cheat_oxygen","Infinite oxygen",false,"Survival: enables the native underwater-breathing option. Use Invulnerable for ordinary health and fall damage."));
        Add(5,new Option("cheat_ammo","Infinite ammunition",false,"Refills the equipped player's ammunition reserve. Reloading and the weapon's normal firing restrictions remain."));
        Add(5,new Option("cheat_damage","One Hit Kill / damage boost",false,"Multiplies outgoing player damage by the factor below. Default x99. Armor, blocking and scripted immunity can still prevent a one-hit kill."));
        Add(5,new Option("cheat_damage_multiplier","Damage multiplier",99,"Outgoing damage factor; used only when damage boost is enabled. Does not amplify damage received by the player.",1,99));
        Add(0,new Option("vr_render_scale","RenderScale",1.0,"Linear resolution scale per eye (0.5 to 2.0).",.5,2));
        Add(0,new Option("vr_stereo_cinematics","Stereoscopic Cinema Screen",false,"Off: flat 16:9 cutscenes, the same image in both eyes. On: stereoscopic cutscenes. Item inspection always stays 2D. Applies on next game launch."));
        Add(0,new Option("vr_mono","Mono VR",false,"Render the scene once from a centred camera and share the image between both eyes. Retains head/controller tracking and DLSS, but removes stereoscopic depth. Cinema is also 2D in this mode. Restart to apply."));
        Add(0,new Option("enable_logging","Enable Logging",false,"Write launcher and runtime diagnostic logs. Default off; restart the game to apply. The performance overlay and Auto FG remain available without logs."));
        Add(0,new Option("disable_vr_intro","Disable VR Intro",false,"Skip the CIRCLE VR intro before starting Indiana Jones. Available after the first intro start; saved globally across releases."));
        Add(3,new Option("vr_performance_overlay","Live VR performance overlay",false,"Headset panel: produced stereo pairs, fresh submitted images, XR submissions, repeats and image age. Changes apply on the next launch."));
        Add(2,new Option("vr_laser_sight","Laser Sight",true,"Show a muzzle laser only while the held firearm can shoot. Off while empty, reloading, in melee mode or holstered. Restart to apply."));
        Add(2,new Option("vr_hands_only","Hands Only",false,"Hide Indiana's body and clothing in normal VR gameplay while retaining hands, finger control and held items. Scripted interactions, item inspection and cinematics keep their native models. Restart to apply. Experimental; other outfits still require testing."));
        Add(1,new Option("vr_movement_direction","Movement direction","head","Use head direction or the off-hand aim direction.",0,0,"head","offhand"));
        Add(1,new Option("vr_turn_mode","Turn mode","smooth","Right stick turning.",0,0,"smooth","snap"));
        Add(1,new Option("vr_snap_angle","Snap angle (degrees)",60,"Snap turn angle.",10,90));
        Add(1,new Option("vr_smooth_turn_speed","Smooth turn speed (degrees/s)",240,"Smooth right-stick turning speed.",10,360));
        Add(3,new Option("vr_input_debug","Input change diagnostics",false,"Logs resolved native controller changes and context transitions."));
        Add(3,new Option("vr_roomscale_debug","Roomscale debug logging",false,"Enables periodic native movement and body-yaw diagnostic lines."));
        Add(3,new Option("capture_shaders","Capture shaders",false,"Writes shader captures into the session log directory."));
        Add(3,new Option("capture_start_frame","Capture start frame",2400,"First frame to capture.",0,10000000));
        Add(3,new Option("capture_frame_count","Capture frame count",2,"Number of frames to capture.",1,1000));
        Add(3,new Option("monitor_seconds","Monitor after launch (seconds)",0,"0 returns after launch; monitoring records process and layer status.",0,3600));
        strip.Children.Remove(tabs[4]);strip.Children.Add(tabs[4]);
        tabs[3].Visibility=Visibility.Collapsed;strip.Columns=5;
        foreach(var name in new[]{"interaction.png","combat.png"})
            ((StackPanel)pages[4].Content).Children.Add(new Image {Source=EmbeddedResources.Image(name),Stretch=Stretch.Uniform,Margin=new Thickness(0,0,0,16)});
        Note(4,"START IN VR",true);
        Note(4,"The game folder is detected on every launcher start, with Steam preferred when both versions are installed. You can change the folder manually. OpenXR runtime selection is automatic. Settings save automatically and apply on the next launch; correct invalid entries before closing. Click Launch in VR to play the CIRCLE intro first. Press a controller button to continue to Indiana Jones. Disable VR Intro becomes available under Rendering after the first intro start; this preference is saved across releases.");
        Note(4,"RENDERING",true);
        Note(4,"Anti-aliasing / Upscaling: Off, TAA, DLSS SR, or DLSS SR + VR Frame Generation. DLSS SR uses the quality setting selected in-game. DLSS VR Frame Generation requires supported NVIDIA hardware and DLSS SR. AMD FSR and AMD FSR + VR FG are experimental alternatives using the native game FSR quality setting. AMD VR FG uses FidelityFX with adaptive pacing; it generates an intermediate image for each eye. Selecting it disables Mono VR. Missing inputs or runtime errors fall back to real images.");
        Note(4,"RenderScale changes resolution per eye (0.5-2.0); lower values reduce GPU load and detail. Stereoscopic Cinema Screen enables 3D cutscenes; off uses a flat 16:9 screen. Item inspection, Journal and Travels remain 2D. Mono VR renders one shared image for both eyes, retaining tracking but removing stereo depth; cinema is also 2D. Enabling Mono VR disables integrated FG and keeps the selected upscaler.");
        Note(4,"Only Submit Real Frames is always enabled: old images are not repeatedly submitted. Integrated FG can still submit its newly generated images. Native desktop FG is blocked in stereo; it is separate from integrated VR FG. Shift-click Made by Cactus to reveal Development: the live performance overlay, diagnostics, Open Logs and Runtime Check. Enable Logging under Rendering defaults off and applies on the next game start. Use the overlay to distinguish requested FG from active generation and fallback.");
        Note(4,"FRAME GENERATION MODES",true);
        Note(4,"Auto Seamless prioritizes fresh rendered images and fills the remaining gap to the headset refresh rate, up to 4x. Auto uses fixed 2x/3x/4x steps with sustained-load checks and controlled downshift trials. Fixed multipliers keep the selected factor. Available modes depend on GPU support; AMD FG is limited to 2x. The performance overlay distinguishes real and generated stereo pairs.");
        Note(4,"MOVEMENT AND BUTTONS",true);
        Note(4,"Quest: left X/Y are lower/upper; Index: left A/B have those roles. Right A/B are lower/upper on both. Left stick moves; left stick click sprints. Right stick turns using the configured Smooth or Snap mode. Right B jumps / climbs up; right A crouches / lets go at supported climbing points. Right stick click holsters or drops the held item. Press both stick clicks together to pause.");
        Note(4,"Tap left Y to open/close the map/inventory. Hold Y for the item-selection layer: right stick selects, X confirms while Y remains held. In menus, use the right-hand ray and right trigger to click. Native menu confirmation/back uses right A/B. Hands Only under VR Options hides the gameplay body while retaining hands and held items.");
        Note(4,"PICKUP, THROWING AND COMBAT",true);
        Note(4,"Look at a usable object and squeeze either grip with that trigger released to interact; X remains the button fallback. Keep grip held to carry. Release at a valid placement prompt to place; otherwise release to drop. For an item picked up with X, first grip it to take ownership.");
        Note(4,"To throw a throwable object: hold grip, touch the same hand's Quest thumb rest or Index trackpad, keep that trigger released, swing and release grip. Throw direction follows your gaze. Holding the trigger selects striking instead of touch-assisted throwing; grip release alone does not create a strike.");
        Note(4,"Fists and melee items: hold the corresponding grip and swing to strike. Hold grip + that hand's trigger to charge, then swing while still holding both. Trigger release alone cancels the charge. Left/right fists work independently. Separate swings can repeat without releasing the buttons; let the hand slow between strokes. Both grips maintain guard when native combat allows it and restore it after attacks/hits. With empty hands, fully squeeze both grips into combat readiness and click the RIGHT stick to push an eligible nearby enemy. With a held item, right stick click still holsters. Release the click before pushing again.");
        Note(4,"Firearms start in melee mode. Tap the left trigger to toggle shooting/melee. In shooting mode, hold right grip and use the right trigger to fire along the weapon. In melee mode, right grip + swing strikes; adding right trigger charges the physical strike. Right stick click returns to fists.");
        Note(4,"WHIP AND SCRIPTED ACTIONS",true);
        Note(4,"With empty hands, reach over your right shoulder and squeeze right grip to draw the whip. Hold grip to keep it equipped; swing to crack toward the gaze target. Release grip to stow. When attached at a supported anchor, keep grip held and press right trigger for Whip-Yank. This shortcut does not apply while climbing or swinging.");
        Note(4,"Supported ledge, ladder, whip climbing/swinging and chain actions stay in first-person VR with native animated hands. Use left stick for their native directions; B climbs/jumps up and A releases at supported traversal points. In supported directional GRIP sequences, move a hand down slowly and hold it there, or swipe left/right, without pressing grip. The native left stick remains the fallback; not every story prompt has been validated.");
        Note(4,"MAP, JOURNAL, CAMERA",true);
        Note(4,"Map allows normal movement and turning. Journal / Travels use a 2D screen: left stick navigates, right stick up/down adjusts zoom, A confirms and B returns. Left/right grips switch tabs; left trigger holds zoom/detail. Tap Y to close. Ability-book selection also stays 2D.");
        Note(4,"Photo camera: left trigger opens the viewfinder, right trigger takes a photo; left/right grips zoom out/in. Viewfinder and targeting follow head pitch as well as yaw. Magnifier inspection also follows your full look direction. Ordinary HUD stays level and follows horizontal head turns with a deadzone and soft catch-up.");
        Note(4,"FALLBACK CONTROLS",true);
        Note(4,"Hold Y: X = native use/confirm; A = native RT contextual action (including reload where supported); B = quick-inventory left; left/right triggers = native LB/RB (block/whip); right stick = D-pad; right/left stick clicks = native RS/Y. Y + left grip exposes native sticks, analog triggers, X/A/B and stick clicks for scripted prompts. Release modifiers and held controls before returning to normal play.");
        Note(4,"Quest cosmetic gesture: unarmed, hold grip with your thumb on that hand's thumb rest, then pull its trigger to extend the middle finger. Physical strikes remain available. Index has no equivalent cosmetic binding. Index mappings and untested story contexts still need hardware/gameplay validation.");
        status=new TextBlock {Text="Ready",ToolTip=SettingsPath,Foreground=muted,FontSize=11,VerticalAlignment=VerticalAlignment.Bottom,TextTrimming=TextTrimming.CharacterEllipsis,Margin=new Thickness(3,0,16,0)};
        var actions=new Grid(); actions.ColumnDefinitions.Add(new ColumnDefinition {Width=new GridLength(110)});actions.ColumnDefinitions.Add(new ColumnDefinition());actions.ColumnDefinitions.Add(new ColumnDefinition {Width=new GridLength(220)});
        var logs=Button("Open Logs",(s,e)=>Safe(()=>{Directory.CreateDirectory(Path.Combine(root,"logs"));Process.Start(new ProcessStartInfo(Path.Combine(root,"logs")){UseShellExecute=true});}));
        diagnose=Button("Runtime Check",async(s,e)=>await Run(true)); launch=Button("Launch in VR",async(s,e)=>await Run(false));
        var support=Button("",(s,e)=>Safe(()=>Process.Start(new ProcessStartInfo("https://ko-fi.com/cactusvrstudios/tip"){UseShellExecute=true})));
        support.ToolTip="Support Cactus VR Studios on Ko-fi";
        System.Windows.Automation.AutomationProperties.SetName(support,"Support Cactus VR Studios on Ko-fi");
        support.Background=Brushes.Transparent;support.BorderBrush=Brushes.Transparent;support.Padding=new Thickness(0);
        var supportImage=EmbeddedResources.Image("support.png");
        support.Content=new Image {Source=supportImage,Stretch=Stretch.Uniform,Height=44};
        var devActions=new UniformGridCompat(2) {Margin=new Thickness(12,18,12,8)};
        devActions.Children.Add(logs);devActions.Children.Add(diagnose);((StackPanel)pages[3].Content).Children.Add(devActions);
        actions.Children.Add(support);Grid.SetColumn(launch,2);actions.Children.Add(launch);Grid.SetRow(actions,5);layout.Children.Add(actions);
        var footerRow=new Grid();footerRow.ColumnDefinitions.Add(new ColumnDefinition());footerRow.ColumnDefinitions.Add(new ColumnDefinition {Width=GridLength.Auto});
        footerRow.Children.Add(status);
        var footer=new TextBlock {Text="Made by Cactus",Foreground=muted,FontSize=11,HorizontalAlignment=HorizontalAlignment.Right,VerticalAlignment=VerticalAlignment.Bottom};
        footer.MouseLeftButtonUp+=(s,e)=>{RevealDevelopment(System.Windows.Input.Keyboard.Modifiers);e.Handled=true;};
        Grid.SetColumn(footer,1);footerRow.Children.Add(footer);Grid.SetRow(footerRow,6);layout.Children.Add(footerRow);
        foreach(var o in options){var check=o.Control as CheckBox;var combo=o.Control as ComboBox;if(check!=null){check.Checked+=(s,e)=>UpdateAvailability();check.Unchecked+=(s,e)=>UpdateAvailability();}if(combo!=null)combo.SelectionChanged+=(s,e)=>{if(!loading && o.Key=="upscaling" && e.AddedItems.Count>0 && (((Choice)e.AddedItems[0]).Key=="dlss-sr-fg" || ((Choice)e.AddedItems[0]).Key=="fsr-fg"))((CheckBox)options.Single(x=>x.Key=="vr_mono").Control).IsChecked=false;UpdateAvailability();};}
        // Dedicated VRAM only: shared system memory is not graphics-card VRAM.
        // Inventory uses the largest hardware adapter; the renderer chooses its
        // Vulkan device independently. Unknown inventory must not imply zero VRAM.
        bool vramWarningShown=false;
        Loaded+=(s,e)=>{if(!preview&&!vramWarningShown&&gpu.LowVram){vramWarningShown=true;
            MessageBox.Show(this,GpuInfo.LowVramWarning,"KHARVOX: CIRCLE \u2014 VRAM",MessageBoxButton.OK,MessageBoxImage.Warning);}};
        ShowTab(0); loading=false; UpdateAvailability();
        // Persist refreshed discovery on every start. Verification stays read-only.
        if(!preview && installations.Length>0)Save();
        savedSelection=SelectionSnapshot();
        autoSaveTimer.Tick+=(s,e)=>{if(busy)return;autoSaveTimer.Stop();TryAutoSave();};
        foreach(var o in options){
            var check=o.Control as CheckBox;var combo=o.Control as ComboBox;var edit=o.Control as TextBox;
            if(check!=null){check.Checked+=(s,e)=>QueueAutoSave();check.Unchecked+=(s,e)=>QueueAutoSave();}
            if(combo!=null)combo.SelectionChanged+=(s,e)=>QueueAutoSave();
            if(edit!=null)edit.TextChanged+=(s,e)=>QueueAutoSave();
        }
        gamePath.AddHandler(System.Windows.Controls.Primitives.TextBoxBase.TextChangedEvent,new TextChangedEventHandler((s,e)=>QueueAutoSave()));
        Closing+=(s,e)=>{if(preview)return;if(busy){e.Cancel=true;if(launchCancellation!=null){closeAfterLaunch=true;launchCancellation.Set();status.Text="Cancelling launch and closing VR intro...";}else status.Text="Wait for the runtime check to finish.";return;}autoSaveTimer.Stop();if(HasUnsavedChanges()&&!TryAutoSave())e.Cancel=true;};

    }
    void QueueAutoSave() {
        if(loading||preview)return;
        autoSaveTimer.Stop();autoSaveTimer.Start();
    }
    bool TryAutoSave() {
        if(!HasUnsavedChanges())return true;
        try {Save();return true;}
        catch(Exception ex){status.Text="Not saved: "+ex.Message;return false;}
    }
    string SettingsPath {get{return settingsPath;}}
    static string ChoiceLabel(string option,string key) {
        if(option=="vr_fg_multiplier")return key=="auto"?"Auto Seamless":key=="auto-fixed"?"Auto":key=="4"?"4x (experimental)":key=="3"?"3x (experimental)":"2x";
        if(option!="upscaling")return key;
        switch(key){case "official":return "Official NVIDIA";case "community":return "Community (RTX 20/30/40, experimental)";case "2":return "2x";case "3":return "3x (experimental)";case "4":return "4x (experimental)";case "off":return "Off";case "native":return "TAA";case "dlss-sr":return "DLSS SR";case "dlss-sr-fg":return "DLSS SR + VR Frame Generation";case "fsr":return "AMD FSR (experimental)";case "fsr-fg":return "AMD FSR + VR FG (experimental)";default:return key;}
    }
    object Value(string key,object fallback) {object value; return settings.TryGetValue(key,out value)&&value!=null?value:fallback;}
    void Note(int page,string text,bool title=false) {((StackPanel)pages[page].Content).Children.Add(new TextBlock {Text=text,TextWrapping=TextWrapping.Wrap,Foreground=title?Brushes.White:muted,FontSize=title?16:13,Margin=new Thickness(12,18,12,8),LineHeight=22});}
    void Add(int page,Option o) {
        options.Add(o); var row=new Grid {Margin=new Thickness(12,page==0?3:5,12,page==0?3:5),ToolTip=o.Tip}; row.ColumnDefinitions.Add(new ColumnDefinition()); row.ColumnDefinitions.Add(new ColumnDefinition {Width=new GridLength(265)});
        var value=Value(o.Key,o.Default);
        if(o.Key=="upscaling"&&value.ToString()=="game")value="off";
        if(o.Key=="upscaling"&&value.ToString()=="dlss-rr")value="dlss-sr";
        if(o.Key=="upscaling" && value.ToString()=="fsr" && Equals(Value("vr_fsr_frame_generation",false),true) && !Equals(Value("vr_mono",false),true))value="fsr-fg";
        if(o.Key=="upscaling" && value.ToString()=="dlss-sr" && Equals(Value("vr_dlss_frame_generation",false),true) && !Equals(Value("vr_mono",false),true))value="dlss-sr-fg";
        if(gpu.Amd&&o.Key=="upscaling") {if(value.ToString()=="dlss-sr-fg")value="fsr-fg";else if(value.ToString()=="dlss-sr")value="fsr";}
        if(o.Key=="vr_fg_multiplier"&&(value.ToString()=="adaptive"||new[]{"2.2","2.4","2.6","2.8","3.2","3.4","3.6","3.8"}.Contains(value.ToString())))value="auto";
        if(!gpu.Triple&&o.Key=="vr_fg_multiplier"&&value.ToString()!="auto"&&value.ToString()!="auto-fixed")value="2";
        if(o.Default is bool) {var check=new CheckBox {Content=o.Label,IsChecked=Convert.ToBoolean(value,CultureInfo.InvariantCulture),MinHeight=32};o.Control=check;Grid.SetColumnSpan(check,2);row.Children.Add(check);}
        else {
            row.Children.Add(new TextBlock {Text=o.Label,VerticalAlignment=VerticalAlignment.Center,TextWrapping=TextWrapping.Wrap,Margin=new Thickness(0,0,12,0)});
            if(o.Choices.Length>0) {var combo=new ComboBox {ItemsSource=o.Choices.Select(key=>new Choice(key,ChoiceLabel(o.Key,key))).ToArray(),DisplayMemberPath="Label",SelectedValuePath="Key",SelectedValue=value.ToString(),MinHeight=32};if(combo.SelectedIndex<0)throw new InvalidDataException(o.Key+": unsupported value "+value);o.Control=combo;}
            else {var edit=new TextBox {Text=Convert.ToString(value,CultureInfo.InvariantCulture),MinHeight=32,VerticalContentAlignment=VerticalAlignment.Center};o.Control=edit;}
            Grid.SetColumn(o.Control,1);row.Children.Add(o.Control);
        }
        ((StackPanel)pages[page].Content).Children.Add(row);
    }
    string SelectionSnapshot() {
        // Compare effective selections, not control events (which can fire on focus/layout).
        // Keep invalid edits as text so closing still offers to discard or correct them.
        var values=new List<object>();
        values.Add(gamePath.Text.Trim());
        foreach(var o in options) {
            if(o.Default is bool)values.Add(((CheckBox)o.Control).IsChecked==true);
            else if(o.Choices.Length>0)values.Add(((ComboBox)o.Control).SelectedValue);
            else {
                string text=((TextBox)o.Control).Text.Trim();double number;
                if(!(o.Default is string) && double.TryParse(text.Replace(',','.'),NumberStyles.Float,CultureInfo.InvariantCulture,out number)
                    && !double.IsNaN(number) && !double.IsInfinity(number))values.Add(number);
                else values.Add(text);
            }
        }
        return json.Serialize(values);
    }
    bool HasUnsavedChanges() {return savedSelection!=SelectionSnapshot();}
    Dictionary<string,object> Collect() {
        var result=new Dictionary<string,object>(settings);
        result.Remove("vr_low_performance");
        result["vr_intro_started"]=Equals(Value("vr_intro_started",false),true);
        result["vr_projection"]="immersive";result["skip_logo_intros"]=true;result["vr_disable_facial_hair"]=false;result.Remove("vr_disable_strand_hair");
        foreach(var pair in FixedDefaults)result[pair.Key]=pair.Value;
        foreach(var o in options) {
            if(o.Default is bool)result[o.Key]=((CheckBox)o.Control).IsChecked==true;
            else if(o.Choices.Length>0)result[o.Key]=(string)((ComboBox)o.Control).SelectedValue;
            else if(o.Default is string)result[o.Key]=((TextBox)o.Control).Text.Trim();
            else {double n;string text=((TextBox)o.Control).Text;if(!double.TryParse(text.Replace(',','.'),NumberStyles.Float,CultureInfo.InvariantCulture,out n)||double.IsNaN(n)||double.IsInfinity(n)||n<o.Min||n>o.Max||(o.Default is int&&n!=Math.Floor(n)))throw new InvalidDataException(o.Label+": enter a value from "+o.Min.ToString(CultureInfo.InvariantCulture)+" to "+o.Max.ToString(CultureInfo.InvariantCulture)+".");result[o.Key]=n;}
        }
        result["vr_fg_backend"]="auto";
        bool fsrgen=Convert.ToString(result["upscaling"])=="fsr-fg";
        result["vr_fsr_frame_generation"]=fsrgen;
        if(fsrgen&&Convert.ToString(result["vr_fg_multiplier"])!="auto"&&Convert.ToString(result["vr_fg_multiplier"])!="auto-fixed")result["vr_fg_multiplier"]="2";
        if(fsrgen){result["upscaling"]="fsr";result["vr_mono"]=false;}
        bool framegen=Convert.ToString(result["upscaling"])=="dlss-sr-fg";
        result["vr_dlss_frame_generation"]=framegen;
        if(framegen||fsrgen)result["vr_hand_smoothing_ms"]=0;
        if(framegen){result["upscaling"]="dlss-sr";result["vr_mono"]=false;}
        if(Convert.ToDouble(result["vr_roomscale_stop_threshold_m"])>Convert.ToDouble(result["vr_roomscale_start_threshold_m"])*.9)throw new InvalidDataException("Roomscale stop threshold must be at most 90% of the start threshold.");
        if(Convert.ToDouble(result["vr_body_yaw_max_residual"])<Convert.ToDouble(result["vr_body_yaw_deadzone"]))throw new InvalidDataException("Maximum yaw residual must be at least the body yaw deadzone.");
        result["game_directory"]=gamePath.Text.Trim();
        result.Remove("launcher_game_directory_detected");
        result["schema_version"]=1;return result;
    }
    void Save() {
        var next=Collect();Directory.CreateDirectory(Path.GetDirectoryName(SettingsPath));string temp=SettingsPath+"."+Guid.NewGuid().ToString("N")+".tmp";
        try {File.WriteAllText(temp,json.Serialize(next),new UTF8Encoding(false));if(File.Exists(SettingsPath))File.Replace(temp,SettingsPath,SettingsPath+".bak");else File.Move(temp,SettingsPath);}finally{if(File.Exists(temp))File.Delete(temp);}
        settings=next;savedSelection=SelectionSnapshot();status.Text="Settings saved. Changes apply on next launch.";
    }
    [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern Microsoft.Win32.SafeHandles.SafeFileHandle CreateFileW(string name,uint access,uint share,IntPtr security,uint disposition,uint flags,IntPtr template);
    [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern uint GetFinalPathNameByHandleW(Microsoft.Win32.SafeHandles.SafeFileHandle file,System.Text.StringBuilder path,uint size,uint flags);
    // Final folder behind every junction, or the path itself when it cannot be resolved.
    static string StoreInstallFolder(string path) {
        if(String.IsNullOrWhiteSpace(path))return path;
        using(var handle=CreateFileW(path,0,7,IntPtr.Zero,3,0x02000000,IntPtr.Zero)) { // no data access, share all, OPEN_EXISTING, BACKUP_SEMANTICS
            if(handle.IsInvalid)return path;
            var buffer=new System.Text.StringBuilder(32768);
            var n=GetFinalPathNameByHandleW(handle,buffer,(uint)buffer.Capacity,0);
            if(n==0||n>=buffer.Capacity)return path;
            var resolved=buffer.ToString();
            if(resolved.StartsWith(@"\\?\UNC\"))return @"\\"+resolved.Substring(8);
            return resolved.StartsWith(@"\\?\")?resolved.Substring(4):resolved;
        }
    }
    static IEnumerable<string> InstalledGames(string selected) {
        var games=new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var steamGames=new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var steamRoots=new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        Action<string> addGame=p=>{if(!String.IsNullOrWhiteSpace(p)&&File.Exists(Path.Combine(p,"TheGreatCircle.exe")))games.Add(Path.GetFullPath(p).TrimEnd(Path.DirectorySeparatorChar,Path.AltDirectorySeparatorChar));};
        addGame(selected);addGame(@"D:\Games\Jones");
        // Galaxy and standalone installers register their install location.
        foreach(var hive in new[]{Microsoft.Win32.RegistryHive.LocalMachine,Microsoft.Win32.RegistryHive.CurrentUser})
        foreach(var view in new[]{Microsoft.Win32.RegistryView.Registry32,Microsoft.Win32.RegistryView.Registry64}) {
            try {
                using(var registry=Microsoft.Win32.RegistryKey.OpenBaseKey(hive,view))
                using(var uninstall=registry.OpenSubKey(@"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall")) {
                    if(uninstall==null)continue;
                    foreach(var name in uninstall.GetSubKeyNames())using(var entry=uninstall.OpenSubKey(name)) {
                        if(entry==null)continue;
                        var title=Convert.ToString(entry.GetValue("DisplayName"));
                        if(title.IndexOf("Great Circle",StringComparison.OrdinalIgnoreCase)>=0)
                            addGame(Convert.ToString(entry.GetValue("InstallLocation")));
                    }
                }
            }catch(System.Security.SecurityException){}catch(UnauthorizedAccessException){}catch(IOException){}
        }
        // Microsoft Store / Game Pass registers the package root, which reaches the install folder through junctions.
        try {
            using(var packages=Microsoft.Win32.Registry.CurrentUser.OpenSubKey(@"Software\Classes\Local Settings\Software\Microsoft\Windows\CurrentVersion\AppModel\Repository\Packages")) {
                if(packages!=null)foreach(var name in packages.GetSubKeyNames()) {
                    if(!name.StartsWith("BethesdaSoftworks.ProjectRelic_",StringComparison.OrdinalIgnoreCase))continue;
                    using(var entry=packages.OpenSubKey(name))if(entry!=null)addGame(StoreInstallFolder(Convert.ToString(entry.GetValue("PackageRootFolder"))));
                }
            }
        }catch(System.Security.SecurityException){}catch(UnauthorizedAccessException){}catch(IOException){}
        foreach(var key in new[]{@"HKEY_CURRENT_USER\Software\Valve\Steam",@"HKEY_LOCAL_MACHINE\SOFTWARE\WOW6432Node\Valve\Steam"}) {
            var value=Microsoft.Win32.Registry.GetValue(key,key.StartsWith("HKEY_CURRENT_USER")?"SteamPath":"InstallPath",null) as string;
            if(!String.IsNullOrWhiteSpace(value))steamRoots.Add(value);
        }
        steamRoots.Add(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86),"Steam"));
        foreach(var process in Process.GetProcessesByName("steam")) {
            try {steamRoots.Add(Path.GetDirectoryName(process.MainModule.FileName));}catch(System.ComponentModel.Win32Exception){}catch(InvalidOperationException){}finally{process.Dispose();}
        }
        var libraries=new HashSet<string>(steamRoots,StringComparer.OrdinalIgnoreCase);
        foreach(var path in steamRoots) {
            var file=Path.Combine(path,"steamapps","libraryfolders.vdf");if(!File.Exists(file))continue;
            try {
                foreach(System.Text.RegularExpressions.Match m in System.Text.RegularExpressions.Regex.Matches(File.ReadAllText(file),"\"path\"\\s+\"([^\"]+)\""))libraries.Add(m.Groups[1].Value.Replace("\\\\","\\"));
            }catch(IOException){}catch(UnauthorizedAccessException){}
        }
        foreach(var library in libraries) {
            var manifest=Path.Combine(library,"steamapps","appmanifest_2677660.acf");if(!File.Exists(manifest))continue;
            try {
                var m=System.Text.RegularExpressions.Regex.Match(File.ReadAllText(manifest),"\"installdir\"\\s+\"([^\"]+)\"");
                if(m.Success) {
                    var installed=Path.GetFullPath(Path.Combine(library,"steamapps","common",m.Groups[1].Value));
                    addGame(installed);
                    if(games.Contains(installed))steamGames.Add(installed);
                }
            }catch(IOException){}catch(UnauthorizedAccessException){}
        }
        return games.OrderByDescending(p=>steamGames.Contains(p)).ThenBy(p=>p,StringComparer.OrdinalIgnoreCase);
    }
    void Browse() {var dialog=new Microsoft.Win32.OpenFileDialog {Title="Select TheGreatCircle.exe (GOG / Steam / Game Pass)",Filter="Indiana Jones executable|TheGreatCircle.exe",CheckFileExists=true};if(Directory.Exists(gamePath.Text))dialog.InitialDirectory=gamePath.Text;if(dialog.ShowDialog(this)==true){gamePath.Text=Path.GetDirectoryName(dialog.FileName);if(!gamePath.Items.Contains(gamePath.Text))gamePath.Items.Add(gamePath.Text);}}
    Dictionary<string,object> LaunchParameters(bool check) {
        var mode=Convert.ToString(settings["launch_mode"]);
        var args=new Dictionary<string,object>{{"PackageRoot",root},{"SettingsFile",SettingsPath},{"GameDirectory",Convert.ToString(settings["game_directory"])},{"Mode",check?"diagnostic":mode=="stereo"||mode=="diagnostic-aer"||mode=="experimental-stereo"?"cinema":mode}};
        if(!String.IsNullOrWhiteSpace(Convert.ToString(settings["runtime_json"])))args["RuntimeJson"]=Convert.ToString(settings["runtime_json"]);
        if(!check){if(mode=="stereo")args["Stereo"]=true;if(mode=="diagnostic-aer")args["DiagnosticAER"]=true;if(mode=="experimental-stereo")args["ExperimentalStereo"]=true;if(mode=="stereo"&&Convert.ToBoolean(settings["experimental_hand_ik"]))args["ExperimentalHandIK"]=true;if(args.ContainsKey("ExperimentalHandIK")&&Convert.ToBoolean(settings["experimental_punch"]))args["ExperimentalPunch"]=true;if(Convert.ToBoolean(settings["capture_shaders"]))args["CaptureShaders"]=true;args["CaptureStartFrame"]=settings["capture_start_frame"];args["CaptureFrameCount"]=settings["capture_frame_count"];args["MonitorSeconds"]=settings["monitor_seconds"];}
        return args;
    }
    internal string Arguments(bool check) {
        return String.Join("",LaunchParameters(check).Select(p=>" -"+p.Key+(p.Value is bool?"":" "+Convert.ToString(p.Value,CultureInfo.InvariantCulture))));
    }
    async Task Run(bool check) {
        if(busy)return;
        try {Save();if(!check&&!Directory.Exists(gamePath.Text.Trim()))throw new DirectoryNotFoundException("Select an existing game folder.");busy=true;launch.IsEnabled=diagnose.IsEnabled=false;status.Text=check?"Checking OpenXR runtime...":"Starting Indiana Jones...";
            bool logging=Convert.ToBoolean(settings["enable_logging"]);var args=LaunchParameters(check);
            if(!check){string eventName="Local\\CircleLaunch-"+Guid.NewGuid().ToString("N");launchCancellation=new EventWaitHandle(false,EventResetMode.ManualReset,eventName);args["LaunchCancelEvent"]=eventName;}
            string payload=EmbeddedLaunch.Payload(args);
            string output=await Task.Run(()=>{
                var info=EmbeddedLaunch.StartInfo(root);
                using(var process=Process.Start(info)){
                    var stderr=process.StandardError.ReadToEndAsync();var stdout=new StringBuilder();string line;
                    var bytes=Encoding.UTF8.GetBytes(payload);process.StandardInput.BaseStream.Write(bytes,0,bytes.Length);process.StandardInput.Close();
                    while((line=process.StandardOutput.ReadLine())!=null){
                        stdout.AppendLine(line);
                        if(line=="CIRCLE_INTRO_STARTED"||line=="CIRCLE_INTRO_FINISHED"){
                            string progress=line;Dispatcher.BeginInvoke(new Action(()=>IntroProgress(progress)));
                        }
                    }
                    process.WaitForExit();string log=stdout.ToString()+stderr.Result;
                    if(logging){Directory.CreateDirectory(Path.Combine(root,"logs"));File.WriteAllText(Path.Combine(root,"logs",check?"launcher-runtime-check.txt":"launcher-output.txt"),log);}
                    if(process.ExitCode!=0)throw new InvalidOperationException(log.Trim());return log;
                }
            });
            status.Text=check?"Runtime check complete.":logging?"Game launched. See the session folder in Logs.":"Game launched. Logging is disabled.";
            if(check)MessageBox.Show(this,output,"OpenXR Runtime Check",MessageBoxButton.OK,MessageBoxImage.Information);
        }catch(Exception ex){if(!closeAfterLaunch)Error(ex);}finally{
            if(launchCancellation!=null){launchCancellation.Dispose();launchCancellation=null;}
            busy=false;launch.IsEnabled=diagnose.IsEnabled=true;
            if(closeAfterLaunch){closeAfterLaunch=false;Close();}
        }
    }
    void IntroProgress(string progress) {
        settings["vr_intro_started"]=true;UpdateAvailability();
        if(!closeAfterLaunch)status.Text=progress=="CIRCLE_INTRO_STARTED"?
            "VR intro running. Press a controller button to start Indiana Jones.":"VR intro finished. Starting Indiana Jones...";
    }
    void Safe(Action action) {try{action();}catch(Exception ex){Error(ex);}}
    void Error(Exception ex) {status.Text="Action failed. "+ex.Message;MessageBox.Show(this,ex.Message,"KHARVOX: CIRCLE",MessageBoxButton.OK,MessageBoxImage.Error);}
    void RevealDevelopment(System.Windows.Input.ModifierKeys modifiers) {
        if((modifiers & System.Windows.Input.ModifierKeys.Shift)==0)return;
        tabs[3].Visibility=Visibility.Visible;
        ((System.Windows.Controls.Primitives.UniformGrid)tabs[3].Parent).Columns=6;
    }
    void ShowTab(int index) {activeTab=index;for(int i=0;i<pages.Count;i++){pages[i].Visibility=i==index?Visibility.Visible:Visibility.Collapsed;tabs[i].BorderBrush=i==index?accent:Brushes.Black;}}
    void UpdateAvailability() {
        if(loading)return;
        Func<string,Option> find=key=>options.Single(o=>o.Key==key);
        Func<string,bool> check=key=>((CheckBox)find(key).Control).IsChecked==true;
        ((FrameworkElement)find("disable_vr_intro").Control.Parent).Visibility=Equals(Value("vr_intro_started",false),true)?Visibility.Visible:Visibility.Collapsed;
        Func<string,string> choice=key=>{var item=((ComboBox)find(key).Control).SelectedItem as Choice;return item==null?"":item.Key;};
        bool fg=choice("upscaling")=="dlss-sr-fg"||choice("upscaling")=="fsr-fg";
        ((FrameworkElement)find("vr_fg_multiplier").Control.Parent).Visibility=fg?Visibility.Visible:Visibility.Collapsed;
        find("vr_fg_multiplier").Control.IsEnabled=fg;
        if(choice("upscaling")=="fsr-fg"&&choice("vr_fg_multiplier")!="auto"&&choice("vr_fg_multiplier")!="auto-fixed")((ComboBox)find("vr_fg_multiplier").Control).SelectedValue="2";
        if(check("vr_mono") && choice("upscaling")=="fsr-fg")((ComboBox)find("upscaling").Control).SelectedValue="fsr";
        if(check("vr_mono") && choice("upscaling")=="dlss-sr-fg")((ComboBox)find("upscaling").Control).SelectedValue="dlss-sr";
        find("vr_stereo_cinematics").Control.IsEnabled=!check("vr_mono");
        find("vr_snap_angle").Control.IsEnabled=choice("vr_turn_mode")=="snap";
        find("vr_smooth_turn_speed").Control.IsEnabled=choice("vr_turn_mode")=="smooth";
        foreach(var key in new[]{"capture_start_frame","capture_frame_count"})find(key).Control.IsEnabled=check("capture_shaders");
    }

    Style ButtonStyle() {var s=new Style(typeof(Button));s.Setters.Add(new Setter(Control.BackgroundProperty,accent));s.Setters.Add(new Setter(Control.ForegroundProperty,Brushes.White));s.Setters.Add(new Setter(Control.BorderBrushProperty,accent));s.Setters.Add(new Setter(Control.FontSizeProperty,14.0));s.Setters.Add(new Setter(Control.PaddingProperty,new Thickness(8)));s.Setters.Add(new Setter(FrameworkElement.MarginProperty,new Thickness(3,4,3,4)));var template=new ControlTemplate(typeof(Button));var border=new FrameworkElementFactory(typeof(Border));border.SetValue(Border.BackgroundProperty,new TemplateBindingExtension(Control.BackgroundProperty));border.SetValue(Border.BorderBrushProperty,new TemplateBindingExtension(Control.BorderBrushProperty));border.SetValue(Border.BorderThicknessProperty,new TemplateBindingExtension(Control.BorderThicknessProperty));var presenter=new FrameworkElementFactory(typeof(ContentPresenter));presenter.SetValue(FrameworkElement.HorizontalAlignmentProperty,HorizontalAlignment.Center);presenter.SetValue(FrameworkElement.VerticalAlignmentProperty,VerticalAlignment.Center);border.AppendChild(presenter);template.VisualTree=border;s.Setters.Add(new Setter(Control.TemplateProperty,template));var disabled=new Trigger {Property=UIElement.IsEnabledProperty,Value=false};disabled.Setters.Add(new Setter(UIElement.OpacityProperty,.45));s.Triggers.Add(disabled);var hover=new Trigger {Property=UIElement.IsMouseOverProperty,Value=true};hover.Setters.Add(new Setter(UIElement.OpacityProperty,.8));s.Triggers.Add(hover);return s;}
    Style TabStyle() {var s=new Style(typeof(Button), (Style)Resources[typeof(Button)]);s.Setters.Add(new Setter(Control.BackgroundProperty,Brushes.Black));s.Setters.Add(new Setter(Control.BorderThicknessProperty,new Thickness(0,0,0,3)));s.Setters.Add(new Setter(Control.BorderBrushProperty,Brushes.Black));return s;}
    Style TextStyle() {var s=new Style(typeof(TextBox));s.Setters.Add(new Setter(Control.BackgroundProperty,Brushes.Black));s.Setters.Add(new Setter(Control.ForegroundProperty,Brushes.White));s.Setters.Add(new Setter(Control.BorderBrushProperty,new SolidColorBrush(Color.FromRgb(62,62,66))));s.Setters.Add(new Setter(Control.PaddingProperty,new Thickness(10,3,10,3)));return s;}
    Style CheckStyle() {
        var s=new Style(typeof(CheckBox));s.Setters.Add(new Setter(Control.ForegroundProperty,Brushes.White));s.Setters.Add(new Setter(Control.VerticalContentAlignmentProperty,VerticalAlignment.Center));
        var t=new ControlTemplate(typeof(CheckBox));var panel=new FrameworkElementFactory(typeof(StackPanel));panel.SetValue(StackPanel.OrientationProperty,Orientation.Horizontal);
        var box=new FrameworkElementFactory(typeof(Border),"Box");box.SetValue(FrameworkElement.WidthProperty,20.0);box.SetValue(FrameworkElement.HeightProperty,20.0);box.SetValue(FrameworkElement.VerticalAlignmentProperty,VerticalAlignment.Center);box.SetValue(Border.BorderThicknessProperty,new Thickness(1));box.SetValue(Border.BorderBrushProperty,new SolidColorBrush(Color.FromRgb(62,62,66)));box.SetValue(Border.BackgroundProperty,Brushes.Black);
        var mark=new FrameworkElementFactory(typeof(TextBlock),"Mark");mark.SetValue(TextBlock.TextProperty,"\u2713");mark.SetValue(TextBlock.FontSizeProperty,16.0);mark.SetValue(TextBlock.ForegroundProperty,Brushes.White);mark.SetValue(FrameworkElement.HorizontalAlignmentProperty,HorizontalAlignment.Center);mark.SetValue(UIElement.VisibilityProperty,Visibility.Hidden);box.AppendChild(mark);panel.AppendChild(box);
        var content=new FrameworkElementFactory(typeof(ContentPresenter));content.SetValue(FrameworkElement.MarginProperty,new Thickness(14,0,0,0));content.SetValue(FrameworkElement.VerticalAlignmentProperty,VerticalAlignment.Center);panel.AppendChild(content);t.VisualTree=panel;
        var on=new Trigger {Property=System.Windows.Controls.Primitives.ToggleButton.IsCheckedProperty,Value=true};on.Setters.Add(new Setter(Border.BackgroundProperty,accent,"Box"));on.Setters.Add(new Setter(Border.BorderBrushProperty,accent,"Box"));on.Setters.Add(new Setter(UIElement.VisibilityProperty,Visibility.Visible,"Mark"));t.Triggers.Add(on);s.Setters.Add(new Setter(Control.TemplateProperty,t));return s;
    }
    Style ComboStyle() {
        return (Style)System.Windows.Markup.XamlReader.Parse(@"
<Style xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation' xmlns:x='http://schemas.microsoft.com/winfx/2006/xaml' TargetType='{x:Type ComboBox}'>
 <Setter Property='Foreground' Value='#F5F5F5'/><Setter Property='Background' Value='Black'/><Setter Property='BorderBrush' Value='#3E3E42'/><Setter Property='Padding' Value='10,5'/><Setter Property='ScrollViewer.CanContentScroll' Value='True'/>
 <Setter Property='ItemContainerStyle'><Setter.Value><Style TargetType='{x:Type ComboBoxItem}'>
  <Setter Property='Foreground' Value='#F5F5F5'/><Setter Property='Background' Value='Black'/><Setter Property='Padding' Value='10,6'/>
  <Setter Property='Template'><Setter.Value><ControlTemplate TargetType='{x:Type ComboBoxItem}'><Border x:Name='Item' Background='{TemplateBinding Background}' Padding='{TemplateBinding Padding}'><ContentPresenter/></Border><ControlTemplate.Triggers><Trigger Property='IsHighlighted' Value='True'><Setter TargetName='Item' Property='Background' Value='#3E3E42'/></Trigger></ControlTemplate.Triggers></ControlTemplate></Setter.Value></Setter>
 </Style></Setter.Value></Setter>
 <Setter Property='Template'><Setter.Value><ControlTemplate TargetType='{x:Type ComboBox}'>
  <Grid>
   <ToggleButton Focusable='False' IsChecked='{Binding IsDropDownOpen, Mode=TwoWay, RelativeSource={RelativeSource TemplatedParent}}' ClickMode='Press'>
    <ToggleButton.Template><ControlTemplate TargetType='{x:Type ToggleButton}'><Border Background='Black' BorderBrush='#3E3E42' BorderThickness='1'><TextBlock Text='&#x2304;' Foreground='#F5F5F5' HorizontalAlignment='Right' VerticalAlignment='Center' Margin='0,0,10,2'/></Border></ControlTemplate></ToggleButton.Template>
   </ToggleButton>
   <ContentPresenter x:Name='SelectionContent' IsHitTestVisible='False' Content='{TemplateBinding SelectionBoxItem}' ContentTemplate='{TemplateBinding SelectionBoxItemTemplate}' Margin='10,5,28,5' VerticalAlignment='Center'/>
   <TextBox x:Name='PART_EditableTextBox' Visibility='Collapsed' Background='Black' Foreground='#F5F5F5' BorderThickness='0' Padding='0' Margin='10,5,28,5' VerticalContentAlignment='Center' IsReadOnly='{TemplateBinding IsReadOnly}'/>
   <Popup x:Name='PART_Popup' Placement='Bottom' IsOpen='{TemplateBinding IsDropDownOpen}' AllowsTransparency='True' Focusable='False' PopupAnimation='Slide'>
    <Border Background='Black' BorderBrush='#3E3E42' BorderThickness='1' MinWidth='{Binding ActualWidth, RelativeSource={RelativeSource TemplatedParent}}' MaxHeight='300'><ScrollViewer><ItemsPresenter KeyboardNavigation.DirectionalNavigation='Contained'/></ScrollViewer></Border>
   </Popup>
  </Grid>
  <ControlTemplate.Triggers><Trigger Property='IsEnabled' Value='False'><Setter Property='Opacity' Value='0.45'/></Trigger><Trigger Property='IsEditable' Value='True'><Setter TargetName='SelectionContent' Property='Visibility' Value='Collapsed'/><Setter TargetName='PART_EditableTextBox' Property='Visibility' Value='Visible'/></Trigger></ControlTemplate.Triggers>
 </ControlTemplate></Setter.Value></Setter>
</Style>");
    }
    Button Button(string text,RoutedEventHandler handler) {var b=new Button {Content=text};b.Click+=handler;return b;}
    internal void VerifyGpuLayouts() {
        // Model fixtures exercise UI migration and restrictions without another physical GPU.
        foreach(var model in new[]{"RTX 2080 Ti","RTX 3080 Laptop GPU","RTX 4080 SUPER","RTX 5090","AMD Radeon RX 7900 XTX","Unknown GPU"}) {
            var info=new GpuInfo {Name=model,Vendor=model.StartsWith("RTX")?0x10deu:model.StartsWith("AMD")?0x1002u:0u};
            var window=new Launcher(root,true,info);
            var aa=(ComboBox)window.options.Single(o=>o.Key=="upscaling").Control;
            var mult=(ComboBox)window.options.Single(o=>o.Key=="vr_fg_multiplier").Control;
            if(mult.Items.Cast<Choice>().Any(c=>c.Key.Contains(".")||c.Key=="adaptive"))throw new Exception("Fractional FG choices still exposed");
            if(ChoiceLabel("vr_fg_multiplier","auto")!="Auto Seamless"||ChoiceLabel("vr_fg_multiplier","auto-fixed")!="Auto")throw new Exception("Auto mode labels are ambiguous");
            if(window.options.Any(o=>o.Key=="vr_fg_backend"))throw new Exception("Manual backend still exposed");
            if(info.Amd&&aa.Items.Cast<Choice>().Any(c=>c.Key.StartsWith("dlss")))throw new Exception("AMD exposes DLSS");
            if(!info.Triple&&mult.Items.Cast<Choice>().Any(c=>c.Key=="3"||c.Key=="4"))throw new Exception("Unsupported 3x exposed");
            aa.SelectedValue=info.Amd?"fsr-fg":"dlss-sr-fg";
            foreach(var factor in info.Triple?new[]{"auto","auto-fixed","2","3","4"}:new[]{"auto","auto-fixed","2"}) {
                mult.SelectedValue=factor;var result=window.Collect();
                if(!Equals(result["vr_fg_backend"],"auto")||!Equals(result["vr_fg_multiplier"],factor))throw new Exception("GPU auto settings mismatch: "+model);
                if(Convert.ToDouble(result["vr_hand_smoothing_ms"])!=0)throw new Exception("FG must disable hand smoothing: "+model+" "+factor);
                bool community=model.Contains("RTX 20")||model.Contains("RTX 30")||model.Contains("RTX 40");
                if(info.Backend(factor).StartsWith("Community")!=community)throw new Exception("GPU backend hint mismatch: "+model);
            }
            window.Close();
        }
        var mixed=new GpuInfo {Name="Mixed AMD/NVIDIA",Vendor=0x1002,Mixed=true};
        if(mixed.Amd||!mixed.Triple)throw new Exception("Mixed GPU options suppressed");
        var detected=GpuInfo.Detect();Directory.CreateDirectory(Path.Combine(root,"logs"));
        File.WriteAllText(Path.Combine(root,"logs","launcher-gpu-verification.txt"),"PASS: GPU family layouts, automatic backend, multiplier restrictions. Detected: "+detected.Name+" vendor="+detected.Vendor.ToString("X4")+" mixed="+detected.Mixed);
    }
    internal void VerifyInstallations() {
        var defaults=json.Deserialize<Dictionary<string,object>>(EmbeddedResources.Text("defaults.json"));
        if(Convert.ToString(defaults["vr_fg_multiplier"])!="auto-fixed"||
           Convert.ToBoolean(defaults["vr_performance_overlay"])||Convert.ToBoolean(defaults["vr_hands_only"])||
           !Convert.ToBoolean(defaults["vr_laser_sight"])||Convert.ToDouble(defaults["vr_smooth_turn_speed"])!=240||
           Convert.ToDouble(defaults["vr_snap_angle"])!=60)throw new Exception("Beta 0.2 embedded first-start defaults mismatch.");
        foreach(ulong size in new ulong[]{0,8UL<<30,10UL<<30})
            if(!new GpuInfo{DedicatedVramBytes=size}.LowVram)throw new Exception("Low VRAM warning missing.");
        foreach(ulong size in new ulong[]{(10UL<<30)+1,12UL<<30,16UL<<30,24UL<<30})
            if(new GpuInfo{DedicatedVramBytes=size}.LowVram)throw new Exception("Unexpected low VRAM warning.");
        if(new GpuInfo().LowVram)throw new Exception("Unknown VRAM must not trigger a low-memory warning.");
        if(!Equals(options.Single(o=>o.Key=="vr_fg_multiplier").Default,"auto-fixed")||
           !Equals(options.Single(o=>o.Key=="vr_snap_angle").Default,60)||
           !Equals(options.Single(o=>o.Key=="vr_smooth_turn_speed").Default,240))throw new Exception("Beta 0.2 fallback defaults mismatch.");
        // Headless WPF template/selection check: never show or control a window.
        var perf=options.Single(o=>o.Key=="vr_performance_overlay");
        if(!Equals(perf.Default,false))throw new Exception("Performance overlay must default off.");
        var perfCheck=(CheckBox)perf.Control;var oldPerf=perfCheck.IsChecked;
        var laser=options.Single(o=>o.Key=="vr_laser_sight");
        if(!Equals(laser.Default,true))throw new Exception("Laser Sight must default on.");
        var laserCheck=(CheckBox)laser.Control;var oldLaser=laserCheck.IsChecked;
        var hands=options.Single(o=>o.Key=="vr_hands_only");
        if(!Equals(hands.Default,false))throw new Exception("Hands Only must default off.");
        var handsCheck=(CheckBox)hands.Control;var oldHands=handsCheck.IsChecked;
        var cinema=options.Single(o=>o.Key=="vr_stereo_cinematics");
        if(!Equals(cinema.Default,false))throw new Exception("Cinema must default to 2D.");
        var cinemaCheck=(CheckBox)cinema.Control;var oldCinema=cinemaCheck.IsChecked;

        var mono=options.Single(o=>o.Key=="vr_mono");
        if(!Equals(mono.Default,false))throw new Exception("Mono VR must default off.");
        var monoCheck=(CheckBox)mono.Control;var oldMono=monoCheck.IsChecked;
        var low=options.Single(o=>o.Key=="enable_logging");
        if(!Equals(low.Default,false))throw new Exception("Enable Logging must default off.");
        if(options.IndexOf(low)!=options.IndexOf(mono)+1)throw new Exception("Enable Logging must follow Mono VR.");
        var lowCheck=(CheckBox)low.Control;var oldLow=lowCheck.IsChecked;
        foreach(string key in new[]{"cheat_invulnerable","cheat_stealth","cheat_stamina","cheat_oxygen","cheat_ammo","cheat_damage"}) {
            var cheat=options.Single(x=>x.Key==key);
            if(!Equals(cheat.Default,false))throw new Exception("Cheat must default off: "+key);
        }
        var cheatMultiplier=options.Single(x=>x.Key=="cheat_damage_multiplier");
        if(!Equals(cheatMultiplier.Default,99)||cheatMultiplier.Min!=1||cheatMultiplier.Max!=99)throw new Exception("Invalid damage multiplier limits.");
        VerifyFixedDefaults();
        if(HasUnsavedChanges())throw new Exception("Unchanged launcher reports unsaved settings.");
        handsCheck.IsChecked=!(oldHands==true);
        if(!HasUnsavedChanges())throw new Exception("Changed setting not detected.");
        handsCheck.IsChecked=oldHands;
        if(HasUnsavedChanges())throw new Exception("Reverted setting still reports unsaved settings.");
        var scaleText=(TextBox)options.Single(o=>o.Key=="vr_render_scale").Control;
        var oldScale=scaleText.Text;
        scaleText.Text=Convert.ToDouble(oldScale,CultureInfo.InvariantCulture).ToString("F3",CultureInfo.InvariantCulture).Replace('.',',');
        if(HasUnsavedChanges())throw new Exception("Equivalent numeric formatting reports unsaved settings.");
        scaleText.Text=oldScale;
        var oldPath=gamePath.Text;gamePath.Text=oldPath+"-changed";
        if(!HasUnsavedChanges())throw new Exception("Changed installation not detected.");
        gamePath.Text=oldPath;
        if(HasUnsavedChanges())throw new Exception("Reverted installation reports unsaved settings.");
        foreach(bool selected in new[]{false,true}){
            handsCheck.IsChecked=selected;perfCheck.IsChecked=selected;laserCheck.IsChecked=selected;
            cinemaCheck.IsChecked=selected;
            monoCheck.IsChecked=selected;
            lowCheck.IsChecked=selected;

            var roundtrip=json.Deserialize<Dictionary<string,object>>(json.Serialize(Collect()));
            if(!Equals(roundtrip["vr_performance_overlay"],selected))throw new Exception("Performance setting serialization failed.");
            if(!Equals(roundtrip["vr_laser_sight"],selected))throw new Exception("Laser Sight serialization failed.");
            if(!Equals(roundtrip["vr_hands_only"],selected))throw new Exception("Hands Only serialization failed.");
            if(!Equals(roundtrip["vr_stereo_cinematics"],selected))throw new Exception("Cinema setting serialization failed.");
            if(!Equals(roundtrip["vr_only_submit_real_frames"],true))throw new Exception("Real-only serialization failed.");
            if(!Equals(roundtrip["vr_mono"],selected))throw new Exception("Mono VR serialization failed.");
            if(!Equals(roundtrip["enable_logging"],selected))throw new Exception("Enable Logging serialization failed.");
        }
        lowCheck.IsChecked=oldLow;
        UpdateAvailability();
        if(!monoCheck.IsEnabled||cinemaCheck.IsEnabled)throw new Exception("Mono VR cinema override failed.");
        monoCheck.IsChecked=false;UpdateAvailability();
        if(!cinemaCheck.IsEnabled)throw new Exception("Cinema option unavailable in VR.");
        if(!perfCheck.IsEnabled)throw new Exception("Performance overlay unavailable in stereo.");
        if(!handsCheck.IsEnabled)throw new Exception("Hands Only unavailable in stereo.");

        perfCheck.IsChecked=oldPerf;handsCheck.IsChecked=oldHands;laserCheck.IsChecked=oldLaser;
        cinemaCheck.IsChecked=oldCinema;monoCheck.IsChecked=oldMono;UpdateAvailability();
        var aa=(ComboBox)options.Single(o=>o.Key=="upscaling").Control;
        var oldAa=aa.SelectedValue;
        aa.SelectedValue="dlss-sr-fg";
        var multiplier=(ComboBox)options.Single(o=>o.Key=="vr_fg_multiplier").Control;
        var oldMultiplier=multiplier.SelectedValue;
        multiplier.SelectedValue="3";
        if(!multiplier.IsEnabled||!Equals(Collect()["vr_fg_backend"],"auto")||!Equals(Collect()["vr_fg_multiplier"],"3"))throw new Exception("Community 3x settings failed.");
        multiplier.SelectedValue="4";
        if(!Equals(Collect()["vr_fg_multiplier"],"4"))throw new Exception("Community 4x settings failed.");
        multiplier.SelectedValue="auto";
        if(!Equals(Collect()["vr_fg_multiplier"],"auto")||!Equals(Collect()["vr_hand_smoothing_ms"],0))throw new Exception("Auto FG settings failed.");
        multiplier.SelectedValue="auto-fixed";
        if(!Equals(Collect()["vr_fg_multiplier"],"auto-fixed")||!Equals(Collect()["vr_hand_smoothing_ms"],0))throw new Exception("Integer Auto FG settings failed.");
        multiplier.SelectedValue="4";
        var fgSettings=Collect();
        if(!Equals(fgSettings["upscaling"],"dlss-sr") || !Equals(fgSettings["vr_dlss_frame_generation"],true) || !Equals(fgSettings["vr_only_submit_real_frames"],true) || !Equals(fgSettings["vr_mono"],false))throw new Exception("Combined FG selection failed.");
        aa.SelectedValue="native";
        if(!Equals(Collect()["vr_dlss_frame_generation"],false))throw new Exception("FG not cleared.");
        aa.SelectedValue="dlss-sr-fg";monoCheck.IsChecked=true;
        if(Convert.ToString(aa.SelectedValue)!="dlss-sr" || !Equals(Collect()["vr_dlss_frame_generation"],false))throw new Exception("Mono / FG exclusion failed.");
        aa.SelectedValue="fsr-fg";
        var amdSettings=Collect();
        if(!multiplier.IsEnabled||!Equals(amdSettings["vr_fg_multiplier"],"2"))throw new Exception("AMD 2x limit failed.");
        multiplier.SelectedValue="auto";
        if(!Equals(Collect()["vr_fg_multiplier"],"auto"))throw new Exception("AMD Auto Seamless settings failed.");
        multiplier.SelectedValue="auto-fixed";
        if(!Equals(Collect()["vr_fg_multiplier"],"auto-fixed"))throw new Exception("AMD integer Auto settings failed.");
        if(!Equals(amdSettings["upscaling"],"fsr") || !Equals(amdSettings["vr_fsr_frame_generation"],true) || !Equals(amdSettings["vr_dlss_frame_generation"],false) || !Equals(amdSettings["vr_mono"],false))throw new Exception("AMD FG selection failed.");
        monoCheck.IsChecked=true;
        if(Convert.ToString(aa.SelectedValue)!="fsr" || !Equals(Collect()["vr_fsr_frame_generation"],false))throw new Exception("AMD Mono / FG exclusion failed.");
        aa.SelectedValue="dlss-sr-fg";
        if(!Equals(Collect()["vr_fsr_frame_generation"],false))throw new Exception("AMD FG not cleared when changing vendor.");
        monoCheck.IsChecked=oldMono;aa.SelectedValue=oldAa;multiplier.SelectedValue=oldMultiplier;
        gamePath.Measure(new Size(700,40));gamePath.Arrange(new Rect(0,0,700,40));gamePath.ApplyTemplate();
        var edit=gamePath.Template.FindName("PART_EditableTextBox",gamePath) as TextBox;
        if(edit==null||edit.Visibility!=Visibility.Visible)throw new Exception("Game directory has no editable template part.");
        foreach(string path in gamePath.Items){gamePath.SelectedItem=path;gamePath.UpdateLayout();if(gamePath.Text!=path||edit.Text!=path||Convert.ToString(Collect()["game_directory"])!=path)throw new Exception("Installation selection does not reach launch settings.");}
        edit.Text=@"D:\Manual game folder";
        if(gamePath.Text!=edit.Text||Convert.ToString(Collect()["game_directory"])!=edit.Text)throw new Exception("Manual game directory does not reach launch settings.");
        var result=Path.Combine(root,"logs","launcher-installation-check.txt");Directory.CreateDirectory(Path.GetDirectoryName(result));
        File.WriteAllText(result,"PASS: unchanged/changed/reverted selections and numeric equivalence; cinema 2D default, boolean serialization and availability; editable directory, dropdown selection, collected settings. Detected:\n"+String.Join("\n",InstalledGames(Convert.ToString(settings["game_directory"]))));
    }
    void VerifyFixedDefaults() {
        var original=settings;
        try {
            settings=new Dictionary<string,object>(settings);
            // Even legacy/custom values must not revive removed controls.
            foreach(var pair in FixedDefaults)settings[pair.Key]="obsolete";
            var collected=Collect();
            foreach(var pair in FixedDefaults) {
                object expected=pair.Key=="vr_hand_smoothing_ms"&&(Equals(collected["vr_dlss_frame_generation"],true)||Equals(collected["vr_fsr_frame_generation"],true))?0:pair.Value;
                if(options.Any(o=>o.Key==pair.Key)||!Equals(collected[pair.Key],expected))
                    throw new Exception("Fixed default failed: "+pair.Key);
            }
            if(((StackPanel)pages[1].Content).Children.Count!=4 || ((StackPanel)pages[2].Content).Children.Count!=2)
                throw new Exception("Movement / VR Options cleanup failed.");
            foreach(var entry in new[]{Tuple.Create("vr_hands_only",2),Tuple.Create("vr_laser_sight",2),Tuple.Create("vr_performance_overlay",3)}) {
                var control=options.Single(o=>o.Key==entry.Item1).Control;
                var row=control.Parent as Grid;
                if(row==null || row.Parent!=pages[entry.Item2].Content)throw new Exception("Option placement failed: "+entry.Item1);
            }
            settings=collected;
            string command=Arguments(false);
            if(!command.Contains(" -Stereo")||!command.Contains(" -ExperimentalHandIK")||!command.Contains(" -ExperimentalPunch"))
                throw new Exception("Fixed VR launch flags missing.");
        } finally {settings=original;}
    }
    internal void VerifyAndRender(string destination) {
        Directory.CreateDirectory(destination);Collect();
        var tabStrip=(System.Windows.Controls.Primitives.UniformGrid)tabs[3].Parent;
        if(tabs[3].Visibility!=Visibility.Collapsed||tabStrip.Children[tabStrip.Children.Count-1]!=tabs[4])throw new Exception("Development visibility / Instructions order failed.");
        RevealDevelopment(System.Windows.Input.ModifierKeys.None);
        if(tabs[3].Visibility!=Visibility.Collapsed)throw new Exception("Ordinary click revealed Development.");
        RevealDevelopment(System.Windows.Input.ModifierKeys.Shift);
        if(tabs[3].Visibility!=Visibility.Visible||tabStrip.Columns!=6)throw new Exception("Shift click did not reveal Development.");
        tabs[3].Visibility=Visibility.Collapsed;tabStrip.Columns=5;
        var instructions=(StackPanel)pages[4].Content;
        if(!(instructions.Children[0] is Image)||!(instructions.Children[1] is Image))throw new Exception("Controller schematics must lead Instructions.");
        if(!(((StackPanel)pages[5].Content).Children[0] is Grid))throw new Exception("Unexpected cheat introduction.");
        VerifyFixedDefaults();
        var saved=settings;settings=Collect();if(!Arguments(false).Contains("-SettingsFile")||!Arguments(true).Contains("-Mode diagnostic"))throw new Exception("Launch integration failed.");
        foreach(var mode in new[]{"stereo","cinema","attach","off","diagnostic-aer","experimental-stereo"}){
            settings["launch_mode"]=mode;string command=Arguments(false);
            if(mode=="stereo"&&!command.Contains(" -Stereo"))throw new Exception("Stereo argument missing.");
            if(mode=="diagnostic-aer"&&!command.Contains(" -DiagnosticAER"))throw new Exception("AER argument missing.");
            if(mode=="experimental-stereo"&&!command.Contains(" -ExperimentalStereo"))throw new Exception("Experimental argument missing.");
            if(mode!="stereo"&&(command.Contains(" -ExperimentalHandIK")||command.Contains(" -ExperimentalPunch")))throw new Exception("Hand flags escaped stereo gate.");
        }
        settings=saved;
        var storage=Path.Combine(destination,"storage-check");Directory.CreateDirectory(storage);
        var original=Collect();foreach(string key in new[]{"cheat_invulnerable","cheat_stealth","cheat_stamina","cheat_oxygen","cheat_ammo","cheat_damage"})original[key]=true;original["cheat_damage_multiplier"]=23;original["launcher_game_directory_detected"]=true;original["future_extension"]=new Dictionary<string,object>{{"nested","preserved"}};File.WriteAllText(Path.Combine(storage,"circle-settings.json"),json.Serialize(original));
        var roundtrip=new Launcher(storage,true,new GpuInfo {Name="NVIDIA GeForce RTX 4080 SUPER",Vendor=0x10de});
        var rediscovered=InstalledGames(Convert.ToString(original["game_directory"])).ToArray();
        if(rediscovered.Contains(Convert.ToString(original["game_directory"]),StringComparer.OrdinalIgnoreCase)&&!String.Equals(roundtrip.gamePath.Text,Convert.ToString(original["game_directory"]),StringComparison.OrdinalIgnoreCase))throw new Exception("Saved installation preference was lost.");
        foreach(string key in new[]{"cheat_invulnerable","cheat_stealth","cheat_stamina","cheat_oxygen","cheat_ammo","cheat_damage"})
            if(!Equals(roundtrip.Collect()[key],true))throw new Exception("Cheat persistence failed: "+key);
        if(Convert.ToDouble(roundtrip.Collect()["cheat_damage_multiplier"])!=23)throw new Exception("Damage multiplier persistence failed.");
        if(roundtrip.Collect().ContainsKey("launcher_game_directory_detected"))throw new Exception("Legacy one-time detection flag retained.");
        ((TextBox)roundtrip.options.Single(o=>o.Key=="vr_render_scale").Control).Text="1,25";roundtrip.Save();
        var persisted=json.Deserialize<Dictionary<string,object>>(File.ReadAllText(Path.Combine(storage,"circle-settings.json")));
        if(Convert.ToDouble(persisted["vr_render_scale"])!=1.25||((Dictionary<string,object>)persisted["future_extension"])["nested"].ToString()!="preserved"||!File.Exists(Path.Combine(storage,"circle-settings.json.bak")))throw new Exception("Storage roundtrip failed.");
        roundtrip.Save();roundtrip.Close();
        original["vr_intro_started"]=false;original["disable_vr_intro"]=false;
        File.WriteAllText(Path.Combine(storage,"circle-settings.json"),json.Serialize(original));
        var firstIntro=new Launcher(storage,true,new GpuInfo {Name="NVIDIA GeForce RTX 4080 SUPER",Vendor=0x10de});
        var skipIntro=(CheckBox)firstIntro.options.Single(o=>o.Key=="disable_vr_intro").Control;
        if(((FrameworkElement)skipIntro.Parent).Visibility!=Visibility.Collapsed||skipIntro.IsChecked==true)throw new Exception("Intro opt-out must initially be hidden and off.");
        firstIntro.IntroProgress("CIRCLE_INTRO_STARTED");
        if(((FrameworkElement)skipIntro.Parent).Visibility!=Visibility.Visible||((FrameworkElement)skipIntro.Parent).Parent!=firstIntro.pages[0].Content)throw new Exception("Intro first start did not reveal Rendering option.");
        skipIntro.IsChecked=true;firstIntro.Save();firstIntro.Close();
        var restoredIntro=new Launcher(storage,true,new GpuInfo {Name="NVIDIA GeForce RTX 4080 SUPER",Vendor=0x10de});
        if(!Equals(restoredIntro.Collect()["vr_intro_started"],true)||!Equals(restoredIntro.Collect()["disable_vr_intro"],true))throw new Exception("Intro preference did not survive launcher restart.");
        restoredIntro.ShowInTaskbar=false;restoredIntro.Show();restoredIntro.UpdateLayout();
        var introBitmap=new RenderTargetBitmap((int)restoredIntro.ActualWidth,(int)restoredIntro.ActualHeight,96,96,PixelFormats.Pbgra32);introBitmap.Render(restoredIntro);
        var introEncoder=new PngBitmapEncoder();introEncoder.Frames.Add(BitmapFrame.Create(introBitmap));using(var stream=File.Create(Path.Combine(destination,"launcher-intro-enabled.png")))introEncoder.Save(stream);restoredIntro.Close();
        foreach(var mode in new[]{"off","native","dlss-sr","dlss-rr","game","fsr"}){
            original["vr_dlss_frame_generation"]=false;original["vr_fsr_frame_generation"]=false;original["upscaling"]=mode;File.WriteAllText(Path.Combine(storage,"circle-settings.json"),json.Serialize(original));
            var temporal=new Launcher(storage,true,new GpuInfo {Name="NVIDIA GeForce RTX 4080 SUPER",Vendor=0x10de});var selected=(ComboBox)temporal.options.Single(o=>o.Key=="upscaling").Control;
            string expected=mode=="game"?"off":mode=="dlss-rr"?"dlss-sr":mode;
            if(selected.Items.Count!=6)throw new Exception("Retired RR mode still selectable.");
            if(Convert.ToString(selected.SelectedValue)!=expected)throw new Exception("AA selection/migration failed: "+mode);
            temporal.Save();var readback=json.Deserialize<Dictionary<string,object>>(File.ReadAllText(Path.Combine(storage,"circle-settings.json")));
            if(Convert.ToString(readback["upscaling"])!=expected)throw new Exception("AA selection persistence failed: "+mode);
            temporal.Close();
        }
        foreach(var factor in new[]{"auto","auto-fixed","adaptive","2.4"}) {
            original["vr_fg_multiplier"]=factor;
            File.WriteAllText(Path.Combine(storage,"circle-settings.json"),json.Serialize(original));
            var restored=new Launcher(storage,true,new GpuInfo {Name="NVIDIA GeForce RTX 4080 SUPER",Vendor=0x10de});
            var expected=factor=="auto-fixed"?"auto-fixed":"auto";
            if(!Equals(restored.Collect()["vr_fg_multiplier"],expected))throw new Exception("Auto mode reload/migration failed: "+factor);
            restored.Save();restored.Close();
            var readback=json.Deserialize<Dictionary<string,object>>(File.ReadAllText(Path.Combine(storage,"circle-settings.json")));
            if(!Equals(readback["vr_fg_multiplier"],expected))throw new Exception("Auto mode save failed: "+factor);
        }
        original["upscaling"]="dlss-sr";original["vr_dlss_frame_generation"]=true;original["vr_fsr_frame_generation"]=false;original["vr_fg_backend"]="community";original["vr_fg_multiplier"]="3";original["vr_mono"]=false;
        File.WriteAllText(Path.Combine(storage,"circle-settings.json"),json.Serialize(original));
        var amd=new Launcher(storage,true,new GpuInfo {Name="AMD Radeon RX 7900 XTX",Vendor=0x1002});
        var migrated=amd.Collect();
        if(!Equals(migrated["vr_fg_backend"],"auto")||!Equals(migrated["upscaling"],"fsr")||!Equals(migrated["vr_fsr_frame_generation"],true)||!Equals(migrated["vr_dlss_frame_generation"],false)||!Equals(migrated["vr_fg_multiplier"],"2"))throw new Exception("GPU change migration failed");
        amd.ShowInTaskbar=false;amd.Show();amd.UpdateLayout();
        var amdBitmap=new RenderTargetBitmap((int)amd.ActualWidth,(int)amd.ActualHeight,96,96,PixelFormats.Pbgra32);amdBitmap.Render(amd);
        var amdEncoder=new PngBitmapEncoder();amdEncoder.Frames.Add(BitmapFrame.Create(amdBitmap));using(var stream=File.Create(Path.Combine(destination,"launcher-amd.png")))amdEncoder.Save(stream);amd.Close();
        VerifyGpuLayouts();
        for(int i=0;i<pages.Count;i++){
            var shot=new Launcher(root,true,new GpuInfo {Name="NVIDIA GeForce RTX 4080 SUPER",Vendor=0x10de});shot.ShowInTaskbar=false;shot.Show();shot.ShowTab(i);if(i==0){((ComboBox)shot.options.Single(o=>o.Key=="upscaling").Control).SelectedValue="dlss-sr-fg";((ComboBox)shot.options.Single(o=>o.Key=="vr_fg_multiplier").Control).SelectedValue="3";}shot.UpdateLayout();
            var frame=new DispatcherFrame();Dispatcher.BeginInvoke(DispatcherPriority.Background,new Action(()=>frame.Continue=false));Dispatcher.PushFrame(frame);
            var bitmap=new RenderTargetBitmap((int)shot.ActualWidth,(int)shot.ActualHeight,96,96,PixelFormats.Pbgra32);bitmap.Render(shot);var encoder=new PngBitmapEncoder();encoder.Frames.Add(BitmapFrame.Create(bitmap));using(var stream=File.Create(Path.Combine(destination,"launcher-"+i+".png")))encoder.Save(stream);shot.Close();
        }
        File.WriteAllText(Path.Combine(destination,"launcher-verification.txt"),"PASS: all "+options.Count+" settings created; fixed defaults and page placement; six launch modes and hand gates; six AA/upscaling choices and legacy/RR migration; atomic storage roundtrip, backup and nested unknown keys; all six pages rendered. No game launched or user settings changed.");ShowTab(0);savedSelection=SelectionSnapshot();
    }
}
sealed class UniformGridCompat : System.Windows.Controls.Primitives.UniformGrid {public UniformGridCompat(int columns){Columns=columns;}}
static class Program {
    [STAThread] public static int Main(string[] args) {
        if(args.Length>0&&args[0]=="--verify-settings"){
            try {SettingsStore.Verify();return 0;}catch(Exception ex){Directory.CreateDirectory(Path.Combine(AppDomain.CurrentDomain.BaseDirectory,"logs"));File.WriteAllText(Path.Combine(AppDomain.CurrentDomain.BaseDirectory,"logs","settings-verification-error.txt"),ex.ToString());return 1;}
        }
        if(args.Length>0&&args[0]=="--verify-installations"){
            try{var app=new Application();var window=new Launcher(AppDomain.CurrentDomain.BaseDirectory,true,new GpuInfo {Name="NVIDIA GeForce RTX 4080 SUPER",Vendor=0x10de});window.VerifyInstallations();window.VerifyGpuLayouts();window.Close();return 0;}
            catch(Exception ex){var logs=Path.Combine(AppDomain.CurrentDomain.BaseDirectory,"logs");Directory.CreateDirectory(logs);File.WriteAllText(Path.Combine(logs,"launcher-installation-error.txt"),ex.ToString());return 1;}
        }
        try {string root=AppDomain.CurrentDomain.BaseDirectory;bool test=args.Length>0&&args[0]=="--verify";var app=new Application();var window=new Launcher(root,test,test?new GpuInfo {Name="NVIDIA GeForce RTX 4080 SUPER",Vendor=0x10de}:null);if(test){window.ShowInTaskbar=false;window.Loaded+=(s,e)=>window.Dispatcher.BeginInvoke(new Action(()=>{try{window.VerifyAndRender(Path.Combine(root,"logs","launcher-preview"));app.Shutdown(0);}catch(Exception ex){Directory.CreateDirectory(Path.Combine(root,"logs"));File.WriteAllText(Path.Combine(root,"logs","launcher-verification-error.txt"),ex.ToString());app.Shutdown(1);}}),DispatcherPriority.ApplicationIdle);}return app.Run(window);}catch(Exception ex){MessageBox.Show(ex.Message,"Circle Launcher",MessageBoxButton.OK,MessageBoxImage.Error);return 1;}
    }
}
}



