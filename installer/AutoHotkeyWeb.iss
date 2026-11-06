; ============================================================================
;  AutoHotkey Web - Inno Setup installer
;  Builds a single setup.exe that installs the engine + compiler + docs, sets
;  up the .ahkw file type and a "New -> AutoHotkey Web Script" entry, wires the
;  SciTE4 "AHK Web (x64)" platform, and (optionally) adds the install dir to PATH.
;
;  Build:  "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" AutoHotkeyWeb.iss
;
;  Encoding: this .iss is UTF-8. The ShellNew template (Template.ahk) is copied
;  byte-for-byte with its UTF-8 BOM, so Korean in new scripts stays intact.
; ============================================================================

#define AppName "AutoHotkey Web"
#define AppVersion "1.1.37.02"
#define AppPublisher "AutoHotkey Web"
#define AppExe "AutoHotkeyWeb.exe"
; All source paths below are RELATIVE to this .iss file's folder (installer\),
; so the build never hardcodes the Korean project path. ".." is the project root.

[Setup]
AppId={{9E2B7C14-AHKW-4E21-9F3A-AUTOHOTKEYWEB}}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
DefaultDirName={autopf}\AutoHotkey Web
DefaultGroupName=AutoHotkey Web
DisableProgramGroupPage=yes
LicenseFile=LICENSE.txt
OutputDir=output
OutputBaseFilename=AutoHotkeyWeb_Setup_{#AppVersion}
UninstallDisplayIcon={app}\{#AppExe}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Broadcast SHCNE_ASSOCCHANGED so Explorer picks up the new .ahkw type / ShellNew entry.
ChangesAssociations=yes

[Languages]
Name: "korean"; MessagesFile: "compiler:Languages\Korean.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[CustomMessages]
korean.TaskPath=AutoHotkey Web 폴더를 PATH 에 추가 (터미널에서 AutoHotkeyWeb/AhkWeb2Exe 사용)
english.TaskPath=Add the AutoHotkey Web folder to PATH (use AutoHotkeyWeb/AhkWeb2Exe from a terminal)
korean.TaskScite=SciTE4AutoHotkey 에서 .ahkw 지원 (구문 강조 · 실행 F5 · 디버그 F7 · 컴파일 · 자동완성)
english.TaskScite=Set up .ahkw in SciTE4AutoHotkey (highlighting, run F5, debug F7, compile, autocomplete)
korean.NoWebView2=Microsoft Edge WebView2 런타임이 감지되지 않았습니다.%n%nWebGui 창은 이 런타임이 있어야 동작합니다. 설치는 계속되지만, 나중에 Microsoft 에서 WebView2 런타임(Evergreen)을 설치하세요.
english.NoWebView2=Microsoft Edge WebView2 Runtime was not detected.%n%nWebGui windows need it to work. Setup will continue; please install the WebView2 Runtime (Evergreen) from Microsoft afterwards.
korean.OpenDocs=문서 열기 (시작하기)
english.OpenDocs=Open the documentation (Getting started)
korean.TypeFull=전체 설치 (딥러닝 모델 포함)
english.TypeFull=Full installation (includes deep-learning models)
korean.TypeMinimal=최소 설치 (엔진/컴파일러/예제만, 모델 제외)
english.TypeMinimal=Minimal installation (engine/compiler/examples only, no models)
korean.CompEngine=엔진 · 컴파일러 · 문서 · 예제 (필수)
english.CompEngine=Engine, compiler, docs and examples (required)
korean.CompModels=딥러닝 모델 (한글 OCR + kor_unsmile 분류 + kor_embed 백본, 약 230MB)
english.CompModels=Deep-learning models (Korean OCR + kor_unsmile classifier + kor_embed backbone, ~230MB)
korean.CompilerName=AhkWeb2Exe 컴파일러
english.CompilerName=AhkWeb2Exe Compiler
korean.TaskDesktop=바탕화면에 컴파일러 바로가기 만들기
english.TaskDesktop=Create a desktop shortcut for the compiler
korean.TaskAhkCompile=.ahk 파일 우클릭 메뉴에도 "AutoHotkey Web으로 컴파일" 추가
english.TaskAhkCompile=Also add "Compile with AutoHotkey Web" to the .ahk right-click menu
korean.VerbCompile=컴파일 (.exe)
english.VerbCompile=Compile Script (.exe)
korean.VerbCompileGui=컴파일 (옵션 창: 아이콘 · 버전 정보)...
english.VerbCompileGui=Compile with Options (icon, version info)...
korean.VerbAhkCompileGui=AutoHotkey Web으로 컴파일 (옵션 창)...
english.VerbAhkCompileGui=Compile with AutoHotkey Web (options)...

[Types]
Name: "full"; Description: "{cm:TypeFull}"
Name: "minimal"; Description: "{cm:TypeMinimal}"

[Components]
Name: "engine"; Description: "{cm:CompEngine}"; Types: full minimal; Flags: fixed
Name: "models"; Description: "{cm:CompModels}"; Types: full

[Tasks]
Name: "addtopath"; Description: "{cm:TaskPath}"; Flags: unchecked
Name: "scite"; Description: "{cm:TaskScite}"; Check: SciTEInstalled
Name: "desktopicon"; Description: "{cm:TaskDesktop}"; GroupDescription: "{cm:AdditionalIcons}"
Name: "ahkcompile"; Description: "{cm:TaskAhkCompile}"; Flags: unchecked

[Dirs]
Name: "{win}\ShellNew"; Flags: uninsneveruninstall

[Files]
Source: "..\{#AppExe}";                      DestDir: "{app}";          Flags: ignoreversion
; ONNX Runtime - delay-loaded by the engine for the native deep-learning (TextCls/TextNN/OcrImageAI) functions.
Source: "..\onnxruntime.dll";                DestDir: "{app}";          Flags: ignoreversion
Source: "..\compiler\AhkWeb2Exe.exe";        DestDir: "{app}\compiler"; Flags: ignoreversion
Source: "..\compiler\AutoHotkeyWeb.bin";     DestDir: "{app}\compiler"; Flags: ignoreversion
; Setup-wizard stub used by AhkWeb2Exe's "EXE + Setup.exe" output.
Source: "..\compiler\AhkWebSetup.bin";       DestDir: "{app}\compiler"; Flags: ignoreversion
Source: "..\scite\ahkweb.api";               DestDir: "{app}\scite";    Flags: ignoreversion
Source: "..\docs\*";                         DestDir: "{app}\docs";     Flags: ignoreversion recursesubdirs createallsubdirs
; Bundled libraries (native CDP adapter + pure-AHK JSON) and runnable examples.
Source: "..\Lib\*";                          DestDir: "{app}\Lib";      Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\examples\*";                     DestDir: "{app}\examples"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "LICENSE.txt";                       DestDir: "{app}";          Flags: ignoreversion
Source: "..\README.md";                      DestDir: "{app}";          Flags: ignoreversion skipifsourcedoesntexist
; Deep-learning models (optional, ~230MB): ocr (PaddleOCR det/rec/cls) + kor_unsmile classifier + kor_embed backbone.
Source: "..\models\*";                       DestDir: "{app}\models";   Components: models; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
; ShellNew template (preserves UTF-8 BOM via a byte copy) + a reference copy.
Source: "Template.ahk";                      DestDir: "{win}\ShellNew"; DestName: "AutoHotkeyWeb.ahk"; Flags: ignoreversion uninsneveruninstall
Source: "Template.ahk";                      DestDir: "{app}";          DestName: "Template.ahk"; Flags: ignoreversion

[Registry]
; Engine install dir (read by A_AhkPath / GetAHKInstallDir).
Root: HKLM; Subkey: "SOFTWARE\AutoHotkeyWeb"; ValueType: string; ValueName: "InstallDir"; ValueData: "{app}"; Flags: uninsdeletekey

; .ahkw file type -> AutoHotkey Web
Root: HKCR; Subkey: ".ahkw"; ValueType: string; ValueData: "AutoHotkeyWeb.Script"; Flags: uninsdeletevalue
Root: HKCR; Subkey: ".ahkw"; ValueType: string; ValueName: "PerceivedType"; ValueData: "text"; Flags: uninsdeletevalue
Root: HKCR; Subkey: "AutoHotkeyWeb.Script"; ValueType: string; ValueData: "AutoHotkey Web Script"; Flags: uninsdeletekey
Root: HKCR; Subkey: "AutoHotkeyWeb.Script\DefaultIcon"; ValueType: string; ValueData: "{app}\{#AppExe},0"
Root: HKCR; Subkey: "AutoHotkeyWeb.Script\shell\open\command"; ValueType: string; ValueData: """{app}\{#AppExe}"" ""%1"" %*"
Root: HKCR; Subkey: "AutoHotkeyWeb.Script\shell\edit"; ValueType: string; ValueData: "Edit Script"
Root: HKCR; Subkey: "AutoHotkeyWeb.Script\shell\edit\command"; ValueType: string; ValueData: "{code:GetEditCmd}"
Root: HKCR; Subkey: "AutoHotkeyWeb.Script\shell\compile"; ValueType: string; ValueData: "{cm:VerbCompile}"
Root: HKCR; Subkey: "AutoHotkeyWeb.Script\shell\compile\command"; ValueType: string; ValueData: """{app}\compiler\AhkWeb2Exe.exe"" /in ""%1"""
; Compile with options: opens the AhkWeb2Exe window with the script prefilled.
Root: HKCR; Subkey: "AutoHotkeyWeb.Script\shell\compilegui"; ValueType: string; ValueData: "{cm:VerbCompileGui}"
Root: HKCR; Subkey: "AutoHotkeyWeb.Script\shell\compilegui"; ValueType: string; ValueName: "Icon"; ValueData: "{app}\compiler\AhkWeb2Exe.exe,0"
Root: HKCR; Subkey: "AutoHotkeyWeb.Script\shell\compilegui\command"; ValueType: string; ValueData: """{app}\compiler\AhkWeb2Exe.exe"" /gui ""%1"""
; Optional: same for plain .ahk files, without taking over the .ahk association (SystemFileAssociations).
Root: HKCR; Subkey: "SystemFileAssociations\.ahk\shell\AhkWebCompileGui"; ValueType: string; ValueData: "{cm:VerbAhkCompileGui}"; Tasks: ahkcompile; Flags: uninsdeletekey
Root: HKCR; Subkey: "SystemFileAssociations\.ahk\shell\AhkWebCompileGui"; ValueType: string; ValueName: "Icon"; ValueData: "{app}\compiler\AhkWeb2Exe.exe,0"; Tasks: ahkcompile
Root: HKCR; Subkey: "SystemFileAssociations\.ahk\shell\AhkWebCompileGui\command"; ValueType: string; ValueData: """{app}\compiler\AhkWeb2Exe.exe"" /gui ""%1"""; Tasks: ahkcompile

; "New -> AutoHotkey Web Script" shell entry.
Root: HKCR; Subkey: ".ahkw\ShellNew"; ValueType: string; ValueName: "FileName"; ValueData: "AutoHotkeyWeb.ahk"; Flags: uninsdeletekey
; The Explorer "New" menu cache (HKCU) is cleared in [Code] as the ORIGINAL user, see ClearShellNewCache.

; Optional: add install dir to the system PATH.
Root: HKLM; Subkey: "SYSTEM\CurrentControlSet\Control\Session Manager\Environment"; ValueType: expandsz; ValueName: "Path"; ValueData: "{olddata};{app}"; Tasks: addtopath; Check: NeedsAddPath

[Icons]
Name: "{group}\{cm:CompilerName}"; Filename: "{app}\compiler\AhkWeb2Exe.exe"; WorkingDir: "{app}\compiler"
Name: "{autodesktop}\{cm:CompilerName}"; Filename: "{app}\compiler\AhkWeb2Exe.exe"; WorkingDir: "{app}\compiler"; Tasks: desktopicon
Name: "{group}\AutoHotkey Web 문서"; Filename: "{app}\docs\index.htm"
Name: "{group}\예제 폴더 (딥러닝 · CDP)"; Filename: "{app}\examples"
Name: "{group}\새 스크립트 템플릿"; Filename: "{app}\Template.ahk"
Name: "{group}\{cm:UninstallProgram,AutoHotkey Web}"; Filename: "{uninstallexe}"

[Run]
Filename: "{app}\docs\index.htm"; Description: "{cm:OpenDocs}"; Flags: postinstall shellexec skipifsilent nowait

[UninstallDelete]
Type: files; Name: "{win}\ShellNew\AutoHotkeyWeb.ahk"

[Code]
function SciTEDir(): String;
var ahk: String;
begin
  Result := '';
  if RegQueryStringValue(HKLM, 'SOFTWARE\AutoHotkey', 'InstallDir', ahk) then
    if DirExists(ahk + '\SciTE') then
      Result := ahk + '\SciTE';
  if (Result = '') and DirExists(ExpandConstant('{commonpf}\AutoHotkey\SciTE')) then
    Result := ExpandConstant('{commonpf}\AutoHotkey\SciTE');
end;

function SciTEInstalled(): Boolean;
begin
  Result := SciTEDir() <> '';
end;

function GetEditCmd(Param: String): String;
var sc: String;
begin
  sc := SciTEDir();
  if sc <> '' then
    Result := '"' + sc + '\SciTE.exe" "%1"'
  else
    Result := 'notepad.exe "%1"';
end;

function NeedsAddPath(): Boolean;
var OrigPath: String;
begin
  if not RegQueryStringValue(HKLM, 'SYSTEM\CurrentControlSet\Control\Session Manager\Environment', 'Path', OrigPath) then
  begin
    Result := True; exit;
  end;
  Result := Pos(';' + ExpandConstant('{app}') + ';', ';' + OrigPath + ';') = 0;
end;

procedure InitializeWizard();
begin
  // WebView2 runtime presence check (per-machine, x64).
  if not RegKeyExists(HKLM, 'SOFTWARE\WOW6432Node\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}')
     and not RegKeyExists(HKCU, 'SOFTWARE\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}') then
    MsgBox(ExpandConstant('{cm:NoWebView2}'), mbInformation, MB_OK);
end;

// Build the SciTE platform .properties content, pointing at the install dir.
function ScitePropsText(): String;
var app, nl: String;
begin
  app := ExpandConstant('{app}');
  nl := #13#10;
  // Plain SciTE syntax keyed on *.ahkw only. Do NOT redefine file.patterns.ahk: SciTE expands
  // $(...) in property NAMES when a line is read, so ahk.properties' lexer/keywords/commands are
  // already bound to "*.ahk;*.scriptlet" and redefining it later leaves .ahkw with no lexer
  // (plain font, no highlighting, ahk.lua/debugger disabled) and makes .ahk commands ambiguous.
  // ".condplat/.if" only work inside platforms.properties, so they are not used here either.
  Result :=
    '# AutoHotkey Web - SciTE4AutoHotkey settings for *.ahkw (generated by the installer).' + nl +
    '# Plain SciTE syntax keyed on *.ahkw only, so regular *.ahk files are not affected.' + nl +
    'AhkWebDir=' + app + nl +
    'lexer.*.ahkw=ahk1' + nl +
    'keywords.*.ahkw=$(ahk.keywords)' + nl +
    'keywords2.*.ahkw=$(ahk.keywords2)' + nl +
    'keywords3.*.ahkw=$(ahk.keywords3)' + nl +
    'keywords4.*.ahkw=$(ahk.keywords4)' + nl +
    'keywords5.*.ahkw=$(ahk.keywords5)' + nl +
    'keywords6.*.ahkw=$(ahk.keywords6)' + nl +
    'keywords7.*.ahkw=$(ahk.keywords7)' + nl +
    'word.characters.*.ahkw=$(chars.alpha)$(chars.numeric)$(chars.accented)$_@#' + nl +
    'abbreviations.*.ahkw=$(SciteUserHome)\AhkAbbrevs.properties' + nl +
    'indent.maintain.*.ahkw=0' + nl +
    'api.*.ahkw=$(ahk.api);$(AhkWebDir)\scite\ahkweb.api' + nl +
    'command.go.*.ahkw="$(AhkWebDir)\AutoHotkeyWeb.exe" /ErrorStdOut "$(FilePath)" $(1) $(2) $(3) $(4)' + nl +
    'command.build.*.ahkw="$(LocalAHK)" "$(SciteDefaultHome)\tools\SciTEDebug.ahk" "$(AhkWebDir)\AutoHotkeyWeb.exe"' + nl +
    'command.compile.*.ahkw="$(AhkWebDir)\compiler\AhkWeb2Exe.exe" /in "$(FilePath)" /bin "$(AhkWebDir)\compiler\AutoHotkeyWeb.bin"' + nl +
    'command.help.subsystem.*.ahkw=2' + nl +
    'command.help.*.ahkw="$(AhkWebDir)\docs\index.htm"' + nl;
end;

// The import goes into SciTEGlobal.properties (machine-wide, next to SciTE.exe): SciTEUser.properties
// lives in each user's Documents\AutoHotkey\SciTE, which an elevated Setup may not even see.
// Append raw ASCII bytes so the file's UTF-8 BOM and Korean text are never re-encoded.
procedure EnsureSciteImport(propsFile: String);
var lines: TArrayOfString; i: Integer; found: Boolean;
begin
  if not LoadStringsFromFile(propsFile, lines) then exit;
  found := False;
  for i := 0 to GetArrayLength(lines) - 1 do
    if Pos('import AutoHotkeyWeb', lines[i]) > 0 then found := True;
  if not found then
    SaveStringToFile(propsFile, #13#10 + '# AutoHotkey Web (*.ahkw)' + #13#10 + 'import AutoHotkeyWeb' + #13#10, True);
end;

procedure RemoveSciteImport(userProps: String);
var lines, outl: TArrayOfString; i, n: Integer;
begin
  if not LoadStringsFromFile(userProps, lines) then exit;
  n := 0;
  SetArrayLength(outl, GetArrayLength(lines));
  for i := 0 to GetArrayLength(lines) - 1 do
    if (Pos('import AutoHotkeyWeb', lines[i]) = 0) and (Pos('# AutoHotkey Web', lines[i]) <> 1) then
    begin
      outl[n] := lines[i];
      n := n + 1;
    end;
  if n = GetArrayLength(lines) then exit;  // nothing to remove: leave the file untouched
  SetArrayLength(outl, n);
  // SciTE4AutoHotkey keeps these files as UTF-8 with BOM (Korean comments); keep that encoding.
  SaveStringsToUTF8File(userProps, outl, False);
end;

// Explorer caches the "New" menu list per user under HKCU. Setup runs elevated, and when the
// elevation used a DIFFERENT admin account, HKCU would be that admin's hive. Run reg.exe as the
// user who launched Setup so their cache is dropped; Explorer rebuilds it (with .ahkw) on demand.
procedure ClearShellNewCache();
var rc: Integer;
begin
  ExecAsOriginalUser(ExpandConstant('{sys}\reg.exe'),
    'delete "HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\Discardable\PostSetup\ShellNew" /f',
    '', SW_HIDE, ewWaitUntilTerminated, rc);
end;

procedure CurStepChanged(CurStep: TSetupStep);
var sc, userProps: String;
begin
  if CurStep = ssPostInstall then
  begin
    ClearShellNewCache();
    if WizardIsTaskSelected('scite') then
    begin
      sc := SciTEDir();
      if sc <> '' then
      begin
        SaveStringToFile(sc + '\AutoHotkeyWeb.properties', ScitePropsText(), False);
        if FileExists(sc + '\SciTEGlobal.properties') then
          EnsureSciteImport(sc + '\SciTEGlobal.properties');
        // Clean up the import older installers put into the (unused) default SciTEUser.properties.
        userProps := sc + '\SciTEUser.properties';
        if FileExists(userProps) then
          RemoveSciteImport(userProps);
      end;
    end;
  end;
end;


procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var sc: String;
begin
  if CurUninstallStep = usUninstall then
  begin
    sc := SciTEDir();
    if sc <> '' then
    begin
      DeleteFile(sc + '\AutoHotkeyWeb.properties');
      if FileExists(sc + '\SciTEGlobal.properties') then
        RemoveSciteImport(sc + '\SciTEGlobal.properties');
      if FileExists(sc + '\SciTEUser.properties') then
        RemoveSciteImport(sc + '\SciTEUser.properties');
    end;
  end;
end;
