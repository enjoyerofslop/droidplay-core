; DroidPlay for Windows' installer (Inno Setup 7): DroidPlay-Setup-<version>.exe, for the user who runs it, with no administrator prompt.
;   ISCC /DVersion=<x.y.z> /DStage=<folder stage.ps1 made> [/DNumVersion=<x.y.z.0>] [/O<output folder>] dm\android\windows\droidplay.iss
; It installs the staged folder (runtime\: Python with the app's modules, core\: DroidPlay Core's tree, licenses\: their licences, DroidPlay.ico) into %LOCALAPPDATA%\Programs\DroidPlay,
; with Start menu and desktop shortcuts and an entry in Settings > Apps. Per user, never Program Files: the app writes into core\ (the .NET build of
; nx_extract, Python's caches). Games, builds, the Android SDK, the tools Install what's missing brought, the signing key and the settings live outside it,
; so an update replaces the folder whole and uninstalling leaves them.
;   Setup.exe /VERYSILENT /SUPPRESSMSGBOXES [/RELAUNCH]   an update from the app: closes it (Restart Manager), installs, and opens it again with /RELAUNCH
;   With /RELAUNCH and /VERYSILENT it shows a small window of its own while it works ("Updating DroidPlay", with the install's progress): DroidPlay has closed
;   by then, and the wizard's own window is hidden.

#ifndef Version
  #define Version "0.0.0"
#endif
#ifndef Stage
  #error Give /DStage=<the folder stage.ps1 made>
#endif

[Setup]
AppId={{F4672997-257F-40CC-AACF-A40DA5BF3E6E}
AppName=DroidPlay
AppVersion={#Version}
AppVerName=DroidPlay {#Version}
AppPublisher=enjoyerofslop
AppPublisherURL=https://github.com/enjoyerofslop/droidplay-core
AppSupportURL=https://github.com/enjoyerofslop/droidplay-core#help-and-reports
AppUpdatesURL=https://github.com/enjoyerofslop/droidplay-core/releases
AppComments=Pack the Switch games you own for your Android phone
#ifdef NumVersion
VersionInfoVersion={#NumVersion}
#endif
SetupArchitecture=x64
MinVersion=10.0.17763
PrivilegesRequired=lowest
DefaultDirName={autopf}\DroidPlay
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableWelcomePage=no
DisableReadyPage=yes
ShowLanguageDialog=no
WizardStyle=modern
SetupIconFile={#Stage}\DroidPlay.ico
UninstallDisplayIcon={app}\DroidPlay.ico
UninstallDisplayName=DroidPlay
OutputBaseFilename=DroidPlay-Setup-{#Version}
Compression=lzma2/ultra64
SolidCompression=yes
LZMAUseSeparateProcess=yes
; a running DroidPlay, or a build it started, holds runtime\ (python.exe, its DLLs and .pyd modules): closed before its files are replaced. An update
; started from the app reopens it itself (/RELAUNCH), so Windows' restart of closed applications is not used
CloseApplications=force
CloseApplicationsFilter=*.exe,*.dll,*.pyd
RestartApplications=no
SetupLogging=yes

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"

[Messages]
WelcomeLabel2=DroidPlay packs the Switch games you own for your Android phone.%n%nIt installs for you only, in your user folder, and doesn't need administrator rights. Your games, their builds and your saves aren't touched.
FinishedLabel=DroidPlay is installed. Open it from the Start menu or the desktop; its Settings page says what else the first build needs.

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[InstallDelete]
; the folder as the release has it, with nothing left from the version before (a file removed upstream, an old module)
Type: filesandordirs; Name: "{app}\core"
Type: filesandordirs; Name: "{app}\runtime"
Type: filesandordirs; Name: "{app}\licenses"

[Files]
Source: "{#Stage}\runtime\*"; DestDir: "{app}\runtime"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#Stage}\core\*"; DestDir: "{app}\core"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#Stage}\licenses\*"; DestDir: "{app}\licenses"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#Stage}\DroidPlay.ico"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
; the AppUserModelID the app sets for itself (nxnative_android.py main): its taskbar button and the shortcut's are one
Name: "{autoprograms}\DroidPlay"; Filename: "{app}\runtime\pythonw.exe"; Parameters: """{app}\core\scripts\nxnative_android.py"""; WorkingDir: "{app}\core"; IconFilename: "{app}\DroidPlay.ico"; Comment: "Pack your Switch games for Android"; AppUserModelID: "io.github.enjoyerofslop.droidplay.desktop"
Name: "{autodesktop}\DroidPlay"; Filename: "{app}\runtime\pythonw.exe"; Parameters: """{app}\core\scripts\nxnative_android.py"""; WorkingDir: "{app}\core"; IconFilename: "{app}\DroidPlay.ico"; Comment: "Pack your Switch games for Android"; AppUserModelID: "io.github.enjoyerofslop.droidplay.desktop"; Tasks: desktopicon

[Run]
Filename: "{app}\runtime\pythonw.exe"; Parameters: """{app}\core\scripts\nxnative_android.py"""; WorkingDir: "{app}\core"; Description: "{cm:LaunchProgram,DroidPlay}"; Flags: nowait postinstall skipifsilent
Filename: "{app}\runtime\pythonw.exe"; Parameters: """{app}\core\scripts\nxnative_android.py"""; WorkingDir: "{app}\core"; Flags: nowait; Check: Relaunch

[UninstallDelete]
; what the app wrote into its own folder (nx_extract's .NET build, Python's caches)
Type: filesandordirs; Name: "{app}\core"
Type: filesandordirs; Name: "{app}\runtime"

[Code]
const
  OldHome = '{localappdata}\DroidPlay';   // where Install DroidPlay.cmd (0.2.0 to 0.4.x) put DroidPlay, run by uv

var
  DeleteTools: Boolean;
  UpdateForm: TSetupForm;
  UpdateBar: TNewProgressBar;

function Relaunch: Boolean;
var
  I: Integer;
begin
  Result := False;
  for I := 1 to ParamCount do
    if CompareText(ParamStr(I), '/RELAUNCH') = 0 then Result := True;
end;

function VerySilent: Boolean;
var
  I: Integer;
begin
  Result := False;
  for I := 1 to ParamCount do
    if CompareText(ParamStr(I), '/VERYSILENT') = 0 then Result := True;
end;

// An update DroidPlay started (Update: /VERYSILENT /RELAUNCH) shows nothing of the wizard, and DroidPlay has closed: without a window of its own, nothing on
// screen says anything is happening until DroidPlay opens again
procedure ShowUpdateWindow;
var
  Title, Body: TNewStaticText;
  M, W: Integer;
begin
  UpdateForm := CreateCustomForm(ScaleX(400), ScaleY(110), False, False);
  UpdateForm.Caption := 'Updating DroidPlay';
  UpdateForm.BorderIcons := [];
  UpdateForm.Position := poScreenCenter;
  M := ScaleX(16); W := UpdateForm.ClientWidth - 2 * M;   // the form scales itself: the text and the bar follow its width, and its height them
  Title := TNewStaticText.Create(UpdateForm);
  Title.Parent := UpdateForm;
  Title.Left := M; Title.Top := ScaleY(14);
  Title.Font.Style := [fsBold];
  Title.Caption := 'Updating DroidPlay to {#Version}';
  Body := TNewStaticText.Create(UpdateForm);
  Body.Parent := UpdateForm;
  Body.Left := M; Body.Top := Title.Top + Title.Height + ScaleY(8);
  Body.WordWrap := True; Body.Width := W;
  Body.Caption := 'DroidPlay opens again when it''s done. Your games, builds and settings stay.';
  UpdateBar := TNewProgressBar.Create(UpdateForm);
  UpdateBar.Parent := UpdateForm;
  UpdateBar.Left := M; UpdateBar.Top := Body.Top + Body.Height + ScaleY(12); UpdateBar.Width := W; UpdateBar.Height := ScaleY(16);
  UpdateBar.Style := npbstMarquee;   // until the files start: closing DroidPlay, unpacking
  UpdateForm.ClientHeight := UpdateBar.Top + UpdateBar.Height + M;
  UpdateForm.Show;
  Log('Showing the update window');
end;

procedure CurInstallProgressChanged(CurProgress, MaxProgress: Integer);
begin
  if (UpdateBar = nil) or (MaxProgress <= 0) then Exit;
  UpdateBar.Style := npbstNormal;
  UpdateBar.Max := MaxProgress;
  UpdateBar.Position := CurProgress;
end;

procedure InitializeWizard;
begin
  if Relaunch and VerySilent then ShowUpdateWindow;
end;

procedure DeinitializeSetup;
begin
  if UpdateForm <> nil then UpdateForm.Close;
end;

// Whether a running program's command line names Path: DroidPlay's window, or a build it started, run from that folder. Replacing a folder under a
// program that uses it deletes its files one by one before Windows refuses the folder itself (what the 0.2.0 and 0.2.1 installers did)
function InUse(const Path: String): Boolean;
var
  Locator, Service, Items, Item: Variant;
  I: Integer;
begin
  Result := False;
  try
    Locator := CreateOleObject('WbemScripting.SWbemLocator');
    Service := Locator.ConnectServer('.', 'root\CIMV2');
    Items := Service.ExecQuery('SELECT CommandLine FROM Win32_Process');
    for I := 0 to Items.Count - 1 do
    begin
      Item := Items.ItemIndex(I);
      if not VarIsNull(Item.CommandLine) then
        if Pos(Lowercase(Path), Lowercase(Item.CommandLine)) > 0 then
        begin
          Result := True;
          Exit;
        end;
    end;
  except
    Log('The running programs could not be listed: ' + GetExceptionMessage);
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  if DirExists(ExpandConstant(OldHome + '\droidnative')) and InUse(ExpandConstant(OldHome + '\droidnative')) then
    Result := 'DroidPlay is open, or one of its builds is running (the DroidPlay that Install DroidPlay.cmd installed). Close it, then run Setup again.';
end;

function ShortcutTarget(const Lnk: String): String;
var
  Shell, Shortcut: Variant;
begin
  Result := '';
  try
    Shell := CreateOleObject('WScript.Shell');
    Shortcut := Shell.CreateShortcut(Lnk);
    Result := Shortcut.TargetPath;
  except
  end;
end;

// The DroidPlay Install DroidPlay.cmd installed goes once this one is in: its source folder and its downloads. Its shortcuts have the names Setup's have, so
// they were replaced, but for one Setup didn't make (the desktop's, left unticked), which still runs uv. uv and its cache stay: the player may use uv for more
procedure RemoveOldInstall;
var
  Home: String;
begin
  Home := ExpandConstant(OldHome);
  if DirExists(Home + '\droidnative') then
  begin
    Log('Removing the DroidPlay Install DroidPlay.cmd installed: ' + Home);
    DelTree(Home + '\droidnative', True, True, True);
    DelTree(Home + '\droidnative.old', True, True, True);
    DelTree(Home + '\downloads', True, True, True);
    DelTree(Home + '\download', True, True, True);
    DeleteFile(Home + '\version.txt');
    DeleteFile(Home + '\DroidPlay.ico');
    RemoveDir(Home);
  end;
  if Pos('uvw.exe', Lowercase(ShortcutTarget(ExpandConstant('{userdesktop}\DroidPlay.lnk')))) > 0 then DeleteFile(ExpandConstant('{userdesktop}\DroidPlay.lnk'));
  if Pos('uvw.exe', Lowercase(ShortcutTarget(ExpandConstant('{userprograms}\DroidPlay.lnk')))) > 0 then DeleteFile(ExpandConstant('{userprograms}\DroidPlay.lnk'));
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then RemoveOldInstall;
end;

function InitializeUninstall: Boolean;
var
  Tools: String;
begin
  Result := True;
  DeleteTools := False;
  Tools := ExpandConstant('{localappdata}\nxnative\android\tools');
  if (not UninstallSilent) and DirExists(Tools) then
    DeleteTools := MsgBox('Also delete the Java and .NET tools DroidPlay installed, in ' + Tools + '?' + #13#10#13#10 +
                          'Keep them if you will use DroidPlay again: Install what''s missing would have to download them again.',
                          mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep <> usPostUninstall then Exit;
  if DeleteTools then DelTree(ExpandConstant('{localappdata}\nxnative\android\tools'), True, True, True);
  if not UninstallSilent then
    MsgBox('DroidPlay is removed. These stay, for you to delete if you want them gone:' + #13#10#13#10 +
           '- your games, and their builds beside them' + #13#10 +
           '- the Android SDK, in ' + ExpandConstant('{localappdata}\Android\Sdk') + ' (Android Studio uses it too)' + #13#10 +
           '- DroidPlay''s library of built games, and its tools if you kept them, in ' + ExpandConstant('{localappdata}\nxnative') + #13#10 +
           '- the key the phone app is signed with, in ' + ExpandConstant('{%USERPROFILE}\.android\nxnative-debug.jks') + #13#10 +
           '  (keep it: a DroidPlay app signed with another key can''t update this one without deleting its saves)',
           mbInformation, MB_OK);
end;
