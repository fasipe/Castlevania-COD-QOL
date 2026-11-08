# Mostra assembly e C decompilado das funcoes nos enderecos dados, usando o projeto em port/ghidra.
# Exemplo: .\tools\ghidra\dump.ps1 0x00767EC8 0x00767F08
param([Parameter(Mandatory = $true, ValueFromRemainingArguments = $true)][string[]]$Addresses)
$port = Resolve-Path "$PSScriptRoot\..\.."
if (-not $env:GHIDRA_INSTALL_DIR) { $env:GHIDRA_INSTALL_DIR = "$env:USERPROFILE\Desktop\ghidra_12.1.4_PUBLIC" }
if (-not $env:JAVA_HOME) { $env:JAVA_HOME = 'C:\Program Files\Eclipse Adoptium\jdk-25.0.4.101-hotspot' }
$log = & "$env:GHIDRA_INSTALL_DIR\support\analyzeHeadless.bat" "$port\ghidra" CoDEE -process SLES_537.55 -noanalysis `
  -scriptPath "$PSScriptRoot" -postScript Dump.java @Addresses 2>&1
$log | ForEach-Object { "$_" } | Where-Object { $_ -match 'Dump\.java>' -or $_ -notmatch '^(INFO|WARN)|NativeCommandError|CategoryInfo|FullyQualifiedErrorId|^\s*$' } |
  ForEach-Object { $_ -replace '^INFO\s+Dump\.java> ', '' -replace ' \(GhidraScript\)\s*$', '' }
