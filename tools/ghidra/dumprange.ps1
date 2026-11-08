# Mostra o C decompilado de todas as funcoes entre dois enderecos.
# Exemplo: .\tools\ghidra\dumprange.ps1 0x00753000 0x00755000
#          .\tools\ghidra\dumprange.ps1 0x0 0x2000 -Project IOP -Program MODSD.elf
param(
  [Parameter(Mandatory = $true, Position = 0)][string]$Start,
  [Parameter(Mandatory = $true, Position = 1)][string]$End,
  [string]$Project = 'CoDEE',
  [string]$Program = 'SLES_537.55'
)
$port = Resolve-Path "$PSScriptRoot\..\.."
if (-not $env:GHIDRA_INSTALL_DIR) { $env:GHIDRA_INSTALL_DIR = "$env:USERPROFILE\Desktop\ghidra_12.1.4_PUBLIC" }
if (-not $env:JAVA_HOME) { $env:JAVA_HOME = 'C:\Program Files\Eclipse Adoptium\jdk-25.0.4.101-hotspot' }
$log = & "$env:GHIDRA_INSTALL_DIR\support\analyzeHeadless.bat" "$port\ghidra" $Project -process $Program -noanalysis `
  -scriptPath "$PSScriptRoot" -postScript DumpRange.java $Start $End 2>&1
$log | ForEach-Object { "$_" } | Where-Object { $_ -match 'DumpRange.java>' -or $_ -notmatch '^(INFO|WARN)|NativeCommandError|CategoryInfo|FullyQualifiedErrorId|^\s*$' } |
  ForEach-Object { $_ -replace '^INFO\s+DumpRange.java> ', '' -replace ' \(GhidraScript\)\s*$', '' }
