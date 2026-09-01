; Windows context menu registry entries (Phase 5.3, locked decision #3): static per-user
; (HKCU, no elevation) registry keys under SystemFileAssociations, written on install and
; removed on uninstall via Tauri's NSIS installerHooks (bundle.windows.nsis.installerHooks
; in tauri.conf.json). SystemFileAssociations specifically (not the blanket "*\shell" or a
; ProgID) so entries apply per-extension regardless of whatever program is currently the
; registered handler for that file type, and don't add noise to every file's context menu.
;
; $INSTDIR\${MAINBINARYNAME}.exe is the Tauri shell binary itself (not the bundled
; mediatool-core.exe sidecar) -- it parses --convert/--compress via
; app/desktop/src-tauri/src/cli.rs and, if already running, tauri-plugin-single-instance
; hands the args to that instance instead of starting a second one.
;
; ${MAINBINARYNAME}, NOT a literal "Gravity" (issue #85). Tauri names the installed
; executable after `mainBinaryName` in tauri.conf.json, which defaults to the CARGO PACKAGE
; NAME -- "gravity-desktop" here -- not to productName. (Tauri's own template carries the
; comment "We used to use product name as MAINBINARYNAME" over its shortcut-migration
; code, which is where the older convention this file was written against comes from.)
; So these keys used to register a command pointing at $INSTDIR\Gravity.exe, a file the
; installer never creates: every verb was dead on arrival, and Explorer answered a click on
; one with its generic "How do you want to open this file?" chooser -- issue #85's exact
; symptom, and why #52 kept reproducing after the installer was supposedly fixed.
;
; Tauri !include's this file (line ~35 of its installer.nsi) BEFORE it !define's
; MAINBINARYNAME (line ~52), which is fine and deliberate: NSIS expands ${...} inside a
; macro body when the macro is INSERTED, not when it is defined, and the insertion points
; are in sections far below the define. Verified by compiling this file against a harness
; that reproduces that exact ordering -- makensis emitted
; `\gravity-desktop.exe" --convert "%1"` with no warnings.
;
; Idempotent by construction: WriteRegStr overwrites rather than erroring if the key
; already exists, so re-running on upgrade is safe without any existence checks.

!macro GravityAddConvertEntry Ext
  WriteRegStr HKCU "Software\Classes\SystemFileAssociations\${Ext}\shell\GravityConvert" "" "Convert with Gravity"
  WriteRegStr HKCU "Software\Classes\SystemFileAssociations\${Ext}\shell\GravityConvert" "Icon" "$INSTDIR\${MAINBINARYNAME}.exe,0"
  WriteRegStr HKCU "Software\Classes\SystemFileAssociations\${Ext}\shell\GravityConvert\command" "" '"$INSTDIR\${MAINBINARYNAME}.exe" --convert "%1"'
!macroend

!macro GravityAddCompressEntry Ext
  WriteRegStr HKCU "Software\Classes\SystemFileAssociations\${Ext}\shell\GravityCompress" "" "Compress with Gravity"
  WriteRegStr HKCU "Software\Classes\SystemFileAssociations\${Ext}\shell\GravityCompress" "Icon" "$INSTDIR\${MAINBINARYNAME}.exe,0"
  WriteRegStr HKCU "Software\Classes\SystemFileAssociations\${Ext}\shell\GravityCompress\command" "" '"$INSTDIR\${MAINBINARYNAME}.exe" --compress "%1"'
!macroend

!macro GravityRemoveConvertEntry Ext
  DeleteRegKey HKCU "Software\Classes\SystemFileAssociations\${Ext}\shell\GravityConvert"
!macroend

!macro GravityRemoveCompressEntry Ext
  DeleteRegKey HKCU "Software\Classes\SystemFileAssociations\${Ext}\shell\GravityCompress"
!macroend

!macro NSIS_HOOK_POSTINSTALL
  ; Video: both Convert and Compress.
  !insertmacro GravityAddConvertEntry ".mp4"
  !insertmacro GravityAddCompressEntry ".mp4"
  !insertmacro GravityAddConvertEntry ".mov"
  !insertmacro GravityAddCompressEntry ".mov"
  !insertmacro GravityAddConvertEntry ".mkv"
  !insertmacro GravityAddCompressEntry ".mkv"
  !insertmacro GravityAddConvertEntry ".avi"
  !insertmacro GravityAddCompressEntry ".avi"
  !insertmacro GravityAddConvertEntry ".webm"
  !insertmacro GravityAddCompressEntry ".webm"
  !insertmacro GravityAddConvertEntry ".wmv"
  !insertmacro GravityAddCompressEntry ".wmv"
  !insertmacro GravityAddConvertEntry ".flv"
  !insertmacro GravityAddCompressEntry ".flv"

  ; Audio: Convert only.
  !insertmacro GravityAddConvertEntry ".mp3"
  !insertmacro GravityAddConvertEntry ".wav"
  !insertmacro GravityAddConvertEntry ".flac"
  !insertmacro GravityAddConvertEntry ".aac"
  !insertmacro GravityAddConvertEntry ".m4a"
  !insertmacro GravityAddConvertEntry ".ogg"

  ; Image: Convert only.
  !insertmacro GravityAddConvertEntry ".jpg"
  !insertmacro GravityAddConvertEntry ".jpeg"
  !insertmacro GravityAddConvertEntry ".png"
  !insertmacro GravityAddConvertEntry ".webp"
  !insertmacro GravityAddConvertEntry ".gif"
  !insertmacro GravityAddConvertEntry ".bmp"
!macroend

; issue #60: the base NSIS uninstaller only removes what it installed (program files,
; shortcuts, registry keys) -- it never touches user data written by the running app
; itself (%LOCALAPPDATA%\Gravity: settings.json, logs/, the bundled Python venv). Ask
; once, up front, rather than silently keeping or silently deleting either way.
!macro NSIS_HOOK_PREUNINSTALL
  MessageBox MB_YESNO|MB_ICONQUESTION "Also remove your Gravity settings, logs, and cached data?$\n$\n$LOCALAPPDATA\Gravity" IDNO gravity_keep_userdata
    RMDir /r "$LOCALAPPDATA\Gravity"
  gravity_keep_userdata:

  !insertmacro GravityRemoveConvertEntry ".mp4"
  !insertmacro GravityRemoveCompressEntry ".mp4"
  !insertmacro GravityRemoveConvertEntry ".mov"
  !insertmacro GravityRemoveCompressEntry ".mov"
  !insertmacro GravityRemoveConvertEntry ".mkv"
  !insertmacro GravityRemoveCompressEntry ".mkv"
  !insertmacro GravityRemoveConvertEntry ".avi"
  !insertmacro GravityRemoveCompressEntry ".avi"
  !insertmacro GravityRemoveConvertEntry ".webm"
  !insertmacro GravityRemoveCompressEntry ".webm"
  !insertmacro GravityRemoveConvertEntry ".wmv"
  !insertmacro GravityRemoveCompressEntry ".wmv"
  !insertmacro GravityRemoveConvertEntry ".flv"
  !insertmacro GravityRemoveCompressEntry ".flv"

  !insertmacro GravityRemoveConvertEntry ".mp3"
  !insertmacro GravityRemoveConvertEntry ".wav"
  !insertmacro GravityRemoveConvertEntry ".flac"
  !insertmacro GravityRemoveConvertEntry ".aac"
  !insertmacro GravityRemoveConvertEntry ".m4a"
  !insertmacro GravityRemoveConvertEntry ".ogg"

  !insertmacro GravityRemoveConvertEntry ".jpg"
  !insertmacro GravityRemoveConvertEntry ".jpeg"
  !insertmacro GravityRemoveConvertEntry ".png"
  !insertmacro GravityRemoveConvertEntry ".webp"
  !insertmacro GravityRemoveConvertEntry ".gif"
  !insertmacro GravityRemoveConvertEntry ".bmp"
!macroend
