; ═══════════════════════════════════════════════════════════════════════
;  Rose — installateur Windows x64 (HKCU, sans droits administrateur)
;  Installe le moteur, l'association .rose avec son icône et le PATH.
;  Construit par build-installer.sh via makensis (NSIS 3).
; ═══════════════════════════════════════════════════════════════════════
Unicode true
!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"

!define APP_NAME    "Rose"
!ifndef APP_VERSION
  !define APP_VERSION "3.14.0"
!endif
!define APP_PUBLISHER "Stef302"
!define EXE_NAME    "rose.exe"
!define UNINST_KEY  "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_NAME}"

Name "${APP_NAME} ${APP_VERSION}"
OutFile "Rose-Setup-${APP_VERSION}.exe"
InstallDir "$LOCALAPPDATA\Programs\Rose"
InstallDirRegKey HKCU "Software\${APP_NAME}" "InstallDir"
RequestExecutionLevel user
SetCompressor /SOLID lzma

!define MUI_ICON   "assets\setup.ico"
!define MUI_UNICON "assets\setup.ico"
!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN "$INSTDIR\bin\${EXE_NAME}"
!define MUI_FINISHPAGE_RUN_TEXT "Lancer « rose info » pour vérifier l'installation"
!define MUI_FINISHPAGE_SHOWREADME "$INSTDIR\README-PARTAGE.md"
!define MUI_FINISHPAGE_SHOWREADME_TEXT "Lire README-PARTAGE.md"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "French"

; ── aide : retire toutes les occurrences de $R1 dans $R0 (résultat $R0) ──
Var BinDir

!macro CORPS_ENLEVER TAG
  StrLen $R2 "$R0"
  StrLen $R3 "$R1"
  StrCpy $R4 0
  StrCpy $R5 ""
  ${TAG}_boucle:
    StrCmp $R4 $R2 ${TAG}_fini
    StrCpy $R6 "$R0" $R3 $R4
    StrCmp $R6 "$R1" ${TAG}_saute
    StrCpy $R7 "$R0" 1 $R4
    StrCpy $R5 "$R5$R7"
    IntOp $R4 $R4 + 1
    Goto ${TAG}_boucle
  ${TAG}_saute:
    IntOp $R4 $R4 + $R3
    Goto ${TAG}_boucle
  ${TAG}_fini:
    StrCpy $R0 "$R5"
!macroend

Function Enlever
  !insertmacro CORPS_ENLEVER INST
FunctionEnd

Function un.Enlever
  !insertmacro CORPS_ENLEVER UNS
FunctionEnd

; ── PATH utilisateur : préfixe $INSTDIR\bin à l'installation, le retirer
# à la désinstallation, puis prévenir l'explorateur (WM_SETTINGCHANGE). ──
Function AjoutPath
  ReadRegStr $R0 HKCU "Environment" "Path"
  StrCpy $R1 "$BinDir;"
  Call Enlever
  StrCpy $R1 "$BinDir"
  Call Enlever
  StrCmp $R0 "" 0 avec_sep
    WriteRegExpandStr HKCU "Environment" "Path" "$BinDir"
    Goto diffuser
  avec_sep:
    WriteRegExpandStr HKCU "Environment" "Path" "$BinDir;$R0"
  diffuser:
  SendMessage ${HWND_BROADCAST} ${WM_SETTINGCHANGE} 0 "UTF:Environment" /TIMEOUT=2000
FunctionEnd

Function un.RetraitPath
  ReadRegStr $R0 HKCU "Environment" "Path"
  StrCpy $R1 "$BinDir;"
  Call un.Enlever
  StrCpy $R1 "$BinDir"
  Call un.Enlever
  WriteRegExpandStr HKCU "Environment" "Path" "$R0"
  SendMessage ${HWND_BROADCAST} ${WM_SETTINGCHANGE} 0 "UTF:Environment" /TIMEOUT=2000
FunctionEnd

Section "Moteur Rose" SecMoteur
  SectionIn RO
  StrCpy $BinDir "$INSTDIR\bin"

  SetOutPath "$INSTDIR\bin"
  File "staging\rose-win64.exe"                  ; → rose.exe
  Rename "rose-win64.exe" "${EXE_NAME}"
  File "assets\rose.ico"                          ; icône des fichiers .rose
  File "assets\\setup.ico"                          ; icône de l'exécutable Rose
  Rename "setup.ico" "rose_app.ico"


  SetOutPath "$INSTDIR"
  File /nonfatal "staging\README-PARTAGE.md"
  File /nonfatal "staging\TESTER.md"

  ; ── association .rose ────────────────────────────────────────────────
  WriteRegStr HKCU "Software\Classes\.rose" "" "Rose.File"
  WriteRegStr HKCU "Software\Classes\Rose.File" "" "Programme Rose"
  WriteRegStr HKCU "Software\Classes\Rose.File\DefaultIcon" "" "$INSTDIR\bin\rose.ico,0"
  WriteRegStr HKCU "Software\Classes\Rose.File\shell\open\command" "" \
              '"$INSTDIR\bin\${EXE_NAME}" "%1"'
  ; « rose » perçoit aussi le type pour l'éditeur de texte
  WriteRegStr HKCU "Software\Classes\SystemFileAssociations\.rose\Shell\edit" "" "Modifier"
  WriteRegStr HKCU "Software\Classes\SystemFileAssociations\.rose\Shell\edit\command" "" \
              '%SystemRoot%\System32\notepad.exe "%1"'

  ; ── trace d'installation + désinstalleur ─────────────────────
  WriteRegStr HKCU "Software\${APP_NAME}" "InstallDir" "$INSTDIR"
  WriteRegStr HKCU "Software\${APP_NAME}" "Version" "${APP_VERSION}"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayName" "${APP_NAME}"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayVersion" "${APP_VERSION}"
  WriteRegStr HKCU "${UNINST_KEY}" "Publisher" "${APP_PUBLISHER}"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\bin\rose_app.ico"
  WriteRegStr HKCU "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINST_KEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegStr HKCU "${UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegDWORD HKCU "${UNINST_KEY}" "EstimatedSize" "$0"

  Call AjoutPath
  WriteUninstaller "$INSTDIR\uninstall.exe"
SectionEnd

Section "Uninstall"
  StrCpy $BinDir "$INSTDIR\bin"
  Call un.RetraitPath

  DeleteRegKey HKCU "Software\Classes\.rose"
  DeleteRegKey HKCU "Software\Classes\Rose.File"
  DeleteRegKey HKCU "Software\Classes\SystemFileAssociations\.rose"
  DeleteRegKey HKCU "${UNINST_KEY}"
  DeleteRegKey HKCU "Software\${APP_NAME}"

  RMDir /r "$INSTDIR\bin"
  Delete "$INSTDIR\README-PARTAGE.md"
  Delete "$INSTDIR\TESTER.md"
  Delete "$INSTDIR\README.txt"
  RMDir /r "$INSTDIR\exemples"
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"
SectionEnd
